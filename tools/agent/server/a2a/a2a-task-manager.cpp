#include "a2a-task-manager.h"

#include <iomanip>
#include <sstream>

namespace a2a {

a2a_task_manager::a2a_task_manager(agent_session_manager & session_mgr)
    : session_mgr_(session_mgr) {
}

std::string a2a_task_manager::generate_task_id() {
    uint64_t counter = task_counter_.fetch_add(1);
    std::stringstream ss;
    ss << "task_" << std::hex << std::setfill('0') << std::setw(8) << counter;
    return ss.str();
}

std::string a2a_task_manager::ensure_session(const std::string & context_id, const agent_session_config & config) {
    if (!context_id.empty()) {
        // Check if session exists
        if (session_mgr_.get_session(context_id)) {
            return context_id;
        }
    }

    // Create new session
    return session_mgr_.create_session(config);
}

std::string a2a_task_manager::create_task(const std::string & context_id) {
    std::string task_id = generate_task_id();
    std::string session_id = context_id.empty() ? "" : context_id;

    std::lock_guard<std::mutex> lock(mutex_);

    // Create task with context_id (may be empty until execution)
    auto task = std::make_unique<a2a_task>(task_id, session_id);
    tasks_[task_id] = std::move(task);

    if (!session_id.empty()) {
        task_to_session_[task_id] = session_id;
        session_to_tasks_[session_id].push_back(task_id);
    }

    return task_id;
}

a2a_task * a2a_task_manager::get_task(const std::string & task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(task_id);
    if (it != tasks_.end()) {
        return it->second.get();
    }
    return nullptr;
}

const a2a_task * a2a_task_manager::get_task(const std::string & task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(task_id);
    if (it != tasks_.end()) {
        return it->second.get();
    }
    return nullptr;
}

list_tasks_result a2a_task_manager::list_tasks(const list_tasks_options & options) const {
    std::lock_guard<std::mutex> lock(mutex_);

    list_tasks_result result;
    int count = 0;
    bool started = !options.cursor.has_value();

    for (const auto & [id, task_ptr] : tasks_) {
        // Handle pagination cursor
        if (!started) {
            if (id == *options.cursor) {
                started = true;
            }
            continue;
        }

        // Apply filters
        if (options.context_id && task_ptr->context_id() != *options.context_id) {
            continue;
        }
        if (options.state && task_ptr->state() != *options.state) {
            continue;
        }

        // Check limit
        if (count >= options.limit) {
            result.next_cursor = id;
            break;
        }

        result.tasks.push_back(task_ptr->to_task_object());
        count++;
    }

    return result;
}

bool a2a_task_manager::cancel_task(const std::string & task_id) {
    a2a_task * task = get_task(task_id);
    if (!task) {
        return false;
    }

    // Check if task is in a cancelable state
    if (is_terminal_state(task->state())) {
        return false;
    }

    // Transition to CANCELLED
    message cancel_msg;
    cancel_msg.role = "agent";
    cancel_msg.parts.push_back(part::from_text("Task cancelled"));

    if (!task->transition_to(task_state::CANCELLED, cancel_msg)) {
        return false;
    }

    // Cancel the underlying session operation if running
    std::optional<std::string> session_id = get_session_for_task(task_id);
    if (session_id) {
        agent_session * session = session_mgr_.get_session(*session_id);
        if (session) {
            session->cancel();
        }
    }

    // Broadcast final status
    task->broadcast_status_update(true);

    return true;
}

std::string a2a_task_manager::execute_task(
    const std::string & context_id,
    const message & input_message,
    const agent_session_config & config
) {
    // Ensure we have a session
    std::string session_id = ensure_session(context_id, config);

    // Create task
    std::string task_id = generate_task_id();

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto task = std::make_unique<a2a_task>(task_id, session_id);

        // Add input message to history
        task->add_to_history(input_message);

        tasks_[task_id] = std::move(task);
        task_to_session_[task_id] = session_id;
        session_to_tasks_[session_id].push_back(task_id);
    }

    // Get task pointer
    a2a_task * task = get_task(task_id);
    if (!task) {
        return "";  // Should not happen
    }

    // Get session
    agent_session * session = session_mgr_.get_session(session_id);
    if (!session) {
        task->transition_to(task_state::FAILED, message::text("agent", "Session not found"));
        task->broadcast_status_update(true);
        return task_id;
    }

    // Extract text content from message
    std::string content = input_message.get_text_content();

    // Create event translator
    auto translator = std::make_shared<a2a_event_translator>(*task);

    // Start execution
    session->send_message(content, translator->get_callback());

    return task_id;
}

bool a2a_task_manager::resume_task(const std::string & task_id, const message & input) {
    a2a_task * task = get_task(task_id);
    if (!task) {
        return false;
    }

    // Task must be in INPUT_REQUIRED state
    if (task->state() != task_state::INPUT_REQUIRED) {
        return false;
    }

    // Get the pending permission request ID
    std::string permission_id = task->get_pending_permission_id();
    if (permission_id.empty()) {
        return false;
    }

    // Get the session
    std::optional<std::string> session_id = get_session_for_task(task_id);
    if (!session_id) {
        return false;
    }

    agent_session * session = session_mgr_.get_session(*session_id);
    if (!session) {
        return false;
    }

    // Interpret the input message as a permission response
    // Look for "allow", "yes", "approve" -> allowed
    // Otherwise -> denied
    std::string input_text = input.get_text_content();
    bool allowed = false;

    // Check for common approval patterns
    std::string lower_input;
    for (char c : input_text) {
        lower_input += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (lower_input.find("allow") != std::string::npos ||
        lower_input.find("yes") != std::string::npos ||
        lower_input.find("approve") != std::string::npos ||
        lower_input.find("permit") != std::string::npos ||
        lower_input.find("grant") != std::string::npos ||
        lower_input == "y" ||
        lower_input == "ok") {
        allowed = true;
    }

    // Check for structured response in metadata
    if (input.metadata && (*input.metadata).contains("allowed")) {
        allowed = (*input.metadata)["allowed"].get<bool>();
    }

    // Respond to permission
    session->respond_permission(permission_id, allowed, permission_scope::ONCE);

    // Add input to history
    task->add_to_history(input);

    return true;
}

std::optional<std::string> a2a_task_manager::get_session_for_task(const std::string & task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = task_to_session_.find(task_id);
    if (it != task_to_session_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<a2a_task *> a2a_task_manager::get_tasks_for_context(const std::string & context_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<a2a_task *> result;
    auto it = session_to_tasks_.find(context_id);
    if (it != session_to_tasks_.end()) {
        for (const auto & task_id : it->second) {
            auto task_it = tasks_.find(task_id);
            if (task_it != tasks_.end()) {
                result.push_back(task_it->second.get());
            }
        }
    }
    return result;
}

void a2a_task_manager::cleanup(std::chrono::seconds max_age) {
    auto now = std::chrono::system_clock::now();

    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> to_remove;

    for (const auto & [id, task] : tasks_) {
        // Only clean up terminal tasks
        if (!is_terminal_state(task->state())) {
            continue;
        }

        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            now - task->updated_at());

        if (age > max_age) {
            to_remove.push_back(id);
        }
    }

    for (const auto & id : to_remove) {
        // Remove from session_to_tasks
        auto session_it = task_to_session_.find(id);
        if (session_it != task_to_session_.end()) {
            auto & task_list = session_to_tasks_[session_it->second];
            task_list.erase(
                std::remove(task_list.begin(), task_list.end(), id),
                task_list.end());
            task_to_session_.erase(session_it);
        }

        // Remove task
        tasks_.erase(id);
    }
}

} // namespace a2a
