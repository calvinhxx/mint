#pragma once

#include "application/model_settings_runtime.hpp"

#include <utility>
#include <variant>

namespace mint::infrastructure::desktop {

// EN: One request owns its draft key until execution or cancellation erases it.
// ZH-CN: 每个请求持有自己的草稿密钥，执行或取消时擦除。
struct ModelSettingsOperation {
    using Request =
        std::variant<mint::desktop::ModelInspectionRequest, mint::desktop::ModelUpdateRequest,
                     mint::desktop::LegacyModelResolution>;
    explicit ModelSettingsOperation(Request value) : request(std::move(value)) {}
    ~ModelSettingsOperation();
    ModelSettingsOperation(const ModelSettingsOperation&) = delete;
    ModelSettingsOperation& operator=(const ModelSettingsOperation&) = delete;

    void clear_api_key() noexcept;
    [[nodiscard]] bool is_mutation() const noexcept;
    Request request;
};

struct ModelSettingsOperationResult {
    std::optional<mint::desktop::ProviderDescriptor> provider;
    std::optional<mint::desktop::LegacyModelConflict> conflict;
    mint::desktop::ModelSettingsFailure failure = mint::desktop::ModelSettingsFailure::unavailable;
    bool migrated = false;
    std::filesystem::path shared_config;
};

// EN: Synchronous storage work only; it never captures Qt objects or invokes UI callbacks.
// ZH-CN: 仅同步执行存储操作，不捕获 Qt 对象，也不调用 UI 回调。
[[nodiscard]] ModelSettingsOperationResult
execute_model_settings_operation(const std::filesystem::path& state_directory,
                                 const std::filesystem::path& legacy,
                                 ModelSettingsOperation& operation);

} // namespace mint::infrastructure::desktop
