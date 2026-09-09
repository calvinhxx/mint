#pragma once

#include "application/desktop_types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mint::infrastructure::desktop {

class MintCliCommandBuilder final {
  public:
    explicit MintCliCommandBuilder(std::filesystem::path state_root = {});

    [[nodiscard]] std::vector<std::string> status(const std::filesystem::path& root,
                                                  const std::string& language) const;
    [[nodiscard]] std::vector<std::string> run(const mint::desktop::RunRequest& request,
                                               const std::filesystem::path& cancel_file,
                                               std::string_view api_key_environment = {}) const;
    [[nodiscard]] std::vector<std::string> resume(const mint::desktop::ResumeRequest& request,
                                                  const std::filesystem::path& cancel_file,
                                                  std::string_view api_key_environment = {}) const;

  private:
    void append_common(std::vector<std::string>& arguments, const std::string& language) const;
    static void append_api_key_environment(std::vector<std::string>& arguments,
                                           std::string_view environment);
    void append_state_root(std::vector<std::string>& arguments) const;

    std::filesystem::path state_root_;
};

} // namespace mint::infrastructure::desktop
