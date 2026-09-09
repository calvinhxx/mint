#pragma once

#include "mint/infrastructure/model_provider_client.hpp"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

namespace mint {

enum class ModelConfigErrorCode { invalid_input, invalid_config, conflict, unavailable };

// EN: Display-safe errors never include submitted JSON or credential values.
// ZH-CN: 可展示的错误消息绝不包含提交的 JSON 或凭据值。
class ModelConfigError : public std::runtime_error {
  public:
    ModelConfigError(ModelConfigErrorCode code, const std::string& message)
        : std::runtime_error(message), code_(code) {}
    [[nodiscard]] ModelConfigErrorCode code() const noexcept {
        return code_;
    }

  private:
    ModelConfigErrorCode code_;
};

struct ModelConfigSnapshot {
    // EN: Always redacted. Only load_runtime() may return a secret.
    // ZH-CN: 始终脱敏；只有 load_runtime() 可以返回密钥。
    ModelProviderConfig config;
    std::filesystem::path path;
    std::string revision;
    bool configured = false;
    bool saved_api_key = false;
    bool credential_available = false;
    bool is_override = false;
};

struct ModelConfigUpdate {
    // EN: Empty means the shared configuration must not exist yet.
    // ZH-CN: 空值表示共享配置必须尚未存在。
    std::string expected_revision;
    ModelAdapter adapter = ModelAdapter::chat_completions;
    std::string base_url;
    std::string model;
    // EN: Absence preserves the current key only for the same endpoint and adapter.
    // ZH-CN: 未提供时，仅在端点和适配器相同时保留现有密钥。
    std::optional<std::string> api_key;
    bool remove_api_key = false;
};

// EN: The sole model/credential persistence boundary, shared by CLI and GUI adapters.
//     Shared storage operations acquire a cross-process lock. Call outside the GUI thread.
// ZH-CN: CLI 和 GUI 适配器共用的模型与凭据持久化入口。
//        共享存储操作获取跨进程锁；应在 GUI 线程以外调用。
class ModelConfigService final {
  public:
    explicit ModelConfigService(std::filesystem::path state_directory = {});

    // EN: Create/verify private storage before other adapters create child directories.
    // ZH-CN: 在其他适配器创建子目录前，创建并校验私有存储目录。
    void prepare_storage() const;

    // EN: Resolve shared paths only on demand; standalone overrides need no user state directory.
    // ZH-CN: 按需解析共享路径；独立覆盖配置不依赖用户状态目录。
    [[nodiscard]] std::filesystem::path config_path() const;
    [[nodiscard]] std::filesystem::path credentials_path() const;
    [[nodiscard]] ModelConfigSnapshot
    inspect(const std::filesystem::path& override_path = {}) const;
    [[nodiscard]] ModelConfigSnapshot update(ModelConfigUpdate update);
    // EN: Import into an empty store, or replace an existing configuration only with its
    //     current nonempty revision. Originals are never changed or removed.
    // ZH-CN: 默认仅导入空存储；仅显式提供当前非空版本号时才替换已有配置。
    //        绝不修改或删除原文件。
    [[nodiscard]] ModelConfigSnapshot
    migrate_legacy(const std::filesystem::path& legacy_path,
                   std::optional<std::string> expected_revision = std::nullopt);
    // EN: Reads shared settings unless an explicit per-run override is supplied.
    //     Never discovers or imports workspace files. Credentials form a per-run snapshot.
    // ZH-CN: 默认读取共享设置，也可显式指定单次覆盖；绝不发现或导入工作区文件。
    //        返回的文件凭据是当前运行的快照。
    // EN: Overrides are read directly without creating or locking shared storage. Optional
    //     metadata describes the same selected file; its shared revision is best-effort only.
    // ZH-CN: 直接读取覆盖文件，不创建或锁定共享存储。可选元数据来自同一份选中文件，
    //        其中共享版本号仅在可读取时提供。
    [[nodiscard]] ModelProviderConfig load_runtime(const std::filesystem::path& override_path = {},
                                                   ModelConfigSnapshot* inspected = nullptr) const;

  private:
    struct StoragePaths {
        std::filesystem::path directory;
        std::filesystem::path config;
        std::filesystem::path credentials;
    };
    [[nodiscard]] StoragePaths storage_paths() const;
    std::filesystem::path state_directory_;
};

} // namespace mint
