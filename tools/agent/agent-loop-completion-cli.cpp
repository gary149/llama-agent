#include "agent-loop.h"
#include "agent-loop-internal.h"
#include "console.h"
#include "log.h"

#include <chrono>
#include <functional>

#if defined(_WIN32)
#include <conio.h>
#else
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

// Check for ESC key press without blocking
bool check_escape_key() {
#if defined(_WIN32)
    if (_kbhit()) {
        int ch = _getch();
        if (ch == 27) { // ESC
            return true;
        }
    }
    return false;
#else
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);

    struct timeval tv = {0, 0}; // Zero timeout = non-blocking

    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
        char ch;
        if (read(STDIN_FILENO, &ch, 1) == 1 && ch == 27) { // ESC
            return true;
        }
    }
    return false;
#endif
}

// Extract a string-valued field from a partial (possibly incomplete) JSON object.
// Returns the decoded string value, or empty string if the key is not yet present.
// `complete` is set to true if the closing quote of the value was found.
std::string extract_partial_json_string(
    const std::string & json_fragment,
    const std::string & key,
    bool & complete)
{
    complete = false;
    // Look for "key": or "key" :
    std::string needle = "\"" + key + "\"";
    auto kpos = json_fragment.find(needle);
    if (kpos == std::string::npos) {
        return "";
    }
    // Skip past key, then find ':'
    size_t pos = kpos + needle.size();
    while (pos < json_fragment.size() && json_fragment[pos] == ' ') pos++;
    if (pos >= json_fragment.size() || json_fragment[pos] != ':') {
        return "";
    }
    pos++; // skip ':'
    while (pos < json_fragment.size() && json_fragment[pos] == ' ') pos++;
    if (pos >= json_fragment.size() || json_fragment[pos] != '"') {
        return "";  // value not started yet or not a string
    }
    pos++; // skip opening quote

    // Scan value, handling JSON escape sequences
    std::string result;
    result.reserve(json_fragment.size() - pos);
    for (size_t i = pos; i < json_fragment.size(); ) {
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
                        char hex[5] = {
                            json_fragment[i+2], json_fragment[i+3],
                            json_fragment[i+4], json_fragment[i+5], 0
                        };
                        unsigned cp = std::strtoul(hex, nullptr, 16);
                        if (cp < 0x80) {
                            result += (char)cp;
                        } else if (cp < 0x800) {
                            result += (char)(0xC0 | (cp >> 6));
                            result += (char)(0x80 | (cp & 0x3F));
                        } else {
                            result += (char)(0xE0 | (cp >> 12));
                            result += (char)(0x80 | ((cp >> 6) & 0x3F));
                            result += (char)(0x80 | (cp & 0x3F));
                        }
                        i += 6;
                    } else {
                        return result; // incomplete \uXXXX, stop here
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

// Incrementally decode more of a JSON string field into tcs.content_buffer.
// Finds the field start once (caching in content_scan_pos), then decodes only
// new bytes on each call.  Returns true if the field's closing quote was found.
bool decode_field_incremental(
    tool_stream_state & tcs,
    const std::string & key)
{
    // Find the value start position once
    if (tcs.content_scan_pos == 0) {
        std::string needle = "\"" + key + "\"";
        auto kpos = tcs.accumulated_args.find(needle);
        if (kpos == std::string::npos) return false;
        size_t pos = kpos + needle.size();
        while (pos < tcs.accumulated_args.size() && tcs.accumulated_args[pos] == ' ') pos++;
        if (pos >= tcs.accumulated_args.size() || tcs.accumulated_args[pos] != ':') return false;
        pos++;
        while (pos < tcs.accumulated_args.size() && tcs.accumulated_args[pos] == ' ') pos++;
        if (pos >= tcs.accumulated_args.size() || tcs.accumulated_args[pos] != '"') return false;
        pos++; // skip opening quote
        tcs.content_scan_pos = pos;
        tcs.content_raw_end = pos;
    }

    // Decode only the new bytes since last call
    const auto & s = tcs.accumulated_args;
    for (size_t i = tcs.content_raw_end; i < s.size(); ) {
        char c = s[i];
        if (c == '"') {
            tcs.content_raw_end = i + 1;
            return true; // field complete
        }
        if (c == '\\') {
            if (i + 1 >= s.size()) {
                // Backslash at end of buffer - wait for the escape char
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
                        char hex[5] = { s[i+2], s[i+3], s[i+4], s[i+5], 0 };
                        unsigned cp = std::strtoul(hex, nullptr, 16);
                        if (cp < 0x80) {
                            tcs.content_buffer += (char)cp;
                        } else if (cp < 0x800) {
                            tcs.content_buffer += (char)(0xC0 | (cp >> 6));
                            tcs.content_buffer += (char)(0x80 | (cp & 0x3F));
                        } else {
                            tcs.content_buffer += (char)(0xE0 | (cp >> 12));
                            tcs.content_buffer += (char)(0x80 | ((cp >> 6) & 0x3F));
                            tcs.content_buffer += (char)(0x80 | (cp & 0x3F));
                        }
                        i += 6;
                    } else {
                        tcs.content_raw_end = i; // incomplete \uXXXX, wait for more
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

inference_result agent_loop::generate_completion() {
    auto should_stop = [this]() {
        if (is_interrupted_.load()) {
            return true;
        }
        // Check for ESC key to abort generation
        if (check_escape_key()) {
            is_interrupted_.store(true);
            return true;
        }
        return false;
    };

    auto chat_tools = tool_registry::instance().to_chat_tools();
    inference_request request = build_inference_request(chat_tools);

    std::string full_content;
    bool is_thinking = false;
    std::vector<tool_stream_state> tc_states;

    bool spinner_running = false;
    auto stop_spinner = [&]() {
        if (spinner_running) {
            console::spinner::stop();
            spinner_running = false;
        }
    };

    console::spinner::start();
    spinner_running = true;

    inference_result result = backend_.complete(
        request,
        [&](const inference_event & event) {
            if (event.type == inference_event_type::ERROR) {
                stop_spinner();
                if (!event.error.empty()) {
                    console::error("Error: %s\n", event.error.c_str());
                }
                return;
            }

            if (event.type == inference_event_type::PROMPT_PROGRESS) {
                return;
            }

            stop_spinner();

            const auto & diff = event.diff;
            if (event.type == inference_event_type::TEXT_DELTA) {
                if (!diff.content_delta.empty()) {
                    if (is_thinking) {
                        console::log("\n---\n\n");
                        console::set_display(DISPLAY_TYPE_RESET);
                    }
                    console::log("%s", diff.content_delta.c_str());
                    console::flush();
                    if (is_thinking) {
                        is_thinking = false;
                    }
                    full_content += diff.content_delta;
                }
                return;
            }

            if (event.type == inference_event_type::REASONING_DELTA) {
                if (!diff.reasoning_content_delta.empty()) {
                    console::set_display(DISPLAY_TYPE_REASONING);
                    if (!is_thinking) {
                        console::log("---\n");
                    }
                    console::log("%s", diff.reasoning_content_delta.c_str());
                    console::flush();
                    is_thinking = true;
                }
                return;
            }

            if (event.type == inference_event_type::TOOL_CALL_DELTA) {
                if (diff.tool_call_index != std::string::npos) {
                    size_t idx = diff.tool_call_index;
                    if (idx >= tc_states.size()) {
                        tc_states.resize(idx + 1);
                    }
                    auto & tcs = tc_states[idx];

                    if (!diff.tool_call_delta.name.empty()) {
                        tcs.name = diff.tool_call_delta.name;
                    }
                    tcs.accumulated_args += diff.tool_call_delta.arguments;

                    // Close reasoning block if still open
                    if (is_thinking) {
                        console::log("\n---\n");
                        console::set_display(DISPLAY_TYPE_RESET);
                        is_thinking = false;
                    }

                    // Print tool header on first appearance
                    if (!tcs.header_printed && !tcs.name.empty()) {
                        console::set_display(DISPLAY_TYPE_INFO);
                        console::log("\n› %s", tcs.name.c_str());
                        console::set_display(DISPLAY_TYPE_RESET);
                        tcs.header_printed = true;
                    }

                    // Extract file_path once complete
                    if (tcs.header_printed && tcs.displayed_path.empty()) {
                        bool path_complete = false;
                        std::string fp = extract_partial_json_string(tcs.accumulated_args, "file_path", path_complete);
                        if (path_complete && !fp.empty()) {
                            console::set_display(DISPLAY_TYPE_INFO);
                            console::log(" %s", fp.c_str());
                            console::set_display(DISPLAY_TYPE_RESET);
                            tcs.displayed_path = fp;
                        }
                    }

                    // For write/edit: stream content lines incrementally
                    const char * content_field = nullptr;
                    if (tcs.name == "write") {
                        content_field = "content";
                    } else if (tcs.name == "edit") {
                        content_field = "new_string";
                    }
                    if (content_field && tcs.header_printed && !tcs.content_complete) {
                        bool field_complete = decode_field_incremental(tcs, content_field);
                        if (field_complete) tcs.content_complete = true;

                        // Print new complete lines starting from where we left off
                        const std::string & buf = tcs.content_buffer;
                        size_t pos = tcs.displayed_bytes;
                        while (pos < buf.size()) {
                            size_t nl = buf.find('\n', pos);
                            if (nl != std::string::npos) {
                                console::set_display(DISPLAY_TYPE_TOOL_STREAM);
                                console::log("\n  %.*s", (int)(nl - pos), buf.c_str() + pos);
                                console::set_display(DISPLAY_TYPE_RESET);
                                tcs.displayed_lines++;
                                pos = nl + 1;
                                tcs.displayed_bytes = pos;
                            } else if (field_complete) {
                                if (pos < buf.size()) {
                                    console::set_display(DISPLAY_TYPE_TOOL_STREAM);
                                    console::log("\n  %s", buf.c_str() + pos);
                                    console::set_display(DISPLAY_TYPE_RESET);
                                    tcs.displayed_lines++;
                                    tcs.displayed_bytes = buf.size();
                                }
                                break;
                            } else {
                                break; // incomplete last line, wait for more
                            }
                        }
                    }
                    console::flush();
                }
            }
        },
        should_stop);

    // Ensure spinner is stopped before returning (may have been started during tool call arg generation)
    stop_spinner();

    // Reset interrupted flag for next interaction
    is_interrupted_.store(false);

    last_prompt_tokens_ = result.prompt_tokens;
    last_completion_overflowed_ = result.context_overflow;

    if (result.cancelled) {
        console::log("\n[Generation aborted]\n");
    } else if (!result.error.empty() && !result.context_overflow) {
        LOG_WRN("Failed to complete model output: %s\n", result.error.c_str());
    }

    if (result.message.empty() && !full_content.empty()) {
        result.message.role = "assistant";
        result.message.content = full_content;
    }

    return result;
}
