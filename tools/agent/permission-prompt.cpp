#include "permission.h"
#include "console.h"

#if defined(_WIN32)
#include <conio.h>
#else
#include <cerrno>
#include <termios.h>
#include <unistd.h>
#endif

// Read a single character without waiting for Enter.
static char read_single_char() {
#if defined(_WIN32)
    return static_cast<char>(_getch());
#else
    // Use raw read(2) instead of getchar() here: the advanced console
    // (non --simple-io mode) uses getwchar() for readline, which sets
    // stdin's orientation to "wide". Mixing with narrow stdio is undefined.
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    newt.c_cc[VMIN]  = 1;
    newt.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    char ch = 0;
    ssize_t n = read(STDIN_FILENO, &ch, 1);
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    if (n <= 0) {
        return 0;
    }
    return ch;
#endif
}

permission_response permission_manager::prompt_user(const permission_request & request) {
    console::set_display(DISPLAY_TYPE_RESET);

    console::log("\n");
    console::log("+-- PERMISSION: %s ", request.tool_name.c_str());
    for (size_t i = request.tool_name.length() + 17; i < 60; i++) console::log("-");
    console::log("+\n");

    if (!request.details.empty()) {
        console::log("| %s\n", request.details.c_str());
    }

    if (request.is_dangerous) {
        console::set_display(DISPLAY_TYPE_ERROR);
        console::log("| WARNING: Potentially dangerous operation\n");
        console::set_display(DISPLAY_TYPE_RESET);
    }

    console::log("+");
    for (int i = 0; i < 59; i++) console::log("-");
    console::log("+\n");

    console::log("| [y]es  [n]o  [a]lways  [d]eny always: ");
    console::flush();

    char ch = read_single_char();
    console::log("%c\n", ch);

    if (ch == 'n' || ch == 'N') {
        return permission_response::DENY_ONCE;
    }
    if (ch == 'y' || ch == 'Y') {
        return permission_response::ALLOW_ONCE;
    }
    if (ch == 'a' || ch == 'A') {
        std::string key = permission_override_key(request.tool_name, request.details);
        session_overrides_[key] = permission_state::ALLOW_SESSION;
        return permission_response::ALLOW_ALWAYS;
    }
    if (ch == 'd' || ch == 'D') {
        std::string key = permission_override_key(request.tool_name, request.details);
        session_overrides_[key] = permission_state::DENY_SESSION;
        return permission_response::DENY_ALWAYS;
    }

    return permission_response::DENY_ONCE;
}
