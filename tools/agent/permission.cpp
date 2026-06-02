#include "permission.h"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

permission_manager::permission_manager() = default;

void permission_manager::set_project_root(const std::string & path) {
    project_root_ = fs::absolute(path).string();
    policy_.set_project_root(path);
}

permission_state permission_manager::check_permission(const permission_request & request) {
    return policy_.classify(request, yolo_mode_, session_overrides_);
}

void permission_manager::record_tool_call(const std::string & tool, const std::string & args_hash) {
    // Check if this is same as last call
    if (!recent_calls_.empty()) {
        auto & last = recent_calls_.back();
        if (last.tool == tool && last.args_hash == args_hash) {
            last.count++;
            return;
        }
    }

    // Add new record
    recent_calls_.push_back({tool, args_hash, 1});

    // Keep only last 10 records
    if (recent_calls_.size() > 10) {
        recent_calls_.erase(recent_calls_.begin());
    }
}

bool permission_manager::is_doom_loop(const std::string & tool, const std::string & args_hash) const {
    if (recent_calls_.empty()) return false;

    const auto & last = recent_calls_.back();
    return last.tool == tool && last.args_hash == args_hash && last.count >= 3;
}

void permission_manager::clear_session() {
    session_overrides_.clear();
    recent_calls_.clear();
}

bool permission_manager::is_sensitive_file(const std::string & path) {
    // Get filename and extension
    fs::path p(path);
    std::string filename = p.filename().string();
    std::string ext = p.extension().string();

    // Convert to lowercase for comparison
    std::string filename_lower = filename;
    std::string ext_lower = ext;
    for (auto & c : filename_lower) c = std::tolower(c);
    for (auto & c : ext_lower) c = std::tolower(c);

    // Sensitive file patterns
    static const std::vector<std::string> sensitive_names = {
        ".env", ".env.local", ".env.production", ".env.development",
        ".netrc", ".npmrc", ".pypirc",
        "id_rsa", "id_dsa", "id_ecdsa", "id_ed25519",
        "credentials", "credentials.json", "credentials.yaml",
        "secrets", "secrets.json", "secrets.yaml", "secrets.yml",
        ".htpasswd", ".htaccess",
        "shadow", "passwd",
        "private_key", "privatekey",
        "service_account", "service-account",
        "token", "token.json",
        "keystore", "keystore.jks",
        ".pgpass", ".my.cnf",
    };

    static const std::vector<std::string> sensitive_extensions = {
        ".pem", ".key", ".p12", ".pfx", ".jks",
        ".keystore", ".secret", ".secrets",
        ".cert", ".crt", ".cer",
    };

    // Check exact filename matches
    for (const auto & name : sensitive_names) {
        if (filename_lower == name) {
            return true;
        }
        // Also check if filename contains the sensitive name (e.g., "prod.env")
        if (filename_lower.find(name) != std::string::npos && name.front() != '.') {
            return true;
        }
    }

    // Check extensions
    for (const auto & sensitive_ext : sensitive_extensions) {
        if (ext_lower == sensitive_ext) {
            return true;
        }
    }

    // Check for AWS/cloud credentials
    if (filename_lower.find("aws") != std::string::npos &&
        (filename_lower.find("credential") != std::string::npos ||
         filename_lower.find("config") != std::string::npos)) {
        return true;
    }

    return false;
}

bool permission_manager::is_external_path(const std::string & path) const {
    return policy_.is_external_path(path);
}

bool permission_manager::is_dangerous_bash_command(const std::string & cmd) const {
    return policy_.is_dangerous_bash_command(cmd);
}
