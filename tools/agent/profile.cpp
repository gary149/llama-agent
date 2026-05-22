#include "profile.h"

#define TOML_EXCEPTIONS 0   // Use parse_result error returns instead of throwing
#include "vendor/toml.hpp"

#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

// Search paths for built-in profiles. Tries (in order):
//   <binary_dir>/profiles/<name>.toml
//   <binary_dir>/../share/llama-agent/profiles/<name>.toml
//   <binary_dir>/../tools/agent/profiles/<name>.toml          (dev build)
//   <binary_dir>/../../tools/agent/profiles/<name>.toml       (deeper dev build)
//   <cwd>/tools/agent/profiles/<name>.toml                    (running from source root)
static std::string resolve_builtin_profile(const std::string & name,
                                           const std::string & binary_path) {
    fs::path bin_dir;
    try {
        bin_dir = fs::weakly_canonical(fs::path(binary_path)).parent_path();
    } catch (...) {
        bin_dir = fs::current_path();
    }

    std::vector<fs::path> candidates = {
        bin_dir / "profiles" / (name + ".toml"),
        bin_dir / ".." / "share" / "llama-agent" / "profiles" / (name + ".toml"),
        bin_dir / ".." / "tools" / "agent" / "profiles" / (name + ".toml"),
        bin_dir / ".." / ".." / "tools" / "agent" / "profiles" / (name + ".toml"),
        fs::current_path() / "tools" / "agent" / "profiles" / (name + ".toml"),
    };

    for (const auto & p : candidates) {
        std::error_code ec;
        if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
            return fs::weakly_canonical(p, ec).string();
        }
    }
    return "";
}

// Convert a toml::node to std::optional<T> where T is int/float/bool.
template <typename T>
static std::optional<T> opt_value(const toml::node & node) {
    if (auto v = node.value<T>()) return *v;
    return std::nullopt;
}

std::optional<profile_settings>
load_profile(const std::string & name_or_path,
             const std::string & binary_path) {
    // Resolve to absolute file path.
    std::string toml_path;
    bool is_direct_path =
        name_or_path.find('/') != std::string::npos ||
        name_or_path.find('\\') != std::string::npos ||
        (name_or_path.size() >= 5 &&
         name_or_path.compare(name_or_path.size() - 5, 5, ".toml") == 0);

    if (is_direct_path) {
        toml_path = name_or_path;
    } else {
        toml_path = resolve_builtin_profile(name_or_path, binary_path);
        if (toml_path.empty()) {
            fprintf(stderr,
                    "profile: could not find built-in profile '%s'. "
                    "Searched <binary_dir>/profiles, share/llama-agent/profiles, "
                    "and ../tools/agent/profiles.\n",
                    name_or_path.c_str());
            return std::nullopt;
        }
    }

    // Parse TOML.
    toml::parse_result parsed = toml::parse_file(toml_path);
    if (!parsed) {
        const auto & err = parsed.error();
        fprintf(stderr, "profile: TOML parse error in %s:\n  %s\n",
                toml_path.c_str(),
                std::string(err.description()).c_str());
        return std::nullopt;
    }
    const toml::table & root = parsed.table();

    profile_settings p;

    // [meta]
    if (auto meta = root["meta"].as_table()) {
        if (auto v = (*meta)["name"].value<std::string>())        p.name = *v;
        if (auto v = (*meta)["description"].value<std::string>()) p.description = *v;
        if (auto arr = (*meta)["model_id_match"].as_array()) {
            for (const auto & el : *arr) {
                if (auto s = el.value<std::string>()) p.model_id_match.push_back(*s);
            }
        }
    }

    // [sampler] — optional fields; absent = don't change
    if (auto s = root["sampler"].as_table()) {
        if (auto v = (*s)["temperature"].value<double>())      p.sampler.temperature      = static_cast<float>(*v);
        if (auto v = (*s)["top_p"].value<double>())            p.sampler.top_p            = static_cast<float>(*v);
        if (auto v = (*s)["top_k"].value<int64_t>())           p.sampler.top_k            = static_cast<int>(*v);
        if (auto v = (*s)["min_p"].value<double>())            p.sampler.min_p            = static_cast<float>(*v);
        if (auto v = (*s)["presence_penalty"].value<double>()) p.sampler.presence_penalty = static_cast<float>(*v);
        if (auto v = (*s)["repeat_penalty"].value<double>())   p.sampler.repeat_penalty   = static_cast<float>(*v);
    }

    // [template_kwargs]
    if (auto t = root["template_kwargs"].as_table()) {
        if (auto v = (*t)["enable_thinking"].value<bool>())   p.template_kwargs.enable_thinking   = *v;
        if (auto v = (*t)["preserve_thinking"].value<bool>()) p.template_kwargs.preserve_thinking = *v;
    }

    // [agent]
    if (auto a = root["agent"].as_table()) {
        if (auto v = (*a)["max_iterations"].value<int64_t>())  p.agent.max_iterations  = static_cast<int>(*v);
        if (auto v = (*a)["bash_timeout_ms"].value<int64_t>()) p.agent.bash_timeout_ms = static_cast<int>(*v);
    }

    // [thinking]
    if (auto th = root["thinking"].as_table()) {
        if (auto v = (*th)["budget_chars"].value<int64_t>()) p.thinking.budget_chars = static_cast<int>(*v);
    }

    // [tools]
    if (auto tl = root["tools"].as_table()) {
        if (auto v = (*tl)["write_guard"].value<bool>())                  p.tools.write_guard                     = *v;
        if (auto v = (*tl)["quality_monitor"].value<bool>())              p.tools.quality_monitor                 = *v;
        if (auto v = (*tl)["quality_monitor_max_corrections"].value<int64_t>())
            p.tools.quality_monitor_max_corrections = static_cast<int>(*v);
    }

    // [skills]
    if (auto sk = root["skills"].as_table()) {
        if (auto v = (*sk)["inject_token_budget"].value<int64_t>())
            p.skills.inject_token_budget = static_cast<int>(*v);
    }

    return p;
}
