#include "agent-resources.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static void test_skill_search_path_order() {
    const std::vector<std::string> paths = agent_build_skill_search_paths(
            "/work", "/cfg", {"/extra-one", "/extra-two"});

    assert(paths.size() >= 5);
    assert(paths[0] == "/work/.llama-agent/skills");
    assert(paths[1] == "/work/.agents/skills");
    assert(paths[2] == "/cfg/skills");
    assert(paths[paths.size() - 2] == "/extra-one");
    assert(paths[paths.size() - 1] == "/extra-two");

    printf("  PASS: skill_search_path_order\n");
}

static void test_empty_working_dir_defaults_to_current_dir() {
    const std::vector<std::string> paths = agent_build_skill_search_paths(
            "", "", {"/extra"});

    assert(paths.size() >= 3);
    assert(paths[0] == "./.llama-agent/skills");
    assert(paths[1] == "./.agents/skills");
    assert(paths[paths.size() - 1] == "/extra");

    printf("  PASS: empty_working_dir_defaults_to_current_dir\n");
}

static void write_file(const fs::path & path, const std::string & content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path);
    assert(file.good());
    file << content;
}

static void test_discover_resources_respects_enable_flags() {
    const fs::path root = fs::temp_directory_path() / "llama-agent-resources-test";
    fs::remove_all(root);

    write_file(root / "AGENTS.md", "project instructions\n");
    write_file(root / ".llama-agent" / "skills" / "test-skill" / "SKILL.md",
               "---\n"
               "name: test-skill\n"
               "description: Test skill\n"
               "---\n"
               "Body\n");

    agent_resource_config enabled;
    enabled.working_dir = root.string();
    enabled.enable_skills = true;
    enabled.enable_agents_md = true;
    agent_resource_discovery discovered = agent_discover_resources(enabled);
    assert(discovered.skills_count == 1);
    assert(discovered.agents_md_count == 1);
    assert(!discovered.skills_prompt_section().empty());
    assert(!discovered.agents_md_prompt_section().empty());

    agent_resource_config disabled = enabled;
    disabled.enable_skills = false;
    disabled.enable_agents_md = false;
    agent_resource_discovery skipped = agent_discover_resources(disabled);
    assert(skipped.skills_count == 0);
    assert(skipped.agents_md_count == 0);
    assert(skipped.skills_prompt_section().empty());
    assert(skipped.agents_md_prompt_section().empty());

    fs::remove_all(root);

    printf("  PASS: discover_resources_respects_enable_flags\n");
}

int main() {
    printf("Running agent resource tests...\n");
    test_skill_search_path_order();
    test_empty_working_dir_defaults_to_current_dir();
    test_discover_resources_respects_enable_flags();
    printf("\nAll agent resource tests passed!\n");
    return 0;
}
