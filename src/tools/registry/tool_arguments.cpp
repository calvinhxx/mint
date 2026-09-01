#include "tool_arguments.hpp"

#include "mint/localization/localization.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace mint::tools::detail {

using localization::Message;
using localization::Placeholder;

std::string dump_json(const Json& value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

Json error_result(std::string message) {
    return {{"ok", false}, {"error", std::move(message)}};
}

void require_only_fields(const Json& arguments, std::string_view context,
                         std::initializer_list<std::string_view> allowed_fields) {
    if (!arguments.is_object()) {
        throw std::invalid_argument(localization::message(
            Message::tools_argument_object, {localization::arg(Placeholder::context, context)}));
    }
    for (auto field = arguments.begin(); field != arguments.end(); ++field) {
        const auto allowed = std::find(allowed_fields.begin(), allowed_fields.end(), field.key()) !=
                             allowed_fields.end();
        if (!allowed) {
            throw std::invalid_argument(
                localization::message(Message::tools_argument_unknown_field,
                                      {localization::arg(Placeholder::context, context),
                                       localization::arg(Placeholder::field, field.key())}));
        }
    }
}

std::string require_string(const Json& arguments, std::string_view name) {
    const std::string key(name);
    if (!arguments.contains(key) || !arguments.at(key).is_string()) {
        throw std::invalid_argument(localization::message(
            Message::tools_argument_string, {localization::arg(Placeholder::name, key)}));
    }
    return arguments.at(key).get<std::string>();
}

} // namespace mint::tools::detail
