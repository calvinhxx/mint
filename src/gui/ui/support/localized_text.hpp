#pragma once

#include "mint/localization/localization.hpp"

#include <QString>

namespace mint::ui {

inline QString text(localization::Message id,
                    std::initializer_list<localization::Argument> arguments = {}) {
    return QString::fromUtf8(localization::message(id, arguments));
}

} // namespace mint::ui
