#include "http-inference-backend.h"

#include <cpp-httplib/httplib.h>

#undef NDEBUG
#include <cassert>
#include <cstdlib>
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
    // Native tool calling: tools are sent as a real field (not injected as text) and the
    // server is asked to parse them, since the backend is a detected llama-server.
    assert(captured_body.contains("tools"));
    assert(captured_body["tools"][0]["function"]["name"] == "read");
    assert(captured_body["parse_tool_calls"] == true);
    assert(captured_body["messages"].dump().find("image_url") != std::string::npos);

    // --- Context overflow: HTTP 400 exceed_context_size_error must set context_overflow ---
    {
        httplib::Server overflow_server;
        overflow_server.Post("/v1/chat/completions", [](const httplib::Request &, httplib::Response & res) {
            res.status = 400;
            res.set_content(json{
                {"error", {
                    {"type", "exceed_context_size_error"},
                    {"message", "the request exceeds the available context size"},
                    {"code", 400},
                    {"n_prompt_tokens", 5000},
                }},
            }.dump(), "application/json");
        });

        int oport = overflow_server.bind_to_any_port("127.0.0.1");
        assert(oport > 0);
        std::thread overflow_thread([&]() { overflow_server.listen_after_bind(); });

        http_inference_backend_config ocfg;
        ocfg.base_url = "http://127.0.0.1:" + std::to_string(oport);
        ocfg.detect_llama_server = false;
        http_inference_backend obackend(ocfg);

        inference_request oreq;
        oreq.messages = json::array({{{"role", "user"}, {"content", "hello"}}});
        inference_result ores = obackend.complete(
            oreq,
            [](const inference_event &) {},
            []() { return false; });

        overflow_server.stop();
        if (overflow_thread.joinable()) {
            overflow_thread.join();
        }

        assert(ores.context_overflow);
        assert(ores.prompt_tokens == 5000);
    }

    // --- /props without modalities must NOT claim authoritative image support ---
    {
        httplib::Server props_server;
        props_server.Get("/props", [](const httplib::Request &, httplib::Response & res) {
            res.set_content(json{
                {"model_alias", "no-modalities-model"},
                {"total_slots", 1},
                {"default_generation_settings", {{"n_ctx", 4096}}},
            }.dump(), "application/json");
        });

        int pport = props_server.bind_to_any_port("127.0.0.1");
        assert(pport > 0);
        std::thread props_thread([&]() { props_server.listen_after_bind(); });

        http_inference_backend_config pcfg;
        pcfg.base_url = "http://127.0.0.1:" + std::to_string(pport);
        http_inference_backend pbackend(pcfg);

        props_server.stop();
        if (props_thread.joinable()) {
            props_thread.join();
        }

        assert(pbackend.meta().is_llama_server);
        assert(!pbackend.meta().image_support_known);
        assert(!pbackend.meta().has_vision);
    }

    // --- OpenAI-compatible server (no /props): native tools, auth, /v1/models detection, retry ---
    {
        httplib::Server oai_server;
        json oai_body;
        json oai_models = json::array({{{"id", "org/oai-model"}, {"max_model_len", 32768}}});
        std::string models_auth;
        std::string chat_auth;
        int chat_requests = 0;

        oai_server.Get("/v1/models", [&](const httplib::Request & req, httplib::Response & res) {
            models_auth = req.get_header_value("Authorization");
            res.set_content(json{{"object", "list"}, {"data", oai_models}}.dump(), "application/json");
        });

        oai_server.Post("/v1/chat/completions", [&](const httplib::Request & req, httplib::Response & res) {
            chat_requests++;
            if (chat_requests == 1) {
                res.status = 503;
                res.set_content("Service Unavailable", "text/plain");
                return;
            }
            chat_auth = req.get_header_value("Authorization");
            oai_body = json::parse(req.body);
            std::string stream;
            stream += sse({{"choices", json::array({{{"index", 0}, {"delta", {{"reasoning", "thi"}}}}})}});
            stream += sse({{"choices", json::array({{{"index", 0}, {"delta", {{"content", "ok"}, {"reasoning", "nk"}}}}})}});
            stream += sse({{"choices", json::array({{{"index", 0}, {"delta", {{"tool_calls", json::array({
                {{"index", 0}, {"id", "call_a"}, {"type", "function"}, {"function", {{"name", "read"}, {"arguments", ""}}}},
            })}}}}})}});
            stream += sse({{"choices", json::array({{{"index", 0}, {"delta", {{"tool_calls", json::array({
                {{"index", 0}, {"function", {{"arguments", R"({"file_path":)"}}}},
            })}}}}})}});
            stream += sse({{"choices", json::array({{{"index", 0}, {"delta", {{"tool_calls", json::array({
                {{"index", 0}, {"function", {{"arguments", R"("foo.txt"})"}}}},
            })}}}}})}});
            stream += sse({{"choices", json::array()}, {"usage", {{"prompt_tokens", 20}, {"completion_tokens", 7}}}});
            stream += "data: [DONE]\n\n";
            res.set_content(stream, "text/event-stream");
        });

        int port2 = oai_server.bind_to_any_port("127.0.0.1");
        assert(port2 > 0);
        std::thread oai_thread([&]() { oai_server.listen_after_bind(); });

        http_inference_backend_config ocfg;
        ocfg.base_url = "http://127.0.0.1:" + std::to_string(port2) + "/v1";
        ocfg.api_key = "test-key";
        ocfg.has_vision = true;
        ocfg.n_predict = 512;
        ocfg.reasoning_effort = "low";
        ocfg.retry_delay_ms = 10;
        http_inference_backend obackend(ocfg);

        assert(!obackend.meta().is_llama_server);
        assert(!obackend.meta().local);
        assert(obackend.meta().model_name == "org/oai-model");
        assert(obackend.meta().n_ctx == 32768);
        assert(obackend.meta().has_vision);
        assert(obackend.meta().image_support_known);
        assert(models_auth == "Bearer test-key");

        inference_request oreq;
        oreq.messages = json::array({
            {{"role", "system"}, {"content", "be brief"}},
            {{"role", "user"}, {"content", "read foo.txt"}},
        });
        oreq.tools = request.tools;

        std::string event_order;
        int error_events = 0;
        inference_result ores = obackend.complete(
            oreq,
            [&](const inference_event & event) {
                if (event.type == inference_event_type::REASONING_DELTA) {
                    event_order += "R";
                } else if (event.type == inference_event_type::TEXT_DELTA) {
                    event_order += "T";
                } else if (event.type == inference_event_type::ERROR) {
                    error_events++;
                }
            },
            []() { return false; });

        // the 503 was retried and never reached the caller
        assert(chat_requests == 2);
        assert(error_events == 0);
        assert(ores.error.empty());
        assert(chat_auth == "Bearer test-key");
        // a delta with both fields ends the reasoning before the text starts
        assert(event_order == "RRT");
        assert(ores.message.reasoning_content == "think");
        assert(ores.message.content == "ok");
        assert(ores.message.tool_calls.size() == 1);
        assert(ores.message.tool_calls[0].id == "call_a");
        assert(ores.message.tool_calls[0].name == "read");
        assert(ores.message.tool_calls[0].arguments == R"({"file_path":"foo.txt"})");
        assert(ores.prompt_tokens == 20);

        // same tools as a llama-server backend: the "tools" field, not a text protocol in the system prompt
        assert(oai_body["model"] == "org/oai-model");
        assert(oai_body["tools"][0]["function"]["name"] == "read");
        assert(oai_body["messages"][0]["content"] == "be brief");
        assert(oai_body["max_tokens"] == 512);
        assert(oai_body["reasoning_effort"] == "low");
        assert(!oai_body.contains("cache_prompt"));
        assert(!oai_body.contains("id_slot"));
        assert(!oai_body.contains("n_predict"));

        // native_tools = false keeps the text protocol for servers without tool support
        http_inference_backend_config tcfg = ocfg;
        tcfg.native_tools = false;
        http_inference_backend tbackend(tcfg);
        tbackend.complete(oreq, [](const inference_event &) {}, []() { return false; });
        assert(!oai_body.contains("tools"));
        assert(oai_body["messages"][0]["content"].get<std::string>().find("# Tool Call Protocol") != std::string::npos);

        // several models: no silent choice, the model given by the user selects the entry
        oai_models.push_back({{"id", "org/other-model"}, {"max_model_len", 4096}});
        http_inference_backend mbackend(ocfg);
        assert(mbackend.meta().model_name == "default");
        assert(mbackend.meta().n_ctx == 0);
        http_inference_backend_config scfg = ocfg;
        scfg.model = "org/other-model";
        http_inference_backend sbackend(scfg);
        assert(sbackend.meta().model_name == "org/other-model");
        assert(sbackend.meta().n_ctx == 4096);

        oai_server.stop();
        if (oai_thread.joinable()) {
            oai_thread.join();
        }
    }

    // --- OpenAI-style context overflow (vLLM wording) must set context_overflow, with no retry ---
    {
        httplib::Server vllm_server;
        int requests = 0;
        vllm_server.Post("/v1/chat/completions", [&](const httplib::Request &, httplib::Response & res) {
            requests++;
            res.status = 400;
            res.set_content(json{
                {"error", {
                    {"message", "This model's maximum context length is 262144 tokens. However, your prompt contains 300016 input tokens."},
                    {"type", "BadRequestError"},
                    {"param", "input_tokens"},
                    {"code", 400},
                }},
            }.dump(), "application/json");
        });

        int vport = vllm_server.bind_to_any_port("127.0.0.1");
        assert(vport > 0);
        std::thread vllm_thread([&]() { vllm_server.listen_after_bind(); });

        http_inference_backend_config vcfg;
        vcfg.base_url = "http://127.0.0.1:" + std::to_string(vport);
        vcfg.detect_llama_server = false;
        vcfg.n_ctx = 8192;
        http_inference_backend vbackend(vcfg);

        // no /v1/models route: the context size comes from the config
        assert(vbackend.meta().n_ctx == 8192);

        inference_request vreq;
        vreq.messages = json::array({{{"role", "user"}, {"content", "hello"}}});
        inference_result vres = vbackend.complete(vreq, [](const inference_event &) {}, []() { return false; });

        vllm_server.stop();
        if (vllm_thread.joinable()) {
            vllm_thread.join();
        }

        assert(requests == 1);
        assert(vres.context_overflow);
        assert(vres.error.rfind("HTTP 400: This model's maximum context length", 0) == 0);
    }

    // --- config from the common arguments and the environment ---
    {
#if defined(_WIN32)
        _putenv_s("LLAMA_AGENT_BACKEND_API_KEY", "env-key");
#else
        setenv("LLAMA_AGENT_BACKEND_API_KEY", "env-key", 1);
#endif
        common_params params;
        params.n_ctx = 65536;
        params.n_predict = 4096;
        params.default_template_kwargs["reasoning_effort"] = "\"medium\"";
        http_inference_backend_config from_params = http_inference_backend_config_from_params(params);
        assert(from_params.api_key == "env-key");
        assert(from_params.n_ctx == 65536);
        assert(from_params.n_predict == 4096);
        assert(from_params.reasoning_effort == "medium");
        assert(from_params.native_tools);
        assert(!from_params.local);
    }

    return 0;
}
