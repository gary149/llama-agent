#include "agent-loop.h"
#include "agent-loop-internal.h"

#include <functional>
#include <mutex>

// Definition of the shared mutex declared in agent-loop-internal.h
std::mutex g_completion_mutex;

// Streaming version of generate_completion
common_chat_msg agent_loop::generate_completion_streaming(
    result_timings & out_timings,
    agent_event_callback on_event,
    std::function<bool()> should_stop) {

    server_response_reader rd = server_ctx_.get_response_reader();
    {
        std::lock_guard<std::mutex> lock(g_completion_mutex);

        server_task task = server_task(SERVER_TASK_TYPE_COMPLETION);
        task.id        = rd.get_new_id();
        task.index     = 0;

        // Route through the same OAI-compat code path as the HTTP server.
        auto meta = server_ctx_.get_meta();
        auto chat_tools = tool_registry::instance().to_chat_tools();
        json body = build_oai_request_body(chat_tools, meta.has_inp_image);
        std::vector<raw_buffer> files;
        json data = oaicompat_chat_params_parse(body, meta.chat_params, files);

        task.params = server_task::params_from_json_cmpl(vocab_, *params_, meta.slot_n_ctx, meta.logit_bias_eog, data);

        task.cli        = true;
        task.cli_prompt = data.at("prompt").get<std::string>();
        task.cli_files  = std::move(files);

        rd.post_task(std::move(task));
    }

    server_task_result_ptr result;
    try {
        result = rd.next(should_stop);
    } catch (const std::exception & e) {
        LOG_WRN("Failed to parse model output: %s\n", e.what());
        common_chat_msg msg;
        msg.role = "assistant";
        return msg;
    }

    std::string full_content;
    bool was_aborted = false;

    while (result) {
        if (should_stop()) {
            was_aborted = true;
            break;
        }
        if (result->is_error()) {
            auto * err = dynamic_cast<server_task_result_error *>(result.get());
            if (err && err->err_type == ERROR_TYPE_EXCEED_CONTEXT_SIZE) {
                last_completion_overflowed_ = true;
                // Don't emit error event — overflow may be recovered via compaction
                common_chat_msg empty_msg;
                return empty_msg;
            }
            json err_data = result->to_json();
            std::string err_msg = err_data.value("message", "Unknown error");
            on_event(agent_event::error(err_msg));
            common_chat_msg empty_msg;
            return empty_msg;
        }

        auto res_partial = dynamic_cast<server_task_result_cmpl_partial *>(result.get());
        if (res_partial) {
            out_timings = std::move(res_partial->timings);
            for (const auto & diff : res_partial->oaicompat_msg_diffs) {
                if (!diff.content_delta.empty()) {
                    on_event(agent_event::text_delta(diff.content_delta));
                    full_content += diff.content_delta;
                }
                if (!diff.reasoning_content_delta.empty()) {
                    on_event(agent_event::reasoning_delta(diff.reasoning_content_delta));
                }
            }
        }

        auto res_final = dynamic_cast<server_task_result_cmpl_final *>(result.get());
        if (res_final) {
            out_timings = std::move(res_final->timings);
            last_prompt_tokens_ = res_final->n_prompt_tokens;
            if (!res_final->oaicompat_msg.empty()) {
                return res_final->oaicompat_msg;
            }
            if (!res_final->content.empty()) {
                full_content = res_final->content;
            }
            break;
        }

        try {
            result = rd.next(should_stop);
        } catch (const std::exception & e) {
            LOG_WRN("Failed to parse model output: %s\n", e.what());
            break;
        }
    }

    if (was_aborted) {
        common_chat_msg msg;
        msg.role = "assistant";
        msg.content = full_content;
        return msg;
    }

    common_chat_msg msg;
    msg.role = "assistant";
    msg.content = full_content;
    return msg;
}
