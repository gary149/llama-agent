#pragma once

#include "console.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Represents a single output segment with display type
struct output_segment {
    display_type type = DISPLAY_TYPE_RESET;
    std::string content;
};

// Buffered output for a single subagent task
// Collects output segments and flushes them atomically to console
class subagent_output_buffer {
public:
    explicit subagent_output_buffer(const std::string & task_id);

    // Buffer text with a display type
    void write(display_type type, const char * fmt, ...);

    // Buffer text without changing display type (uses DISPLAY_TYPE_RESET)
    void write(const char * fmt, ...);

    // Flush all buffered content atomically to console
    // Optionally prefix each line with task ID
    void flush(bool with_task_prefix = true);

    // Clear buffer without flushing
    void clear();

    // Check if buffer has content
    bool empty() const;

    // Get task ID
    const std::string & task_id() const { return task_id_; }

private:
    std::string task_id_;
    mutable std::mutex buffer_mutex_;
    std::vector<output_segment> segments_;
};

// Manager for all active subagent output buffers
// Thread-safe singleton
class subagent_output_manager {
public:
    static subagent_output_manager & instance();

    // Create buffer for a new task (returns raw pointer, manager owns the buffer)
    subagent_output_buffer * create_buffer(const std::string & task_id);

    // Get buffer for an existing task (returns nullptr if not found)
    subagent_output_buffer * get_buffer(const std::string & task_id);

    // Remove and destroy buffer for a task
    void remove_buffer(const std::string & task_id);

    // Flush all buffers (for status display or shutdown)
    void flush_all();

    // Get count of active buffers
    size_t active_count() const;

private:
    subagent_output_manager() = default;

    mutable std::mutex buffers_mutex_;
    std::map<std::string, std::unique_ptr<subagent_output_buffer>> buffers_;
};
