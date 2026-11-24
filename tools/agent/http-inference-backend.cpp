#include "http-inference-backend.h"

#include "agent-tool-parser.h"
#include "common.h"
#include "http.h"
#include "log.h"

#if defined(_WIN32)
// windows.h (pulled in by http.h) defines ERROR, which clashes with inference_event_type::ERROR
#undef ERROR
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <thread>
#include <utility>

static std::string trim_trailing_slashes(std::string path) {
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

static bool ends_with(const std::string & value, const std::string & suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::string join_path(const std::string & base, const std::string & suffix) {
    if (base.empty() || base == "/") {
        return suffix;
    }
    return base + suffix;
}

static httplib::Headers make_headers(const http_inference_backend_config & config) {
    httplib::Headers headers;
    headers.emplace("Accept", "text/event-stream");
    for (const auto & header : config.headers) {
        headers.emplace(header.first, header.second);
    }
    if (!config.api_key.empty()) {
        headers.emplace("Authorization", "Bearer " + config.api_key);
    }
    return headers;
}

static std::string json_string_value(const json & obj, const std::string & key) {
    if (!obj.contains(key) || !obj.at(key).is_string()) {
        return "";
    }
    return obj.at(key).get<std::string>();
}

static int32_t json_i32_value(const json & obj, const std::string & key, int32_t fallback = 0) {
    if (!obj.contains(key) || !obj.at(key).is_number_integer()) {
        return fallback;
    }
    return obj.at(key).get<int32_t>();
}

static double json_double_value(const json & obj, const std::string & key, double fallback = 0.0) {
    if (!obj.contains(key) || !obj.at(key).is_number()) {
        return fallback;
    }
    return obj.at(key).get<double>();
}

// llama-server reports overflow in error.type, OpenAI-compatible servers in error.code or in the message
static bool is_context_overflow_error(const json & error) {
    return json_string_value(error, "type") == "exceed_context_size_error" ||
           json_string_value(error, "code") == "context_length_exceeded" ||
           json_string_value(error, "message").find("maximum context length") != std::string::npos;
}

static bool is_transient_status(int status) {
    return status == 408 || status == 429 || status >= 500;
}

static void apply_timings(const json & timings, inference_timings & out) {
    out.cache_n = json_i32_value(timings, "cache_n", out.cache_n);
    out.prompt_n = json_i32_value(timings, "prompt_n", out.prompt_n);
    out.prompt_ms = json_double_value(timings, "prompt_ms", out.prompt_ms);
    out.prompt_per_token_ms = json_double_value(timings, "prompt_per_token_ms", out.prompt_per_token_ms);
    out.prompt_per_second = json_double_value(timings, "prompt_per_second", out.prompt_per_second);
    out.predicted_n = json_i32_value(timings, "predicted_n", out.predicted_n);
    out.predicted_ms = json_double_value(timings, "predicted_ms", out.predicted_ms);
    out.predicted_per_token_ms = json_double_value(timings, "predicted_per_token_ms", out.predicted_per_token_ms);
    out.predicted_per_second = json_double_value(timings, "predicted_per_second", out.predicted_per_second);
    out.draft_n = json_i32_value(timings, "draft_n", out.draft_n);
    out.draft_n_accepted = json_i32_value(timings, "draft_n_accepted", out.draft_n_accepted);
}

static inference_prompt_progress parse_progress(const json & progress) {
    inference_prompt_progress out;
    out.total = json_i32_value(progress, "total", 0);
    out.cache = json_i32_value(progress, "cache", 0);
    out.processed = json_i32_value(progress, "processed", 0);
    if (progress.contains("time_ms") && progress.at("time_ms").is_number_integer()) {
        out.time_ms = progress.at("time_ms").get<int64_t>();
    }
    return out;
}

static std::string sse_payload_from_event(const std::string & event) {
    std::istringstream in(event);
    std::string line;
    std::string payload;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("data:", 0) != 0) {
            continue;
        }
        std::string data = line.substr(5);
        if (!data.empty() && data[0] == ' ') {
            data.erase(0, 1);
        }
        if (!payload.empty()) {
            payload += "\n";
        }
        payload += data;
    }
    return payload;
}

