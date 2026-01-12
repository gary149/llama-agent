#pragma once

// A2A Protocol HTTP Routes
// Implements A2A-compliant endpoints alongside existing /v1/agent/* endpoints

#include "a2a-task-manager.h"

// Forward declarations instead of full includes
struct server_http_context;
struct server_http_req;
struct server_http_res;
using server_http_res_ptr = std::unique_ptr<server_http_res>;

#include <functional>
#include <memory>

namespace a2a {

// A2A Protocol route handlers
struct a2a_routes {
    using handler_t = server_http_context::handler_t;

    // Message endpoints
    handler_t message_send;         // POST /v1/message:send - Sync message
    handler_t message_stream;       // POST /v1/message:stream - Streaming message

    // Task endpoints
    handler_t get_task;             // GET /v1/tasks/:id - Get task status
    handler_t list_tasks;           // GET /v1/tasks - List tasks
    handler_t cancel_task;          // POST /v1/tasks/:id:cancel - Cancel task
    handler_t subscribe_task;       // POST /v1/tasks/:id:subscribe - Subscribe to task stream
    handler_t send_input;           // POST /v1/tasks/:id:input - Send input (for input-required)

    // Discovery
    handler_t get_agent_card;       // GET /.well-known/agent-card.json - Agent Card

    // Constructor
    explicit a2a_routes(a2a_task_manager & task_mgr);

    // Helper to create error response (A2A format)
    static server_http_res_ptr make_error(int status, const std::string & code, const std::string & msg);

    // Helper to create JSON response
    static server_http_res_ptr make_json(const json & data, int status = 200);

private:
    a2a_task_manager & task_mgr_;
};

// Register A2A routes with HTTP context
void register_a2a_routes(server_http_context & ctx, a2a_routes & routes);

} // namespace a2a
