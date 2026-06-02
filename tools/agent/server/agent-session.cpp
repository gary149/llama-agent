#include "agent-session.h"
#include "../agent-resources.h"
#include "../config-dir.h"

#include <iomanip>
#include <sstream>

// agent_session implementation

agent_session::agent_session(const std::string & id,
                             inference_backend & backend,
                             int32_t inference_id_slot,
                             const agent_session_config & config)
    : id_(id)
    , backend_(backend)
    , inference_id_slot_(inference_id_slot)
    , config_(config)
    , created_at_(std::chrono::steady_clock::now())
    , last_activity_(created_at_) {

    // Set up permission manager
    if (!config_.working_dir.empty()) {
        permissions_.set_project_root(config_.working_dir);
    }
    permissions_.set_yolo_mode(config_.yolo_mode);

    std::string config_dir = get_config_dir();

    agent_resource_config resource_cfg;
    resource_cfg.working_dir = config_.working_dir.empty() ? "." : config_.working_dir;
    resource_cfg.config_dir = config_dir;
    resource_cfg.enable_skills = config_.enable_skills;
    resource_cfg.enable_agents_md = config_.enable_agents_md;
    resource_cfg.extra_skills_paths = config_.extra_skills_paths;
    agent_resource_discovery resources = agent_discover_resources(resource_cfg);

    skills_prompt_section_ = resources.skills_prompt_section();
    agents_md_prompt_section_ = resources.agents_md_prompt_section();
}

agent_session::~agent_session() {
    stop();
}

agent_session_info agent_session::info() const {
    agent_session_info info;
    info.id = id_;
    info.state = state_.load();
    info.created_at = created_at_;
    info.last_activity = last_activity_;
    info.message_count = loop_ ? static_cast<int>(loop_->get_messages().size()) : 0;
    info.stats = loop_ ? loop_->get_stats() : session_stats{};
    return info;
}

void agent_session::send_message(const json & content,
                                  agent_event_callback on_event) {
    // Wait for any previous operation to complete
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    last_activity_ = std::chrono::steady_clock::now();
    is_running_.store(true);
    is_interrupted_.store(false);
    state_.store(agent_session_state::RUNNING);

    // Create agent_loop if it doesn't exist
    if (!loop_) {
        agent_config agent_cfg;
        agent_cfg.max_iterations = config_.max_iterations;
        agent_cfg.tool_timeout_ms = config_.tool_timeout_ms;
        agent_cfg.working_dir = config_.working_dir;
        agent_cfg.yolo_mode = config_.yolo_mode;
        agent_cfg.inference_id_slot = inference_id_slot_;

        // Skills configuration
        agent_cfg.enable_skills = config_.enable_skills;
        agent_cfg.skills_search_paths = config_.extra_skills_paths;
        agent_cfg.skills_prompt_section = skills_prompt_section_;

        // AGENTS.md configuration
        agent_cfg.enable_agents_md = config_.enable_agents_md;
        agent_cfg.agents_md_prompt_section = agents_md_prompt_section_;

        loop_ = std::make_unique<agent_loop>(
            backend_,
            agent_cfg,
            is_interrupted_
        );
    }

    // Run in background thread
    // Pass permissions_ for async permission handling (non-blocking)
    worker_thread_ = std::thread([this, content, on_event]() {
        auto should_stop = [this]() {
            return is_interrupted_.load();
        };

        // Pass async permission manager to avoid blocking on console prompts
        agent_loop_result result = loop_->run_streaming(content, on_event, should_stop, &permissions_);

        {
            std::lock_guard<std::mutex> lock(result_mutex_);
            last_result_ = result;
        }

        state_.store(agent_session_state::IDLE);
        is_running_.store(false);
        last_activity_ = std::chrono::steady_clock::now();
    });
}

std::optional<agent_loop_result> agent_session::get_result() {
    std::lock_guard<std::mutex> lock(result_mutex_);
    return last_result_;
}

void agent_session::cancel() {
    is_interrupted_.store(true);
    permissions_.cancel_all();
}