template <typename Fn>
static void process_sse_buffer(std::string & buffer, Fn && fn) {
    while (true) {
        size_t pos = buffer.find("\n\n");
        size_t delim = 2;
        if (pos == std::string::npos) {
            pos = buffer.find("\r\n\r\n");
            delim = 4;
        }
        if (pos == std::string::npos) {
            break;
        }

        std::string event = buffer.substr(0, pos);
        buffer.erase(0, pos + delim);
        std::string payload = sse_payload_from_event(event);
        if (!payload.empty()) {
            fn(payload);
        }
    }
}

http_inference_backend_config http_inference_backend_config_from_params(const common_params & params) {
    http_inference_backend_config config;
    if (const char * key = std::getenv("LLAMA_AGENT_BACKEND_API_KEY")) {
        config.api_key = key;
    }
    config.n_ctx = params.n_ctx;
    config.n_predict = params.n_predict;
    auto effort = params.default_template_kwargs.find("reasoning_effort");
    if (effort != params.default_template_kwargs.end()) {
        // the value is stored as JSON
        json value = json::parse(effort->second, nullptr, false);
        if (value.is_string()) {
            config.reasoning_effort = value.get<std::string>();
        }
    }
    return config;
}

http_inference_backend::http_inference_backend(http_inference_backend_config config)
    : config_(std::move(config)) {

    meta_.model_name = config_.model.empty() ? "default" : config_.model;
    meta_.image_support_known = false;
    meta_.local = config_.local;

    if (config_.detect_llama_server) {
        fetch_props();
    }
    use_llama_server_extensions_ = config_.llama_server_extensions || meta_.is_llama_server;

    if (!meta_.is_llama_server) {
        fetch_models();
        if (config_.n_ctx > 0) {
            meta_.n_ctx = config_.n_ctx;
        }
    }
    if (config_.has_vision && !meta_.image_support_known) {
        meta_.has_vision = true;
        meta_.image_support_known = true;
    }
}

const inference_backend_meta & http_inference_backend::meta() const {
    return meta_;
}

std::string http_inference_backend::api_path(const std::string & suffix) const {
    auto parts = common_http_parse_url(config_.base_url);
    std::string base = trim_trailing_slashes(parts.path);
    if (ends_with(base, "/v1")) {
        return join_path(base, suffix);
    }
    return join_path(base, "/v1" + suffix);
}

std::string http_inference_backend::props_path() const {
    auto parts = common_http_parse_url(config_.base_url);
    std::string base = trim_trailing_slashes(parts.path);
    if (ends_with(base, "/v1")) {
        base.resize(base.size() - 3);
    }
    return join_path(base, "/props");
}

void http_inference_backend::fetch_props() {
    try {
        auto [cli, parts] = common_http_client(config_.base_url);
        if (config_.timeout_ms > 0) {
            cli.set_connection_timeout(std::chrono::milliseconds(config_.timeout_ms));
            cli.set_read_timeout(std::chrono::milliseconds(config_.timeout_ms));
        }

        auto res = cli.Get(props_path(), make_headers(config_));
        if (!res || res->status < 200 || res->status >= 300) {
            return;
        }

        json props = json::parse(res->body);
        meta_.is_llama_server = true;
        meta_.model_name = props.value("model_alias", meta_.model_name);
        meta_.total_slots = props.value("total_slots", 0);
        if (props.contains("default_generation_settings")) {
            const auto & settings = props["default_generation_settings"];
            meta_.n_ctx = settings.value("n_ctx", 0);
        }
        if (props.contains("modalities")) {
            const auto & modalities = props["modalities"];
            meta_.has_vision = modalities.value("vision", false);
            meta_.has_audio = modalities.value("audio", false);
            // Only claim authoritative knowledge of image support when /props
            // actually reported modalities. Older servers/proxies that omit it
            // must fall back to image_support_known=false so images aren't dropped.
            meta_.image_support_known = true;
        }
    } catch (const std::exception & e) {
        LOG_DBG("HTTP inference /props detection failed: %s\n", e.what());
    }
}

