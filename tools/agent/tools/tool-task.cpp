#include "../tool-registry.h"
#include "../subagent/subagent-types.h"
#include "../subagent/subagent-display.h"
#include "../subagent/subagent-runner.h"
#include "../agent-loop.h"

#include <map>
#include <memory>
#include <mutex>
#include <sstream>

// Global runner storage for background tasks (shared across tool calls)
// This is needed because the runner must persist across multiple tool invocations
static std::mutex g_runner_mutex;
static std::map<std::string, std::unique_ptr<subagent_runner>> g_runners;
static std::map<std::string, std::string> g_task_to_runner;  // Maps task_id to runner_key

// Generate a key for identifying a runner instance
static std::string get_runner_key(const tool_context & ctx) {
    // Use server context pointer as unique identifier
    std::ostringstream key;
    key << "runner_" << ctx.server_ctx_ptr;
    return key.str();
}

// Get or create a runner for this context
static subagent_runner & get_runner(const tool_context & ctx) {
    std::lock_guard<std::mutex> lock(g_runner_mutex);

    std::string key = get_runner_key(ctx);
    auto it = g_runners.find(key);
    if (it == g_runners.end()) {
        auto * server_ctx = static_cast<server_context *>(ctx.server_ctx_ptr);
        auto * agent_config = static_cast<struct agent_config *>(ctx.agent_config_ptr);
        auto * common_params_ptr = static_cast<common_params *>(ctx.common_params_ptr);

        auto runner = std::make_unique<subagent_runner>(*server_ctx, *agent_config, *common_params_ptr, ctx);
        g_runners[key] = std::move(runner);
    }
    return *g_runners[key];
}

// Update parent's session stats with subagent token usage
static void update_parent_stats(const tool_context & ctx, const subagent_result & result) {
    if (!ctx.session_stats_ptr) {
        return;
    }

    auto * stats = static_cast<session_stats *>(ctx.session_stats_ptr);

    // Add to subagent-specific counters (for breakdown display)
    stats->subagent_input += result.input_tokens;
    stats->subagent_output += result.output_tokens;
    stats->subagent_cached += result.cached_tokens;
    stats->subagent_count++;

    // Also add to totals so "Main = Total - Subagent" works correctly
    stats->total_input += result.input_tokens;
    stats->total_output += result.output_tokens;
    stats->total_cached += result.cached_tokens;
}

