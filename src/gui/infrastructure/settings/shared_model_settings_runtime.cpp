#include "shared_model_settings_runtime.hpp"

#include "mint/infrastructure/model_config_service.hpp"

#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrentRun>

#include <algorithm>

namespace mint::infrastructure::desktop {
namespace {

struct OperationResult {
    std::optional<mint::desktop::ProviderDescriptor> provider;
    std::optional<mint::desktop::LegacyModelConflict> conflict;
    mint::desktop::ModelSettingsFailure failure = mint::desktop::ModelSettingsFailure::unavailable;
    bool migrated = false;
    std::filesystem::path shared_config;
};

mint::desktop::ProviderDescriptor describe(const ModelConfigSnapshot& snapshot) {
    return {.provider = snapshot.configured
                            ? std::string(model_provider_name(
                                  resolve_model_provider_profile(snapshot.config).provider))
                            : std::string{},
            .model = snapshot.configured ? snapshot.config.model : std::string{},
            .adapter = std::string(model_adapter_name(snapshot.config.adapter)),
            .endpoint = snapshot.configured ? snapshot.config.api_url : std::string{},
            .config = snapshot.path,
            .credential_available = snapshot.credential_available,
            .revision = snapshot.revision,
            .configured = snapshot.configured,
            .saved_api_key = snapshot.saved_api_key,
            .is_override = snapshot.is_override};
}

mint::desktop::LegacyModelConflict describe_conflict(ModelConfigService& service,
                                                     const ModelConfigSnapshot& shared,
                                                     const std::filesystem::path& legacy) {
    mint::desktop::LegacyModelConflict result{.shared = describe(shared), .legacy_config = legacy};
    try {
        result.legacy = describe(service.inspect(legacy));
    } catch (...) {
        // EN: An unavailable old source still requires an explicit choice to keep shared settings.
        // ZH-CN: 旧来源不可用时，仍须明确选择保留共享配置。
    }
    return result;
}

ModelAdapter adapter_from(mint::desktop::CustomModelAdapter adapter) {
    switch (adapter) {
    case mint::desktop::CustomModelAdapter::chat_completions:
        return ModelAdapter::chat_completions;
    case mint::desktop::CustomModelAdapter::responses:
        return ModelAdapter::responses;
    case mint::desktop::CustomModelAdapter::anthropic_messages:
        return ModelAdapter::anthropic_messages;
    }
    return ModelAdapter::chat_completions;
}

QString qstring_from_path(const std::filesystem::path& path) {
#if defined(_WIN32)
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromUtf8(path.native());
#endif
}

} // namespace

SharedModelSettingsRuntime::SharedModelSettingsRuntime(std::filesystem::path state_directory,
                                                       std::filesystem::path legacy_config,
                                                       QObject* parent)
    : QObject(parent),
      state_directory_(state_directory.empty()
                           ? std::filesystem::path{}
                           : std::filesystem::absolute(state_directory).lexically_normal()),
      legacy_config_(std::move(legacy_config)) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(120);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { debounce_.start(); });
    connect(&debounce_, &QTimer::timeout, this, [this] {
        if (callbacks_.changed)
            callbacks_.changed();
    });
    // EN: Retry only when the directory is missing or the OS refuses a watch.
    // ZH-CN: 仅在目录缺失或系统无法建立监视时重试。
    watch_retry_.setInterval(2000);
    connect(&watch_retry_, &QTimer::timeout, this, [this] {
        if (callbacks_.changed)
            callbacks_.changed();
    });
}

void SharedModelSettingsRuntime::set_model_settings_callbacks(
    mint::desktop::ModelSettingsCallbacks callbacks) {
    ++generation_;
    callbacks_ = std::move(callbacks);
    if (!callbacks_.inspected && !callbacks_.saved) {
        pending_inspection_.reset();
        if (pending_update_ && pending_update_->api_key) {
            std::fill(pending_update_->api_key->begin(), pending_update_->api_key->end(), '\0');
        }
        pending_update_.reset();
        pending_resolution_.reset();
        watch_retry_.stop();
        debounce_.stop();
    }
}

