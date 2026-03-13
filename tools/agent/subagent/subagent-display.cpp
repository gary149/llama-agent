#include "subagent-display.h"
#include "subagent-output.h"
#include "console.h"

#include <sstream>

// Tree drawing characters (UTF-8)
static const char * TREE_CORNER_TOP    = "\xE2\x94\x8C";  // ┌
static const char * TREE_VERTICAL      = "\xE2\x94\x82";  // │
static const char * TREE_TEE           = "\xE2\x94\x9C";  // ├
static const char * TREE_CORNER_BOTTOM = "\xE2\x94\x94";  // └
static const char * TREE_HORIZONTAL    = "\xE2\x94\x80";  // ─
static const char * ARROW_RIGHT        = "\xE2\x80\xBA";  // ›

std::string subagent_indent_prefix(int depth) {
    if (depth <= 0) {
        return "";
    }
    std::string prefix;
    for (int i = 0; i < depth; i++) {
        prefix += TREE_VERTICAL;
        prefix += "   ";
    }
    return prefix;
}

subagent_display & subagent_display::instance() {
    static subagent_display inst;
    return inst;
}

void subagent_display::print_header(int depth, const std::string & icon,
                                     const std::string & name,
                                     const std::string & type_name,
                                     const std::string & description,
                                     subagent_output_buffer * buffer) {
    std::string prefix = subagent_indent_prefix(depth);

    if (buffer) {
        // Buffered mode: write to buffer
        buffer->write(DISPLAY_TYPE_RESET, "\n%s%s%s%s ", prefix.c_str(), TREE_CORNER_TOP, TREE_HORIZONTAL, TREE_HORIZONTAL);
        buffer->write(DISPLAY_TYPE_RESET, "%s ", icon.c_str());
        buffer->write(DISPLAY_TYPE_SUBAGENT, "%s", name.c_str());
        buffer->write(DISPLAY_TYPE_REASONING, " (%s)\n", type_name.c_str());

        if (!description.empty()) {
            buffer->write(DISPLAY_TYPE_RESET, "%s%s   ", prefix.c_str(), TREE_VERTICAL);
            buffer->write(DISPLAY_TYPE_REASONING, "%s\n", description.c_str());
        }
    } else {
        // Direct mode: use output_guard for atomic output
        console::output_guard guard;

        guard.write("\n%s%s%s%s ", prefix.c_str(), TREE_CORNER_TOP, TREE_HORIZONTAL, TREE_HORIZONTAL);
        guard.write("%s ", icon.c_str());
        guard.set_display(DISPLAY_TYPE_SUBAGENT);
        guard.write("%s", name.c_str());
        guard.set_display(DISPLAY_TYPE_RESET);
        guard.set_display(DISPLAY_TYPE_REASONING);
        guard.write(" (%s)\n", type_name.c_str());
        guard.set_display(DISPLAY_TYPE_RESET);

        if (!description.empty()) {
            guard.write("%s%s   ", prefix.c_str(), TREE_VERTICAL);
            guard.set_display(DISPLAY_TYPE_REASONING);
            guard.write("%s\n", description.c_str());
            guard.set_display(DISPLAY_TYPE_RESET);
        }
    }
}

void subagent_display::print_tool_call(int depth, const std::string & tool_name,
                                        const std::string & args_summary,
                                        int elapsed_ms,
                                        subagent_output_buffer * buffer) {
    std::string prefix = subagent_indent_prefix(depth);

    if (buffer) {
        // Buffered mode
        buffer->write(DISPLAY_TYPE_RESET, "%s%s   %s%s%s ", prefix.c_str(), TREE_VERTICAL, TREE_TEE, TREE_HORIZONTAL, ARROW_RIGHT);
        buffer->write(DISPLAY_TYPE_INFO, "%s", tool_name.c_str());

        if (!args_summary.empty()) {
            buffer->write(DISPLAY_TYPE_RESET, " %s", args_summary.c_str());
        }

        buffer->write(DISPLAY_TYPE_RESET, " ");
        if (elapsed_ms < 1000) {
            buffer->write(DISPLAY_TYPE_REASONING, "(%dms)", elapsed_ms);
        } else {
            buffer->write(DISPLAY_TYPE_REASONING, "(%.1fs)", elapsed_ms / 1000.0);
        }
        buffer->write(DISPLAY_TYPE_RESET, "\n");
    } else {
        // Direct mode
        console::output_guard guard;

        guard.write("%s%s   %s%s%s ", prefix.c_str(), TREE_VERTICAL, TREE_TEE, TREE_HORIZONTAL, ARROW_RIGHT);
        guard.set_display(DISPLAY_TYPE_INFO);
        guard.write("%s", tool_name.c_str());
        guard.set_display(DISPLAY_TYPE_RESET);

        if (!args_summary.empty()) {
            guard.write(" %s", args_summary.c_str());
        }

        guard.write(" ");
        guard.set_display(DISPLAY_TYPE_REASONING);
        if (elapsed_ms < 1000) {
            guard.write("(%dms)", elapsed_ms);
        } else {
            guard.write("(%.1fs)", elapsed_ms / 1000.0);
        }
        guard.set_display(DISPLAY_TYPE_RESET);
        guard.write("\n");
    }
}

