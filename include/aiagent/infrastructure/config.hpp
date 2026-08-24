#pragma once

#include "aiagent/infrastructure/chat_completions_client.hpp"

#include <filesystem>

namespace aiagent {

ChatCompletionsConfig load_chat_completions_config(const std::filesystem::path& config_path);

} // namespace aiagent
