#pragma once

#include <set>
#include <string>
#include <vector>

// Subagent type definitions
enum class subagent_type {
    EXPLORE,   // Read-only: read, glob, bash (read-only cmds)
    PLAN,      // Architecture planning: read, glob
    GENERAL,   // Multi-step tasks: all tools except task
    BASH,      // Command execution: bash only
};

// Configuration for a subagent type
struct subagent_type_config {
    std::string name;
    std::string description;
    std::string icon;                      // Unicode icon for display
    std::string color_code;                // ANSI color code
    std::set<std::string> allowed_tools;   // Tool whitelist
    std::vector<std::string> bash_patterns; // For EXPLORE: allowed bash command prefixes
    bool can_write_files;
    int max_iterations;
};

// Get configuration for a subagent type
const subagent_type_config & get_subagent_config(subagent_type type);

// Parse subagent type from string (throws on unknown type)
subagent_type parse_subagent_type(const std::string & str);

// Get string name for subagent type
const char * subagent_type_name(subagent_type type);
