#pragma once

#include "chat.h"

#include <nlohmann/json.hpp>

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

    // Subagent support: pointers to parent agent's context
    void * server_ctx_ptr = nullptr;       // Pointer to server_context
    void * agent_config_ptr = nullptr;     // Pointer to agent_config
    void * common_params_ptr = nullptr;    // Pointer to common_params (for model inference params)
    void * session_stats_ptr = nullptr;    // Pointer to session_stats (for tracking subagent tokens)
    int subagent_depth = 0;                // Current nesting depth (0 = main agent)

    // Prefix caching: base system prompt shared between parent and subagents
    // Subagent prompts start with this prefix to maximize KV cache reuse
    std::string base_system_prompt;
};

// Result returned from tool execution
struct tool_result {
    bool success = true;
    std::string output;
    std::string error;
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

    // Convert filtered subset of tools to common_chat_tool format
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
