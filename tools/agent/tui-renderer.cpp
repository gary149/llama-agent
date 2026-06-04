#include "tui-renderer.h"

#include "console.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

static const char * ANSI_RESET   = "\x1b[0m";
static const char * ANSI_RED     = "\x1b[31m";
static const char * ANSI_YELLOW  = "\x1b[33m";
static const char * ANSI_MAGENTA = "\x1b[35m";
static const char * ANSI_CYAN    = "\x1b[36m";
static const char * ANSI_GRAY    = "\x1b[90m";
static const char * ANSI_DIM     = "\x1b[2m";
static const char * ANSI_BOLD    = "\x1b[1m";
static const char SPINNER_CHARS[] = {'|', '/', '-', '\\'};

static std::string truncate_display(std::string text, size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text;
    }
    text.resize(max_bytes);
    text.resize(tui_prev_utf8_char_pos(text, text.size()));
    return text + "...";
}

static std::string basename_or_path(const std::string & path) {
    if (path.empty()) {
        return "";
    }
    fs::path p(path);
    std::string name = p.filename().string();
    return name.empty() ? path : name;
}

static std::string shorten_middle(const std::string & text, int max_width) {
    if (max_width <= 0) {
        return "";
    }
    if (tui_string_width(text) <= max_width) {
        return text;
    }
    if (max_width <= 3) {
        return "...";
    }

    std::string left;
    std::string right;
    int left_width = (max_width - 3) / 2;
    int right_width = max_width - 3 - left_width;

    for (size_t i = 0; i < text.size() && tui_string_width(left) < left_width;) {
        size_t advance = 0;
        (void) tui_decode_utf8(text, i, advance);
        std::string next = left + text.substr(i, advance);
        if (tui_string_width(next) > left_width) {
            break;
        }
        left = next;
        i += advance;
    }

    size_t pos = text.size();
    while (pos > 0 && tui_string_width(right) < right_width) {
        size_t prev = tui_prev_utf8_char_pos(text, pos);
        std::string next = text.substr(prev, pos - prev) + right;
        if (tui_string_width(next) > right_width) {
            break;
        }
        right = next;
        pos = prev;
    }
    return left + "..." + right;
}

static std::string right_align_trimmed(const std::string & left, const std::string & right, int width) {
    if (width <= 0) {
        return "";
    }
    std::string trimmed_left = left;
    int right_width = tui_string_width(right);
    int max_left = std::max(0, width - right_width - 1);
    trimmed_left = shorten_middle(trimmed_left, max_left);
    int spaces = std::max(1, width - tui_string_width(trimmed_left) - right_width);
    if (right.empty()) {
        spaces = 0;
    }
    return trimmed_left + std::string(spaces, ' ') + right;
}

static bool replace_once(std::string & text, const std::string & from, const std::string & to) {
    size_t pos = text.find(from);
    if (pos == std::string::npos) {
        return false;
    }
    text.replace(pos, from.size(), to);
    return true;
}

std::vector<std::string> tui_render_footer(const tui_footer_state & state, int width, bool color) {
    width = std::max(width, 20);
    std::vector<std::string> lines;
    std::string reset = color ? ANSI_RESET : "";
    std::string warn = color ? ANSI_YELLOW : "";
    std::string error = color ? ANSI_RED : "";

    std::string cwd = shorten_middle(state.working_dir.empty() ? "." : state.working_dir, width / 2);
    std::string session = basename_or_path(state.session_path);
    std::string left = cwd;
    if (!session.empty()) {
        left += " \xE2\x80\xA2 " + session;
    }
    lines.push_back(shorten_middle(left, width));

    double speed = state.stats.total_predicted_ms > 0.0
        ? state.stats.total_output * 1000.0 / state.stats.total_predicted_ms
        : 0.0;
    double ctx = state.meta.n_ctx > 0 ? (double) state.last_prompt_tokens / state.meta.n_ctx : 0.0;
    int ctx_pct = std::max(0, std::min(100, (int) (ctx * 100.0 + 0.5)));
    int ctx_k = state.meta.n_ctx > 0 ? std::max(1, (state.meta.n_ctx + 999) / 1000) : 0;

    std::ostringstream ss;
    if (state.generating) {
        ss << SPINNER_CHARS[state.spinner_frame % 4] << " ";
    } else {
        ss << "  ";
    }
    ss << "\xE2\x86\x91 " << state.stats.total_input << " \xE2\x86\x93 " << state.stats.total_output;
    ss << " | ";
    if (speed > 0.0) {
        ss.setf(std::ios::fixed);
        ss.precision(1);
        ss << speed << " tok/s";
        ss.unsetf(std::ios::fixed);
    } else {
        ss << "-- tok/s";
    }
    std::string ctx_text = std::to_string(ctx_pct) + "%/";
    ctx_text += ctx_k > 0 ? std::to_string(ctx_k) + "K" : "--K";
    ss << " | " << ctx_text;
    if (!state.transient_label.empty()) {
        ss << " | " << state.transient_label;
    }

    std::string model = state.meta.model_name.empty() ? "" : state.meta.model_name;
    std::string footer = right_align_trimmed(ss.str(), model, width);
    if (color) {
        if (ctx_pct > 90) {
            replace_once(footer, ctx_text, error + ctx_text + reset);
        } else if (ctx_pct >= 70) {
            replace_once(footer, ctx_text, warn + ctx_text + reset);
        }
        // Use ANSI_DIM (faint) for the model name so it is visually distinct
        // from the dim-gray (ANSI_GRAY/\x1b[90m) used for reasoning blocks (L2).
        if (!model.empty()) {
            size_t pos = footer.rfind(model);
            if (pos != std::string::npos) {
                footer.replace(pos, model.size(), std::string(ANSI_DIM) + model + reset);
            }
        }
        // Always end the footer line with a reset so no SGR state leaks (L2).
        if (footer.size() < 4 || footer.substr(footer.size() - 4) != reset) {
            footer += reset;
        }
    }
    lines.push_back(footer);
    return lines;
}

