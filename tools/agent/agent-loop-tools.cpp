#include "agent-loop.h"
#include "utf8-sanitize.h"

#include <filesystem>
#include <functional>

// Async version of execute_tool_call for API use
// Uses async permission manager and emits events instead of blocking on console
tool_result agent_loop::execute_tool_call_async(
    const common_chat_tool_call & call,
    agent_event_callback on_event,
    permission_manager_async & async_perms,
    std::function<bool()> should_stop) {

    auto & registry = tool_registry::instance();

    // Check if tool exists
    const tool_def * tool = registry.get_tool(call.name);
    if (!tool) {
        return {false, "", "Unknown tool: " + call.name};
    }

    // Parse arguments (retry after UTF-8 sanitize: the model may echo legacy
    // encoding bytes it saw in tool output into the argument string)
    json args;
    try {
        args = json::parse(call.arguments);
    } catch (const json::parse_error & e) {
        auto san = utf8_sanitize(call.arguments);
        try {
            args = json::parse(san.text);
        } catch (const json::parse_error & e2) {
            return {false, "", std::string("Invalid JSON arguments: ") + e2.what()};
        }
    }

    // Determine permission type
    permission_type ptype = permission_type::BASH;
    if (call.name == "read") {
        ptype = permission_type::FILE_READ;
    } else if (call.name == "write") {
        ptype = permission_type::FILE_WRITE;
    } else if (call.name == "edit") {
        ptype = permission_type::FILE_EDIT;
    } else if (call.name == "update_plan") {
        ptype = permission_type::GLOB;
    }

    // Build permission request
    permission_request req;
    req.type = ptype;
    req.tool_name = call.name;
    req.details = call.arguments;

    // Check for external directory access on file operations
    if (call.name == "read" || call.name == "write" || call.name == "edit") {
        std::string file_path = args.value("file_path", "");
        if (!file_path.empty()) {
            std::filesystem::path path(file_path);
            if (path.is_relative()) {
                path = std::filesystem::path(tool_ctx_.working_dir) / path;
            }
            // In yolo mode all permissions are auto-approved, so skip the
            // external-dir prompt (it would otherwise block in non-TUI runs).
            if (async_perms.is_external_path(path.string()) && !async_perms.is_yolo()) {
                permission_request ext_req;
                ext_req.type = permission_type::EXTERNAL_DIR;
                ext_req.tool_name = call.name;
                ext_req.details = "External file: " + path.string();
                ext_req.is_dangerous = true;
                ext_req.description = "Operation outside working directory";

                // Request permission asynchronously
                std::string req_id = async_perms.request_permission(ext_req);
                on_event(agent_event::permission_required(
                    req_id, call.name, ext_req.description, ext_req.details, true));

                // Wait for response (cancellable via should_stop)
                auto response = async_perms.wait_for_response_or_stop(req_id, 300000, should_stop);
                if (should_stop()) {
                    on_event(agent_event::permission_resolved(req_id, false));
                    return {false, "", "Operation cancelled"};
                }
                if (!response || !response->allowed) {
                    on_event(agent_event::permission_resolved(req_id, false));
                    return {false, "", "Blocked: File is outside working directory"};
                }
                on_event(agent_event::permission_resolved(req_id, true));
            }
        }
    }

    // Check for dangerous commands
    if (call.name == "bash") {
        std::string cmd = args.value("command", "");
        req.details = cmd;
        req.is_dangerous = async_perms.is_dangerous_bash_command(cmd);
    }

    // Check doom loop
    std::hash<std::string> hasher;
    std::string args_hash = std::to_string(hasher(call.arguments));
    // In yolo mode all permissions are auto-approved (no blocking prompt).
    if (async_perms.is_doom_loop(call.name, args_hash) && !async_perms.is_yolo()) {
        req.description = "Detected repeated identical tool calls (doom loop)";

        std::string req_id = async_perms.request_permission(req);
        on_event(agent_event::permission_required(req_id, call.name, req.description, req.details, true));

        auto response = async_perms.wait_for_response_or_stop(req_id, 300000, should_stop);
        if (should_stop()) {
            on_event(agent_event::permission_resolved(req_id, false));
            return {false, "", "Operation cancelled"};
        }
        if (!response || !response->allowed) {
            on_event(agent_event::permission_resolved(req_id, false));
            return {false, "", "Blocked: Detected repeated identical tool calls"};
        }
        on_event(agent_event::permission_resolved(req_id, true));
    }

    // Check permission
    permission_state state = async_perms.check_permission(req);
    if (state == permission_state::DENY || state == permission_state::DENY_SESSION) {
        return {false, "", "Permission denied for " + call.name};
    }

    if (state == permission_state::ASK) {
        // Request permission asynchronously
        std::string req_id = async_perms.request_permission(req);
        on_event(agent_event::permission_required(
            req_id, call.name, req.description, req.details, req.is_dangerous));

        // Wait for response (cancellable via should_stop)
        auto response = async_perms.wait_for_response_or_stop(req_id, 300000, should_stop);

        if (should_stop()) {
            on_event(agent_event::permission_resolved(req_id, false));
            return {false, "", "Operation cancelled"};
        }

        if (!response) {
            on_event(agent_event::permission_resolved(req_id, false));
            return {false, "", "Permission request timed out"};
        }

        on_event(agent_event::permission_resolved(req_id, response->allowed));

        if (!response->allowed) {
            return {false, "", "User denied permission for " + call.name};
        }
    }

    // Record this call
    async_perms.record_tool_call(call.name, args_hash);

    // Execute the tool
    return registry.execute(call.name, args, tool_ctx_);
}
