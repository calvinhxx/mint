#pragma once

#include "application/desktop_types.hpp"

namespace mint::infrastructure::desktop {

class TaskHistoryReader final {
  public:
    [[nodiscard]] static mint::desktop::TaskHistory
    read(const std::filesystem::path& task_directory);
};

} // namespace mint::infrastructure::desktop
