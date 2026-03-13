#pragma once

#include "subagent-types.h"
#include "../tool-registry.h"

#include <atomic>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward declarations
struct server_context;
struct agent_config;
struct common_params;
class subagent_output_buffer;

// Parameters for running a subagent
struct subagent_params {
    subagent_type type = subagent_type::GENERAL;
    std::string prompt;
    std::string description;  // Short description for display
};

// Result from a subagent run
struct subagent_result {
    bool success = false;
    std::string output;
    std::string error;
    int iterations = 0;
    std::vector<std::string> tool_calls_summary;  // List of tools called with timing

    // Token statistics from the subagent run
    int32_t input_tokens = 0;
    int32_t output_tokens = 0;
    int32_t cached_tokens = 0;
};

// Background task state
struct subagent_task {
    std::string id;
    std::thread thread;
    std::promise<subagent_result> promise;
    std::atomic<bool> complete{false};
    std::atomic<bool> cancelled{false};
    subagent_params params;  // Store params for display

    // Non-copyable, non-movable (atomics prevent move)
    subagent_task() = default;
    subagent_task(const subagent_task &) = delete;
    subagent_task & operator=(const subagent_task &) = delete;
    subagent_task(subagent_task &&) = delete;
    subagent_task & operator=(subagent_task &&) = delete;
};

// Runs a subagent with restricted tool access
// This uses the same server_context (model) but with filtered tools
class subagent_runner {
public:
    subagent_runner(server_context & server_ctx,
                    const agent_config & parent_config,
                    const common_params & params,
                    const tool_context & parent_tool_ctx);

    // Run a subagent synchronously (blocking)
    subagent_result run(const subagent_params & params);

    // Run a subagent in the background (non-blocking)
    // Returns a task ID that can be used to check status and get results
    std::string start_background(const subagent_params & params);

    // Check if a background task is complete
    bool is_complete(const std::string & task_id) const;

    // Get the result of a completed background task
    // Returns empty result if task doesn't exist or isn't complete
    subagent_result get_result(const std::string & task_id);

    // Cancel a background task
    void cancel(const std::string & task_id);

    // Get all active task IDs
    std::vector<std::string> get_active_tasks() const;

    // Cleanup completed tasks (call periodically)
    void cleanup_completed();

private:
    server_context & server_ctx_;
    const agent_config & parent_config_;
    const common_params & params_;
    tool_context parent_tool_ctx_;

    // Background task management
    mutable std::mutex tasks_mutex_;
    std::map<std::string, std::unique_ptr<subagent_task>> tasks_;
    std::map<std::string, subagent_result> completed_results_;  // Store results after thread joins

    // Build a system prompt for the subagent type
    std::string build_system_prompt(subagent_type type) const;

    // Generate unique task ID
    static std::string generate_task_id();

    // Internal run method with optional buffer for background tasks
    subagent_result run_internal(const subagent_params & params,
                                  subagent_output_buffer * buffer);
};
