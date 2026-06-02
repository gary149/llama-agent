#pragma once

#include "inference-backend.h"

#include <mutex>

struct common_params;
struct llama_vocab;
struct server_context;

class local_inference_backend : public inference_backend {
public:
    local_inference_backend(server_context & server_ctx, const common_params & params);

    const inference_backend_meta & meta() const override;

    inference_result complete(
        const inference_request & request,
        std::function<void(const inference_event &)> on_event,
        std::function<bool()> should_stop) override;

private:
    void refresh_meta() const;
    const llama_vocab * vocab();

    server_context & server_ctx_;
    const common_params & params_;

    mutable std::mutex meta_mutex_;
    mutable inference_backend_meta meta_;

    std::mutex post_mutex_;
    const llama_vocab * vocab_ = nullptr;
};
