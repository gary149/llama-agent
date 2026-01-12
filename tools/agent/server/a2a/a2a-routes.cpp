#include "a2a-routes.h"

#include "../../tool-registry.h"
#include "../../../server/server-http.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>

namespace a2a {

// SSE streaming response (same pattern as agent-routes.cpp)
struct sse_stream_res : server_http_res {
    std::queue<std::string> chunks;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> done{false};

    sse_stream_res() {
        content_type = "text/event-stream";
        headers["Cache-Control"] = "no-cache";
        headers["Connection"] = "keep-alive";

        next = [this](std::string & output) -> bool {
            std::unique_lock<std::mutex> lock(mutex);

            cv.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return !chunks.empty() || done.load();
            });

            if (!chunks.empty()) {
                output = chunks.front();
                chunks.pop();
                return true;
            }

            if (done.load()) {
                return false;
            }

            output = "";
            return true;
        };
    }

    void send(const std::string & event_type, const json & data) {
        std::string chunk = "event: " + event_type + "\n";
        chunk += "data: " + data.dump() + "\n\n";

        {
            std::lock_guard<std::mutex> lock(mutex);
            chunks.push(chunk);
        }
        cv.notify_one();
    }

    void finish() {
        done.store(true);
        cv.notify_one();
    }
};

// Wrapper for shared ownership
struct sse_shared_wrapper : server_http_res {
    std::shared_ptr<sse_stream_res> sse;

    explicit sse_shared_wrapper(std::shared_ptr<sse_stream_res> s) : sse(std::move(s)) {
        content_type = sse->content_type;
        headers = sse->headers;
        next = [this](std::string & output) -> bool {
            return sse->next(output);
        };
    }
};

server_http_res_ptr a2a_routes::make_error(int status, const std::string & code, const std::string & msg) {
    auto res = std::make_unique<server_http_res>();
    res->status = status;
    res->data = json{
        {"error", {
            {"code", code},
            {"message", msg}
        }}
    }.dump();
    return res;
}

server_http_res_ptr a2a_routes::make_json(const json & data, int status) {
    auto res = std::make_unique<server_http_res>();
    res->status = status;
    res->data = data.dump();
    return res;
}

