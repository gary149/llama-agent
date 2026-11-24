#include "agent-loop.h"

#undef NDEBUG
#include <cassert>

class fake_inference_backend : public inference_backend {
public:
    const inference_backend_meta & meta() const override {
        return meta_;
    }

    inference_result complete(
        const inference_request & request,
        std::function<void(const inference_event &)> on_event,
        std::function<bool()> should_stop) override {

        if (should_stop) {
            assert(!should_stop());
        }
        if (!request.messages.is_array()) {
            assert(false);
        }
        calls++;

        if (fail) {
            inference_result failed;
            failed.error = "backend down";
            on_event({inference_event_type::ERROR, {}, {}, failed.error, {}});
            return failed;
        }
        if (overflow_once) {
            // like an OpenAI-compatible server: no prompt token count with the error
            overflow_once = false;
            inference_result overflowed;
            overflowed.context_overflow = true;
            overflowed.error = "maximum context length";
            return overflowed;
        }

        common_chat_msg_diff diff;
        diff.content_delta = "done";
        on_event({inference_event_type::TEXT_DELTA, diff, {}, "", {}});

        inference_result result;
        result.message.role = "assistant";
        result.message.content = "done";
        result.prompt_tokens = 12;
        result.cached_prompt_tokens = 5;
        result.timings.prompt_n = 12;
        result.timings.predicted_n = 2;
        return result;
    }

    int calls = 0;
    bool fail = false;
    bool overflow_once = false;
    inference_backend_meta meta_ = {
        "fake-model",
        "fake-build",
        4096,
        false,
        false,
        true,
        false,
        1,
    };
};

int main() {
    fake_inference_backend backend;
    std::atomic<bool> interrupted{false};

    agent_config config;
    config.working_dir = ".";
    config.compaction.enabled = false;

    agent_loop loop(backend, config, interrupted);

    int text_events = 0;
    agent_loop_result result = loop.run_streaming(
        "hello",
        [&](const agent_event & event) {
            if (event.type == agent_event_type::TEXT_DELTA) {
                text_events++;
                assert(event.data.at("content") == "done");
            }
        });

    assert(result.stop_reason == agent_stop_reason::COMPLETED);
    assert(result.final_response == "done");
    assert(result.iterations == 1);
    assert(text_events == 1);
    assert(backend.calls == 1);
    assert(loop.get_stats().total_input == 12);
    assert(loop.get_stats().total_output == 2);
    assert(loop.get_stats().total_cached == 5);

    const std::string local_claim = "no data leaves their system";
    assert(loop.get_messages()[0]["content"].get<std::string>().find(local_claim) != std::string::npos);

    // a backend failure with no output is an error, not a completed turn
    backend.fail = true;
    int error_events = 0;
    std::string completed_reason;
    result = loop.run_streaming(
        "again",
        [&](const agent_event & event) {
            if (event.type == agent_event_type::ERROR) {
                error_events++;
            } else if (event.type == agent_event_type::COMPLETED) {
                completed_reason = event.data.at("reason").get<std::string>();
            }
        });
    assert(result.stop_reason == agent_stop_reason::AGENT_ERROR);
    assert(error_events == 1);
    assert(completed_reason == "error");

    // a context overflow error with no prompt token count still compacts and retries
    fake_inference_backend overflow_backend;
    agent_config compact_config = config;
    compact_config.compaction.enabled = true;
    agent_loop compact_loop(overflow_backend, compact_config, interrupted);
    const std::string long_text(4000, 'x');
    for (int i = 0; i < 4; i++) {
        compact_loop.run_streaming(long_text, [](const agent_event &) {});
    }
    overflow_backend.overflow_once = true;
    overflow_backend.calls = 0;
    int compactions = 0;
    result = compact_loop.run_streaming(
        long_text,
        [&](const agent_event & event) {
            if (event.type == agent_event_type::COMPACTION_COMPLETED) {
                compactions++;
            }
        });
    assert(result.stop_reason == agent_stop_reason::COMPLETED);
    assert(compactions == 1);
    // the failed request, the summary, the retry
    assert(overflow_backend.calls == 3);

    // a backend that is not local must not get the local inference claim
    fake_inference_backend remote_backend;
    remote_backend.meta_.local = false;
    agent_loop remote_loop(remote_backend, config, interrupted);
    std::string remote_prompt = remote_loop.get_messages()[0]["content"].get<std::string>();
    assert(remote_prompt.find(local_claim) == std::string::npos);
    assert(remote_prompt.find("llama.cpp") == std::string::npos);
    return 0;
}
