#pragma once

#include "application/desktop_types.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace mint::infrastructure::desktop {

struct StatusResponse {
    std::filesystem::path state_directory;
    std::vector<mint::desktop::TaskSummary> tasks;
};

enum class InteractionKind { task_ready, approval };

struct InteractionMessage {
    InteractionKind kind = InteractionKind::task_ready;
    std::string task_id;
    std::filesystem::path events_path;
    std::optional<mint::desktop::ApprovalRequest> approval;
};

class MintCliProtocol final {
  public:
    [[nodiscard]] static std::optional<StatusResponse> parse_status(std::string_view payload,
                                                                    std::string& error);
    [[nodiscard]] static std::optional<mint::desktop::RunEvent>
    parse_event(std::string_view payload);
    [[nodiscard]] static std::optional<mint::desktop::TaskResult>
    parse_result(std::string_view payload);
    [[nodiscard]] static std::optional<InteractionMessage>
    parse_interaction(std::string_view payload);
    [[nodiscard]] static std::string parse_error(std::string_view payload);
    [[nodiscard]] static std::string approval_decision(bool approved, std::string_view source);
};

} // namespace mint::infrastructure::desktop
