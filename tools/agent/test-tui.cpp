#include "tui-editor.h"
#include "tui-events.h"
#include "tui-renderer.h"
#include "tui-select-list.h"

#include <cstdlib>
#include <iostream>
#include <thread>

static void require(bool ok, const char * message) {
    if (!ok) {
        std::cerr << message << "\n";
        std::exit(1);
    }
}

static tui_input_event ch(char32_t cp) {
    return {tui_input_key::CHARACTER, cp};
}

static void test_editor_submit_and_history() {
    tui_editor editor(false);
    editor.handle_event(ch('h'));
    editor.handle_event(ch('i'));
    auto action = editor.handle_event({tui_input_key::ENTER, 0});
    require(action.submitted, "editor submit did not return submitted action");
    require(action.submission == "hi", "editor submission text mismatch");
    require(editor.empty(), "editor did not clear after submit");

    editor.handle_event({tui_input_key::UP, 0});
    require(editor.buffer() == "hi", "editor history up did not restore last entry");
}

static void test_editor_utf8_and_multiline_render() {
    tui_editor editor(false);
    editor.handle_event(ch('a'));
    editor.handle_event(ch(0x03BB)); // lambda
    editor.handle_event({tui_input_key::ALT_ENTER, 0});
    editor.handle_event(ch('b'));
    require(editor.buffer() == "a\xCE\xBB\nb", "editor UTF-8/multiline buffer mismatch");

    tui_editor_render render = editor.render(20, 10);
    require(render.lines.size() == 2, "editor render line count mismatch");
    require(render.cursor_row == 1, "editor cursor row mismatch");
    require(render.cursor_col == 3, "editor cursor column mismatch");
}

static void test_paste_collapse() {
    tui_editor editor(false);
    editor.handle_event({tui_input_key::PASTE_START, 0});
    for (int i = 0; i < 12; ++i) {
        editor.handle_event(ch('x'));
        editor.handle_event({tui_input_key::ENTER, 0});
    }
    editor.handle_event({tui_input_key::PASTE_END, 0});
    require(editor.buffer().find("[paste #1 +") != std::string::npos, "large paste did not collapse");
}

static void test_select_list() {
    tui_select_list list;
    list.set_items({"/clear", "/compact", "/stats"});
    require(list.visible(), "select list not visible after set_items");
    require(*list.selected() == "/clear", "select list initial selection mismatch");
    list.move_down();
    require(*list.selected() == "/compact", "select list move_down mismatch");
    list.move_up();
    require(*list.selected() == "/clear", "select list move_up mismatch");
    auto rows = list.render(20, 2);
    require(rows.size() == 2, "select list render visible count mismatch");
}

static void test_event_queue() {
    tui_event_queue<int> queue;
    std::thread producer([&]() {
        queue.push(42);
    });
    int value = 0;
    require(queue.wait_pop(value, 1000), "event queue did not deliver item");
    require(value == 42, "event queue delivered wrong value");
    producer.join();
}

static void test_footer_and_agent_event_projection() {
    tui_footer_state footer;
    footer.working_dir = "/tmp/project";
    footer.session_path = "/tmp/session.jsonl";
    footer.meta.model_name = "test-model";
    footer.meta.n_ctx = 100;
    footer.stats.total_input = 10;
    footer.stats.total_output = 5;
    footer.stats.total_predicted_ms = 1000.0;
    footer.last_prompt_tokens = 50;
    footer.generating = true;
    auto lines = tui_render_footer(footer, 80, false);
    require(lines.size() == 2, "footer did not render two lines");
    require(lines[1].find("test-model") != std::string::npos, "footer missing model");
    require(lines[1].find("\xE2\x86\x91 10 \xE2\x86\x93 5") != std::string::npos,
            "footer missing token arrows");
    require(lines[1].find("5.0 tok/s") != std::string::npos, "footer missing speed");
    require(lines[1].find("50%/1K") != std::string::npos, "footer missing context fill");

    session_stats stats;
    stats.total_input = 7;
    stats.total_output = 3;
    stats.total_cached = 2;
    stats.total_predicted_ms = 500.0;
    tui_event ev = tui_event_from_agent_event(agent_event::completed(agent_stop_reason::COMPLETED, stats, 77));
    require(ev.type == tui_event_type::COMPLETED, "completed event type mismatch");
    require(ev.stats.total_input == 7, "completed event input stats mismatch");
    require(ev.stats.total_output == 3, "completed event output stats mismatch");
    require(ev.stats.total_cached == 2, "completed event cached stats mismatch");
    require(ev.last_prompt_tokens == 77, "completed event prompt-token stat mismatch");

    tui_event tool = tui_event_from_agent_event(agent_event::tool_call_delta(1, "write", "{\"file_path\""));
    require(tool.type == tui_event_type::TOOL_CALL_DELTA, "tool delta event type mismatch");
    require(tool.tool_call_index == 1, "tool delta index mismatch");
    require(tool.tool_name == "write", "tool delta name mismatch");
}

int main() {
    test_editor_submit_and_history();
    test_editor_utf8_and_multiline_render();
    test_paste_collapse();
    test_select_list();
    test_event_queue();
    test_footer_and_agent_event_projection();
    return 0;
}
