#include "../tool-registry.h"
#include "../permission.h"

#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

static tool_result write_execute(const json & args, const tool_context & ctx) {
    std::string file_path = args.value("file_path", "");
    std::string content = args.value("content", "");

    if (file_path.empty()) {
        return {false, "", "file_path parameter is required"};
    }

    // Make absolute if relative
    fs::path path(file_path);
    if (path.is_relative()) {
        path = fs::path(ctx.working_dir) / path;
    }

    // Block sensitive files
    if (permission_manager::is_sensitive_file(path.string())) {
        return {false, "", "Cannot write to sensitive file (contains credentials/secrets): " + path.string()};
    }

    // Check if file exists (for reporting)
    bool existed = fs::exists(path);

    // write-guard: refuse to overwrite existing files with the recipe to use
    // the edit tool instead. Small/medium models otherwise frequently use
    // Write to "rewrite" files and silently truncate content they didn't
    // intend to change. (Borrowed from itayinbarr/little-coder's write-guard
    // extension, which reports this firing on ~57% of Polyglot exercises.)
    if (existed && ctx.settings.write_guard) {
        return {false, "",
            "File already exists: " + path.string() + "\n\n"
            "Use the `edit` tool to modify existing files (search-and-replace).\n"
            "Write is for creating NEW files only.\n\n"
            "To replace the whole file, use edit with old_string matching the entire\n"
            "current content and new_string set to the desired content.\n\n"
            "To bypass this guard, disable `tools.write_guard` in the active profile."
        };
    }

    // Create parent directories if needed
    if (path.has_parent_path()) {
        try {
            fs::create_directories(path.parent_path());
        } catch (const fs::filesystem_error & e) {
            return {false, "", "Failed to create directories: " + std::string(e.what())};
        }
    }

    // Write file
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return {false, "", "Cannot open file for writing: " + path.string()};
    }

    file << content;
    file.close();

    if (file.fail()) {
        return {false, "", "Error writing to file: " + path.string()};
    }

    std::string msg = existed ? "File updated: " : "File created: ";
    msg += path.string();
    msg += " (" + std::to_string(content.length()) + " bytes)";

    return {true, msg, ""};
}

static tool_def write_tool = {
    "write",
    "Create a new file or overwrite an existing file with the given content.",
    R"json({
        "type": "object",
        "properties": {
            "file_path": {
                "type": "string",
                "description": "Path to the file to write (absolute or relative to working directory)"
            },
            "content": {
                "type": "string",
                "description": "The content to write to the file"
            }
        },
        "required": ["file_path", "content"]
    })json",
    write_execute
};

REGISTER_TOOL(write, write_tool);
