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
    const llama_vocab * vocab();

    server_context & server_ctx_;
    const common_params & params_;

    // meta_ is populated once on the first meta() call (the values from server_ctx_
    // are stable after model load) and immutable afterwards, so concurrent readers
    // across session worker threads need no locking. call_once provides the
    // happens-before so all readers see the fully-initialized snapshot.
    mutable std::once_flag meta_once_;
    mutable inference_backend_meta meta_;

    std::mutex post_mutex_;
    const llama_vocab * vocab_ = nullptr;
};
