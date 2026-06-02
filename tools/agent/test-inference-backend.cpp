#include "agent-loop.h"

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

private:
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
    return 0;
}
