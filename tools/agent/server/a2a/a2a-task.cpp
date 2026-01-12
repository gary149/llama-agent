#include "a2a-task.h"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace a2a {

a2a_task::a2a_task(const std::string & id, const std::string & context_id)
    : id_(id)
    , context_id_(context_id)
    , created_at_(std::chrono::system_clock::now())
    , updated_at_(created_at_) {

    // Record initial status
    task_status initial;
    initial.state = task_state::SUBMITTED;
    initial.timestamp = get_timestamp();
    status_history_.push_back(initial);
}

std::string a2a_task::get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return ss.str();
}

bool a2a_task::is_valid_transition(task_state from, task_state to) {
    // Terminal states cannot transition to anything
    if (is_terminal_state(from)) {
        return false;
    }

    // Valid transitions:
    // SUBMITTED -> WORKING, CANCELLED, REJECTED
    // WORKING -> INPUT_REQUIRED, AUTH_REQUIRED, COMPLETED, FAILED, CANCELLED
    // INPUT_REQUIRED -> WORKING, CANCELLED, FAILED
    // AUTH_REQUIRED -> WORKING, CANCELLED, FAILED

    switch (from) {
        case task_state::SUBMITTED:
            return to == task_state::WORKING ||
                   to == task_state::CANCELLED ||
                   to == task_state::REJECTED;

        case task_state::WORKING:
            return to == task_state::INPUT_REQUIRED ||
                   to == task_state::AUTH_REQUIRED ||
                   to == task_state::COMPLETED ||
                   to == task_state::FAILED ||
                   to == task_state::CANCELLED;

        case task_state::INPUT_REQUIRED:
        case task_state::AUTH_REQUIRED:
            return to == task_state::WORKING ||
                   to == task_state::CANCELLED ||
                   to == task_state::FAILED;

        default:
            return false;
    }
}

bool a2a_task::transition_to(task_state new_state, const std::optional<message> & status_msg) {
    task_state current = state_.load();

    if (!is_valid_transition(current, new_state)) {
        return false;
    }

    // Update state atomically
    state_.store(new_state);

    // Record status change
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        task_status status;
        status.state = new_state;
        status.status_message = status_msg;
        status.timestamp = get_timestamp();
        status_history_.push_back(status);
        current_status_message_ = status_msg;
    }

    // Update timestamp
    {
        std::lock_guard<std::mutex> lock(updated_mutex_);
        updated_at_ = std::chrono::system_clock::now();
    }

    return true;
}

task_status a2a_task::current_status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    task_status status;
    status.state = state_.load();
    status.status_message = current_status_message_;
    status.timestamp = get_timestamp();
    return status;
}

std::vector<task_status> a2a_task::status_history() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_history_;
}

void a2a_task::add_artifact(const artifact & art) {
    std::lock_guard<std::mutex> lock(artifacts_mutex_);
    artifacts_.push_back(art);

    std::lock_guard<std::mutex> lock2(updated_mutex_);
    updated_at_ = std::chrono::system_clock::now();
}

void a2a_task::update_artifact(const std::string & artifact_id, const part & p, bool is_last_chunk) {
    std::lock_guard<std::mutex> lock(artifacts_mutex_);

    for (auto & art : artifacts_) {
        if (art.id == artifact_id) {
            art.parts.push_back(p);
            art.last_chunk = is_last_chunk;
            break;
        }
    }

    std::lock_guard<std::mutex> lock2(updated_mutex_);
    updated_at_ = std::chrono::system_clock::now();
}

std::vector<artifact> a2a_task::get_artifacts() const {
    std::lock_guard<std::mutex> lock(artifacts_mutex_);
    return artifacts_;
}

artifact * a2a_task::find_artifact(const std::string & artifact_id) {
    std::lock_guard<std::mutex> lock(artifacts_mutex_);
    for (auto & art : artifacts_) {
        if (art.id == artifact_id) {
            return &art;
        }
    }
    return nullptr;
}

void a2a_task::add_to_history(const message & msg) {
    std::lock_guard<std::mutex> lock(history_mutex_);
    history_.push_back(msg);

    std::lock_guard<std::mutex> lock2(updated_mutex_);
    updated_at_ = std::chrono::system_clock::now();
}

std::vector<message> a2a_task::get_history() const {
    std::lock_guard<std::mutex> lock(history_mutex_);
    return history_;
}

int a2a_task::subscribe(event_callback cb) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    int id = next_subscriber_id_.fetch_add(1);
    subscribers_[id] = std::move(cb);
    return id;
}

void a2a_task::unsubscribe(int subscriber_id) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    subscribers_.erase(subscriber_id);
}

void a2a_task::broadcast(const json & event) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    for (const auto & [id, cb] : subscribers_) {
        try {
            cb(event);
        } catch (...) {
            // Ignore callback errors
        }
    }
}

void a2a_task::broadcast_status_update(bool is_final) {
    task_status_update_event event;
    event.task_id = id_;
    event.context_id = context_id_;
    event.status = current_status();
    event.final = is_final;

    broadcast(json{{"statusUpdate", event.to_json()}});
}

void a2a_task::broadcast_artifact_update(const artifact & art) {
    task_artifact_update_event event;
    event.task_id = id_;
    event.context_id = context_id_;
    event.art = art;

    broadcast(json{{"artifactUpdate", event.to_json()}});
}

task a2a_task::to_task_object() const {
    task t;
    t.id = id_;
    t.context_id = context_id_;
    t.status = current_status();
    t.artifacts = get_artifacts();
    t.history = get_history();
    return t;
}

json a2a_task::to_json() const {
    return to_task_object().to_json();
}

void a2a_task::set_pending_permission_id(const std::string & request_id) {
    std::lock_guard<std::mutex> lock(permission_mutex_);
    pending_permission_id_ = request_id;
}

std::string a2a_task::get_pending_permission_id() const {
    std::lock_guard<std::mutex> lock(permission_mutex_);
    return pending_permission_id_;
}

void a2a_task::clear_pending_permission() {
    std::lock_guard<std::mutex> lock(permission_mutex_);
    pending_permission_id_.clear();
}

std::chrono::system_clock::time_point a2a_task::updated_at() const {
    std::lock_guard<std::mutex> lock(updated_mutex_);
    return updated_at_;
}

} // namespace a2a
