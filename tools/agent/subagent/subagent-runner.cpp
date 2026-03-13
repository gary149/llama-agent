#include "subagent-runner.h"
#include "subagent-display.h"
#include "subagent-output.h"
#include "../agent-loop.h"

#include "common.h"

#include <chrono>
#include <random>
#include <sstream>

subagent_runner::subagent_runner(server_context & server_ctx,
                                  const agent_config & parent_config,
                                  const common_params & params,
                                  const tool_context & parent_tool_ctx)
    : server_ctx_(server_ctx)
    , parent_config_(parent_config)
    , params_(params)
    , parent_tool_ctx_(parent_tool_ctx)
{
}

std::string subagent_runner::build_system_prompt(subagent_type type) const {
    const auto & config = get_subagent_config(type);

    std::ostringstream prompt;

    // Start with parent's base prompt to enable KV cache prefix sharing
    // The server's prompt cache will detect the common prefix and reuse cached tokens
    if (!parent_tool_ctx_.base_system_prompt.empty()) {
        prompt << parent_tool_ctx_.base_system_prompt;
        prompt << "# Subagent Mode: " << config.name << "\n\n";
    } else {
        // Fallback if no base prompt (shouldn't happen in normal operation)
        prompt << "You are a specialized " << config.name << " subagent.\n\n";
    }

    prompt << config.description << "\n\n";

    // Add tool restrictions (overrides base prompt's tool list)
    prompt << "## Tools Available in This Mode\n\n";
    prompt << "You have access to: ";
    bool first = true;
    for (const auto & tool : config.allowed_tools) {
        if (!first) {
            prompt << ", ";
        }
        prompt << tool;
        first = false;
    }
    prompt << "\n\n";

    // Add behavior guidelines based on type
    switch (type) {
        case subagent_type::EXPLORE:
            prompt << R"(# Guidelines

You are in READ-ONLY mode. Your task is to explore and understand the codebase.

- Use `glob` to find files matching patterns
- Use `read` to examine file contents
- Use `bash` ONLY for read-only commands: ls, cat, head, tail, grep, find, git status, git log, git diff
- DO NOT modify any files
- DO NOT run destructive commands

Be thorough but efficient. Report what you find clearly.
)";
            break;

        case subagent_type::PLAN:
            prompt << R"(# Guidelines

You are a planning agent. Your task is to design an implementation approach.

- Use `glob` and `read` to understand existing code structure
- Identify patterns and conventions in the codebase
- Consider edge cases and potential issues
- Provide a clear, actionable plan

Output a structured plan with:
1. Overview of the approach
2. Files to modify/create
3. Step-by-step implementation details
4. Potential risks or considerations
)";
            break;

        case subagent_type::GENERAL:
            prompt << R"(# Guidelines

You are a general-purpose task agent. Complete the assigned task efficiently.

- Read files before modifying them
- Make targeted edits rather than full rewrites
- Test changes when possible
- Report what you accomplished
)";
            break;

        case subagent_type::BASH:
            prompt << R"(# Guidelines

You are a command execution agent. Run shell commands to complete the task.

- Execute commands carefully
- Check command output for errors
- Report results clearly
)";
            break;
    }

    return prompt.str();
}

std::string subagent_runner::generate_task_id() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 35);

    const char charset[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string id = "task-";
    for (int i = 0; i < 8; ++i) {
        id += charset[dis(gen)];
    }
    return id;
}

subagent_result subagent_runner::run(const subagent_params & params) {
    // Synchronous run uses direct console output (no buffer)
    return run_internal(params, nullptr);
}

subagent_result subagent_runner::run_internal(const subagent_params & params,
                                               subagent_output_buffer * buffer) {
    subagent_result result;
    const auto & type_config = get_subagent_config(params.type);

    // Start display scope
    // If buffer is provided (background mode), output goes to buffer
    // Otherwise (synchronous mode), output goes directly to console
    auto & display = subagent_display::instance();
    std::string desc = params.description.empty() ? "subagent" : params.description;
    std::string prompt_preview = params.prompt.substr(0, 60) + (params.prompt.length() > 60 ? "..." : "");

    // Use the appropriate constructor based on whether we have a buffer
    std::unique_ptr<subagent_display::scope> display_scope;
    if (buffer) {
        display_scope = std::make_unique<subagent_display::scope>(display, desc, params.type, prompt_preview, buffer);
    } else {
        display_scope = std::make_unique<subagent_display::scope>(display, desc, params.type, prompt_preview);
    }

    auto start_time = std::chrono::steady_clock::now();

    // Create subagent config with restricted iterations
    agent_config subagent_config = parent_config_;
    subagent_config.max_iterations = type_config.max_iterations;
    subagent_config.verbose = false;  // Suppress verbose output in subagent
    subagent_config.enable_skills = false;  // No skills for subagents
    subagent_config.enable_agents_md = false;  // No agents.md for subagents
    subagent_config.skills_prompt_section = "";
    subagent_config.agents_md_prompt_section = "";

    // Use parent's interrupted flag so Ctrl+C stops subagents too
    // Note: parent_tool_ctx_.is_interrupted is set by the parent agent_loop

    // Build system prompt
    std::string system_prompt = build_system_prompt(params.type);

    // Create tool call callback to report to display
    auto tool_callback = [&display_scope, &result](const std::string & tool_name,
                                                    const std::string & args_summary,
                                                    int elapsed_ms) {
        display_scope->report_tool_call(tool_name, args_summary, elapsed_ms);
        // Also track in result
        std::ostringstream summary;
        summary << tool_name << " (" << elapsed_ms << "ms)";
        result.tool_calls_summary.push_back(summary.str());
    };

    // Create nested agent_loop with filtered tools
    // Pass incremented depth to prevent infinite subagent recursion
    // Pass bash_patterns to enforce read-only restrictions for EXPLORE subagents
    int new_depth = parent_tool_ctx_.subagent_depth + 1;
    std::vector<std::string> bash_patterns(type_config.bash_patterns.begin(), type_config.bash_patterns.end());
    agent_loop subagent(server_ctx_, params_, subagent_config, *parent_tool_ctx_.is_interrupted,
                        type_config.allowed_tools, bash_patterns, system_prompt, new_depth, tool_callback);

    // Run the subagent
    agent_loop_result loop_result = subagent.run(params.prompt);

    auto end_time = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    // Collect token stats from the subagent
    const auto & subagent_stats = subagent.get_stats();
    result.input_tokens = subagent_stats.total_input;
    result.output_tokens = subagent_stats.total_output;
    result.cached_tokens = subagent_stats.total_cached;
    int32_t total_tokens = subagent_stats.total_input + subagent_stats.total_output;

    // Report completion with timing and token usage
    display_scope->report_done(static_cast<int>(elapsed_ms), total_tokens);

    // Convert result
    result.iterations = loop_result.iterations;

    switch (loop_result.stop_reason) {
        case agent_stop_reason::COMPLETED:
            result.success = true;
            result.output = loop_result.final_response;
            break;

        case agent_stop_reason::MAX_ITERATIONS:
            result.success = false;
            result.output = loop_result.final_response;
            result.error = "Reached maximum iterations (" + std::to_string(type_config.max_iterations) + ")";
            break;

        case agent_stop_reason::USER_CANCELLED:
            result.success = false;
            result.error = "User cancelled";
            break;

        case agent_stop_reason::AGENT_ERROR:
            result.success = false;
            result.error = "Agent error: " + loop_result.final_response;
            break;
    }

    return result;
}

