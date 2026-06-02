#include "agent-resources.h"

#include <cstdlib>

std::string agent_resource_discovery::skills_prompt_section() const {
    return skills.generate_prompt_section();
}

std::string agent_resource_discovery::agents_md_prompt_section() const {
    return agents_md.generate_prompt_section();
}

std::vector<std::string> agent_build_skill_search_paths(
        const std::string & working_dir,
        const std::string & config_dir,
        const std::vector<std::string> & extra_paths) {
    const std::string search_working_dir = working_dir.empty() ? "." : working_dir;

    std::vector<std::string> skill_paths;

    // Project-local skills have highest priority.
    skill_paths.push_back(search_working_dir + "/.llama-agent/skills");
    skill_paths.push_back(search_working_dir + "/.agents/skills");

    if (!config_dir.empty()) {
        skill_paths.push_back(config_dir + "/skills");
    }

#ifdef _WIN32
    const char * home_skills = std::getenv("APPDATA");
    if (home_skills) {
        skill_paths.push_back(std::string(home_skills) + "\\agents\\skills");
    }
#else
    const char * home_skills = std::getenv("HOME");
    if (home_skills) {
        skill_paths.push_back(std::string(home_skills) + "/.agents/skills");
    }
#endif

    skill_paths.insert(skill_paths.end(), extra_paths.begin(), extra_paths.end());

    return skill_paths;
}

agent_resource_discovery agent_discover_resources(const agent_resource_config & config) {
    const std::string working_dir = config.working_dir.empty() ? "." : config.working_dir;

    agent_resource_discovery discovery;

    if (config.enable_skills) {
        const std::vector<std::string> skill_paths = agent_build_skill_search_paths(
                working_dir, config.config_dir, config.extra_skills_paths);
        discovery.skills_count = discovery.skills.discover(skill_paths);
    }

    if (config.enable_agents_md) {
        discovery.agents_md_count = discovery.agents_md.discover(working_dir, config.config_dir);
        discovery.agents_md_total_content_size = discovery.agents_md.total_content_size();
    }

    return discovery;
}
