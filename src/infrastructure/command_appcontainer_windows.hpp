#pragma once

#if !defined(_WIN32)
#error "command_appcontainer_windows.hpp is only available on Windows"
#endif

#include <windows.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mint::command_detail {

class WindowsAppContainer final {
  public:
    static std::shared_ptr<WindowsAppContainer>
    create(const std::filesystem::path& workspace,
           std::vector<std::filesystem::path> allowed_executables,
           std::vector<std::filesystem::path> denied_paths);

    ~WindowsAppContainer();

    WindowsAppContainer(const WindowsAppContainer&) = delete;
    WindowsAppContainer& operator=(const WindowsAppContainer&) = delete;

    [[nodiscard]] PSID sid() const noexcept;
    [[nodiscard]] const std::filesystem::path& profile_directory() const noexcept;
    [[nodiscard]] const std::filesystem::path& temp_directory() const noexcept;

  private:
    WindowsAppContainer(const std::filesystem::path& workspace,
                        std::vector<std::filesystem::path> allowed_executables,
                        std::vector<std::filesystem::path> denied_paths);

    void initialize(const std::filesystem::path& workspace,
                    std::vector<std::filesystem::path> allowed_executables,
                    std::vector<std::filesystem::path> denied_paths);
    void cleanup() noexcept;

    std::wstring profile_name_;
    PSID sid_ = nullptr;
    std::filesystem::path profile_directory_;
    std::filesystem::path temp_directory_;
    std::vector<std::filesystem::path> acl_paths_;
};

} // namespace mint::command_detail
