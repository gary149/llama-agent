#pragma once

// A2A (Agent-to-Agent) Protocol Data Types
// Based on: https://a2a-protocol.org/latest/specification/

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace a2a {

using json = nlohmann::ordered_json;

// Task status states per A2A spec
enum class task_state {
    SUBMITTED,       // Task acknowledged, processing queued
    WORKING,         // Task actively being processed
    INPUT_REQUIRED,  // Agent needs additional information
    AUTH_REQUIRED,   // Secondary authentication required
    COMPLETED,       // Task finished successfully
    FAILED,          // Task failed
    CANCELLED,       // User or system cancelled the task
    REJECTED         // Agent declined to perform the task
};

// Convert task_state to string
inline std::string task_state_to_string(task_state state) {
    switch (state) {
        case task_state::SUBMITTED:      return "submitted";
        case task_state::WORKING:        return "working";
        case task_state::INPUT_REQUIRED: return "input-required";
        case task_state::AUTH_REQUIRED:  return "auth-required";
        case task_state::COMPLETED:      return "completed";
        case task_state::FAILED:         return "failed";
        case task_state::CANCELLED:      return "cancelled";
        case task_state::REJECTED:       return "rejected";
    }
    return "unknown";
}

// Parse task_state from string
inline task_state task_state_from_string(const std::string & s) {
    if (s == "submitted")      return task_state::SUBMITTED;
    if (s == "working")        return task_state::WORKING;
    if (s == "input-required") return task_state::INPUT_REQUIRED;
    if (s == "auth-required")  return task_state::AUTH_REQUIRED;
    if (s == "completed")      return task_state::COMPLETED;
    if (s == "failed")         return task_state::FAILED;
    if (s == "cancelled")      return task_state::CANCELLED;
    if (s == "rejected")       return task_state::REJECTED;
    return task_state::SUBMITTED;
}

// Check if state is terminal (no more processing possible)
inline bool is_terminal_state(task_state state) {
    return state == task_state::COMPLETED ||
           state == task_state::FAILED ||
           state == task_state::CANCELLED ||
           state == task_state::REJECTED;
}

// Text part
struct text_part {
    std::string text;

    json to_json() const {
        return json{{"text", text}};
    }

    static text_part from_json(const json & j) {
        text_part p;
        p.text = j.value("text", "");
        return p;
    }
};

// File part
struct file_part {
    std::optional<std::string> uri;
    std::optional<std::string> bytes;  // base64 encoded
    std::string media_type;
    std::optional<std::string> name;

    json to_json() const {
        json j;
        if (uri) {
            j["uri"] = *uri;
        }
        if (bytes) {
            j["bytes"] = *bytes;
        }
        if (!media_type.empty()) {
            j["mediaType"] = media_type;
        }
        if (name) {
            j["name"] = *name;
        }
        return j;
    }

    static file_part from_json(const json & j) {
        file_part p;
        if (j.contains("uri")) {
            p.uri = j["uri"].get<std::string>();
        }
        if (j.contains("bytes")) {
            p.bytes = j["bytes"].get<std::string>();
        }
        p.media_type = j.value("mediaType", "");
        if (j.contains("name")) {
            p.name = j["name"].get<std::string>();
        }
        return p;
    }
};

// Data part (structured JSON)
struct data_part {
    json data;

    json to_json() const {
        return json{{"data", data}};
    }

    static data_part from_json(const json & j) {
        data_part p;
        p.data = j.value("data", json::object());
        return p;
    }
};

// Message part (union-like: text, file, or data)
struct part {
    std::optional<text_part> text;
    std::optional<file_part> file;
    std::optional<data_part> data;
    std::optional<json> metadata;

    // Factory methods
    static part from_text(const std::string & t) {
        part p;
        p.text = text_part{t};
        return p;
    }

    static part from_file_uri(const std::string & uri, const std::string & media_type, const std::string & name = "") {
        part p;
        file_part fp;
        fp.uri = uri;
        fp.media_type = media_type;
        if (!name.empty()) {
            fp.name = name;
        }
        p.file = fp;
        return p;
    }

    static part from_data(const json & d) {
        part p;
        p.data = data_part{d};
        return p;
    }

    json to_json() const {
        json j;
        if (text) {
            j["textPart"] = text->to_json();
        } else if (file) {
            j["filePart"] = file->to_json();
        } else if (data) {
            j["dataPart"] = data->to_json();
        }
        if (metadata) {
            j["metadata"] = *metadata;
        }
        return j;
    }

    static part from_json(const json & j) {
        part p;
        if (j.contains("textPart")) {
            p.text = text_part::from_json(j["textPart"]);
        } else if (j.contains("filePart")) {
            p.file = file_part::from_json(j["filePart"]);
        } else if (j.contains("dataPart")) {
            p.data = data_part::from_json(j["dataPart"]);
        } else if (j.contains("text")) {
            // Shorthand: {"text": "..."} instead of {"textPart": {"text": "..."}}
            p.text = text_part{j["text"].get<std::string>()};
        }
        if (j.contains("metadata")) {
            p.metadata = j["metadata"];
        }
        return p;
    }

    // Get text content if this is a text part
    std::string get_text() const {
        if (text) {
            return text->text;
        }
        return "";
    }
};

// A2A Message
struct message {
    std::string role;  // "user" or "agent"
    std::vector<part> parts;
    std::optional<std::string> name;
    std::optional<json> metadata;

    json to_json() const {
        json j;
        j["role"] = role;
        j["parts"] = json::array();
        for (const auto & p : parts) {
            j["parts"].push_back(p.to_json());
        }
        if (name) {
            j["name"] = *name;
        }
        if (metadata) {
            j["metadata"] = *metadata;
        }
        return j;
    }

    static message from_json(const json & j) {
        message m;
        m.role = j.value("role", "user");
        if (j.contains("parts") && j["parts"].is_array()) {
            for (const auto & pj : j["parts"]) {
                m.parts.push_back(part::from_json(pj));
            }
        }
        if (j.contains("name")) {
            m.name = j["name"].get<std::string>();
        }
        if (j.contains("metadata")) {
            m.metadata = j["metadata"];
        }
        return m;
    }

    // Helper to get all text content
    std::string get_text_content() const {
        std::string result;
        for (const auto & p : parts) {
            if (!result.empty() && !p.get_text().empty()) {
                result += "\n";
            }
            result += p.get_text();
        }
        return result;
    }

    // Helper to create a simple text message
    static message text(const std::string & role, const std::string & content) {
        message m;
        m.role = role;
        m.parts.push_back(part::from_text(content));
        return m;
    }
};

// Artifact (task output)
struct artifact {
    std::string id;
    std::optional<std::string> name;
    std::optional<std::string> description;
    std::vector<part> parts;
    std::optional<size_t> index;
    std::optional<bool> append;
    std::optional<bool> last_chunk;
    std::optional<json> metadata;

    json to_json() const {
        json j;
        j["id"] = id;
        if (name) {
            j["name"] = *name;
        }
        if (description) {
            j["description"] = *description;
        }
        j["parts"] = json::array();
        for (const auto & p : parts) {
            j["parts"].push_back(p.to_json());
        }
        if (index) {
            j["index"] = *index;
        }
        if (append) {
            j["append"] = *append;
        }
        if (last_chunk) {
            j["lastChunk"] = *last_chunk;
        }
        if (metadata) {
            j["metadata"] = *metadata;
        }
        return j;
    }

    static artifact from_json(const json & j) {
        artifact a;
        a.id = j.value("id", "");
        if (j.contains("name")) {
            a.name = j["name"].get<std::string>();
        }
        if (j.contains("description")) {
            a.description = j["description"].get<std::string>();
        }
        if (j.contains("parts") && j["parts"].is_array()) {
            for (const auto & pj : j["parts"]) {
                a.parts.push_back(part::from_json(pj));
            }
        }
        if (j.contains("index")) {
            a.index = j["index"].get<size_t>();
        }
        if (j.contains("append")) {
            a.append = j["append"].get<bool>();
        }
        if (j.contains("lastChunk")) {
            a.last_chunk = j["lastChunk"].get<bool>();
        }
        if (j.contains("metadata")) {
            a.metadata = j["metadata"];
        }
        return a;
    }
};

// Task status
struct task_status {
    task_state state = task_state::SUBMITTED;
    std::optional<message> status_message;
    std::optional<std::string> timestamp;

    json to_json() const {
        json j;
        j["state"] = task_state_to_string(state);
        if (status_message) {
            j["message"] = status_message->to_json();
        }
        if (timestamp) {
            j["timestamp"] = *timestamp;
        }
        return j;
    }

    static task_status from_json(const json & j) {
        task_status s;
        s.state = task_state_from_string(j.value("state", "submitted"));
        if (j.contains("message")) {
            s.status_message = message::from_json(j["message"]);
        }
        if (j.contains("timestamp")) {
            s.timestamp = j["timestamp"].get<std::string>();
        }
        return s;
    }
};

// Task object
struct task {
    std::string id;
    std::string context_id;
    task_status status;
    std::optional<std::vector<artifact>> artifacts;
    std::optional<std::vector<message>> history;
    std::optional<json> metadata;

    json to_json() const {
        json j;
        j["id"] = id;
        j["contextId"] = context_id;
        j["status"] = status.to_json();
        if (artifacts) {
            j["artifacts"] = json::array();
            for (const auto & a : *artifacts) {
                j["artifacts"].push_back(a.to_json());
            }
        }
        if (history) {
            j["history"] = json::array();
            for (const auto & m : *history) {
                j["history"].push_back(m.to_json());
            }
        }
        if (metadata) {
            j["metadata"] = *metadata;
        }
        return j;
    }

    static task from_json(const json & j) {
        task t;
        t.id = j.value("id", "");
        t.context_id = j.value("contextId", "");
        if (j.contains("status")) {
            t.status = task_status::from_json(j["status"]);
        }
        if (j.contains("artifacts") && j["artifacts"].is_array()) {
            t.artifacts = std::vector<artifact>{};
            for (const auto & aj : j["artifacts"]) {
                t.artifacts->push_back(artifact::from_json(aj));
            }
        }
        if (j.contains("history") && j["history"].is_array()) {
            t.history = std::vector<message>{};
            for (const auto & mj : j["history"]) {
                t.history->push_back(message::from_json(mj));
            }
        }
        if (j.contains("metadata")) {
            t.metadata = j["metadata"];
        }
        return t;
    }
};

// TaskStatusUpdateEvent (SSE event)
struct task_status_update_event {
    std::string task_id;
    std::string context_id;
    task_status status;
    bool final = false;

    json to_json() const {
        json j;
        j["taskId"] = task_id;
        j["contextId"] = context_id;
        j["status"] = status.to_json();
        j["final"] = final;
        return j;
    }
};

// TaskArtifactUpdateEvent (SSE event)
struct task_artifact_update_event {
    std::string task_id;
    std::string context_id;
    artifact art;

    json to_json() const {
        json j;
        j["taskId"] = task_id;
        j["contextId"] = context_id;
        j["artifact"] = art.to_json();
        return j;
    }
};

// Agent skill (for AgentCard)
struct agent_skill {
    std::string id;
    std::string name;
    std::optional<std::string> description;
    std::optional<std::vector<std::string>> tags;
    std::optional<std::vector<std::string>> examples;
    std::optional<std::vector<std::string>> input_modes;
    std::optional<std::vector<std::string>> output_modes;

    json to_json() const {
        json j;
        j["id"] = id;
        j["name"] = name;
        if (description) {
            j["description"] = *description;
        }
        if (tags) {
            j["tags"] = *tags;
        }
        if (examples) {
            j["examples"] = *examples;
        }
        if (input_modes) {
            j["inputModes"] = *input_modes;
        }
        if (output_modes) {
            j["outputModes"] = *output_modes;
        }
        return j;
    }
};

// Agent capabilities
struct agent_capabilities {
    bool streaming = true;
    bool push_notifications = false;
    bool state_transition_history = true;

    json to_json() const {
        return json{
            {"streaming", streaming},
            {"pushNotifications", push_notifications},
            {"stateTransitionHistory", state_transition_history}
        };
    }
};

// AgentCard (discovery)
struct agent_card {
    std::string name = "llama-agent";
    std::optional<std::string> description;
    std::optional<std::string> url;
    std::optional<std::string> provider;
    std::optional<std::string> version;
    std::optional<std::string> documentation_url;
    agent_capabilities capabilities;
    std::optional<std::vector<std::string>> default_input_modes;
    std::optional<std::vector<std::string>> default_output_modes;
    std::optional<std::vector<agent_skill>> skills;
    std::optional<std::vector<std::string>> supported_content_types;

    json to_json() const {
        json j;
        j["name"] = name;
        if (description) {
            j["description"] = *description;
        }
        if (url) {
            j["url"] = *url;
        }
        if (provider) {
            j["provider"] = *provider;
        }
        if (version) {
            j["version"] = *version;
        }
        if (documentation_url) {
            j["documentationUrl"] = *documentation_url;
        }
        j["capabilities"] = capabilities.to_json();
        if (default_input_modes) {
            j["defaultInputModes"] = *default_input_modes;
        }
        if (default_output_modes) {
            j["defaultOutputModes"] = *default_output_modes;
        }
        if (skills) {
            j["skills"] = json::array();
            for (const auto & s : *skills) {
                j["skills"].push_back(s.to_json());
            }
        }
        if (supported_content_types) {
            j["supportedContentTypes"] = *supported_content_types;
        }
        return j;
    }
};

// A2A Error response
struct a2a_error {
    std::string code;
    std::string message;
    std::optional<json> data;

    json to_json() const {
        json j;
        j["error"] = json{
            {"code", code},
            {"message", message}
        };
        if (data) {
            j["error"]["data"] = *data;
        }
        return j;
    }
};

// Standard error codes
inline a2a_error task_not_found(const std::string & task_id) {
    return {"TaskNotFound", "Task not found: " + task_id, std::nullopt};
}

inline a2a_error invalid_request(const std::string & msg) {
    return {"InvalidRequest", msg, std::nullopt};
}

inline a2a_error task_not_cancelable(const std::string & task_id) {
    return {"TaskNotCancelable", "Task cannot be cancelled: " + task_id, std::nullopt};
}

inline a2a_error unsupported_operation(const std::string & msg) {
    return {"UnsupportedOperation", msg, std::nullopt};
}

inline a2a_error internal_error(const std::string & msg) {
    return {"InternalError", msg, std::nullopt};
}

} // namespace a2a
