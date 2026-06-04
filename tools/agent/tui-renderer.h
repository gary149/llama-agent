#pragma once

#include "inference-backend.h"
#include "permission-async.h"
#include "tui-editor.h"
#include "tui-events.h"
#include "tui-select-list.h"

#include <atomic>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <signal.h>
#include <termios.h>
#endif

struct tui_command {
    std::string text;
    bool eof = false;
};

struct tui_footer_state {
    std::string working_dir;
    std::string session_path;
    inference_backend_meta meta;
    session_stats stats;
    int32_t last_prompt_tokens = 0;
    bool generating = false;
    size_t spinner_frame = 0;
    std::string transient_label;
};

std::vector<std::string> tui_render_footer(const tui_footer_state & state, int width, bool color);
tui_event tui_event_from_agent_event(const agent_event & event);

class tui_renderer {
public:
    struct config {
        FILE * out = stdout;
        bool color = true;
        bool multiline_input = false;
        std::string working_dir;
        std::string session_path;
        inference_backend_meta meta;
        permission_manager_async * permissions = nullptr;
        std::function<void()> interrupt;
    };

    explicit tui_renderer(config cfg);
    ~tui_renderer();

    tui_renderer(const tui_renderer &) = delete;
    tui_renderer & operator=(const tui_renderer &) = delete;

    struct tool_delta_state {
        std::string name;
        std::string accumulated_args;
        bool header_printed = false;
        std::string displayed_path;
        std::string content_buffer;
        size_t content_scan_pos = 0;
        size_t content_raw_end = 0;
        size_t displayed_bytes = 0;
        bool content_complete = false;
    };

    void post_event(tui_event event);
    void post_agent_event(const agent_event & event);
    void post_transcript(std::string text, tui_transcript_style style = tui_transcript_style::NORMAL);
    void post_stats(const session_stats & stats, int32_t last_prompt_tokens);
    void set_generating(bool generating);

    bool wait_for_command(tui_command & command, int timeout_ms = -1);
    void shutdown();

private:
    enum class overlay_kind {
        NONE,
        SLASH,
        FILE,
        PERMISSION,
    };

    void render_loop();
    void input_loop();

    void handle_tui_event(const tui_event & event);
    void handle_input_event(const tui_input_event & event);

    void append_transcript_line(const std::string & text, tui_transcript_style style);
    void append_transcript_raw(const std::string & bytes);
    void flush_text_buffer(bool force_newline);
    void handle_tool_call_delta(const tui_event & event);
    void render_tool_start(const tui_event & event);
    void render_tool_result(const tui_event & event);

    void update_autocomplete();
    void start_file_completion_worker(const std::string & query, uint64_t generation);
    void complete_selected();
    void dismiss_overlay();
    void open_permission_overlay(const tui_event & event);
    void answer_permission(char ch);

    std::vector<std::string> current_region_lines(int & cursor_row, int & cursor_col);
    void redraw_managed_region(bool full_redraw = false);
    void clear_managed_region();
    void flush_pending_transcript();

    void query_terminal_size();
    void detect_synchronized_output();
    void install_resize_handler();
    void restore_resize_handler();
    void setup_raw_mode();
    void restore_raw_mode();

    std::string sgr(tui_transcript_style style) const;
    std::string reset_sgr() const;

    config cfg_;
    FILE * out_ = stdout;

    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_started_{false};
    std::thread render_thread_;
    std::thread input_thread_;

    tui_event_queue<tui_event> events_;
    tui_event_queue<tui_input_event> input_events_;
    tui_event_queue<tui_command> commands_;

    tui_editor editor_;
    tui_select_list select_list_;
    overlay_kind overlay_ = overlay_kind::NONE;
    size_t completion_start_ = 0;
    size_t completion_end_ = 0;
    std::string completion_prefix_;
    std::string last_file_query_;
    uint64_t file_completion_generation_ = 0;
    std::mutex autocomplete_threads_mu_;
    std::vector<std::thread> autocomplete_threads_;

    permission_request active_permission_;
    std::string active_permission_id_;

    tui_footer_state footer_;
    bool color_ = true;
    bool sync_output_ = false;
    int term_rows_ = 24;
    int term_cols_ = 80;
    bool force_full_redraw_ = true;
    bool managed_visible_ = false;
    bool managed_dirty_ = true;
    std::vector<std::string> previous_region_;
    int previous_cursor_row_ = 0;

    std::string transcript_buffer_;
    tui_transcript_style buffer_style_ = tui_transcript_style::NORMAL;
    std::vector<std::string> pending_transcript_;
    std::map<size_t, tool_delta_state> tool_delta_states_;

    std::chrono::steady_clock::time_point last_spinner_tick_;
    std::chrono::steady_clock::time_point last_idle_tick_;
    std::chrono::steady_clock::time_point transient_until_;

#if !defined(_WIN32)
    termios saved_termios_{};
    bool saved_termios_valid_ = false;
    struct sigaction previous_sigwinch_{};
    bool sigwinch_installed_ = false;
#endif
};
