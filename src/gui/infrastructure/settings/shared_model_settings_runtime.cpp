#include "shared_model_settings_runtime.hpp"

#include "shared_model_settings_operations.hpp"

#include <QFileInfo>
#include <QFutureWatcher>
#include <QPointer>
#include <QtConcurrentRun>

namespace mint::infrastructure::desktop {
namespace {

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
      state_directory_(state_directory.empty() ? std::filesystem::path{}
                                               : std::filesystem::absolute(state_directory)),
      legacy_config_(std::move(legacy_config)) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(120);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { debounce_.start(); });
    const auto notify_change = [this] {
        const auto changed = callbacks_.changed;
        if (changed) {
            changed();
        }
    };
    connect(&debounce_, &QTimer::timeout, this, notify_change);
    // EN: Retry only when the directory is missing or the OS refuses a watch.
    // ZH-CN: 仅在目录缺失或系统无法建立监视时重试。
    watch_retry_.setInterval(2000);
    connect(&watch_retry_, &QTimer::timeout, this, notify_change);
}

void SharedModelSettingsRuntime::set_model_settings_callbacks(
    mint::desktop::ModelSettingsCallbacks callbacks) {
    ++subscription_;
    callbacks_ = std::move(callbacks);
    pending_inspection_.reset();
    pending_mutation_.reset();
    if (!callbacks_.inspected && !callbacks_.saved) {
        watch_retry_.stop();
        debounce_.stop();
        if (!watcher_.directories().isEmpty()) {
            (void)watcher_.removePaths(watcher_.directories());
        }
        registered_directory_.clear();
    }
}

void SharedModelSettingsRuntime::set_legacy_imported_callback(std::function<bool()> callback) {
    legacy_imported_callback_ = std::move(callback);
}

void SharedModelSettingsRuntime::inspect_model(mint::desktop::ModelInspectionRequest request) {
    ++inspection_;
    pending_inspection_ = std::move(request);
    start_pending();
}

void SharedModelSettingsRuntime::update_model(mint::desktop::ModelUpdateRequest request) {
    auto operation = std::make_shared<ModelSettingsOperation>(std::move(request));
    if (pending_mutation_) {
        return;
    }
    ++inspection_;
    pending_inspection_.reset();
    pending_mutation_ = std::move(operation);
    start_pending();
}

void SharedModelSettingsRuntime::resolve_legacy_conflict(
    mint::desktop::LegacyModelResolution request) {
    if (pending_mutation_) {
        return;
    }
    ++inspection_;
    pending_inspection_.reset();
    pending_mutation_ = std::make_shared<ModelSettingsOperation>(std::move(request));
    start_pending();
}

void SharedModelSettingsRuntime::start_pending() {
    if (busy_ || (!pending_mutation_ && !pending_inspection_)) {
        return;
    }
    auto operation = std::move(pending_mutation_);
    if (!operation) {
        operation = std::make_shared<ModelSettingsOperation>(std::move(*pending_inspection_));
        pending_inspection_.reset();
    }
    busy_ = true;
    const auto subscription = subscription_;
    const auto inspection = inspection_;
    const auto saving = operation->is_mutation();
    auto* completion = new QFutureWatcher<ModelSettingsOperationResult>(this);
    connect(completion, &QFutureWatcher<ModelSettingsOperationResult>::finished, this,
            [this, completion, subscription, inspection, saving] {
                auto result = completion->result();
                completion->deleteLater();
                complete_operation(std::move(result), subscription, inspection, saving);
            });
    completion->setFuture(
        QtConcurrent::run([state_directory = state_directory_, legacy = legacy_config_,
                           operation = std::move(operation)] {
            return execute_model_settings_operation(state_directory, legacy, *operation);
        }));
}

void SharedModelSettingsRuntime::complete_operation(ModelSettingsOperationResult result,
                                                    std::size_t subscription,
                                                    std::size_t inspection, bool saving) {
    QPointer<SharedModelSettingsRuntime> alive(this);
    if (result.migrated) {
        const auto imported = legacy_imported_callback_;
        const bool retired = !imported || imported();
        if (!alive) {
            return;
        }
        if (retired) {
            legacy_config_.clear();
        } else {
            result.provider.reset();
            result.failure = mint::desktop::ModelSettingsFailure::unavailable;
        }
    }
    if (!result.shared_config.empty() && (callbacks_.inspected || callbacks_.saved)) {
        watch_directory(result.shared_config);
    }
    busy_ = false;
    // EN: New inspections replace older reads, but must not swallow an accepted save result.
    //     Copy callbacks before invoking them; a callback may detach or destroy this adapter.
    // ZH-CN: 新查询覆盖旧查询，但不能吞掉已接受的保存结果。
    //        调用前复制回调，因为回调可能解绑或销毁当前适配器。
    if (subscription == subscription_ && (saving || inspection == inspection_)) {
        if (result.conflict) {
            const auto callback = callbacks_.migration_conflict;
            if (callback) {
                callback(std::move(*result.conflict));
            }
        } else if (result.provider) {
            const auto callback = saving ? callbacks_.saved : callbacks_.inspected;
            if (callback) {
                callback(std::move(*result.provider));
            }
        } else {
            const auto callback = callbacks_.error;
            if (callback) {
                callback(result.failure);
            }
        }
    }
    if (alive) {
        start_pending();
    }
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
        if (!registered_directory_.isEmpty()) {
            (void)watcher_.removePath(registered_directory_);
        }
        registered_directory_.clear();
        if (!watcher_.addPath(directory)) {
            // EN: A failed native registration may leave a path entry; do not disable recovery.
            // ZH-CN: 原生注册失败后可能残留路径记录，不能因此关闭恢复重试。
            if (watcher_.directories().contains(directory)) {
                (void)watcher_.removePath(directory);
            }
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