// Helper to format token count (e.g., 1234 -> "1.2k", 12345 -> "12.3k")
static std::string format_tokens(int32_t tokens) {
    if (tokens < 1000) {
        return std::to_string(tokens) + " tk";
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1fk tk", tokens / 1000.0);
        return buf;
    }
}

void subagent_display::print_done(int depth, int elapsed_ms, int32_t total_tokens,
                                   subagent_output_buffer * buffer) {
    std::string prefix = subagent_indent_prefix(depth);

    if (buffer) {
        // Buffered mode
        buffer->write(DISPLAY_TYPE_RESET, "%s%s   %s%s%s ", prefix.c_str(), TREE_VERTICAL, TREE_CORNER_BOTTOM, TREE_HORIZONTAL, TREE_HORIZONTAL);
        buffer->write(DISPLAY_TYPE_INFO, "done");

        if (elapsed_ms > 0 || total_tokens > 0) {
            buffer->write(DISPLAY_TYPE_RESET, " ");
            buffer->write(DISPLAY_TYPE_REASONING, "(");
            bool need_comma = false;
            if (elapsed_ms > 0) {
                if (elapsed_ms < 1000) {
                    buffer->write(DISPLAY_TYPE_REASONING, "%dms", elapsed_ms);
                } else {
                    buffer->write(DISPLAY_TYPE_REASONING, "%.1fs", elapsed_ms / 1000.0);
                }
                need_comma = true;
            }
            if (total_tokens > 0) {
                if (need_comma) {
                    buffer->write(DISPLAY_TYPE_REASONING, ", ");
                }
                buffer->write(DISPLAY_TYPE_REASONING, "%s", format_tokens(total_tokens).c_str());
            }
            buffer->write(DISPLAY_TYPE_REASONING, ")");
        }
        buffer->write(DISPLAY_TYPE_RESET, "\n");
    } else {
        // Direct mode
        console::output_guard guard;

        guard.write("%s%s   %s%s%s ", prefix.c_str(), TREE_VERTICAL, TREE_CORNER_BOTTOM, TREE_HORIZONTAL, TREE_HORIZONTAL);
        guard.set_display(DISPLAY_TYPE_INFO);
        guard.write("done");
        guard.set_display(DISPLAY_TYPE_RESET);

        if (elapsed_ms > 0 || total_tokens > 0) {
            guard.write(" ");
            guard.set_display(DISPLAY_TYPE_REASONING);
            guard.write("(");
            bool need_comma = false;
            if (elapsed_ms > 0) {
                if (elapsed_ms < 1000) {
                    guard.write("%dms", elapsed_ms);
                } else {
                    guard.write("%.1fs", elapsed_ms / 1000.0);
                }
                need_comma = true;
            }
            if (total_tokens > 0) {
                if (need_comma) {
                    guard.write(", ");
                }
                guard.write("%s", format_tokens(total_tokens).c_str());
            }
            guard.write(")");
            guard.set_display(DISPLAY_TYPE_RESET);
        }
        guard.write("\n");
    }
}

// scope implementation

// Direct mode constructor (synchronous tasks)
subagent_display::scope::scope(subagent_display & display,
                                const std::string & name,
                                subagent_type type,
                                const std::string & description)
    : display_(display)
    , buffer_(nullptr)
{
    std::lock_guard<std::mutex> lock(display_.mtx_);
    depth_ = display_.depth_.fetch_add(1);

    const auto & config = get_subagent_config(type);
    display_.print_header(depth_, config.icon, name, config.name, description, buffer_);
}

// Buffered mode constructor (background tasks)
subagent_display::scope::scope(subagent_display & display,
                                const std::string & name,
                                subagent_type type,
                                const std::string & description,
                                subagent_output_buffer * buffer)
    : display_(display)
    , buffer_(buffer)
{
    std::lock_guard<std::mutex> lock(display_.mtx_);
    depth_ = display_.depth_.fetch_add(1);

    const auto & config = get_subagent_config(type);
    display_.print_header(depth_, config.icon, name, config.name, description, buffer_);
}

subagent_display::scope::~scope() {
    std::lock_guard<std::mutex> lock(display_.mtx_);
    display_.depth_.fetch_sub(1);

    if (!done_reported_) {
        display_.print_done(depth_, 0, 0, buffer_);
    }
}

void subagent_display::scope::report_tool_call(const std::string & tool_name,
                                                const std::string & args_summary,
                                                int elapsed_ms) {
    std::lock_guard<std::mutex> lock(display_.mtx_);
    display_.print_tool_call(depth_, tool_name, args_summary, elapsed_ms, buffer_);
}

void subagent_display::scope::report_done(int elapsed_ms, int32_t total_tokens) {
    std::lock_guard<std::mutex> lock(display_.mtx_);
    display_.print_done(depth_, elapsed_ms, total_tokens, buffer_);
    done_reported_ = true;
}