void SharedModelSettingsRuntime::set_legacy_imported_callback(std::function<bool()> callback) {
    legacy_imported_callback_ = std::move(callback);
}

void SharedModelSettingsRuntime::inspect_model(mint::desktop::ModelInspectionRequest request) {
    ++generation_;
    pending_inspection_ = std::move(request);
    start_pending();
}

void SharedModelSettingsRuntime::update_model(mint::desktop::ModelUpdateRequest request) {
    if (pending_update_ || pending_resolution_) {
        if (request.api_key)
            std::fill(request.api_key->begin(), request.api_key->end(), '\0');
        return;
    }
    ++generation_;
    pending_inspection_.reset();
    pending_update_ = std::move(request);
    start_pending();
}

void SharedModelSettingsRuntime::resolve_legacy_conflict(
    mint::desktop::LegacyModelResolution request) {
    if (pending_update_ || pending_resolution_)
        return;
    ++generation_;
    pending_inspection_.reset();
    pending_resolution_ = std::move(request);
    start_pending();
}

void SharedModelSettingsRuntime::start_pending() {
    if (busy_ || (!pending_update_ && !pending_inspection_ && !pending_resolution_))
        return;
    busy_ = true;
    const auto generation = generation_;
    const auto saving = pending_update_.has_value() || pending_resolution_.has_value();
    auto update = std::move(pending_update_);
    auto inspection = std::move(pending_inspection_);
    auto resolution = std::move(pending_resolution_);
    pending_update_.reset();
    pending_inspection_.reset();
    pending_resolution_.reset();
    auto* completion = new QFutureWatcher<OperationResult>(this);
    connect(completion, &QFutureWatcher<OperationResult>::finished, this,
            [this, completion, generation, saving] {
                auto result = completion->result();
                completion->deleteLater();
                busy_ = false;
                if (result.migrated) {
                    if (!legacy_imported_callback_ || legacy_imported_callback_()) {
                        legacy_config_.clear();
                    } else {
                        result.provider.reset();
                        result.failure = mint::desktop::ModelSettingsFailure::unavailable;
                    }
                }
                if (!result.shared_config.empty() && (callbacks_.inspected || callbacks_.saved)) {
                    watch_directory(result.shared_config);
                }
                if (generation == generation_) {
                    if (result.conflict) {
                        if (callbacks_.migration_conflict)
                            callbacks_.migration_conflict(*result.conflict);
                    } else if (result.provider) {
                        const auto& callback = saving ? callbacks_.saved : callbacks_.inspected;
                        if (callback)
                            callback(*result.provider);
                    } else if (callbacks_.error) {
                        callbacks_.error(result.failure);
                    }
                }
                start_pending();
            });
    completion->setFuture(QtConcurrent::run([state_directory = state_directory_,
                                             legacy = legacy_config_, update = std::move(update),
                                             inspection = std::move(inspection),
                                             resolution = std::move(resolution)]() mutable {
        OperationResult result;
        try {
            ModelConfigService service(state_directory);
            result.shared_config = service.config_path();
            if (update) {
                result.provider = describe(
                    service.update({.expected_revision = std::move(update->expected_revision),
                                    .adapter = adapter_from(update->adapter),
                                    .base_url = std::move(update->base_url),
                                    .model = std::move(update->model),
                                    .api_key = std::move(update->api_key),
                                    .remove_api_key = update->remove_api_key}));
                result.migrated = !legacy.empty();
            } else if (resolution) {
                if (legacy.empty() || resolution->expected_revision.empty()) {
                    throw ModelConfigError(ModelConfigErrorCode::invalid_input,
                                           "No previous GUI configuration is selected.");
                }
                if (resolution->choice == mint::desktop::LegacyModelChoice::import_legacy) {
                    result.provider =
                        describe(service.migrate_legacy(legacy, resolution->expected_revision));
                } else {
                    const auto shared = service.inspect();
                    if (!shared.configured || shared.revision != resolution->expected_revision) {
                        throw ModelConfigError(ModelConfigErrorCode::conflict,
                                               "Shared model settings changed before selection.");
                    }
                    result.provider = describe(shared);
                }
                result.migrated = true;
            } else {
                auto config = inspection->config;
                if (config.empty()) {
                    auto snapshot = service.inspect();
                    if (!legacy.empty()) {
                        // EN: Preserve the old explicit choice until import or user resolution.
                        //     Never discover workspace files or choose a profile by modification
                        //     time.
                        // ZH-CN:
                        // 导入或用户明确选择前保留旧入口；不发现工作区文件，也不按修改时间猜配置。
                        if (!snapshot.configured) {
                            try {
                                snapshot = service.migrate_legacy(legacy);
                                result.migrated = true;
                            } catch (const ModelConfigError& error) {
                                if (error.code() != ModelConfigErrorCode::conflict)
                                    throw;
                                snapshot = service.inspect();
                                if (!snapshot.configured)
                                    throw;
                            }
                        }
                        if (!result.migrated) {
                            result.conflict = describe_conflict(service, snapshot, legacy);
                        }
                    }
                    result.provider = describe(snapshot);
                } else {
                    if (config.is_relative())
                        config = inspection->root / config;
                    result.provider = describe(service.inspect(config));
                }
            }
        } catch (const ModelConfigError& error) {
            result.failure = error.code() == ModelConfigErrorCode::conflict
                                 ? mint::desktop::ModelSettingsFailure::conflict
                             : error.code() == ModelConfigErrorCode::invalid_input
                                 ? mint::desktop::ModelSettingsFailure::invalid_input
                                 : mint::desktop::ModelSettingsFailure::unavailable;
        } catch (...) {
            // EN: Parser and filesystem diagnostics may contain sensitive source text.
            // ZH-CN: 解析器或文件系统诊断可能包含敏感原文。
            result.failure = mint::desktop::ModelSettingsFailure::unavailable;
        }
        return result;
    }));
}

