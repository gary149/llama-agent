#pragma once

// A2A Task Manager - Manages tasks and their relationship to sessions

#include "a2a-task.h"
#include "a2a-event-translator.h"
#include "../agent-session.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace a2a {

// Options for listing tasks
struct list_tasks_options {
    std::optional<std::string> context_id;  // Filter by context
    std::optional<task_state> state;        // Filter by state
    int limit = 100;                        // Max results
    std::optional<std::string> cursor;      // Pagination cursor
};

// Result of listing tasks
struct list_tasks_result {
    std::vector<task> tasks;
    std::optional<std::string> next_cursor;
};

// Manages A2A tasks and their relationship to sessions
class a2a_task_manager {
public:
    a2a_task_manager(agent_session_manager & session_mgr);
    ~a2a_task_manager() = default;

    // Create a new task within a context
    // If context_id is empty, creates a new session
    // Returns task_id
    std::string create_task(const std::string & context_id = "");

    // Get task by ID (nullptr if not found)
    a2a_task * get_task(const std::string & task_id);
    const a2a_task * get_task(const std::string & task_id) const;

    // List tasks with optional filtering
    list_tasks_result list_tasks(const list_tasks_options & options = {}) const;

    // Cancel a task
    // Returns true if task was found and cancellation was initiated
    bool cancel_task(const std::string & task_id);

    // Execute a task with a message
    // Creates the task, starts execution, and returns task_id
    // Events are streamed via the task's subscribers
    std::string execute_task(
        const std::string & context_id,
        const message & input_message,
        const agent_session_config & config = {}
    );

    // Resume a task that's in INPUT_REQUIRED state
    // Used when responding to permission requests
    bool resume_task(const std::string & task_id, const message & input);

    // Get the session ID for a task
    std::optional<std::string> get_session_for_task(const std::string & task_id) const;

    // Get all tasks for a context/session
    std::vector<a2a_task *> get_tasks_for_context(const std::string & context_id);

    // Clean up completed tasks older than max_age
    void cleanup(std::chrono::seconds max_age = std::chrono::seconds(3600));

private:
    agent_session_manager & session_mgr_;

    // Task storage
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<a2a_task>> tasks_;
    std::map<std::string, std::string> task_to_session_;  // task_id -> session_id
    std::map<std::string, std::vector<std::string>> session_to_tasks_;  // session_id -> task_ids

    std::atomic<uint64_t> task_counter_{0};

    // Generate unique task ID
    std::string generate_task_id();

    // Get or create session for a context
    std::string ensure_session(const std::string & context_id, const agent_session_config & config);
};

} // namespace a2a
