#include "http-inference-backend.h"

#include <cpp-httplib/httplib.h>

#include <cassert>
#include <thread>

static std::string sse(const json & payload) {
    return "data: " + payload.dump() + "\n\n";
}

int main() {
    httplib::Server server;
    json captured_body;

    server.Get("/props", [](const httplib::Request &, httplib::Response & res) {
        res.set_content(json{
            {"model_alias", "mock-model"},
            {"total_slots", 1},
            {"default_generation_settings", {{"n_ctx", 2048}}},
            {"modalities", {{"vision", true}, {"audio", false}}},
        }.dump(), "application/json");
    });

    server.Post("/v1/chat/completions", [&](const httplib::Request & req, httplib::Response & res) {
        captured_body = json::parse(req.body);
        res.set_chunked_content_provider(
            "text/event-stream",
            [](size_t, httplib::DataSink & sink) {
                std::string first = sse({
                    {"choices", json::array({{{"index", 0}, {"delta", {{"content", "before\n"}}}}})},
                    {"prompt_progress", {{"total", 10}, {"cache", 4}, {"processed", 6}, {"time_ms", 7}}},
                });
                sink.write(first.c_str(), first.size());
                std::string thinking = sse({
                    {"choices", json::array({{{"index", 0}, {"delta", {{"reasoning_content", "think"}}}}})},
                });
                sink.write(thinking.c_str(), thinking.size());
                std::string tool = sse({
                    {"choices", json::array({{{"index", 0}, {"delta", {{"content", R"(<tool_call>{"name":"read","arguments":{"file_path":"foo.txt"}}</tool_call>)"}}}}})},
                    {"timings", {{"cache_n", 4}, {"prompt_n", 10}, {"predicted_n", 3}}},
                });
                sink.write(tool.c_str(), tool.size());
                std::string usage = sse({
                    {"choices", json::array()},
                    {"usage", {{"prompt_tokens", 10}, {"completion_tokens", 3}, {"prompt_tokens_details", {{"cached_tokens", 4}}}}},
                });
                sink.write(usage.c_str(), usage.size());
                std::string done = "data: [DONE]\n\n";
                sink.write(done.c_str(), done.size());
                sink.done();
                return false;
            });
    });

    int port = server.bind_to_any_port("127.0.0.1");
    assert(port > 0);
    std::thread server_thread([&]() {
        server.listen_after_bind();
    });

    http_inference_backend_config config;
    config.base_url = "http://127.0.0.1:" + std::to_string(port);
    config.id_slot = 0;
    http_inference_backend backend(config);

    assert(backend.meta().is_llama_server);
    assert(backend.meta().n_ctx == 2048);
    assert(backend.meta().has_vision);

    inference_request request;
    request.messages = json::array({
        {{"role", "user"}, {"content", json::array({
            {{"type", "text"}, {"text", "look"}},
            {{"type", "image_url"}, {"image_url", {{"url", "data:image/png;base64,AAAA"}}}},
        })}},
    });
    request.tools = {{
        "read",
        "Read a file",
        R"({"type":"object","properties":{"file_path":{"type":"string"}},"required":["file_path"]})",
    }};
    request.parse_tool_calls = true;

    int text_events = 0;
    int reasoning_events = 0;
    int progress_events = 0;
    inference_result result = backend.complete(
        request,
        [&](const inference_event & event) {
            if (event.type == inference_event_type::TEXT_DELTA) {
                text_events++;
            } else if (event.type == inference_event_type::REASONING_DELTA) {
                reasoning_events++;
            } else if (event.type == inference_event_type::PROMPT_PROGRESS) {
                progress_events++;
            }
        },
        []() { return false; });

    inference_result cancelled = backend.complete(
        request,
        [](const inference_event &) {},
        []() { return true; });

    server.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }

    assert(text_events == 2);
    assert(reasoning_events == 1);
    assert(progress_events == 1);
    assert(cancelled.cancelled);
    assert(result.message.tool_calls.size() == 1);
    assert(result.message.tool_calls[0].name == "read");
    assert(result.message.tool_calls[0].arguments == R"({"file_path":"foo.txt"})");
    assert(result.cached_prompt_tokens == 4);
    assert(result.timings.cache_n == 4);
    assert(result.prompt_tokens == 10);

    assert(captured_body.contains("cache_prompt"));
    assert(captured_body.contains("return_progress"));
    assert(captured_body.contains("timings_per_token"));
    assert(captured_body.contains("id_slot"));
    assert(!captured_body.contains("tools"));
    assert(captured_body["messages"].dump().find("image_url") != std::string::npos);

    return 0;
}
