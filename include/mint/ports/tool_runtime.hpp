#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "mint/domain/model.hpp"
#include "mint/domain/runtime_settings.hpp"

namespace mint {

enum class ChangeTransactionRecovery { none, rolled_back, committed };

struct ToolCapabilities {
    std::filesystem::path workspace_root;
    bool write_enabled = false;
    std::vector<std::string> writable_paths;
    bool commands_enabled = false;
    bool command_approval_required = false;
    bool change_set_approval_required = false;
    bool command_sandboxed = false;
    std::string command_sandbox_backend = "none";
    std::vector<std::string> allowed_programs;
    std::vector<std::string> command_recipes;
    std::string policy_fingerprint;
    ToolRuntimeSettings runtime;
    bool durable_change_transactions = false;
    std::string change_transaction_path;
};

class ToolExecutor {
  public:
    virtual ~ToolExecutor() = default;

    [[nodiscard]] virtual ToolCapabilities capabilities() const = 0;
    [[nodiscard]] virtual Json definitions() const = 0;
    [[nodiscard]] virtual std::string describe_call(const ToolCall& call) const = 0;
    [[nodiscard]] virtual std::string execute(const ToolCall& call) const = 0;
};

class WorkspaceLedger {
  public:
    virtual ~WorkspaceLedger() = default;

    [[nodiscard]] virtual bool has_workspace_changes() const = 0;
    [[nodiscard]] virtual bool workspace_integrity_failed() const noexcept = 0;
    [[nodiscard]] virtual Json workspace_change_snapshot() const = 0;
    [[nodiscard]] virtual Json workspace_change_state() const = 0;
    virtual void restore_workspace_change_state(const Json& state) = 0;
    [[nodiscard]] virtual std::optional<std::string> pending_change_transaction_id() const = 0;
    [[nodiscard]] virtual ChangeTransactionRecovery
    reconcile_change_transaction(const std::optional<std::string>& checkpoint_transaction_id) = 0;
    virtual void finalize_change_transaction() = 0;
};

class AgentTools : public ToolExecutor, public WorkspaceLedger {
  public:
    ~AgentTools() override = default;
};

} // namespace mint
