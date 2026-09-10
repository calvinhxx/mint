#include "../command_sandbox_backend.hpp"

#include "mint/localization/localization.hpp"

#include <stdexcept>
#include <utility>

namespace mint::command_detail::native {

SandboxConfig build_sandbox_config(const std::filesystem::path&,
                                   const std::unordered_map<std::string, std::filesystem::path>&,
                                   std::vector<std::filesystem::path>,
                                   std::vector<std::filesystem::path>) {
    throw std::invalid_argument(
        localization::message(localization::Message::command_sandbox_backend_unavailable));
}

} // namespace mint::command_detail::native

namespace mint::command_detail {

ProcessResult execute_sandboxed_process(ProcessRequest request, const SandboxConfig&) {
    return execute_process(std::move(request));
}

} // namespace mint::command_detail