// OpenAI-compatible servers have no /props: take the model id and the context size from /v1/models
void http_inference_backend::fetch_models() {
    try {
        auto [cli, parts] = common_http_client(config_.base_url);
        if (config_.timeout_ms > 0) {
            cli.set_connection_timeout(std::chrono::milliseconds(config_.timeout_ms));
            cli.set_read_timeout(std::chrono::milliseconds(config_.timeout_ms));
        }

        auto res = cli.Get(api_path("/models"), make_headers(config_));
        if (!res || res->status < 200 || res->status >= 300) {
            return;
        }

        json models = json::parse(res->body);
        if (!models.contains("data") || !models["data"].is_array()) {
            return;
        }
        // with no model given, use the model of the server only when there is no choice to make
        const bool single = models["data"].size() == 1;
        for (const auto & model : models["data"]) {
            std::string id = json_string_value(model, "id");
            if (id.empty() || (config_.model.empty() ? !single : id != config_.model)) {
                continue;
            }
            meta_.model_name = id;
            // vLLM extension
            meta_.n_ctx = json_i32_value(model, "max_model_len", 0);
            break;
        }
    } catch (const std::exception & e) {
        LOG_DBG("HTTP inference /v1/models detection failed: %s\n", e.what());
    }
}

json http_inference_backend::build_request_body(const inference_request & request) const {
    json body;
    // Prefer native tool calling (the model's own chat template) when tools are present,
    // mirroring what local_inference_backend already does in-process. Models trained for
    // tool use (e.g. Qwen3, Inkling 2) produce far more reliable tool calls this way than
    // via the injected text protocol, which they may not follow. The injected XML protocol
    // stays available (native_tools = false) for servers that don't support the tools field.
    // Gate on request.parse_tool_calls too: if the caller doesn't want tool calls parsed we
    // must not ask the server to emit them (complete() would discard them), mirroring the
    // local backend's tool_choice="none" path.
    const bool use_native_tools =
        config_.native_tools && !request.tools.empty() && request.parse_tool_calls;
    if (use_native_tools) {
        body["messages"] = request.messages;
        json tools_arr = json::array();
        for (const auto & t : request.tools) {
            json params;
            try { params = json::parse(t.parameters); } catch (...) { params = json::object(); }
            tools_arr.push_back({{"type", "function"}, {"function", {
                {"name", t.name}, {"description", t.description}, {"parameters", params}}}});
        }
        body["tools"] = tools_arr;
    } else {
        body["messages"] = request.parse_tool_calls
            ? agent_inject_tool_protocol_prompt(request.messages, request.tools)
            : request.messages;
    }
    body["model"] = !config_.model.empty() ? config_.model : meta_.model_name;
    body["stream"] = request.stream;

    int32_t n_predict = request.n_predict >= 0 ? request.n_predict : config_.n_predict;
    if (n_predict >= 0) {
        if (use_llama_server_extensions_) {
            body["n_predict"] = n_predict;
        } else {
            body["max_tokens"] = n_predict;
        }
    }
    if (!config_.reasoning_effort.empty()) {
        body["reasoning_effort"] = config_.reasoning_effort;
    }

    if (use_llama_server_extensions_) {
        body["cache_prompt"] = request.cache_prompt;
        body["return_progress"] = request.return_progress;
        body["timings_per_token"] = request.timings_per_token;
        body["parse_tool_calls"] = use_native_tools;
        body["stream_options"] = {{"include_usage", true}};
        int32_t id_slot = request.id_slot >= 0 ? request.id_slot : config_.id_slot;
        if (id_slot >= 0) {
            body["id_slot"] = id_slot;
        }
    } else {
        body["stream_options"] = {{"include_usage", true}};
    }

    return body;
}