tui_event tui_event_from_agent_event(const agent_event & event) {
    tui_event out;
    switch (event.type) {
        case agent_event_type::TEXT_DELTA:
            out.type = tui_event_type::TEXT_DELTA;
            out.text = event.data.value("content", "");
            break;
        case agent_event_type::REASONING_DELTA:
            out.type = tui_event_type::REASONING_DELTA;
            out.text = event.data.value("content", "");
            break;
        case agent_event_type::TOOL_CALL_DELTA:
            out.type = tui_event_type::TOOL_CALL_DELTA;
            out.tool_call_index = event.data.value("index", std::string::npos);
            out.tool_name = event.data.value("name", "");
            out.tool_args = event.data.value("args_delta", "");
            break;
        case agent_event_type::TOOL_START:
            out.type = tui_event_type::TOOL_START;
            out.tool_name = event.data.value("name", "");
            out.tool_args = event.data.value("args", "");
            break;
        case agent_event_type::TOOL_RESULT:
            out.type = tui_event_type::TOOL_RESULT;
            out.tool_name = event.data.value("name", "");
            out.tool_success = event.data.value("success", true);
            out.tool_output = event.data.value("output", "");
            out.elapsed_ms = event.data.value("duration_ms", 0);
            break;
        case agent_event_type::PERMISSION_REQUIRED:
            out.type = tui_event_type::PERMISSION_REQUIRED;
            out.perm_id = event.data.value("request_id", "");
            out.perm.tool_name = event.data.value("tool", "");
            out.perm.description = event.data.value("description", "");
            out.perm.details = event.data.value("details", "");
            out.perm.is_dangerous = event.data.value("dangerous", false);
            break;
        case agent_event_type::PERMISSION_RESOLVED:
            out.type = tui_event_type::PERMISSION_RESOLVED;
            out.perm_id = event.data.value("request_id", "");
            out.perm_allowed = event.data.value("allowed", false);
            break;
        case agent_event_type::ITERATION_START:
            out.type = tui_event_type::ITERATION_START;
            break;
        case agent_event_type::COMPACTION_COMPLETED:
            out.type = tui_event_type::COMPACTION_COMPLETED;
            out.messages_kept = event.data.value("messages_kept", 0);
            break;
        case agent_event_type::COMPLETED:
            out.type = tui_event_type::COMPLETED;
            if (event.data.contains("stats")) {
                const auto & stats = event.data["stats"];
                out.stats.total_input = stats.value("input_tokens", 0);
                out.stats.total_output = stats.value("output_tokens", 0);
                out.stats.total_cached = stats.value("cached_tokens", 0);
                out.stats.total_predicted_ms = stats.value("total_predicted_ms", 0.0);
                out.last_prompt_tokens = stats.value("last_prompt_tokens", 0);
            }
            break;
        case agent_event_type::ERROR:
            out.type = tui_event_type::ERROR;
            out.text = event.data.value("message", "");
            break;
    }
    return out;
}

#if !defined(_WIN32)
static std::atomic<bool> g_tui_sigwinch{false};
static struct sigaction g_previous_sigwinch{};

static void tui_sigwinch_handler(int) {
    g_tui_sigwinch.store(true);
}
#endif

tui_renderer::tui_renderer(config cfg)
    : cfg_(std::move(cfg))
    , out_(cfg_.out ? cfg_.out : stdout)
    , editor_(cfg_.multiline_input)
    , color_(cfg_.color)
{
    footer_.working_dir = cfg_.working_dir;
    footer_.session_path = cfg_.session_path;
    footer_.meta = cfg_.meta;
    last_spinner_tick_ = std::chrono::steady_clock::now();
    last_idle_tick_ = last_spinner_tick_;

    setup_raw_mode();
    query_terminal_size();
    detect_synchronized_output();
    install_resize_handler();

    console::set_tui_active(true);

    running_.store(true);
    render_thread_ = std::thread(&tui_renderer::render_loop, this);
    input_thread_ = std::thread(&tui_renderer::input_loop, this);
}

tui_renderer::~tui_renderer() {
    shutdown();
}

void tui_renderer::post_event(tui_event event) {
    events_.push(std::move(event));
}

void tui_renderer::post_agent_event(const agent_event & event) {
    post_event(tui_event_from_agent_event(event));
}

void tui_renderer::post_transcript(std::string text, tui_transcript_style style) {
    tui_event event;
    event.type = tui_event_type::TRANSCRIPT;
    event.text = std::move(text);
    event.transcript_style = style;
    post_event(std::move(event));
}

void tui_renderer::post_stats(const session_stats & stats, int32_t last_prompt_tokens) {
    tui_event event;
    event.type = tui_event_type::STATS;
    event.stats = stats;
    event.last_prompt_tokens = last_prompt_tokens;
    post_event(std::move(event));
}

void tui_renderer::set_generating(bool generating) {
    tui_event event;
    event.type = tui_event_type::STATS;
    event.text = generating ? "generating" : "idle";
    post_event(std::move(event));
}

bool tui_renderer::wait_for_command(tui_command & command, int timeout_ms) {
    return commands_.wait_pop(command, timeout_ms);
}

void tui_renderer::shutdown() {
    bool expected = false;
    if (!shutdown_started_.compare_exchange_strong(expected, true)) {
        return;
    }

    running_.store(false);
    restore_raw_mode();

    tui_event shutdown_event;
    shutdown_event.type = tui_event_type::SHUTDOWN;
    events_.push(std::move(shutdown_event));
    events_.close();
    input_events_.close();
    commands_.close();

    if (input_thread_.joinable()) {
        input_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(autocomplete_threads_mu_);
        for (auto & th : autocomplete_threads_) {
            if (th.joinable()) {
                th.join();
            }
        }
        autocomplete_threads_.clear();
    }
    if (render_thread_.joinable()) {
        render_thread_.join();
    }

    restore_resize_handler();
    console::set_tui_active(false);
}

void tui_renderer::setup_raw_mode() {
#if !defined(_WIN32)
    if (tcgetattr(STDIN_FILENO, &saved_termios_) == 0) {
        saved_termios_valid_ = true;
        termios raw = saved_termios_;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
#ifdef VLNEXT
        raw.c_cc[VLNEXT] = _POSIX_VDISABLE;
#endif
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
#endif
}

void tui_renderer::restore_raw_mode() {
#if !defined(_WIN32)
    if (saved_termios_valid_) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios_);
        saved_termios_valid_ = false;
    }
#endif
}

void tui_renderer::install_resize_handler() {
#if !defined(_WIN32)
    struct sigaction act;
    act.sa_handler = tui_sigwinch_handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    if (sigaction(SIGWINCH, &act, &previous_sigwinch_) == 0) {
        g_previous_sigwinch = previous_sigwinch_;
        sigwinch_installed_ = true;
    }
#endif
}

void tui_renderer::restore_resize_handler() {
#if !defined(_WIN32)
    if (sigwinch_installed_) {
        sigaction(SIGWINCH, &previous_sigwinch_, nullptr);
        sigwinch_installed_ = false;
    }
#endif
}

void tui_renderer::query_terminal_size() {
#if defined(_WIN32)
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(out, &csbi)) {
        term_cols_ = csbi.dwSize.X;
        term_rows_ = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0) {
            term_cols_ = ws.ws_col;
        }
        if (ws.ws_row > 0) {
            term_rows_ = ws.ws_row;
        }
    }
#endif
    term_cols_ = std::max(term_cols_, 20);
    term_rows_ = std::max(term_rows_, 6);
}

void tui_renderer::detect_synchronized_output() {
    sync_output_ = false;
#if !defined(_WIN32)
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return;
    }
    fprintf(out_, "\x1b[?2026$p");
    fflush(out_);

    pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    int rc = poll(&pfd, 1, 30);
    if (rc <= 0 || !(pfd.revents & POLLIN)) {
        return;
    }
    char buf[64];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) {
        return;
    }
    std::string response(buf, buf + n);
    sync_output_ = response.find("$y") != std::string::npos;
