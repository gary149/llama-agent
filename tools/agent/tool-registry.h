#pragma once

#include "chat.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::ordered_json;

// Tool execution context passed to each tool
struct tool_context {
    std::string working_dir;
    std::atomic<bool> * is_interrupted = nullptr;
    int timeout_ms = 120000;
    bool has_vision = false;  // whether the loaded model supports image input

    // Subagent support (ported from gary149 subagents-opt-in).
    // Opaque pointers: agent_config/session_stats/inference_backend are declared
    // in agent-loop.h / inference-backend.h which include this header.
    void * backend_ptr = nullptr;       // inference_backend* (parent's inference backend)
    void * agent_config_ptr = nullptr;  // agent_config*
    void * session_stats_ptr = nullptr; // session_stats* (for tracking subagent tokens)
    int subagent_depth = 0;             // Current nesting depth (0 = main agent)
    bool subagents_enabled = false;     // task tool available in this loop
    bool subagent_own_slot = false;     // give subagents their own server slot (--parallel >= 2)
    // Base system prompt shared between parent and subagents.
    // Subagent prompts start with this prefix to maximize KV cache reuse.
    std::string base_system_prompt;
};

// Result returned from tool execution
struct tool_result {
    bool success = true;
    std::string output;
    std::string error;
    json content;  // structured content array (text + image blocks); overrides output when non-empty
    std::vector<uint8_t> image_bytes;  // raw image data for terminal preview
    std::string image_mime;            // e.g. "image/jpeg"
    bool no_truncate_display = false;  // skip the CLI's display truncation (for rich pre-formatted output)

    tool_result() = default;
    // Construct from the common {success, output, error} triple. Defined explicitly so
    // these call sites are constructor calls rather than aggregate initialization, which
    // under GCC -Werror=missing-field-initializers would otherwise fail to build for not
    // listing the remaining members (which already have default member initializers).
    tool_result(bool ok, std::string out, std::string err)
        : success(ok), output(std::move(out)), error(std::move(err)) {}
};

// Tool definition
struct tool_def {
    std::string name;
    std::string description;
    std::string parameters;  // JSON schema string

    std::function<tool_result(const json &, const tool_context &)> execute;

    // Convert to common_chat_tool for llama.cpp infrastructure
    common_chat_tool to_chat_tool() const {
        return {name, description, parameters};
    }
};

// Singleton tool registry
class tool_registry {
public:
    static tool_registry & instance();

    // Register a tool
    void register_tool(const tool_def & tool);

    // Get tool by name
    const tool_def * get_tool(const std::string & name) const;

    // Get all registered tools
    std::vector<const tool_def *> get_all_tools() const;

    // Convert all tools to common_chat_tool format
    std::vector<common_chat_tool> to_chat_tools() const;

    // Convert filtered subset of tools to common_chat_tool format (subagents)
    std::vector<common_chat_tool> to_chat_tools_filtered(const std::set<std::string> & allowed_tools) const;

    // Execute a tool by name
    tool_result execute(const std::string & name, const json & args, const tool_context & ctx) const;

    // Execute with bash command filtering (for read-only subagents)
    tool_result execute_filtered(const std::string & name,
                                 const json & args,
                                 const tool_context & ctx,
                                 const std::vector<std::string> & bash_patterns) const;

private:
    tool_registry() = default;
    std::map<std::string, tool_def> tools_;
};

// Helper macro for tool auto-registration
struct tool_registrar {
    tool_registrar(const tool_def & tool) {
        tool_registry::instance().register_tool(tool);
    }
};

#define REGISTER_TOOL(name, tool_instance) \
    static tool_registrar _tool_reg_##name(tool_instance)
