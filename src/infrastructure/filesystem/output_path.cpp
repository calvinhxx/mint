#include "output_path.hpp"

#include "mint/localization/localization.hpp"

#include <stdexcept>
#include <string>

namespace mint::infrastructure_detail {

using localization::Message;
using localization::Placeholder;

std::filesystem::path validated_output_path(std::filesystem::path path, std::string_view label,
                                            HardLinkPolicy hard_link_policy) {
    if (path.empty() || path.filename().empty()) {
        throw std::invalid_argument(localization::message(
            Message::filesystem_output_empty, {localization::arg(Placeholder::label, label)}));
    }

    std::error_code error;
    auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::current_path();
    parent = std::filesystem::weakly_canonical(parent, error);
    if (error || !std::filesystem::is_directory(parent)) {
        throw std::invalid_argument(
            localization::message(Message::filesystem_output_parent_missing,
                                  {localization::arg(Placeholder::label, label)}));
    }

    const auto resolved = parent / path.filename();
    const auto status = std::filesystem::symlink_status(resolved, error);
    if (!error && std::filesystem::is_symlink(status)) {
        throw std::invalid_argument(localization::message(
            Message::filesystem_output_symlink, {localization::arg(Placeholder::label, label)}));
    }
    if (!error && std::filesystem::exists(status) && !std::filesystem::is_regular_file(status)) {
        throw std::invalid_argument(
            localization::message(Message::filesystem_output_not_regular_file,
                                  {localization::arg(Placeholder::label, label)}));
    }
    if (!error && hard_link_policy == HardLinkPolicy::reject &&
        std::filesystem::is_regular_file(status)) {
        const auto links = std::filesystem::hard_link_count(resolved, error);
        if (!error && links > 1) {
            throw std::invalid_argument(
                localization::message(Message::filesystem_output_hard_link,
                                      {localization::arg(Placeholder::label, label)}));
        }
    }
    return resolved;
}

} // namespace mint::infrastructure_detail