a2a_routes::a2a_routes(a2a_task_manager & task_mgr)
    : task_mgr_(task_mgr) {

    // POST /v1/message:send - Synchronous message send
    message_send = [this](const server_http_req & req) -> server_http_res_ptr {
        try {
            json body = json::parse(req.body);

            // Parse input message
            if (!body.contains("message")) {
                return make_error(400, "InvalidRequest", "Missing 'message' field");
            }
            message input_msg = message::from_json(body["message"]);

            // Get context_id (session_id)
            std::string context_id = body.value("contextId", "");

            // Parse optional configuration
            agent_session_config config;
            if (body.contains("configuration")) {
                const auto & cfg = body["configuration"];
                config.yolo_mode = cfg.value("yolo", false);
                config.max_iterations = cfg.value("maxIterations", 50);
                if (cfg.contains("workingDir")) {
                    config.working_dir = cfg["workingDir"].get<std::string>();
                }
            }

            // Execute task (synchronously wait for completion)
            std::string task_id = task_mgr_.execute_task(context_id, input_msg, config);

            // Wait for task completion
            a2a_task * task = task_mgr_.get_task(task_id);
            if (!task) {
                return make_error(500, "InternalError", "Failed to create task");
            }

            // Poll until terminal state (simple busy wait with sleep)
            while (!is_terminal_state(task->state())) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            return make_json(task->to_json());

        } catch (const json::exception & e) {
            return make_error(400, "InvalidRequest", std::string("JSON parse error: ") + e.what());
        } catch (const std::exception & e) {
            return make_error(500, "InternalError", e.what());
        }
    };

    // POST /v1/message:stream - Streaming message send
    message_stream = [this](const server_http_req & req) -> server_http_res_ptr {
        try {
            json body = json::parse(req.body);

            if (!body.contains("message")) {
                return make_error(400, "InvalidRequest", "Missing 'message' field");
            }
            message input_msg = message::from_json(body["message"]);

            std::string context_id = body.value("contextId", "");

            agent_session_config config;
            if (body.contains("configuration")) {
                const auto & cfg = body["configuration"];
                config.yolo_mode = cfg.value("yolo", false);
                config.max_iterations = cfg.value("maxIterations", 50);
                if (cfg.contains("workingDir")) {
                    config.working_dir = cfg["workingDir"].get<std::string>();
                }
            }

            // Create SSE stream
            auto sse_shared = std::make_shared<sse_stream_res>();

            // Execute task - events will be broadcast to task subscribers
            std::string task_id = task_mgr_.execute_task(context_id, input_msg, config);

            a2a_task * task = task_mgr_.get_task(task_id);
            if (!task) {
                return make_error(500, "InternalError", "Failed to create task");
            }

            // Subscribe to task events and forward to SSE
            task->subscribe([sse_shared](const json & event) {
                if (event.contains("statusUpdate")) {
                    sse_shared->send("TaskStatusUpdateEvent", event["statusUpdate"]);
                    if (event["statusUpdate"].value("final", false)) {
                        sse_shared->finish();
                    }
                } else if (event.contains("artifactUpdate")) {
                    sse_shared->send("TaskArtifactUpdateEvent", event["artifactUpdate"]);
                }
            });

            // Send initial task status
            task_status_update_event initial;
            initial.task_id = task_id;
            initial.context_id = task->context_id();
            initial.status = task->current_status();
            initial.final = false;
            sse_shared->send("TaskStatusUpdateEvent", initial.to_json());

            return std::make_unique<sse_shared_wrapper>(sse_shared);

        } catch (const json::exception & e) {
            return make_error(400, "InvalidRequest", std::string("JSON parse error: ") + e.what());
        } catch (const std::exception & e) {
            return make_error(500, "InternalError", e.what());
        }
    };

    // GET /v1/tasks/:id - Get task status
    get_task = [this](const server_http_req & req) -> server_http_res_ptr {
        std::string task_id = req.get_param("id");

        if (task_id.empty()) {
            return make_error(400, "InvalidRequest", "Missing task ID");
        }

        const a2a_task * task = task_mgr_.get_task(task_id);
        if (!task) {
            return make_error(404, "TaskNotFound", "Task not found: " + task_id);
        }

        return make_json(task->to_json());
    };

    // GET /v1/tasks - List tasks
    list_tasks = [this](const server_http_req & req) -> server_http_res_ptr {
        list_tasks_options options;

        std::string context_id = req.get_param("contextId");
        if (!context_id.empty()) {
            options.context_id = context_id;
        }

        std::string state_str = req.get_param("state");
        if (!state_str.empty()) {
            options.state = task_state_from_string(state_str);
        }

        std::string limit_str = req.get_param("limit", "100");
        options.limit = std::stoi(limit_str);

        std::string cursor = req.get_param("cursor");
        if (!cursor.empty()) {
            options.cursor = cursor;
        }

        list_tasks_result result = task_mgr_.list_tasks(options);

        json response;
        response["tasks"] = json::array();
        for (const auto & t : result.tasks) {
            response["tasks"].push_back(t.to_json());
        }
        if (result.next_cursor) {
            response["nextCursor"] = *result.next_cursor;
        }

        return make_json(response);
    };

    // POST /v1/tasks/:id:cancel - Cancel task
    cancel_task = [this](const server_http_req & req) -> server_http_res_ptr {
        std::string task_id = req.get_param("id");

        if (task_id.empty()) {
            return make_error(400, "InvalidRequest", "Missing task ID");
        }

        if (!task_mgr_.cancel_task(task_id)) {
            const a2a_task * task = task_mgr_.get_task(task_id);
            if (!task) {
                return make_error(404, "TaskNotFound", "Task not found: " + task_id);
            }
            return make_error(409, "TaskNotCancelable", "Task cannot be cancelled: " + task_id);
        }

        const a2a_task * task = task_mgr_.get_task(task_id);
        return make_json(task->to_json());
    };

    // POST /v1/tasks/:id:subscribe - Subscribe to task stream
    subscribe_task = [this](const server_http_req & req) -> server_http_res_ptr {
        std::string task_id = req.get_param("id");

        if (task_id.empty()) {
            return make_error(400, "InvalidRequest", "Missing task ID");
        }

        a2a_task * task = task_mgr_.get_task(task_id);
        if (!task) {
            return make_error(404, "TaskNotFound", "Task not found: " + task_id);
        }

        auto sse_shared = std::make_shared<sse_stream_res>();

        // If task is already terminal, send final status and close
        if (is_terminal_state(task->state())) {
            task_status_update_event event;
            event.task_id = task_id;
            event.context_id = task->context_id();
            event.status = task->current_status();
            event.final = true;
            sse_shared->send("TaskStatusUpdateEvent", event.to_json());
            sse_shared->finish();
        } else {
            // Subscribe to ongoing events
            task->subscribe([sse_shared](const json & event) {
                if (event.contains("statusUpdate")) {
                    sse_shared->send("TaskStatusUpdateEvent", event["statusUpdate"]);
                    if (event["statusUpdate"].value("final", false)) {
                        sse_shared->finish();
                    }
                } else if (event.contains("artifactUpdate")) {
                    sse_shared->send("TaskArtifactUpdateEvent", event["artifactUpdate"]);
                }
            });

            // Send current status immediately
            task_status_update_event current;
            current.task_id = task_id;
            current.context_id = task->context_id();
            current.status = task->current_status();
            current.final = false;
            sse_shared->send("TaskStatusUpdateEvent", current.to_json());
        }

        return std::make_unique<sse_shared_wrapper>(sse_shared);
    };

    // POST /v1/tasks/:id:input - Send input for input-required state
    send_input = [this](const server_http_req & req) -> server_http_res_ptr {
        std::string task_id = req.get_param("id");

        if (task_id.empty()) {
            return make_error(400, "InvalidRequest", "Missing task ID");
        }

        try {
            json body = json::parse(req.body);

            if (!body.contains("message")) {
                return make_error(400, "InvalidRequest", "Missing 'message' field");
            }
            message input_msg = message::from_json(body["message"]);

            if (!task_mgr_.resume_task(task_id, input_msg)) {
                a2a_task * task = task_mgr_.get_task(task_id);
                if (!task) {
                    return make_error(404, "TaskNotFound", "Task not found: " + task_id);
                }
                if (task->state() != task_state::INPUT_REQUIRED) {
                    return make_error(400, "InvalidState", "Task is not awaiting input");
                }
                return make_error(500, "InternalError", "Failed to resume task");
            }

            const a2a_task * task = task_mgr_.get_task(task_id);
            return make_json(task->to_json());

        } catch (const json::exception & e) {
            return make_error(400, "InvalidRequest", std::string("JSON parse error: ") + e.what());
        }
    };

    // GET /.well-known/agent-card.json - Agent discovery
    get_agent_card = [](const server_http_req &) -> server_http_res_ptr {
        agent_card card;
        card.name = "llama-agent";
        card.description = "A local coding agent powered by llama.cpp";
        card.version = "1.0.0";
        card.provider = "llama.cpp";

        card.capabilities.streaming = true;
        card.capabilities.push_notifications = false;
        card.capabilities.state_transition_history = true;

        card.default_input_modes = std::vector<std::string>{"text/plain"};
        card.default_output_modes = std::vector<std::string>{"text/plain", "application/json"};
        card.supported_content_types = std::vector<std::string>{"text/plain", "application/json"};

        // Map tools to skills
        const auto & registry = tool_registry::instance();
        auto tools = registry.get_all_tools();

        card.skills = std::vector<agent_skill>{};
        for (const auto * tool : tools) {
            agent_skill skill;
            skill.id = tool->name;
            skill.name = tool->name;
            skill.description = tool->description;
            skill.input_modes = std::vector<std::string>{"text/plain"};
            skill.output_modes = std::vector<std::string>{"text/plain"};
            card.skills->push_back(skill);
        }

        return make_json(card.to_json());
    };
}

