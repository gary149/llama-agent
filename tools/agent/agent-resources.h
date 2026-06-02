#pragma once

#include "agents-md/agents-md-manager.h"
#include "skills/skills-manager.h"

#include <string>
#include <vector>

struct agent_resource_config {
    std::string working_dir = ".";
    std::string config_dir;
    bool enable_skills = true;
    bool enable_agents_md = true;
    std::vector<std::string> extra_skills_paths;
};

struct agent_resource_discovery {
    skills_manager skills;
    agents_md_manager agents_md;
    int skills_count = 0;
    int agents_md_count = 0;
    size_t agents_md_total_content_size = 0;

    std::string skills_prompt_section() const;
    std::string agents_md_prompt_section() const;
};

std::vector<std::string> agent_build_skill_search_paths(
        const std::string & working_dir,
        const std::string & config_dir,
        const std::vector<std::string> & extra_paths);

agent_resource_discovery agent_discover_resources(const agent_resource_config & config);