void http_inference_backend::process_sse_payload(
    const std::string & payload,
    const inference_request & request,
    inference_result & result,
    std::string & content,
    std::string & reasoning,
    std::vector<common_chat_tool_call> & native_tool_calls,
    std::function<void(const inference_event &)> emit) const {

    if (payload == "[DONE]") {
        return;
    }

    json chunk;
    try {
        chunk = json::parse(payload);
    } catch (const std::exception & e) {
        result.error = e.what();
        emit({inference_event_type::ERROR, {}, {}, result.error, {}});
        return;
    }

    if (chunk.is_array()) {
        for (const auto & item : chunk) {
            process_sse_payload(item.dump(), request, result, content, reasoning, native_tool_calls, emit);
        }
        return;
    }

    if (chunk.contains("error")) {
        const auto & error = chunk["error"];
        result.error = error.is_object() ? error.value("message", error.dump()) : error.dump();
        // Surface context overflow so the agent loop can trigger compaction-on-overflow
        // recovery (mirrors the local backend's ERROR_TYPE_EXCEED_CONTEXT_SIZE handling).
        if (error.is_object() && is_context_overflow_error(error)) {
            result.context_overflow = true;
            result.prompt_tokens = error.value("n_prompt_tokens", result.prompt_tokens);
        }
        emit({inference_event_type::ERROR, {}, {}, result.error, chunk});
        return;
    }

    if (chunk.contains("timings") && chunk["timings"].is_object()) {
        apply_timings(chunk["timings"], result.timings);
        if (result.timings.cache_n > 0) {
            result.cached_prompt_tokens = std::max(result.cached_prompt_tokens, result.timings.cache_n);
        }
        if (result.timings.prompt_n > 0) {
            result.prompt_tokens = result.timings.prompt_n;
        }
    }

    if (chunk.contains("prompt_progress") && chunk["prompt_progress"].is_object()) {
        inference_event event;
        event.type = inference_event_type::PROMPT_PROGRESS;
        event.progress = parse_progress(chunk["prompt_progress"]);
        event.data = chunk["prompt_progress"];
        emit(event);
        result.cached_prompt_tokens = std::max(result.cached_prompt_tokens, event.progress.cache);
    }

    if (chunk.contains("usage") && chunk["usage"].is_object()) {
        const auto & usage = chunk["usage"];
        result.prompt_tokens = usage.value("prompt_tokens", result.prompt_tokens);
        result.timings.prompt_n = usage.value("prompt_tokens", result.timings.prompt_n);
        result.timings.predicted_n = usage.value("completion_tokens", result.timings.predicted_n);
        if (usage.contains("prompt_tokens_details") && usage["prompt_tokens_details"].is_object()) {
            result.cached_prompt_tokens = std::max(
                result.cached_prompt_tokens,
                usage["prompt_tokens_details"].value("cached_tokens", 0));
        }
    }

    if (!chunk.contains("choices") || !chunk["choices"].is_array()) {
        return;
    }

    for (const auto & choice : chunk["choices"]) {
        json delta = json::object();
        if (choice.contains("delta") && choice["delta"].is_object()) {
            delta = choice["delta"];
        } else if (choice.contains("message") && choice["message"].is_object()) {
            delta = choice["message"];
        }

        // one delta can carry the end of the reasoning and the start of the content
        std::string reasoning_delta = json_string_value(delta, "reasoning_content");
        if (reasoning_delta.empty()) {
            reasoning_delta = json_string_value(delta, "reasoning");
        }
        if (reasoning_delta.empty()) {
            reasoning_delta = json_string_value(delta, "reasoning_text");
        }
        if (!reasoning_delta.empty()) {
            common_chat_msg_diff diff;
            diff.reasoning_content_delta = reasoning_delta;
            emit({inference_event_type::REASONING_DELTA, diff, {}, "", chunk});
            reasoning += reasoning_delta;
        }

        std::string content_delta = json_string_value(delta, "content");
        if (!content_delta.empty()) {
            common_chat_msg_diff diff;
            diff.content_delta = content_delta;
            emit({inference_event_type::TEXT_DELTA, diff, {}, "", chunk});
            content += content_delta;
        }

        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
            for (const auto & tool_call_delta : delta["tool_calls"]) {
                size_t idx = tool_call_delta.value("index", (int) native_tool_calls.size());
                if (idx >= native_tool_calls.size()) {
                    native_tool_calls.resize(idx + 1);
                }

                common_chat_tool_call diff_call;
                if (tool_call_delta.contains("id") && tool_call_delta["id"].is_string()) {
                    diff_call.id = tool_call_delta["id"].get<std::string>();
                    native_tool_calls[idx].id = diff_call.id;
                }
                if (tool_call_delta.contains("function") && tool_call_delta["function"].is_object()) {
                    const auto & fn = tool_call_delta["function"];
                    if (fn.contains("name") && fn["name"].is_string()) {
                        diff_call.name = fn["name"].get<std::string>();
                        native_tool_calls[idx].name = diff_call.name;
                    }
                    if (fn.contains("arguments") && fn["arguments"].is_string()) {
                        diff_call.arguments = fn["arguments"].get<std::string>();
                        native_tool_calls[idx].arguments += diff_call.arguments;
                    }
                }

                common_chat_msg_diff diff;
                diff.tool_call_index = idx;
                diff.tool_call_delta = diff_call;
                emit({inference_event_type::TOOL_CALL_DELTA, diff, {}, "", chunk});
            }
        }
    }
}