std::string subagent_runner::start_background(const subagent_params & params) {
    std::string task_id = generate_task_id();

    // Create output buffer for this background task
    auto & output_mgr = subagent_output_manager::instance();
    subagent_output_buffer * buffer = output_mgr.create_buffer(task_id);

    auto task = std::make_unique<subagent_task>();
    task->id = task_id;
    task->params = params;

    // Capture what we need for the thread
    auto * task_ptr = task.get();

    // Start background thread with buffer for output
    task->thread = std::thread([this, task_ptr, params, buffer, task_id]() {
        subagent_result result;
        try {
            result = this->run_internal(params, buffer);
        } catch (const std::exception & e) {
            result.success = false;
            result.error = std::string("Exception: ") + e.what();
        }

        // Flush buffered output before completing
        buffer->flush(true);

        task_ptr->promise.set_value(std::move(result));
        task_ptr->complete.store(true);

        // Cleanup buffer
        subagent_output_manager::instance().remove_buffer(task_id);
    });

    // Store task
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        tasks_[task_id] = std::move(task);
    }

    return task_id;
}

bool subagent_runner::is_complete(const std::string & task_id) const {
    std::lock_guard<std::mutex> lock(tasks_mutex_);

    // Check completed results first
    if (completed_results_.find(task_id) != completed_results_.end()) {
        return true;
    }

    // Check active tasks
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        return false;  // Task doesn't exist
    }

    return it->second->complete.load();
}

subagent_result subagent_runner::get_result(const std::string & task_id) {
    std::lock_guard<std::mutex> lock(tasks_mutex_);

    // Check completed results first
    auto completed_it = completed_results_.find(task_id);
    if (completed_it != completed_results_.end()) {
        return completed_it->second;
    }

    // Check active tasks
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        subagent_result empty;
        empty.error = "Task not found: " + task_id;
        return empty;
    }

    if (!it->second->complete.load()) {
        subagent_result pending;
        pending.error = "Task still running: " + task_id;
        return pending;
    }

    // Get result and cleanup
    subagent_result result;
    try {
        std::future<subagent_result> future = it->second->promise.get_future();
        result = future.get();
    } catch (const std::exception & e) {
        result.success = false;
        result.error = std::string("Failed to get result: ") + e.what();
    }

    // Join thread
    if (it->second->thread.joinable()) {
        it->second->thread.join();
    }

    // Store in completed results and remove from active
    completed_results_[task_id] = result;
    tasks_.erase(it);

    return result;
}

void subagent_runner::cancel(const std::string & task_id) {
    std::lock_guard<std::mutex> lock(tasks_mutex_);

    auto it = tasks_.find(task_id);
    if (it != tasks_.end()) {
        it->second->cancelled.store(true);
        // Note: Actual cancellation depends on the agent loop checking the interrupted flag
        // which is shared with the parent. For true cancellation, we'd need a per-task flag.
    }
}

std::vector<std::string> subagent_runner::get_active_tasks() const {
    std::lock_guard<std::mutex> lock(tasks_mutex_);

    std::vector<std::string> active;
    for (const auto & [id, task] : tasks_) {
        if (!task->complete.load()) {
            active.push_back(id);
        }
    }
    return active;
}

void subagent_runner::cleanup_completed() {
    std::lock_guard<std::mutex> lock(tasks_mutex_);

    for (auto it = tasks_.begin(); it != tasks_.end(); ) {
        if (it->second->complete.load()) {
            // Join thread if joinable
            if (it->second->thread.joinable()) {
                it->second->thread.join();
            }
            it = tasks_.erase(it);
        } else {
            ++it;
        }
    }
}