void register_a2a_routes(server_http_context & ctx, a2a_routes & routes) {
    // Wrapper for exception handling
    auto ex_wrapper = [](const a2a_routes::handler_t & handler) -> a2a_routes::handler_t {
        return [handler](const server_http_req & req) -> server_http_res_ptr {
            try {
                return handler(req);
            } catch (const std::exception & e) {
                return a2a_routes::make_error(500, "InternalError", e.what());
            } catch (...) {
                return a2a_routes::make_error(500, "InternalError", "Unknown error");
            }
        };
    };

    // Message endpoints
    ctx.post("/v1/message:send", ex_wrapper(routes.message_send));
    ctx.post("/v1/message:stream", ex_wrapper(routes.message_stream));

    // Task endpoints
    ctx.get("/v1/tasks/:id", ex_wrapper(routes.get_task));
    ctx.get("/v1/tasks", ex_wrapper(routes.list_tasks));
    ctx.post("/v1/tasks/:id:cancel", ex_wrapper(routes.cancel_task));
    ctx.post("/v1/tasks/:id:subscribe", ex_wrapper(routes.subscribe_task));
    ctx.post("/v1/tasks/:id:input", ex_wrapper(routes.send_input));

    // Discovery
    ctx.get("/.well-known/agent-card.json", ex_wrapper(routes.get_agent_card));
}

} // namespace a2a
