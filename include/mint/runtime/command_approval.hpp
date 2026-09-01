#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace mint {

struct CommandApprovalRequest {
    std::string program{};
    std::vector<std::string> args{};
    std::string cwd{};
    long timeout_seconds = 0;
};

enum class ApprovalDecisionKind {
    approved,
    rejected,
    run_cancelled,
    invalid_response,
    interaction_closed
};

/**
 * EN: Carries the approval outcome and its source across tool boundaries.
 * ZH-CN: 在工具边界之间完整传递审批结果及其来源。
 */
struct ApprovalDecision {
    ApprovalDecisionKind kind = ApprovalDecisionKind::rejected;

    ApprovalDecision() = default;
    ApprovalDecision(bool value) noexcept
        : kind(value ? ApprovalDecisionKind::approved : ApprovalDecisionKind::rejected) {}
    ApprovalDecision(ApprovalDecisionKind value) noexcept : kind(value) {}

    [[nodiscard]] bool approved() const noexcept {
        return kind == ApprovalDecisionKind::approved;
    }
    [[nodiscard]] bool rejected_by_user() const noexcept {
        return kind == ApprovalDecisionKind::rejected;
    }
    [[nodiscard]] bool cancels_run() const noexcept {
        return kind == ApprovalDecisionKind::run_cancelled;
    }
    [[nodiscard]] std::string_view source() const noexcept {
        switch (kind) {
        case ApprovalDecisionKind::approved:
        case ApprovalDecisionKind::rejected:
            return "user";
        case ApprovalDecisionKind::run_cancelled:
            return "run_cancelled";
        case ApprovalDecisionKind::invalid_response:
            return "invalid_response";
        case ApprovalDecisionKind::interaction_closed:
            return "interaction_closed";
        }
        return "invalid_response";
    }
    explicit operator bool() const noexcept {
        return approved();
    }
};

using CommandApproval = std::function<ApprovalDecision(const CommandApprovalRequest&)>;

} // namespace mint