#endif
}

std::string tui_renderer::sgr(tui_transcript_style style) const {
    if (!color_) {
        return "";
    }
    switch (style) {
        case tui_transcript_style::INFO:
            return ANSI_MAGENTA;
        case tui_transcript_style::ERROR:
            return std::string(ANSI_BOLD) + ANSI_RED;
        case tui_transcript_style::REASONING:
            return ANSI_GRAY;
        case tui_transcript_style::TOOL_STREAM:
            return ANSI_CYAN;
        case tui_transcript_style::USER_INPUT:
            return std::string(ANSI_BOLD) + ANSI_YELLOW;
        case tui_transcript_style::NORMAL:
            return "";
    }
    return "";
}

std::string tui_renderer::reset_sgr() const {
    return color_ ? ANSI_RESET : "";
}

static bool has_ctrl_modifier(const std::string & params) {
    size_t start = 0;
    while (start < params.size()) {
        size_t end = params.find(';', start);
        std::string part = params.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty()) {
            int value = 0;
            bool numeric = true;
            for (char ch : part) {
                if (!std::isdigit(static_cast<unsigned char>(ch))) {
                    numeric = false;
                    break;
                }
                value = value * 10 + (ch - '0');
            }
            if (numeric && value == 5) {
                return true;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

static std::string extract_partial_json_string(
    const std::string & json_fragment,
    const std::string & key,
    bool & complete) {
    complete = false;
    std::string needle = "\"" + key + "\"";
    auto kpos = json_fragment.find(needle);
    if (kpos == std::string::npos) {
        return "";
    }
    size_t pos = kpos + needle.size();
    while (pos < json_fragment.size() && json_fragment[pos] == ' ') {
        pos++;
    }
    if (pos >= json_fragment.size() || json_fragment[pos] != ':') {
        return "";
    }
    pos++;
    while (pos < json_fragment.size() && json_fragment[pos] == ' ') {
        pos++;
    }
    if (pos >= json_fragment.size() || json_fragment[pos] != '"') {
        return "";
    }
    pos++;

    std::string result;
    for (size_t i = pos; i < json_fragment.size();) {
        char c = json_fragment[i];
        if (c == '"') {
            complete = true;
            break;
        }
        if (c == '\\' && i + 1 < json_fragment.size()) {
            char esc = json_fragment[i + 1];
            switch (esc) {
                case '"':  result += '"';  i += 2; break;
                case '\\': result += '\\'; i += 2; break;
                case '/':  result += '/';  i += 2; break;
                case 'n':  result += '\n'; i += 2; break;
                case 'r':  result += '\r'; i += 2; break;
                case 't':  result += '\t'; i += 2; break;
                case 'b':  result += '\b'; i += 2; break;
                case 'f':  result += '\f'; i += 2; break;
                case 'u': {
                    if (i + 5 < json_fragment.size()) {
                        char hex[5] = {json_fragment[i + 2], json_fragment[i + 3],
                                       json_fragment[i + 4], json_fragment[i + 5], 0};
                        unsigned cp = std::strtoul(hex, nullptr, 16);
                        if (cp < 0x80) {
                            result += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            result += static_cast<char>(0xC0 | (cp >> 6));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            result += static_cast<char>(0xE0 | (cp >> 12));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        i += 6;
                    } else {
                        return result;
                    }
                    break;
                }
                default:
                    result += esc;
                    i += 2;
                    break;
            }
        } else {
            result += c;
            i++;
        }
    }
    return result;
}

static bool decode_field_incremental(tui_renderer::tool_delta_state & tcs, const std::string & key) {
    if (tcs.content_scan_pos == 0) {
        std::string needle = "\"" + key + "\"";
        auto kpos = tcs.accumulated_args.find(needle);
        if (kpos == std::string::npos) {
            return false;
        }
        size_t pos = kpos + needle.size();
        while (pos < tcs.accumulated_args.size() && tcs.accumulated_args[pos] == ' ') {
            pos++;
        }
        if (pos >= tcs.accumulated_args.size() || tcs.accumulated_args[pos] != ':') {
            return false;
        }
        pos++;
        while (pos < tcs.accumulated_args.size() && tcs.accumulated_args[pos] == ' ') {
            pos++;
        }
        if (pos >= tcs.accumulated_args.size() || tcs.accumulated_args[pos] != '"') {
            return false;
        }
        pos++;
        tcs.content_scan_pos = pos;
        tcs.content_raw_end = pos;
    }

    const auto & s = tcs.accumulated_args;
    for (size_t i = tcs.content_raw_end; i < s.size();) {
        char c = s[i];
        if (c == '"') {
            tcs.content_raw_end = i + 1;
            return true;
        }
        if (c == '\\') {
            if (i + 1 >= s.size()) {
                tcs.content_raw_end = i;
                return false;
            }
            char esc = s[i + 1];
            switch (esc) {
                case '"':  tcs.content_buffer += '"';  i += 2; break;
                case '\\': tcs.content_buffer += '\\'; i += 2; break;
                case '/':  tcs.content_buffer += '/';  i += 2; break;
                case 'n':  tcs.content_buffer += '\n'; i += 2; break;
                case 'r':  tcs.content_buffer += '\r'; i += 2; break;
                case 't':  tcs.content_buffer += '\t'; i += 2; break;
                case 'b':  tcs.content_buffer += '\b'; i += 2; break;
                case 'f':  tcs.content_buffer += '\f'; i += 2; break;
                case 'u': {
                    if (i + 5 < s.size()) {
                        char hex[5] = {s[i + 2], s[i + 3], s[i + 4], s[i + 5], 0};
                        unsigned cp = std::strtoul(hex, nullptr, 16);
                        if (cp < 0x80) {
                            tcs.content_buffer += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            tcs.content_buffer += static_cast<char>(0xC0 | (cp >> 6));
                            tcs.content_buffer += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            tcs.content_buffer += static_cast<char>(0xE0 | (cp >> 12));
                            tcs.content_buffer += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            tcs.content_buffer += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        i += 6;
                    } else {
                        tcs.content_raw_end = i;
                        return false;
                    }
                    break;
                }
                default:
                    tcs.content_buffer += esc;
                    i += 2;
                    break;
            }
        } else {
            tcs.content_buffer += c;
            i++;
        }
        tcs.content_raw_end = i;
    }
    return false;
}

void tui_renderer::append_transcript_raw(const std::string & bytes) {
    if (bytes.empty()) {
        return;
    }
    pending_transcript_.push_back(bytes);
}

void tui_renderer::append_transcript_line(const std::string & text, tui_transcript_style style) {
    append_transcript_raw(sgr(style) + text + reset_sgr());
}

void tui_renderer::flush_text_buffer(bool force_newline) {
    if (transcript_buffer_.empty()) {
        return;
    }
    std::string line = transcript_buffer_;
    transcript_buffer_.clear();
    if (force_newline && !line.empty() && line.back() != '\n') {
        line.push_back('\n');
    }
    if (buffer_style_ == tui_transcript_style::NORMAL) {
        append_transcript_raw(line);
    } else {
        append_transcript_raw(sgr(buffer_style_) + line + reset_sgr());
    }
    buffer_style_ = tui_transcript_style::NORMAL;
}

void tui_renderer::handle_tui_event(const tui_event & event) {
    switch (event.type) {
        case tui_event_type::TEXT_DELTA:
            if (buffer_style_ != tui_transcript_style::NORMAL) {
                flush_text_buffer(true);
            }
            buffer_style_ = tui_transcript_style::NORMAL;
            transcript_buffer_ += event.text;
            break;
        case tui_event_type::REASONING_DELTA:
            if (buffer_style_ != tui_transcript_style::REASONING) {
                flush_text_buffer(true);
                // Emit a subtle dim label so the reasoning block is visually
                // separated from the answer (M1).
                append_transcript_line("thinking:\n", tui_transcript_style::REASONING);
            }
            buffer_style_ = tui_transcript_style::REASONING;
            transcript_buffer_ += event.text;
            break;
        case tui_event_type::TOOL_CALL_DELTA:
            flush_text_buffer(true);
            handle_tool_call_delta(event);
            break;
        case tui_event_type::TOOL_START:
            flush_text_buffer(true);
            render_tool_start(event);
            break;
        case tui_event_type::TOOL_RESULT:
            flush_text_buffer(true);
            render_tool_result(event);
            break;
        case tui_event_type::COMPACTION_COMPLETED:
            flush_text_buffer(true);
            append_transcript_line(
                "[Context compacted; kept " + std::to_string(event.messages_kept) + " messages]\n",
                tui_transcript_style::INFO);
            footer_.transient_label = "compacting...";
            transient_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            managed_dirty_ = true;
            break;
        case tui_event_type::COMPLETED:
            flush_text_buffer(true);
            footer_.stats = event.stats;
            footer_.last_prompt_tokens = event.last_prompt_tokens;
            footer_.generating = false;
            managed_dirty_ = true;
            break;
        case tui_event_type::ERROR:
            flush_text_buffer(true);
            append_transcript_line("Error: " + event.text + "\n", tui_transcript_style::ERROR);
            footer_.generating = false;
            managed_dirty_ = true;
            break;
        case tui_event_type::PERMISSION_REQUIRED:
            flush_text_buffer(true);
            open_permission_overlay(event);
            break;
        case tui_event_type::PERMISSION_RESOLVED:
            if (active_permission_id_ == event.perm_id) {
                dismiss_overlay();
            }
            break;
        case tui_event_type::ITERATION_START:
            footer_.generating = true;
            managed_dirty_ = true;
            break;
        case tui_event_type::AUTOCOMPLETE_RESULTS:
            if (event.autocomplete_generation == file_completion_generation_ &&
                event.autocomplete_query == last_file_query_) {
                select_list_.set_items(event.autocomplete_items);
                overlay_ = select_list_.empty() ? overlay_kind::NONE : overlay_kind::FILE;
                managed_dirty_ = true;
            }
            break;
        case tui_event_type::TRANSCRIPT:
            flush_text_buffer(false);
            append_transcript_line(event.text, event.transcript_style);
            break;
        case tui_event_type::STATS:
            if (event.text == "generating") {
                footer_.generating = true;
            } else if (event.text == "idle") {
                footer_.generating = false;
            } else {
                footer_.stats = event.stats;
                footer_.last_prompt_tokens = event.last_prompt_tokens;
            }
            managed_dirty_ = true;
            break;
        case tui_event_type::RESIZE:
            query_terminal_size();
            needs_full_repaint_ = true;
            force_full_redraw_ = true;
            managed_dirty_ = true;
            break;
        case tui_event_type::SHUTDOWN:
            running_.store(false);
            break;
    }
}

void tui_renderer::handle_tool_call_delta(const tui_event & event) {
    if (event.tool_call_index == std::string::npos) {
        return;
    }
    auto & tcs = tool_delta_states_[event.tool_call_index];
    if (!event.tool_name.empty()) {
        tcs.name = event.tool_name;
    }
    tcs.accumulated_args += event.tool_args;

    if (!tcs.header_printed && !tcs.name.empty()) {
        append_transcript_line("\n> " + tcs.name, tui_transcript_style::INFO);
        tcs.header_printed = true;
    }

    if (tcs.header_printed && tcs.displayed_path.empty()) {
        bool path_complete = false;
        std::string fp = extract_partial_json_string(tcs.accumulated_args, "file_path", path_complete);
        if (path_complete && !fp.empty()) {
            append_transcript_line(" " + fp, tui_transcript_style::INFO);
            tcs.displayed_path = fp;
        }
    }

    const char * content_field = nullptr;
    if (tcs.name == "write") {
        content_field = "content";
    } else if (tcs.name == "edit") {
        content_field = "new_string";
    }
    if (!content_field || !tcs.header_printed || tcs.content_complete) {
        return;
    }

    bool complete = decode_field_incremental(tcs, content_field);
    if (complete) {
        tcs.content_complete = true;
    }

    const std::string & buf = tcs.content_buffer;
    size_t pos = tcs.displayed_bytes;
    while (pos < buf.size()) {
        size_t nl = buf.find('\n', pos);
        if (nl != std::string::npos) {
            append_transcript_line("\n  " + buf.substr(pos, nl - pos), tui_transcript_style::TOOL_STREAM);
            pos = nl + 1;
            tcs.displayed_bytes = pos;
        } else if (complete) {
            append_transcript_line("\n  " + buf.substr(pos), tui_transcript_style::TOOL_STREAM);
            tcs.displayed_bytes = buf.size();
            break;
        } else {
            break;
        }
    }
}

void tui_renderer::render_tool_start(const tui_event & event) {
    std::string suffix;
    try {
        json args = json::parse(event.tool_args);
        if (event.tool_name == "bash") {
            suffix = " " + truncate_display(args.value("command", ""), 100);
        } else if (event.tool_name == "read" || event.tool_name == "write" || event.tool_name == "edit") {
            std::string path = args.value("path", args.value("file_path", ""));
            if (!path.empty()) {
                suffix = " " + path;
            }
        }
    } catch (...) {
    }
    append_transcript_line("\n> " + event.tool_name + suffix + "\n", tui_transcript_style::INFO);
}

void tui_renderer::render_tool_result(const tui_event & event) {
    std::string display = truncate_display(event.tool_output, 500);
    if (!display.empty()) {
        if (display.back() != '\n') {
            display.push_back('\n');
        }
        append_transcript_line(display, event.tool_success ? tui_transcript_style::NORMAL : tui_transcript_style::ERROR);
    }
    std::ostringstream ss;
    if (event.elapsed_ms < 1000) {
        ss << "`- " << event.elapsed_ms << "ms\n";
    } else {
        ss.setf(std::ios::fixed);
        ss.precision(1);
        ss << "`- " << (event.elapsed_ms / 1000.0) << "s\n";
    }
    append_transcript_line(ss.str(), tui_transcript_style::INFO);
}

static bool is_ident_char(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-' || ch == '.' || ch == '/';
}

void tui_renderer::dismiss_overlay() {
    overlay_ = overlay_kind::NONE;
    select_list_.clear();
    active_permission_id_.clear();
    completion_prefix_.clear();
    // Invalidate any in-flight file-completion workers so that results arriving
    // after dismissal (H7, L4) are silently dropped by the AUTOCOMPLETE_RESULTS
    // handler (which checks generation == file_completion_generation_).
    last_file_query_.clear();
    ++file_completion_generation_;
    managed_dirty_ = true;
}

void tui_renderer::complete_selected() {
    const std::string * selected = select_list_.selected();
    if (!selected) {
        return;
    }
    std::string replacement = *selected;
    if (overlay_ == overlay_kind::FILE) {
        replacement = "@" + replacement + " ";
    }
    editor_.complete_range(completion_start_, completion_end_, replacement);
    dismiss_overlay();
    managed_dirty_ = true;
}

void tui_renderer::handle_input_event(const tui_input_event & event) {
    if (overlay_ == overlay_kind::PERMISSION) {
        if (event.key == tui_input_key::ESCAPE) {
            answer_permission('n');
        } else if (event.key == tui_input_key::CHARACTER) {
            char ch = static_cast<char>(event.codepoint);
            if (ch == 'y' || ch == 'Y' || ch == 'n' || ch == 'N' ||
                ch == 'a' || ch == 'A' || ch == 'd' || ch == 'D') {
                answer_permission(ch);
            }
        }
        return;
    }

    if (select_list_.visible()) {
        if (event.key == tui_input_key::UP) {
            select_list_.move_up();
            managed_dirty_ = true;
            return;
        }
        if (event.key == tui_input_key::DOWN) {
            select_list_.move_down();
            managed_dirty_ = true;
            return;
        }
        if (event.key == tui_input_key::TAB) {
            complete_selected();
            return;
        }
        if (event.key == tui_input_key::ENTER) {
            // Slash commands: Enter should run the command, not merely fill it in.
            // File completions: Enter only inserts the path so the user keeps typing.
            bool was_slash = (overlay_ == overlay_kind::SLASH);
            complete_selected();
            if (was_slash) {
                tui_editor_action action = editor_.handle_event(event);
                if (action.submitted) {
                    tui_command cmd;
                    cmd.text = std::move(action.submission);
                    commands_.push(std::move(cmd));
                }
                managed_dirty_ = true;
            }
            return;
        }
        if (event.key == tui_input_key::ESCAPE) {
            dismiss_overlay();
            return;
        }
    }

    if (event.key == tui_input_key::ESCAPE && footer_.generating) {
        if (cfg_.interrupt) {
            cfg_.interrupt();
        }
        footer_.transient_label = "aborting";
        transient_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        managed_dirty_ = true;
        return;
    }

    if (event.key == tui_input_key::TAB) {
        auto candidates = console::complete(editor_.buffer(), editor_.cursor_byte_pos());
        if (!candidates.empty()) {
            editor_.set_buffer(candidates[0].first, candidates[0].second);
            dismiss_overlay();
            managed_dirty_ = true;
            return;
        }
    }

    if (event.key == tui_input_key::CTRL_V) {
        std::string marker;
        if (console::try_paste_image_marker(marker)) {
            for (char ch : marker) {
                tui_input_event ev;
                ev.key = tui_input_key::CHARACTER;
                ev.codepoint = static_cast<unsigned char>(ch);
                editor_.handle_event(ev);
            }
            managed_dirty_ = true;
            update_autocomplete();
        }
        return;
    }

    tui_editor_action action = editor_.handle_event(event);
    if (action.eof) {
        tui_command cmd;
        cmd.eof = true;
        commands_.push(std::move(cmd));
        return;
    }
    if (action.submitted) {
        tui_command cmd;
        cmd.text = std::move(action.submission);
        commands_.push(std::move(cmd));
        dismiss_overlay();
    }
    if (action.changed) {
        managed_dirty_ = true;
        update_autocomplete();
    }
}

void tui_renderer::update_autocomplete() {
    if (overlay_ == overlay_kind::PERMISSION) {
        return;
    }

    const std::string cur = editor_.buffer();
    const size_t cursor = editor_.cursor_byte_pos();
    select_list_.clear();
    overlay_ = overlay_kind::NONE;

    if (cursor <= cur.size() && cursor > 0 && cur[0] == '/') {
        size_t token_end = cur.find_first_of(" \t\n");
        if (token_end == std::string::npos || cursor <= token_end) {
            std::string prefix = cur.substr(0, cursor);
            static const char * COMMANDS[] = {
                "/exit", "/quit", "/clear", "/compact",
                "/tools", "/stats", "/skills", "/agents", "/resume",
            };
            std::vector<std::string> items;
            for (const char * cmd : COMMANDS) {
                std::string s(cmd);
                if (s.rfind(prefix, 0) == 0) {
                    items.push_back(s);
                }
            }
            if (!items.empty()) {
                completion_start_ = 0;
                completion_end_ = cursor;
                select_list_.set_items(std::move(items));
                overlay_ = overlay_kind::SLASH;
            }
        }
        managed_dirty_ = true;
        return;
    }

    size_t at = cur.rfind('@', cursor == 0 ? 0 : cursor - 1);
    if (at == std::string::npos || at > cursor) {
        managed_dirty_ = true;
        return;
    }
    for (size_t i = at + 1; i < cursor; ++i) {
        if (!is_ident_char(cur[i])) {
            managed_dirty_ = true;
            return;
        }
    }

    std::string query = cur.substr(at + 1, cursor - at - 1);
    completion_start_ = at;
    completion_end_ = cursor;
    completion_prefix_ = query;
    // Only skip the worker when the overlay is already showing results for this
    // exact query.  Without the overlay_ check, a bare "@" (empty query) was
    // silently skipped because both query and last_file_query_ start as "".
    // (M3 fix)
    if (query == last_file_query_ && overlay_ == overlay_kind::FILE) {
        managed_dirty_ = true;
        return;
    }
    last_file_query_ = query;
    uint64_t generation = ++file_completion_generation_;
    start_file_completion_worker(query, generation);
    managed_dirty_ = true;
}

void tui_renderer::start_file_completion_worker(const std::string & query, uint64_t generation) {
    std::string cwd = footer_.working_dir.empty() ? "." : footer_.working_dir;
    tui_event refresh;
    refresh.type = tui_event_type::AUTOCOMPLETE_RESULTS;
    events_.push(std::move(refresh));
    std::thread worker([this, cwd, query, generation]() {
        std::vector<std::string> items;
        try {
            fs::path root(cwd);
            fs::path base = root;
            std::string leaf = query;
            fs::path q(query);
            if (query.find('/') != std::string::npos) {
                base = root / q.parent_path();
                leaf = q.filename().string();
            }
            if (fs::exists(base) && fs::is_directory(base)) {
                std::vector<std::string> prefix_matches;
                std::vector<std::string> substring_matches;
                for (const auto & entry : fs::directory_iterator(base)) {
                    std::string name = entry.path().filename().string();
                    fs::path rel = fs::relative(entry.path(), root);
                    std::string display = rel.generic_string();
                    if (entry.is_directory()) {
                        display += "/";
                    }
                    if (leaf.empty() || name.rfind(leaf, 0) == 0) {
                        prefix_matches.push_back(display);
                    } else if (name.find(leaf) != std::string::npos) {
                        substring_matches.push_back(display);
                    }
                }
                std::sort(prefix_matches.begin(), prefix_matches.end());
                std::sort(substring_matches.begin(), substring_matches.end());
                items.reserve(std::min<size_t>(50, prefix_matches.size() + substring_matches.size()));
                for (const auto & item : prefix_matches) {
                    if (items.size() >= 50) {
                        break;
                    }
                    items.push_back(item);
                }
                for (const auto & item : substring_matches) {
                    if (items.size() >= 50) {
                        break;
                    }
                    items.push_back(item);
                }
            }
        } catch (...) {
        }
        tui_event ev;
        ev.type = tui_event_type::AUTOCOMPLETE_RESULTS;
        ev.autocomplete_items = std::move(items);
        ev.autocomplete_generation = generation;
        ev.autocomplete_query = query;
        events_.push(std::move(ev));
    });
    std::lock_guard<std::mutex> lock(autocomplete_threads_mu_);
    autocomplete_threads_.push_back(std::move(worker));
}

void tui_renderer::open_permission_overlay(const tui_event & event) {
    active_permission_ = event.perm;
    active_permission_id_ = event.perm_id;
    overlay_ = overlay_kind::PERMISSION;
    select_list_.clear();
    managed_dirty_ = true;
}

void tui_renderer::answer_permission(char ch) {
    if (active_permission_id_.empty() || !cfg_.permissions) {
        dismiss_overlay();
        return;
    }
    bool allowed = (ch == 'y' || ch == 'Y' || ch == 'a' || ch == 'A');
    permission_scope scope = (ch == 'a' || ch == 'A' || ch == 'd' || ch == 'D')
        ? permission_scope::SESSION
        : permission_scope::ONCE;
    cfg_.permissions->respond(active_permission_id_, allowed, scope);
    append_transcript_line(
        std::string("[Permission ") + (allowed ? "granted" : "denied") + " for " +
        active_permission_.tool_name + "]\n",
        allowed ? tui_transcript_style::INFO : tui_transcript_style::ERROR);
    dismiss_overlay();
}

std::vector<std::string> tui_renderer::current_region_lines(int & cursor_row, int & cursor_col) {
    int max_editor_lines = std::max(1, term_rows_ - 4);
    tui_editor_render editor_render = editor_.render(term_cols_, max_editor_lines);

    std::vector<std::string> lines;
    if (overlay_ == overlay_kind::PERMISSION) {
        int box_width = std::max(20, term_cols_);
        auto box_line = [&](const std::string & text) {
            std::string trimmed = truncate_display(text, box_width > 4 ? box_width - 4 : 16);
            int pad = std::max(0, box_width - 4 - tui_string_width(trimmed));
            return "| " + trimmed + std::string(pad, ' ') + " |";
        };
        lines.push_back("+" + std::string(std::max(0, box_width - 2), '-') + "+");
        std::string title = "Permission: " + active_permission_.tool_name;
        if (active_permission_.is_dangerous) {
            title += " (dangerous)";
        }
        lines.push_back(box_line(title));
        if (!active_permission_.description.empty()) {
            lines.push_back(box_line(active_permission_.description));
        }
        if (!active_permission_.details.empty()) {
            std::string details = active_permission_.details;
            size_t pos = 0;
            while (pos < details.size() && lines.size() < 6) {
                size_t nl = details.find('\n', pos);
                std::string part = details.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                lines.push_back(box_line(part));
                if (nl == std::string::npos) {
                    break;
                }
                pos = nl + 1;
            }
        }
        lines.push_back(box_line("[y]es  [n]o  [a]lways  [d]eny always"));
        lines.push_back("+" + std::string(std::max(0, box_width - 2), '-') + "+");
        cursor_row = static_cast<int>(lines.size()) - 1;
        cursor_col = 0;
    } else {
        if (select_list_.visible()) {
            std::vector<std::string> overlay = select_list_.render(term_cols_, 6);
            lines.insert(lines.end(), overlay.begin(), overlay.end());
        }
        int editor_start = static_cast<int>(lines.size());
        lines.insert(lines.end(), editor_render.lines.begin(), editor_render.lines.end());
        cursor_row = editor_start + editor_render.cursor_row;
        cursor_col = editor_render.cursor_col;
    }

    std::vector<std::string> footer = tui_render_footer(footer_, term_cols_, color_);
    lines.insert(lines.end(), footer.begin(), footer.end());
    if (lines.empty()) {
        lines.push_back("");
    }
    return lines;
}

void tui_renderer::flush_pending_transcript() {
    if (pending_transcript_.empty()) {
        return;
    }

    if (managed_visible_ && !previous_region_.empty()) {
        // Jump to the top of the on-screen managed region and erase it (and everything
        // below it). Without this, leftover footer/editor cells to the right of, or
        // below, the new transcript lines get committed into the scrollback as the
        // screen scrolls up (the "footer bleeds into the transcript" bug).
        int height = static_cast<int>(previous_region_.size());
        int top = std::max(1, term_rows_ - height + 1);
        fprintf(out_, "\x1b[%d;1H\x1b[0J", top);
    } else {
        fputs("\r\x1b[0J", out_);
    }

    for (const auto & bytes : pending_transcript_) {
        fwrite(bytes.data(), 1, bytes.size(), out_);
        bool needs_nl = bytes.empty() || bytes.back() != '\n';
        if (needs_nl) {
            fwrite("\n", 1, 1, out_);
        }
        // Remember committed transcript so we can repaint it from scratch on resize.
        transcript_history_.push_back(needs_nl ? bytes + "\n" : bytes);
    }
    if (transcript_history_.size() > kMaxTranscriptHistory) {
        transcript_history_.erase(
            transcript_history_.begin(),
            transcript_history_.begin() + (transcript_history_.size() - kMaxTranscriptHistory));
    }
    pending_transcript_.clear();
    previous_region_.clear();
    managed_visible_ = false;
    force_full_redraw_ = true;
}

void tui_renderer::repaint_screen() {
    // On resize we cannot know how the terminal reflowed our previously-emitted lines
    // (a real, non-tmux terminal leaves stale region rows behind, producing a trail of
    // prompts). Clear the screen + scrollback and reprint the stored transcript from
    // scratch, then let the region redraw at the bottom. This guarantees a clean layout
    // regardless of the emulator's resize behaviour.
    needs_full_repaint_ = false;
    if (sync_output_) {
        fputs("\x1b[?2026h", out_);
    }
    fputs("\x1b[H\x1b[2J\x1b[3J", out_);
    for (const auto & line : transcript_history_) {
        fwrite(line.data(), 1, line.size(), out_);
    }
    if (sync_output_) {
        fputs("\x1b[?2026l", out_);
    }
    pending_transcript_.clear();
    previous_region_.clear();
    managed_visible_ = false;
    force_full_redraw_ = true;
    last_drawn_top_ = 0;
}

void tui_renderer::redraw_managed_region(bool full_redraw) {
    int cursor_row = 0;
    int cursor_col = 0;
    std::vector<std::string> region = current_region_lines(cursor_row, cursor_col);

    int height = static_cast<int>(region.size());
    int top = std::max(1, term_rows_ - height + 1);

    if (sync_output_) {
        fputs("\x1b[?2026h", out_);
    }

    if (!managed_visible_) {
        // The region is not currently on screen (first draw, or just after a transcript
        // flush). Reserve `height` rows at the bottom by scrolling the history up, then
        // draw the region pinned to the bottom. Scrolling first guarantees we never
        // overwrite the transcript lines we just emitted.
        for (int i = 0; i < height; ++i) {
            fputs("\n", out_);
        }
        fprintf(out_, "\x1b[%d;1H", top);
        for (int i = 0; i < height; ++i) {
            fputs("\r\x1b[2K", out_);
            fwrite(region[i].data(), 1, region[i].size(), out_);
            if (i + 1 < height) {
                fputs("\n", out_);
            }
        }
    } else if (full_redraw || force_full_redraw_ ||
               previous_region_.size() != region.size()) {
        // The region height may have changed (autocomplete dropdown opening/shrinking/
        // dismissing) or the terminal may have been resized. Erase from the topmost row
        // the region previously occupied down to the bottom. We use last_drawn_top_ (the
        // ACTUAL absolute row the region was last drawn at) rather than recomputing from
        // term_rows_: after a resize term_rows_ has already changed, so recomputing would
        // clear the wrong rows and leave a stale copy of the region on screen (the
        // "resize breaks the layout" bug, most visible when growing the terminal).
        int prev_height = static_cast<int>(previous_region_.size());
        int prev_top = last_drawn_top_ > 0
            ? std::min(std::max(1, last_drawn_top_), term_rows_)
            : std::max(1, term_rows_ - prev_height + 1);
        int clear_top = std::min(prev_top, top);
        fprintf(out_, "\x1b[%d;1H\x1b[0J", clear_top);
        fprintf(out_, "\x1b[%d;1H", top);
        for (int i = 0; i < height; ++i) {
            fputs("\r\x1b[2K", out_);
            fwrite(region[i].data(), 1, region[i].size(), out_);
            if (i + 1 < height) {
                fputs("\n", out_);
            }
        }
    } else {
        // In-place diff update using absolute row addressing. The previous version
        // moved the cursor relatively, which drifted whenever an overlay changed the
        // region height and left stale/duplicated footer lines on screen.
        for (int i = 0; i < height; ++i) {
            if (region[i] != previous_region_[i]) {
                fprintf(out_, "\x1b[%d;1H\x1b[2K", top + i);
                fwrite(region[i].data(), 1, region[i].size(), out_);
            }
        }
    }

    int cursor_abs_row = std::min(term_rows_, top + std::max(0, cursor_row));
    int cursor_abs_col = std::min(term_cols_, std::max(1, cursor_col + 1));
    fprintf(out_, "\x1b[%d;%dH", cursor_abs_row, cursor_abs_col);

    if (sync_output_) {
        fputs("\x1b[?2026l", out_);
    }
    fflush(out_);

    previous_region_ = std::move(region);
    previous_cursor_row_ = cursor_row;
    last_drawn_top_ = top;
    managed_visible_ = true;
    force_full_redraw_ = false;
    managed_dirty_ = false;
}

void tui_renderer::clear_managed_region() {
    if (!managed_visible_ || previous_region_.empty()) {
        return;
    }
    int height = static_cast<int>(previous_region_.size());
    int top = std::max(1, term_rows_ - height + 1);
    fprintf(out_, "\x1b[%d;1H", top);
    for (int i = 0; i < height; ++i) {
        fputs("\r\x1b[2K", out_);
        if (i + 1 < height) {
            fputs("\n", out_);
        }
    }
    fputs("\r", out_);
    fflush(out_);
    previous_region_.clear();
    managed_visible_ = false;
}

void tui_renderer::render_loop() {
    redraw_managed_region(true);

    while (running_.load() || !events_.closed()) {
#if !defined(_WIN32)
        if (g_tui_sigwinch.exchange(false)) {
            tui_event ev;
            ev.type = tui_event_type::RESIZE;
            events_.push(std::move(ev));
        }
#endif

        tui_event event;
        while (events_.try_pop(event)) {
            handle_tui_event(event);
        }

        tui_input_event input;
        while (input_events_.try_pop(input)) {
            handle_input_event(input);
        }

        auto now = std::chrono::steady_clock::now();
        if (footer_.generating && now - last_spinner_tick_ >= std::chrono::milliseconds(100)) {
            footer_.spinner_frame = (footer_.spinner_frame + 1) % 4;
            last_spinner_tick_ = now;
            managed_dirty_ = true;
        }
        if (!footer_.generating && now - last_idle_tick_ >= std::chrono::milliseconds(500)) {
            last_idle_tick_ = now;
            managed_dirty_ = true;
        }
        if (!footer_.transient_label.empty() && transient_until_.time_since_epoch().count() > 0 &&
            now >= transient_until_) {
            footer_.transient_label.clear();
            managed_dirty_ = true;
        }

        if (needs_full_repaint_) {
            repaint_screen();
        }
        flush_pending_transcript();
        if (managed_dirty_ || force_full_redraw_) {
            redraw_managed_region(force_full_redraw_);
        }

        tui_event waited;
        if (events_.wait_pop(waited, 8)) {
            handle_tui_event(waited);
        }
    }

    flush_text_buffer(true);
    flush_pending_transcript();
    clear_managed_region();
    fputs("\x1b[?2026l\x1b[?2004l\x1b[?45l\x1b[?25h", out_);
    fflush(out_);
}

#if defined(_WIN32)
void tui_renderer::input_loop() {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    while (running_.load()) {
        DWORD wait = WaitForSingleObject(in, 20);
        if (wait != WAIT_OBJECT_0) {
            continue;
        }
        INPUT_RECORD rec;
        DWORD count = 0;
        if (!ReadConsoleInputW(in, &rec, 1, &count) || count == 0) {
            continue;
        }
        if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            input_events_.push({tui_input_key::RESIZE, 0});
            continue;
        }
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) {
            continue;
        }
        const auto & key = rec.Event.KeyEvent;
        const DWORD ctrl_mask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
        bool ctrl = (key.dwControlKeyState & ctrl_mask) != 0;
        switch (key.wVirtualKeyCode) {
            case VK_RETURN:
                input_events_.push({(key.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) ?
                    tui_input_key::ALT_ENTER : tui_input_key::ENTER, 0});
                break;
            case VK_TAB:
                input_events_.push({tui_input_key::TAB, 0});
                break;
            case VK_ESCAPE:
                input_events_.push({tui_input_key::ESCAPE, 0});
                break;
            case VK_BACK:
                input_events_.push({tui_input_key::BACKSPACE, 0});
                break;
            case VK_DELETE:
                input_events_.push({tui_input_key::DELETE_KEY, 0});
                break;
            case VK_LEFT:
                input_events_.push({ctrl ? tui_input_key::CTRL_LEFT : tui_input_key::LEFT, 0});
                break;
            case VK_RIGHT:
                input_events_.push({ctrl ? tui_input_key::CTRL_RIGHT : tui_input_key::RIGHT, 0});
                break;
            case VK_UP:
                input_events_.push({tui_input_key::UP, 0});
                break;
            case VK_DOWN:
                input_events_.push({tui_input_key::DOWN, 0});
                break;
            case VK_HOME:
                input_events_.push({tui_input_key::HOME, 0});
                break;
            case VK_END:
                input_events_.push({tui_input_key::END, 0});
                break;
            default:
                if (ctrl && (key.uChar.UnicodeChar == 'A' || key.uChar.UnicodeChar == 'a')) {
                    input_events_.push({tui_input_key::CTRL_A, 0});
                } else if (ctrl && (key.uChar.UnicodeChar == 'E' || key.uChar.UnicodeChar == 'e')) {
                    input_events_.push({tui_input_key::CTRL_E, 0});
                } else if (ctrl && (key.uChar.UnicodeChar == 'D' || key.uChar.UnicodeChar == 'd')) {
                    input_events_.push({tui_input_key::CTRL_D, 0});
                } else if (ctrl && (key.uChar.UnicodeChar == 'V' || key.uChar.UnicodeChar == 'v')) {
                    input_events_.push({tui_input_key::CTRL_V, 0});
                } else if (key.uChar.UnicodeChar != 0) {
                    input_events_.push({tui_input_key::CHARACTER, static_cast<char32_t>(key.uChar.UnicodeChar)});
                }
                break;
        }
    }
}
#else
static bool read_byte_with_timeout(unsigned char & out, int timeout_ms) {
    pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    int rc = poll(&pfd, 1, timeout_ms);
    if (rc <= 0 || !(pfd.revents & POLLIN)) {
        return false;
    }
    return read(STDIN_FILENO, &out, 1) == 1;
}

static void push_utf8_input(tui_event_queue<tui_input_event> & queue, unsigned char first) {
    std::string bytes;
    bytes.push_back(static_cast<char>(first));
    int expected = 1;
    if ((first & 0xE0u) == 0xC0u) {
        expected = 2;
    } else if ((first & 0xF0u) == 0xE0u) {
        expected = 3;
    } else if ((first & 0xF8u) == 0xF0u) {
        expected = 4;
    }
    for (int i = 1; i < expected; ++i) {
        unsigned char next = 0;
        if (!read_byte_with_timeout(next, 8)) {
            break;
        }
        bytes.push_back(static_cast<char>(next));
    }
    size_t advance = 0;
    char32_t cp = tui_decode_utf8(bytes, 0, advance);
    queue.push({tui_input_key::CHARACTER, cp});
}

static void parse_csi_sequence(tui_event_queue<tui_input_event> & queue) {
    std::string params;
    unsigned char ch = 0;
    while (read_byte_with_timeout(ch, 8)) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '~') {
            break;
        }
        params.push_back(static_cast<char>(ch));
    }
    const bool ctrl = has_ctrl_modifier(params);
    if (ch == 'A') {
        queue.push({tui_input_key::UP, 0});
    } else if (ch == 'B') {
        queue.push({tui_input_key::DOWN, 0});
    } else if (ch == 'C') {
        queue.push({ctrl ? tui_input_key::CTRL_RIGHT : tui_input_key::RIGHT, 0});
    } else if (ch == 'D') {
        queue.push({ctrl ? tui_input_key::CTRL_LEFT : tui_input_key::LEFT, 0});
    } else if (ch == 'H') {
        queue.push({tui_input_key::HOME, 0});
    } else if (ch == 'F') {
        queue.push({tui_input_key::END, 0});
    } else if (ch == '~') {
        std::string digits;
        for (char p : params) {
            if (p == ';') {
                break;
            }
            if (std::isdigit(static_cast<unsigned char>(p))) {
                digits.push_back(p);
            }
        }
        if (digits == "1" || digits == "7") {
            queue.push({tui_input_key::HOME, 0});
        } else if (digits == "4" || digits == "8") {
            queue.push({tui_input_key::END, 0});
        } else if (digits == "3") {
            queue.push({tui_input_key::DELETE_KEY, 0});
        } else if (digits == "200") {
            queue.push({tui_input_key::PASTE_START, 0});
        } else if (digits == "201") {
            queue.push({tui_input_key::PASTE_END, 0});
        }
    }
}

void tui_renderer::input_loop() {
    while (running_.load()) {
        unsigned char ch = 0;
        if (!read_byte_with_timeout(ch, 20)) {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            input_events_.push({tui_input_key::ENTER, 0});
        } else if (ch == '\t') {
            input_events_.push({tui_input_key::TAB, 0});
        } else if (ch == 0x7F || ch == 0x08) {
            input_events_.push({tui_input_key::BACKSPACE, 0});
        } else if (ch == 0x01) {
            input_events_.push({tui_input_key::CTRL_A, 0});
        } else if (ch == 0x05) {
            input_events_.push({tui_input_key::CTRL_E, 0});
        } else if (ch == 0x04) {
            input_events_.push({tui_input_key::CTRL_D, 0});
        } else if (ch == 0x16) {
            input_events_.push({tui_input_key::CTRL_V, 0});
        } else if (ch == 0x1B) {
            unsigned char next = 0;
            if (!read_byte_with_timeout(next, 8)) {
                input_events_.push({tui_input_key::ESCAPE, 0});
            } else if (next == '[') {
                parse_csi_sequence(input_events_);
            } else if (next == '\r' || next == '\n') {
                input_events_.push({tui_input_key::ALT_ENTER, 0});
            } else {
                input_events_.push({tui_input_key::ESCAPE, 0});
            }
        } else {
            push_utf8_input(input_events_, ch);
        }
    }
}
#endif
