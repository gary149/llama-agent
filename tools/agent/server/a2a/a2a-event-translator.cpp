#include "a2a-event-translator.h"

#include <sstream>

namespace a2a {

a2a_event_translator::a2a_event_translator(a2a_task & task)
    : task_(task) {
}

agent_event_callback a2a_event_translator::get_callback() {
    return [this](const agent_event & event) {
        process(event);
    };
}

void a2a_event_translator::process(const agent_event & event) {
    switch (event.type) {
        case agent_event_type::TEXT_DELTA:
            handle_text_delta(event);
            break;
        case agent_event_type::REASONING_DELTA:
            handle_reasoning_delta(event);
            break;
        case agent_event_type::TOOL_START:
            handle_tool_start(event);
            break;
        case agent_event_type::TOOL_RESULT:
            handle_tool_result(event);
            break;
        case agent_event_type::PERMISSION_REQUIRED:
            handle_permission_required(event);
            break;
        case agent_event_type::PERMISSION_RESOLVED:
            handle_permission_resolved(event);
            break;
        case agent_event_type::ITERATION_START:
            handle_iteration_start(event);
            break;
        case agent_event_type::COMPLETED:
            handle_completed(event);
            break;
        case agent_event_type::ERROR:
            handle_error(event);
            break;
    }
}

std::string a2a_event_translator::generate_artifact_id() {
    int count = artifact_counter_.fetch_add(1);
    std::stringstream ss;
    ss << "artifact_" << task_.id() << "_" << count;
    return ss.str();
}

message a2a_event_translator::create_status_message() const {
    message msg;
    msg.role = "agent";
    if (!accumulated_text_.empty()) {
        msg.parts.push_back(part::from_text(accumulated_text_));
    }
    if (!accumulated_reasoning_.empty()) {
        msg.metadata = json{{"reasoning", accumulated_reasoning_}};
    }
    return msg;
}

void a2a_event_translator::handle_text_delta(const agent_event & event) {
    std::string content = event.data.value("content", "");
    accumulated_text_ += content;

    // Transition to WORKING if still in SUBMITTED
    if (task_.state() == task_state::SUBMITTED) {
        task_.transition_to(task_state::WORKING, create_status_message());
    }

    // Broadcast status update with current accumulated text
    task_status_update_event update;
    update.task_id = task_.id();
    update.context_id = task_.context_id();
    update.status.state = task_.state();
    update.status.status_message = create_status_message();
    update.final = false;

    task_.broadcast(json{{"statusUpdate", update.to_json()}});
}

void a2a_event_translator::handle_reasoning_delta(const agent_event & event) {
    std::string content = event.data.value("content", "");
    accumulated_reasoning_ += content;

    // Optionally broadcast reasoning as part of status
    // (reasoning is typically included in metadata, not as primary content)
}

void a2a_event_translator::handle_tool_start(const agent_event & event) {
    in_tool_execution_ = true;
    current_tool_name_ = event.data.value("name", "unknown");
    current_artifact_id_ = generate_artifact_id();

    // Create artifact for this tool execution
    artifact art;
    art.id = current_artifact_id_;
    art.name = current_tool_name_;
    art.description = "Tool execution: " + current_tool_name_;
    art.metadata = json{
        {"tool_name", current_tool_name_},
        {"arguments", event.data.value("args", "")},
        {"status", "in_progress"}
    };
    art.last_chunk = false;

    task_.add_artifact(art);

    // Broadcast artifact update (tool started)
    task_artifact_update_event artifact_event;
    artifact_event.task_id = task_.id();
    artifact_event.context_id = task_.context_id();
    artifact_event.art = art;

    task_.broadcast(json{{"artifactUpdate", artifact_event.to_json()}});
}

void a2a_event_translator::handle_tool_result(const agent_event & event) {
    in_tool_execution_ = false;

    bool success = event.data.value("success", false);
    std::string output = event.data.value("output", "");
    int64_t duration_ms = event.data.value("duration_ms", 0);

    // Update the artifact with the result
    artifact * art = task_.find_artifact(current_artifact_id_);
    if (art) {
        // Add result as a part
        if (success) {
            art->parts.push_back(part::from_text(output));
        } else {
            art->parts.push_back(part::from_text("Error: " + output));
        }
        art->last_chunk = true;
        art->metadata = json{
            {"tool_name", current_tool_name_},
            {"status", success ? "completed" : "failed"},
            {"duration_ms", duration_ms}
        };

        // Broadcast artifact update (tool completed)
        task_artifact_update_event artifact_event;
        artifact_event.task_id = task_.id();
        artifact_event.context_id = task_.context_id();
        artifact_event.art = *art;

        task_.broadcast(json{{"artifactUpdate", artifact_event.to_json()}});
    }

    current_artifact_id_.clear();
    current_tool_name_.clear();
}

void a2a_event_translator::handle_permission_required(const agent_event & event) {
    std::string request_id = event.data.value("request_id", "");
    std::string tool = event.data.value("tool", "");
    std::string details = event.data.value("details", "");
    bool dangerous = event.data.value("dangerous", false);

    // Store permission request ID for later resolution
    task_.set_pending_permission_id(request_id);

    // Create status message describing the permission request
    message msg;
    msg.role = "agent";
    msg.parts.push_back(part::from_text("Permission required for: " + tool));
    msg.metadata = json{
        {"permission_request_id", request_id},
        {"tool", tool},
        {"details", details},
        {"dangerous", dangerous}
    };

    // Transition to INPUT_REQUIRED state
    task_.transition_to(task_state::INPUT_REQUIRED, msg);

    // Broadcast status update
    task_.broadcast_status_update(false);
}

void a2a_event_translator::handle_permission_resolved(const agent_event & event) {
    std::string request_id = event.data.value("request_id", "");
    bool allowed = event.data.value("allowed", false);

    // Clear pending permission
    task_.clear_pending_permission();

    // Create status message
    message msg;
    msg.role = "agent";
    msg.parts.push_back(part::from_text(allowed ? "Permission granted" : "Permission denied"));
    msg.metadata = json{
        {"permission_request_id", request_id},
        {"allowed", allowed}
    };

    // Transition back to WORKING
    task_.transition_to(task_state::WORKING, msg);

    // Broadcast status update
    task_.broadcast_status_update(false);
}

void a2a_event_translator::handle_iteration_start(const agent_event & event) {
    int iteration = event.data.value("iteration", 0);
    int max_iterations = event.data.value("max_iterations", 50);

    // Ensure we're in WORKING state
    if (task_.state() == task_state::SUBMITTED) {
        message msg;
        msg.role = "agent";
        msg.metadata = json{{"iteration", iteration}, {"max_iterations", max_iterations}};
        task_.transition_to(task_state::WORKING, msg);
        task_.broadcast_status_update(false);
    }
}

void a2a_event_translator::handle_completed(const agent_event & event) {
    std::string reason = event.data.value("reason", "completed");

    // Determine final state based on reason
    task_state final_state;
    if (reason == "completed" || reason == "max_iterations") {
        final_state = task_state::COMPLETED;
    } else if (reason == "user_cancelled") {
        final_state = task_state::CANCELLED;
    } else {
        final_state = task_state::FAILED;
    }

    // Create final status message with stats
    message msg = create_status_message();
    if (event.data.contains("stats")) {
        if (!msg.metadata) {
            msg.metadata = json::object();
        }
        (*msg.metadata)["stats"] = event.data["stats"];
    }
    (*msg.metadata)["completion_reason"] = reason;

    // Add final message to history
    task_.add_to_history(msg);

    // Transition to final state
    task_.transition_to(final_state, msg);

    // Broadcast final status update
    task_.broadcast_status_update(true);
}

void a2a_event_translator::handle_error(const agent_event & event) {
    std::string error_message = event.data.value("message", "Unknown error");

    // Create error message
    message msg;
    msg.role = "agent";
    msg.parts.push_back(part::from_text(error_message));
    msg.metadata = json{{"error", true}};

    // Add to history
    task_.add_to_history(msg);

    // Transition to FAILED
    task_.transition_to(task_state::FAILED, msg);

    // Broadcast final status update
    task_.broadcast_status_update(true);
}

} // namespace a2a