void agent_session::stop() {
    cancel();
    // Joining is idempotent: after the first join joinable() is false, so the
    // destructor calling stop() again will not attempt a second join.
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

std::vector<permission_request_async> agent_session::pending_permissions() {
    return permissions_.pending();
}

bool agent_session::respond_permission(const std::string & request_id, bool allowed, permission_scope scope) {
    return permissions_.respond(request_id, allowed, scope);
}

json agent_session::get_messages() const {
    if (loop_) {
        return loop_->get_messages();
    }
    return json::array();
}

session_stats agent_session::get_stats() const {
    if (loop_) {
        return loop_->get_stats();
    }
    return session_stats{};
}

void agent_session::clear() {
    if (loop_) {
        loop_->clear();
    }
    permissions_.clear_session();
    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        last_result_.reset();
    }
}

// agent_session_manager implementation

agent_session_manager::agent_session_manager(inference_backend & backend)
    : backend_(backend) {
}

agent_session_manager::~agent_session_manager() {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.clear();
}

std::string agent_session_manager::generate_session_id() {
    uint64_t counter = session_counter_.fetch_add(1);
    std::stringstream ss;
    ss << "sess_" << std::hex << std::setfill('0') << std::setw(8) << counter;
    return ss.str();
}

void agent_session_manager::init_slots_locked() {
    if (slots_initialized_) {
        return;
    }
    slots_initialized_ = true;

    const auto & meta = backend_.meta();
    if (!meta.is_llama_server || meta.total_slots <= 0) {
        return;
    }

    for (int32_t slot = 0; slot < meta.total_slots; ++slot) {
        available_slots_.insert(slot);
    }
}

int32_t agent_session_manager::allocate_slot_locked() {
    init_slots_locked();
    if (available_slots_.empty()) {
        return -1;
    }
    int32_t slot = *available_slots_.begin();
    available_slots_.erase(available_slots_.begin());
    return slot;
}

void agent_session_manager::release_slot_locked(int32_t slot) {
    if (slot >= 0 && slots_initialized_) {
        available_slots_.insert(slot);
    }
}

std::string agent_session_manager::create_session(const agent_session_config & config) {
    std::lock_guard<std::mutex> lock(mutex_);

    // When this backend pins sessions to llama-server slots, refuse to create an
    // unpinned session once the pool is exhausted: an unpinned session sends no
    // id_slot, so llama-server could route it onto another session's slot and evict
    // that session's prompt cache, defeating the isolation the pool provides. The
    // local backend reports total_slots == 0 and legitimately uses slot -1 for every
    // session, so only reject when the pool is actually active.
    const auto & meta = backend_.meta();
    const bool slots_pooled = meta.is_llama_server && meta.total_slots > 0;

    int32_t slot = allocate_slot_locked();
    if (slots_pooled && slot < 0) {
        return std::string(); // pool exhausted — caller surfaces this as an error
    }

    std::string id = generate_session_id();
    auto session = std::make_shared<agent_session>(id, backend_, slot, config);
    sessions_[id] = std::move(session);

    return id;
}

std::shared_ptr<agent_session> agent_session_manager::get_session(const std::string & id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it != sessions_.end()) {
        return it->second;
    }
    return nullptr;
}

bool agent_session_manager::delete_session(const std::string & id) {
    std::shared_ptr<agent_session> session;
    int32_t slot = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            return false;
        }
        slot = it->second->inference_id_slot();
        // Move ownership out so the worker is stopped outside the lock.
        // Note: the slot is NOT released yet — a live worker may still be
        // issuing inference requests pinned to it.
        session = std::move(it->second);
        sessions_.erase(it);
    }
    // Cancel + join the worker so it is guaranteed past all inference calls
    // before the slot can be reallocated to a new session.
    session->stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        release_slot_locked(slot);
    }
    // shared_ptr destructs here (or later if other handlers still hold it)
    return true;
}

std::vector<agent_session_info> agent_session_manager::list_sessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<agent_session_info> result;
    result.reserve(sessions_.size());
    for (const auto & [id, session] : sessions_) {
        result.push_back(session->info());
    }
    return result;
}

size_t agent_session_manager::session_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

void agent_session_manager::cleanup(int idle_timeout_seconds) {
    auto now = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(idle_timeout_seconds);

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        auto info = it->second->info();
        auto idle_duration = now - info.last_activity;
        if (idle_duration > timeout && info.state == agent_session_state::IDLE) {
            release_slot_locked(it->second->inference_id_slot());
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}
