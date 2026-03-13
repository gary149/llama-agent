#pragma once

#include "subagent-types.h"

#include <atomic>
#include <mutex>
#include <string>

// Forward declaration
class subagent_output_buffer;

// Manages nested visual output for subagent execution
class subagent_display {
public:
    // RAII class for managing a subagent display scope
    class scope {
    public:
        // Direct mode (synchronous tasks) - outputs immediately to console
        scope(subagent_display & display,
              const std::string & name,
              subagent_type type,
              const std::string & description);

        // Buffered mode (background tasks) - collects output in buffer
        scope(subagent_display & display,
              const std::string & name,
              subagent_type type,
              const std::string & description,
              subagent_output_buffer * buffer);

        ~scope();

        // Prevent copying
        scope(const scope &) = delete;
        scope & operator=(const scope &) = delete;

        // Report a tool call within this subagent
        void report_tool_call(const std::string & tool_name,
                              const std::string & args_summary,
                              int elapsed_ms);

        // Report completion with timing and token usage
        void report_done(int elapsed_ms, int32_t total_tokens = 0);

    private:
        subagent_display & display_;
        subagent_output_buffer * buffer_ = nullptr;  // nullptr = direct mode
        int depth_;
        bool done_reported_ = false;
    };

    // Get singleton instance
    static subagent_display & instance();

    // Current nesting depth (0 = main agent)
    int depth() const { return depth_.load(); }

    // Check if subagents can be spawned (depth < max_depth)
    bool can_spawn() const { return depth_.load() < max_depth_; }

    // Set max depth (default 1)
    void set_max_depth(int d) { max_depth_ = d; }

    // Get max depth
    int max_depth() const { return max_depth_; }

private:
    subagent_display() = default;

    std::mutex mtx_;
    std::atomic<int> depth_{0};
    int max_depth_ = 1;

    // Print tree characters for current depth
    // If buffer is provided, output goes to buffer; otherwise to console
    void print_header(int depth, const std::string & icon, const std::string & name,
                      const std::string & type_name, const std::string & description,
                      subagent_output_buffer * buffer = nullptr);
    void print_tool_call(int depth, const std::string & tool_name,
                         const std::string & args_summary, int elapsed_ms,
                         subagent_output_buffer * buffer = nullptr);
    void print_done(int depth, int elapsed_ms, int32_t total_tokens = 0,
                    subagent_output_buffer * buffer = nullptr);

    friend class scope;
};

// Helper: generate indentation prefix for given depth
std::string subagent_indent_prefix(int depth);