void SharedModelSettingsRuntime::watch_directory(const std::filesystem::path& config_path) {
    // EN: Watch the shared directory; overrides are read-only invocation inputs.
    // ZH-CN: 监视共享目录，覆盖配置仅为本次调用的只读输入。
    const auto requested_directory = qstring_from_path(config_path.parent_path());
    // EN: Watch the physical directory so macOS /var aliases use the same native subscription.
    // ZH-CN: 监视物理目录，让 macOS 的 /var 别名指向同一个原生订阅。
    const auto canonical_directory = QFileInfo(requested_directory).canonicalFilePath();
    const auto directory =
        canonical_directory.isEmpty() ? requested_directory : canonical_directory;
    const bool was_watched =
        registered_directory_ == directory && watcher_.directories().contains(directory);
    if (!was_watched) {
        if (!registered_directory_.isEmpty())
            (void)watcher_.removePath(registered_directory_);
        registered_directory_.clear();
        if (!watcher_.addPath(directory)) {
            // EN: A failed native registration may leave a path entry; do not disable recovery.
            // ZH-CN: 原生注册失败后可能残留路径记录，不能因此关闭恢复重试。
            if (watcher_.directories().contains(directory))
                (void)watcher_.removePath(directory);
            watch_retry_.start();
            return;
        }
        registered_directory_ = directory;
    }
    if (watcher_.directories().contains(directory)) {
        watch_retry_.stop();
        if (!was_watched) {
            // EN: Re-read after subscribing to cover saves made before the watch was installed.
            // ZH-CN: 订阅后再读一次，覆盖监听安装前发生的保存。
            debounce_.start();
        }
    } else {
        watch_retry_.start();
    }
}

} // namespace mint::infrastructure::desktop
