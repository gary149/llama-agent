#pragma once

#include "inference-backend.h"

#include <map>
#include <mutex>

struct http_inference_backend_config {
    std::string base_url;
    std::string model;
    std::string api_key;
    std::map<std::string, std::string> headers;
    int timeout_ms = 0;
    bool llama_server_extensions = false;
    bool detect_llama_server = true;
    int32_t id_slot = -1;
};

class http_inference_backend : public inference_backend {
public:
    explicit http_inference_backend(http_inference_backend_config config);

    const inference_backend_meta & meta() const override;

    inference_result complete(
        const inference_request & request,
        std::function<void(const inference_event &)> on_event,
        std::function<bool()> should_stop) override;

private:
    void fetch_props();

    std::string chat_completions_path() const;
    std::string props_path() const;
    json build_request_body(const inference_request & request) const;

    void process_sse_payload(
        const std::string & payload,
        const inference_request & request,
        inference_result & result,
        std::string & content,
        std::string & reasoning,
        std::vector<common_chat_tool_call> & native_tool_calls,
        std::function<void(const inference_event &)> emit) const;

    http_inference_backend_config config_;
    inference_backend_meta meta_;
    bool use_llama_server_extensions_ = false;
};
