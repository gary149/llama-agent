#pragma once

// A2A Event Translator - Converts agent_event to A2A protocol events

#include "a2a-task.h"
#include "../../agent-loop.h"

#include <atomic>
#include <string>

namespace a2a {

// Translates internal agent_event types to A2A protocol events
// and broadcasts them to task subscribers
class a2a_event_translator {
public:
    explicit a2a_event_translator(a2a_task & task);
    ~a2a_event_translator() = default;

    // Get callback suitable for agent_loop::run_streaming()
    // This callback will translate events and broadcast them
    agent_event_callback get_callback();

    // Process a single agent_event
    void process(const agent_event & event);

    // Get accumulated text (for final message)
    const std::string & accumulated_text() const { return accumulated_text_; }

    // Get accumulated reasoning (for metadata)
    const std::string & accumulated_reasoning() const { return accumulated_reasoning_; }

private:
    a2a_task & task_;

    // Accumulated content for building final message
    std::string accumulated_text_;
    std::string accumulated_reasoning_;

    // Current tool artifact being built
    std::string current_artifact_id_;
    std::string current_tool_name_;
    bool in_tool_execution_ = false;

    // Artifact counter for generating unique IDs
    std::atomic<int> artifact_counter_{0};

    // Event handlers
    void handle_text_delta(const agent_event & event);
    void handle_reasoning_delta(const agent_event & event);
    void handle_tool_start(const agent_event & event);
    void handle_tool_result(const agent_event & event);
    void handle_permission_required(const agent_event & event);
    void handle_permission_resolved(const agent_event & event);
    void handle_iteration_start(const agent_event & event);
    void handle_completed(const agent_event & event);
    void handle_error(const agent_event & event);

    // Helper to generate unique artifact ID
    std::string generate_artifact_id();

    // Helper to create status message from accumulated text
    message create_status_message() const;
};

} // namespace a2a
