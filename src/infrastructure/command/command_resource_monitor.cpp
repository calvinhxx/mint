#include "command_resource_monitor.hpp"

#include "mint/localization/localization.hpp"

#include <stdexcept>
#include <system_error>

namespace mint::command_detail {

using localization::Message;
using localization::Placeholder;
namespace {

bool disappeared(const std::error_code& error) {
    return error == std::errc::no_such_file_or_directory;
}

} // namespace

bool workspace_disk_limit_exceeded(const std::filesystem::path& workspace,
                                   std::size_t limit_bytes) {
    if (limit_bytes == 0) {
        return false;
    }

    std::error_code error;
    if (!std::filesystem::is_directory(workspace, error) || error) {
        throw std::runtime_error(
            localization::message(Message::command_resources_disk_usage_failed));
    }

    std::size_t total = 0;
    std::filesystem::recursive_directory_iterator iterator(
        workspace, std::filesystem::directory_options::skip_permission_denied, error);
    if (error) {
        throw std::runtime_error(
            localization::message(Message::command_resources_iterate_failed,
                                  {localization::arg(Placeholder::error, error.message())}));
    }
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        error.clear();
        const auto status = iterator->symlink_status(error);
        if (!error && std::filesystem::is_regular_file(status)) {
            const auto bytes = iterator->file_size(error);
            if (!error) {
                if (bytes > limit_bytes - total) {
                    return true;
                }
                total += static_cast<std::size_t>(bytes);
            }
        }
        if (error && !disappeared(error)) {
            throw std::runtime_error(
                localization::message(Message::command_resources_read_failed,
                                      {localization::arg(Placeholder::error, error.message())}));
        }

        error.clear();
        iterator.increment(error);
        if (error && !disappeared(error)) {
            throw std::runtime_error(
                localization::message(Message::command_resources_iterate_failed,
                                      {localization::arg(Placeholder::error, error.message())}));
        }
        if (error) {
            error.clear();
        }
    }
    return false;
}

} // namespace mint::command_detail
