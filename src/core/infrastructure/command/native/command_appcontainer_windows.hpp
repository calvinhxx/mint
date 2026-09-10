#pragma once

#include <windows.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mint::command_detail {

class NativeProcessSandbox final {
  public:
    static std::shared_ptr<NativeProcessSandbox>
    create(const std::filesystem::path& workspace,
           std::vector<std::filesystem::path> allowed_executables,
           std::vector<std::filesystem::path> read_only_paths,
           std::vector<std::filesystem::path> denied_paths);

    ~NativeProcessSandbox();

    NativeProcessSandbox(const NativeProcessSandbox&) = delete;
    NativeProcessSandbox& operator=(const NativeProcessSandbox&) = delete;

    [[nodiscard]] PSID sid() const noexcept;
    [[nodiscard]] const std::filesystem::path& profile_directory() const noexcept;
    [[nodiscard]] const std::filesystem::path& temp_directory() const noexcept;

  private:
    struct ProtectedPathState {
        std::filesystem::path path;
        std::vector<std::byte> security_descriptor;
        bool dacl_protected = false;
        bool applied = false;
    };

    NativeProcessSandbox(const std::filesystem::path& workspace,
                         std::vector<std::filesystem::path> allowed_executables,
                         std::vector<std::filesystem::path> read_only_paths,
                         std::vector<std::filesystem::path> denied_paths);

    void initialize(const std::filesystem::path& workspace,
                    std::vector<std::filesystem::path> allowed_executables,
                    std::vector<std::filesystem::path> read_only_paths,
                    std::vector<std::filesystem::path> denied_paths);
    void cleanup() noexcept;

    std::wstring profile_name_;
    PSID sid_ = nullptr;
    std::filesystem::path profile_directory_;
    std::filesystem::path temp_directory_;
    std::vector<std::filesystem::path> acl_paths_;
    std::vector<ProtectedPathState> protected_paths_;
};

} // namespace mint::command_detail
