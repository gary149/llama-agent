#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

// Profile schema — TOML-loaded per-model harness configuration. Mirrors the
// docs/small-model-optimization-plan.md schema. All sampler fields are
// std::optional so "field absent in TOML" → "don't touch the existing
// common_params default".
//
// Resolution order applied by load_profile():
//   1. --profile <name> CLI flag (highest)
//   2. .llama-agent/config.toml in cwd has profile_name = "..."  (future)
//   3. Auto-derive from --model path via [meta].model_id_match    (future)
//   4. default.toml                                               (fallback)
//
// Phase 1 Commit 2 implements (1) and (4); (2) and (3) are TODOs.

struct profile_sampler {
    std::optional<float> temperature;
    std::optional<float> top_p;
    std::optional<int>   top_k;
    std::optional<float> min_p;
    std::optional<float> presence_penalty;
    std::optional<float> repeat_penalty;
};

struct profile_template_kwargs {
    std::optional<bool> enable_thinking;
    std::optional<bool> preserve_thinking;
};

struct profile_agent {
    std::optional<int> max_iterations;
    std::optional<int> bash_timeout_ms;
};

struct profile_thinking {
    // 0 = disabled; Phase 3 cap on reasoning_content chars per turn.
    int budget_chars = 0;
};

struct profile_tools {
    bool write_guard                     = false;
    bool quality_monitor                 = false;
    int  quality_monitor_max_corrections = 2;
};

struct profile_skills {
    // 0 = disabled; Phase 5 token budget for ephemeral skill re-injection.
    int inject_token_budget = 0;
};

struct profile_settings {
    std::string name;
    std::string description;
    std::vector<std::string> model_id_match;

    profile_sampler         sampler;
    profile_template_kwargs template_kwargs;
    profile_agent           agent;
    profile_thinking        thinking;
    profile_tools           tools;
    profile_skills          skills;
};

// Load a profile by name or path.
//
// If `name_or_path` ends in ".toml" or contains a path separator, it's
// treated as a direct file path. Otherwise resolved against the built-in
// profile search path (binary_dir + dev source tree).
//
// On failure: writes an error message to stderr and returns std::nullopt.
std::optional<profile_settings>
load_profile(const std::string & name_or_path,
             const std::string & binary_path /* argv[0] */);