inference_result http_inference_backend::complete(
    const inference_request & request,
    std::function<void(const inference_event &)> on_event,
    std::function<bool()> should_stop) {

    inference_result result;
    auto stop = should_stop ? std::move(should_stop) : std::function<bool()>([]() { return false; });
    auto emit = [&](const inference_event & event) {
        if (on_event) {
            on_event(event);
        }
    };

    json body = build_request_body(request);
    std::string content;
    std::string reasoning;
    std::vector<common_chat_tool_call> native_tool_calls;

    for (int attempt = 0;; attempt++) {
        std::string response_buffer;
        std::string http_error;
        bool received_event = false;
        bool transient = false;

        try {
            auto [cli, parts] = common_http_client(config_.base_url);
            if (config_.timeout_ms > 0) {
                cli.set_connection_timeout(std::chrono::milliseconds(config_.timeout_ms));
                cli.set_read_timeout(std::chrono::milliseconds(config_.timeout_ms));
            }

            auto on_payload = [&](const std::string & payload) {
                received_event = true;
                process_sse_payload(payload, request, result, content, reasoning, native_tool_calls, emit);
            };

            auto receiver = [&](const char * data, size_t data_length) {
                if (stop()) {
                    result.cancelled = true;
                    return false;
                }
                response_buffer.append(data, data_length);
                process_sse_buffer(response_buffer, on_payload);
                return !stop();
            };

            auto res = cli.Post(
                api_path("/chat/completions"),
                make_headers(config_),
                body.dump(),
                "application/json",
                receiver);

            if (!response_buffer.empty()) {
                process_sse_buffer(response_buffer, on_payload);
            }

            if (!res) {
                if (!result.cancelled) {
                    http_error = httplib::to_string(res.error());
                    transient = true;
                }
            } else if (res->status < 200 || res->status >= 300) {
                http_error = response_buffer.empty() ? res->body : response_buffer;
                if (http_error.empty()) {
                    http_error = "HTTP inference request failed with status " + std::to_string(res->status);
                }
                transient = is_transient_status(res->status);
                // Context overflow is reported as HTTP 400 with a JSON error body. Parse
                // it so compaction-on-overflow recovery still fires on the HTTP backend.
                try {
                    json body_json = json::parse(http_error);
                    if (body_json.contains("error") && body_json["error"].is_object()) {
                        const auto & error = body_json["error"];
                        if (is_context_overflow_error(error)) {
                            result.context_overflow = true;
                            result.prompt_tokens = error.value("n_prompt_tokens", result.prompt_tokens);
                        }
                        std::string message = json_string_value(error, "message");
                        if (!message.empty()) {
                            http_error = "HTTP " + std::to_string(res->status) + ": " + message;
                        }
                    }
                } catch (...) {
                    // not JSON, leave as a plain error
                }
                if (res->status == 401 && config_.api_key.empty()) {
                    http_error += " (set LLAMA_AGENT_BACKEND_API_KEY to send an API key)";
                }
            }
        } catch (const std::exception & e) {
            http_error = e.what();
        }

        // retry only when nothing was streamed yet, the caller never saw this attempt
        if (transient && !received_event && attempt < config_.max_retries) {
            LOG_WRN("HTTP inference request failed (%s), retrying\n", http_error.c_str());
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.retry_delay_ms << attempt);
            while (!stop() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!stop()) {
                result = inference_result();
                continue;
            }
            result.cancelled = true;
        } else if (!http_error.empty()) {
            result.error = http_error;
            emit({inference_event_type::ERROR, {}, {}, result.error, {}});
        }
        break;
    }

    if (request.parse_tool_calls) {
        result.message = agent_parse_tool_protocol_response(content, reasoning, request.tools);
        if (result.message.tool_calls.empty() && !native_tool_calls.empty()) {
            result.message.tool_calls = std::move(native_tool_calls);
        }
    } else {
        result.message.role = "assistant";
        result.message.content = content;
        result.message.reasoning_content = reasoning;
    }

    return result;
}
