#pragma once

#include "chat.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using json = nlohmann::ordered_json;

std::string agent_render_tool_protocol_prompt(const std::vector<common_chat_tool> & tools);

json agent_inject_tool_protocol_prompt(
    const json & messages,
    const std::vector<common_chat_tool> & tools);

common_chat_msg agent_parse_tool_protocol_response(
    const std::string & content,
    const std::string & reasoning_content,
    const std::vector<common_chat_tool> & tools);
