#pragma once

// A2A Task - Represents a single agent execution within a context

#include "a2a-types.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace a2a {

class a2a_task {
public:
    using event_callback = std::function<void(const json &)>;

    a2a_task(const std::string & id, const std::string & context_id);
    ~a2a_task() = default;

    // Non-copyable, non-movable (due to mutex)
    a2a_task(const a2a_task &) = delete;
    a2a_task & operator=(const a2a_task &) = delete;

    // Getters
    const std::string & id() const { return id_; }
    const std::string & context_id() const { return context_id_; }
    task_state state() const { return state_.load(); }

    // State management
    // Returns true if transition was valid and applied
    bool transition_to(task_state new_state, const std::optional<message> & status_msg = std::nullopt);

    // Get current status
    task_status current_status() const;

    // Get status history (all state transitions)
    std::vector<task_status> status_history() const;

    // Artifact management
    void add_artifact(const artifact & art);
    void update_artifact(const std::string & artifact_id, const part & p, bool is_last_chunk);
    std::vector<artifact> get_artifacts() const;
    artifact * find_artifact(const std::string & artifact_id);

    // Message history
    void add_to_history(const message & msg);
    std::vector<message> get_history() const;

    // Multi-subscriber streaming support
    // Returns subscriber ID that can be used to unsubscribe
    int subscribe(event_callback cb);
    void unsubscribe(int subscriber_id);

    // Broadcast event to all subscribers
    void broadcast(const json & event);

    // Broadcast typed events
    void broadcast_status_update(bool is_final = false);
    void broadcast_artifact_update(const artifact & art);

    // Serialization
    task to_task_object() const;
    json to_json() const;

    // Permission tracking (for input-required state)
    void set_pending_permission_id(const std::string & request_id);
    std::string get_pending_permission_id() const;
    void clear_pending_permission();

    // Timestamps
    std::chrono::system_clock::time_point created_at() const { return created_at_; }
    std::chrono::system_clock::time_point updated_at() const;

private:
    std::string id_;
    std::string context_id_;
    std::atomic<task_state> state_{task_state::SUBMITTED};

    // Status history for tracking all state changes
    mutable std::mutex status_mutex_;
    std::vector<task_status> status_history_;
    std::optional<message> current_status_message_;

    // Artifacts produced by the task
    mutable std::mutex artifacts_mutex_;
    std::vector<artifact> artifacts_;

    // Message history (conversation within this task)
    mutable std::mutex history_mutex_;
    std::vector<message> history_;

    // Multi-subscriber support
    mutable std::mutex subscribers_mutex_;
    std::map<int, event_callback> subscribers_;
    std::atomic<int> next_subscriber_id_{0};

    // Permission tracking
    mutable std::mutex permission_mutex_;
    std::string pending_permission_id_;

    // Timestamps
    std::chrono::system_clock::time_point created_at_;
    mutable std::mutex updated_mutex_;
    std::chrono::system_clock::time_point updated_at_;

    // Get current ISO timestamp
    static std::string get_timestamp();

    // Valid state transitions
    static bool is_valid_transition(task_state from, task_state to);
};

} // namespace a2a