static tool_result task_execute(const json & args, const tool_context & ctx) {
    // Check depth limit using tool context's actual depth, not display's visual nesting
    auto & display = subagent_display::instance();
    if (ctx.subagent_depth >= display.max_depth()) {
        return {false, "",
            "Cannot spawn subagent: maximum nesting depth reached (depth=" +
            std::to_string(ctx.subagent_depth) + ", max=" +
            std::to_string(display.max_depth()) + ")"};
    }

    // Parse parameters
    std::string type_str = args.value("subagent_type", "general");
    std::string prompt = args.value("prompt", "");
    std::string description = args.value("description", "");
    bool run_in_background = args.value("run_in_background", false);
    std::string resume_id = args.value("resume", "");

    // Check if we have the required context pointers
    if (!ctx.server_ctx_ptr || !ctx.agent_config_ptr || !ctx.common_params_ptr) {
        return {false, "", "Internal error: subagent context not initialized"};
    }

    // Handle resume mode
    if (!resume_id.empty()) {
        auto & runner = get_runner(ctx);

        if (runner.is_complete(resume_id)) {
            subagent_result result = runner.get_result(resume_id);

            // Update parent stats with subagent token usage
            update_parent_stats(ctx, result);

            // Format output
            std::ostringstream output;
            output << "Background task " << resume_id << " completed";
            output << (result.success ? " successfully" : " with errors");
            output << " in " << result.iterations << " iteration(s)\n";

            if (!result.tool_calls_summary.empty()) {
                output << "\nTools called:\n";
                for (const auto & tc : result.tool_calls_summary) {
                    output << "  - " << tc << "\n";
                }
            }

            if (!result.output.empty()) {
                output << "\nResult:\n" << result.output;
            }

            return {result.success, output.str(), result.error};
        } else {
            // Task still running or doesn't exist
            auto active_tasks = runner.get_active_tasks();
            bool task_exists = false;
            for (const auto & task : active_tasks) {
                if (task == resume_id) {
                    task_exists = true;
                    break;
                }
            }

            if (task_exists) {
                return {true, "Task " + resume_id + " is still running. Call task with resume=\"" + resume_id + "\" again later to get results.", ""};
            } else {
                return {false, "", "Task not found: " + resume_id + ". It may have already completed or never existed."};
            }
        }
    }

    // For new tasks, prompt is required
    if (prompt.empty()) {
        return {false, "", "The 'prompt' parameter is required for new tasks"};
    }

    // Parse subagent type
    subagent_type type;
    try {
        type = parse_subagent_type(type_str);
    } catch (const std::exception & e) {
        return {false, "", std::string("Invalid subagent_type: ") + e.what() +
            ". Valid types: explore, plan, general, bash"};
    }

    // Prepare subagent params
    subagent_params task_params;
    task_params.type = type;
    task_params.prompt = prompt;
    task_params.description = description.empty() ? type_str + "-task" : description;

    auto & runner = get_runner(ctx);

    if (run_in_background) {
        // Start background task
        std::string task_id = runner.start_background(task_params);

        std::ostringstream output;
        output << "Started background task: " << task_id << "\n";
        output << "Type: " << subagent_type_name(type) << "\n";
        output << "Description: " << task_params.description << "\n\n";
        output << "To check status or get results, call task with resume=\"" << task_id << "\"";

        return {true, output.str(), ""};
    } else {
        // Run synchronously
        subagent_result result = runner.run(task_params);

        // Update parent stats with subagent token usage
        update_parent_stats(ctx, result);

        // Format output
        std::ostringstream output;
        output << "Subagent (" << subagent_type_name(type) << ") ";
        output << (result.success ? "completed" : "failed");
        output << " in " << result.iterations << " iteration(s)\n";

        if (!result.tool_calls_summary.empty()) {
            output << "\nTools called:\n";
            for (const auto & tc : result.tool_calls_summary) {
                output << "  - " << tc << "\n";
            }
        }

        if (!result.output.empty()) {
            output << "\nResult:\n" << result.output;
        }

        return {result.success, output.str(), result.error};
    }
}

static tool_def task_tool = {
    "task",
    "Spawn a subagent to handle a complex task autonomously. Use for parallel exploration, "
    "planning, or delegating multi-step operations. The subagent runs with restricted tools "
    "based on its type and returns results when complete.\n\n"
    "Types:\n"
    "- explore: Read-only codebase exploration (glob, read, limited bash)\n"
    "- plan: Architecture and design planning (glob, read)\n"
    "- general: Multi-step task execution (all tools except task)\n"
    "- bash: Shell command execution only\n\n"
    "Background mode:\n"
    "- Set run_in_background=true to start the task without waiting\n"
    "- Returns a task_id that can be used with the resume parameter\n"
    "- Call again with resume=\"task_id\" to check status or get results",
    R"json({
        "type": "object",
        "properties": {
            "subagent_type": {
                "type": "string",
                "enum": ["explore", "plan", "general", "bash"],
                "description": "Type of subagent to spawn. Each type has different tool access.",
                "default": "general"
            },
            "prompt": {
                "type": "string",
                "description": "The task description for the subagent to execute. Required for new tasks."
            },
            "description": {
                "type": "string",
                "description": "Short description shown in output (3-5 words)"
            },
            "run_in_background": {
                "type": "boolean",
                "description": "If true, start the task in background and return immediately with a task_id",
                "default": false
            },
            "resume": {
                "type": "string",
                "description": "Task ID to resume/check status. When provided, other parameters are ignored."
            }
        },
        "required": []
    })json",
    task_execute
};

REGISTER_TOOL(task, task_tool);
