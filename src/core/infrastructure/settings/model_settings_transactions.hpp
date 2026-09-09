#pragma once

#include "model_settings_files.hpp"

namespace mint::model_settings_detail {

// EN: Call with the shared store locked; each transaction checks the revision before writing.
// ZH-CN: 调用前须持有共享存储锁；每次事务都在写入前校验版本号。
[[nodiscard]] ModelConfigSnapshot update_locked(const std::filesystem::path& config_path,
                                                const std::filesystem::path& credentials_path,
                                                ModelConfigUpdate update, std::string endpoint);
[[nodiscard]] ModelConfigSnapshot migrate_locked(
    const std::filesystem::path& config_path, const std::filesystem::path& credentials_path,
    const std::filesystem::path& legacy_path, const std::optional<std::string>& expected_revision);

} // namespace mint::model_settings_detail
