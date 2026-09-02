#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mint::command_detail {

struct SandboxConfig {
    std::filesystem::path executable{};
    std::vector<std::string> arguments{};
    std::string backend = "none";
    bool sets_working_directory = false;
    bool uses_native_process_sandbox = false;
    std::vector<std::filesystem::path> allowed_executables{};
    std::vector<std::filesystem::path> read_only_paths{};
    std::vector<std::filesystem::path> denied_paths{};
};

#if defined(__APPLE__)
class CommandScratchDirectory final {
  public:
    explicit CommandScratchDirectory(const std::filesystem::path& workspace);
    ~CommandScratchDirectory();

    CommandScratchDirectory(const CommandScratchDirectory&) = delete;
    CommandScratchDirectory& operator=(const CommandScratchDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    void verify_cleanup_target() const;
    void confirm_cleanup();

  private:
    struct State;
    std::unique_ptr<State> state_;
};
#endif

[[nodiscard]] std::filesystem::path resolve_program(const std::string& requested);

[[nodiscard]] SandboxConfig build_sandbox_config(
    bool required, const std::filesystem::path& root,
    const std::unordered_map<std::string, std::filesystem::path>& resolved_programs,
    std::vector<std::filesystem::path> read_only_paths,
    std::vector<std::filesystem::path> denied_read_paths);

} // namespace mint::command_detail
