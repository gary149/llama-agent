#include "agent-loop.h"
#include "console.h"

agent_loop_result agent_loop::run(const json & user_content) {
    agent_loop_result result;
    result.iterations = 0;

    // Add user message (content can be a string or an array of content blocks)
    messages_.push_back({
        {"role", "user"},
        {"content", user_content}
    });
    if (session_file_) session_file_->append_message(messages_.back());

    while (config_.max_iterations <= 0 || result.iterations < config_.max_iterations) {
        if (is_interrupted_.load()) {
            result.stop_reason = agent_stop_reason::USER_CANCELLED;
            return result;
        }

        result.iterations++;

        if (config_.verbose) {
            if (config_.max_iterations > 0) {
                console::log("\n[Iteration %d/%d]\n", result.iterations, config_.max_iterations);
            } else {
                console::log("\n[Iteration %d]\n", result.iterations);
            }
        }

        // Generate completion - returns parsed message with tool calls
        result_timings timings;
        common_chat_msg parsed = generate_completion(timings);

        accumulate_stats(timings);

        // Overflow recovery: compact and retry this iteration
        if (parsed.content.empty() && parsed.tool_calls.empty() && last_completion_overflowed_) {
            last_completion_overflowed_ = false;
            if (config_.compaction.enabled && try_compact()) {
                console::log("[Context compacted, retrying...]\n");
                result.iterations--;
                continue;
            }
        }

        if (parsed.content.empty() && parsed.tool_calls.empty() && is_interrupted_.load()) {
            result.stop_reason = agent_stop_reason::USER_CANCELLED;
            return result;
        }

        // Threshold compaction after successful completion
        if (config_.compaction.enabled) {
            try_compact();
        }

        // Empty response - don't save to history, just end the turn
        if (parsed.content.empty() && parsed.tool_calls.empty()) {
            result.stop_reason = agent_stop_reason::COMPLETED;
            result.final_response = "";
            return result;
        }

        // Add assistant message to history
        json assistant_msg = build_assistant_msg(parsed, result.iterations);
        messages_.push_back(assistant_msg);
        if (session_file_) session_file_->append_message(assistant_msg);

        // If no tool calls, we're done
        if (parsed.tool_calls.empty()) {
            result.stop_reason = agent_stop_reason::COMPLETED;
            result.final_response = parsed.content;
            return result;
        }

        console::log("\n");

        // Execute each tool call
        for (const auto & call : parsed.tool_calls) {
            if (is_interrupted_.load()) {
                result.stop_reason = agent_stop_reason::USER_CANCELLED;
                return result;
            }

            tool_result tool_res = execute_tool_call(call);
            std::string call_id = call.id.empty() ? ("call_" + std::to_string(result.iterations)) : call.id;
            add_tool_result_message(call.name, call_id, tool_res);
        }
    }

    result.stop_reason = agent_stop_reason::MAX_ITERATIONS;
    result.final_response = "Reached maximum iterations (" + std::to_string(config_.max_iterations) + ")";
    return result;
}
