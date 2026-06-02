#include "permission.h"
#include "permission-async.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <map>

namespace fs = std::filesystem;

static permission_request request(permission_type type, const std::string & tool, const std::string & details) {
    permission_request req;
    req.type = type;
    req.tool_name = tool;
    req.details = details;
    return req;
}

static void test_policy_defaults_and_bash_rules() {
    permission_policy policy;
    const std::map<std::string, permission_state> no_overrides;

    assert(policy.classify(request(permission_type::FILE_READ, "read", "foo.txt"), false, no_overrides) == permission_state::ALLOW);
    assert(policy.classify(request(permission_type::FILE_WRITE, "write", "foo.txt"), false, no_overrides) == permission_state::ASK);
    assert(policy.classify(request(permission_type::GLOB, "glob", "*.cpp"), false, no_overrides) == permission_state::ALLOW);
    assert(policy.classify(request(permission_type::BASH, "bash", "ls -la"), false, no_overrides) == permission_state::ALLOW);
    assert(policy.classify(request(permission_type::BASH, "bash", "ls | head"), false, no_overrides) == permission_state::ASK);
    assert(policy.classify(request(permission_type::BASH, "bash", "rm -rf build"), false, no_overrides) == permission_state::ASK);
    assert(policy.is_dangerous_bash_command("git reset --hard"));
    // Dangerous fragments embedded in compound commands must still be flagged (substring match)
    assert(policy.is_dangerous_bash_command("cd build && rm -rf /"));
    assert(policy.is_dangerous_bash_command("echo hi; sudo reboot"));
    assert(policy.classify(request(permission_type::BASH, "bash", "cd build && rm -rf /"), false, no_overrides) == permission_state::ASK);
    assert(policy.classify(request(permission_type::FILE_WRITE, "write", "foo.txt"), true, no_overrides) == permission_state::ALLOW);

    printf("  PASS: policy_defaults_and_bash_rules\n");
}

static void test_policy_session_overrides() {
    permission_policy policy;
    std::map<std::string, permission_state> overrides;
    overrides[permission_override_key("write", "foo.txt")] = permission_state::ALLOW_SESSION;
    overrides[permission_override_key("mcp__server__tool", "first args")] = permission_state::DENY_SESSION;

    assert(policy.classify(request(permission_type::FILE_WRITE, "write", "foo.txt"), false, overrides) == permission_state::ALLOW_SESSION);
    assert(policy.classify(request(permission_type::BASH, "mcp__server__tool", "different args"), false, overrides) == permission_state::DENY_SESSION);

    printf("  PASS: policy_session_overrides\n");
}

static void test_policy_external_path_boundaries() {
    const fs::path root = fs::temp_directory_path() / "llama-agent-permission-root";
    const fs::path sibling = fs::temp_directory_path() / "llama-agent-permission-root-sibling";
    fs::create_directories(root);
    fs::create_directories(sibling);

    permission_policy policy;
    policy.set_project_root(root.string());

    assert(!policy.is_external_path((root / "file.txt").string()));
    assert(policy.is_external_path((sibling / "file.txt").string()));

    fs::remove_all(root);
    fs::remove_all(sibling);

    printf("  PASS: policy_external_path_boundaries\n");
}

static void test_sync_and_async_managers_share_policy() {
    permission_manager sync;
    permission_manager_async async;

    const permission_request safe_cmd = request(permission_type::BASH, "bash", "git status");
    const permission_request compound_cmd = request(permission_type::BASH, "bash", "git status && echo ok");
    const permission_request write_req = request(permission_type::FILE_WRITE, "write", "foo.txt");

    assert(sync.check_permission(safe_cmd) == permission_state::ALLOW);
    assert(async.check_permission(safe_cmd) == permission_state::ALLOW);
    assert(sync.check_permission(compound_cmd) == permission_state::ASK);
    assert(async.check_permission(compound_cmd) == permission_state::ASK);
    assert(sync.is_dangerous_bash_command("git reset --hard"));
    assert(async.is_dangerous_bash_command("git reset --hard"));

    sync.set_yolo_mode(true);
    async.set_yolo_mode(true);
    assert(sync.check_permission(write_req) == permission_state::ALLOW);
    assert(async.check_permission(write_req) == permission_state::ALLOW);

    async.set_yolo_mode(false);
    const std::string request_id = async.request_permission(write_req);
    assert(async.respond(request_id, true, permission_scope::SESSION));
    assert(async.check_permission(write_req) == permission_state::ALLOW_SESSION);

    printf("  PASS: sync_and_async_managers_share_policy\n");
}

int main() {
    printf("Running agent permission tests...\n");
    test_policy_defaults_and_bash_rules();
    test_policy_session_overrides();
    test_policy_external_path_boundaries();
    test_sync_and_async_managers_share_policy();
    printf("\nAll agent permission tests passed!\n");
    return 0;
}
