#include "mint_cli_process.hpp"

#include "infrastructure/cli/mint_cli_protocol.hpp"
#include "infrastructure/cli/task_history_reader.hpp"

#include "mint/localization/localization.hpp"

#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QPointer>
#include <QStandardPaths>
#include <QStringList>
#include <QUuid>

#include <QtConcurrentRun>

#include <memory>
#include <utility>

namespace mint::infrastructure::desktop {
namespace {

QString qstring_from_path(const std::filesystem::path& path) {
#if defined(_WIN32)
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromUtf8(path.native());
#endif
}

std::filesystem::path path_from_qstring(const QString& path) {
#if defined(_WIN32)
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().constData());
#endif
}

QStringList qstring_arguments(const std::vector<std::string>& arguments) {
    QStringList result;
    result.reserve(static_cast<qsizetype>(arguments.size()));
    for (const auto& argument : arguments) {
        result.push_back(
            QString::fromUtf8(argument.data(), static_cast<qsizetype>(argument.size())));
    }
    return result;
}

void consume_lines(QByteArray& buffer, const std::function<bool(const QByteArray&)>& consume) {
    for (;;) {
        const auto newline = buffer.indexOf('\n');
        if (newline < 0) {
            return;
        }
        const auto line = buffer.left(newline).trimmed();
        buffer.remove(0, newline + 1);
        if (!line.isEmpty() && !consume(line)) {
            return;
        }
    }
}

} // namespace

MintCliProcess::MintCliProcess(std::filesystem::path executable, std::filesystem::path state_root,
                               QObject* parent)
    : QObject(parent), executable_(std::move(executable)), command_builder_(std::move(state_root)),
      event_stream_(
          [this](std::vector<mint::desktop::RunEvent> events) {
              if (const auto received = callbacks_.events_received) {
                  received(std::move(events));
              } else if (const auto received_one = callbacks_.event_received) {
                  const QPointer<MintCliProcess> lifetime(this);
                  const auto generation = callbacks_generation_;
                  for (auto& event : events) {
                      received_one(std::move(event));
                      if (!lifetime || generation != callbacks_generation_) {
                          return;
                      }
                  }
              }
          },
          [this] { complete_run(); }) {
    connect(&run_process_, &QProcess::readyReadStandardOutput, this,
            [this] { consume_standard_output(); });
    // EN: Deliver external callbacks after Qt finishes accessing the process in its signal stack.
    // ZH-CN: 等 Qt 退出当前进程信号调用栈后再交付外部回调，避免回调销毁仍被 Qt 使用的进程。
    connect(
        &run_process_, &QProcess::readyReadStandardError, this,
        [this] { consume_standard_error(); }, Qt::QueuedConnection);
    connect(
        &run_process_, &QProcess::finished, this,
        [this](int code, QProcess::ExitStatus status) { handle_run_finished(code, status); },
        Qt::QueuedConnection);
    // EN: QProcess still accesses itself after emitting FailedToStart; external callbacks may
    //     synchronously destroy this runtime and its process, so deliver the failure afterward.
    // ZH-CN: QProcess 发出 FailedToStart 后仍会访问自身；外部回调可能立即销毁运行时和进程，
    //        因此应在 Qt 完成当前信号调用后再交付失败。
    connect(
        &run_process_, &QProcess::errorOccurred, this,
        [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                run_active_ = false;
                pending_exit_.reset();
                event_stream_.reset();
                QFile::remove(qstring_from_path(cancel_file_));
                emit_error(mint::desktop::RuntimeOperation::task_run, run_process_.errorString());
            }
        },
        Qt::QueuedConnection);
}

MintCliProcess::~MintCliProcess() {
    callbacks_ = {};
    run_active_ = false;
    event_stream_.reset();
    if (run_process_.state() == QProcess::NotRunning) {
        return;
    }
    cancel();
    if (!run_process_.waitForFinished(800)) {
        run_process_.terminate();
        if (!run_process_.waitForFinished(400)) {
            run_process_.kill();
            run_process_.waitForFinished(400);
        }
    }
}

void MintCliProcess::set_callbacks(mint::desktop::AgentRuntimeCallbacks callbacks) {
    ++callbacks_generation_;
    callbacks_ = std::move(callbacks);
}

void MintCliProcess::run_query(const std::filesystem::path& root,
                               const std::vector<std::string>& arguments,
                               QueryCompletion completion) {
    auto* process = new QProcess(this);
    const auto output = std::make_shared<QByteArray>();
    const auto error = std::make_shared<QByteArray>();
    const auto handled = std::make_shared<bool>(false);
    connect(process, &QProcess::readyReadStandardOutput, this,
            [process, output] { output->append(process->readAllStandardOutput()); });
    connect(process, &QProcess::readyReadStandardError, this,
            [process, error] { error->append(process->readAllStandardError()); });
    connect(
        process, &QProcess::errorOccurred, this,
        [process, output, error, handled, completion](QProcess::ProcessError process_error) {
            if (process_error != QProcess::FailedToStart || *handled) {
                return;
            }
            *handled = true;
            const auto detail = process->errorString();
            process->deleteLater();
            completion(-1, QProcess::CrashExit, *output, *error, detail);
        },
        Qt::QueuedConnection);
    connect(
        process, &QProcess::finished, this,
        [process, output, error, handled, completion](int exit_code, QProcess::ExitStatus status) {
            if (*handled) {
                return;
            }
            *handled = true;
            output->append(process->readAllStandardOutput());
            error->append(process->readAllStandardError());
            const auto detail = process->errorString();
            process->deleteLater();
            completion(exit_code, status, *output, *error, detail);
        },
        Qt::QueuedConnection);
    if (!root.empty()) {
        process->setWorkingDirectory(qstring_from_path(root));
    }
    process->start(qstring_from_path(executable_), qstring_arguments(arguments));
}

void MintCliProcess::refresh_tasks(const std::filesystem::path& root, const std::string& language) {
    const auto generation = ++status_generation_;
    ++load_generation_;
    run_query(root, command_builder_.status(root, language),
              [this, generation](int exit_code, QProcess::ExitStatus status, QByteArray output,
                                 QByteArray error, QString process_error) {
                  if (generation == status_generation_) {
                      if (status != QProcess::NormalExit || exit_code != 0) {
                          const auto protocol_error =
                              MintCliProtocol::parse_error(output.trimmed().toStdString());
                          emit_error(mint::desktop::RuntimeOperation::task_history,
                                     protocol_error.empty()
                                         ? QString::fromUtf8(error.trimmed().isEmpty()
                                                                 ? process_error.toUtf8()
                                                                 : error.trimmed())
                                         : QString::fromStdString(protocol_error));
                      } else {
                          std::string parse_error;
                          auto response = MintCliProtocol::parse_status(
                              output.trimmed().toStdString(), parse_error);
                          if (response.has_value()) {
                              project_directory_ = std::move(response->state_directory);
                              if (const auto loaded = callbacks_.tasks_loaded) {
                                  loaded(std::move(response->tasks), project_directory_);
                              }
                          } else {
                              emit_error(mint::desktop::RuntimeOperation::task_history,
                                         QString::fromStdString(parse_error));
                          }
                      }
                  }
              });
}

void MintCliProcess::load_task(const std::string& task_id) {
    if (project_directory_.empty() || task_id.empty()) {
        return;
    }
    const auto generation = ++load_generation_;
    const auto task_directory = project_directory_ / "tasks" / task_id;
    auto* watcher = new QFutureWatcher<mint::desktop::TaskHistory>(this);
    connect(watcher, &QFutureWatcher<mint::desktop::TaskHistory>::finished, this,
            [this, watcher, generation, task_id] {
                auto history = watcher->future().takeResult();
                watcher->deleteLater();
                if (const auto loaded = callbacks_.task_loaded;
                    generation == load_generation_ && loaded) {
                    loaded(task_id, std::move(history));
                }
            });
    watcher->setFuture(
        QtConcurrent::run([task_directory] { return TaskHistoryReader::read(task_directory); }));
}

void MintCliProcess::start_task(mint::desktop::RunRequest request) {
    if (running()) {
        return;
    }
    prepare_run();
    const auto arguments = command_builder_.run(request, cancel_file_);
    launch_run(request.root, arguments);
}

void MintCliProcess::resume_task(mint::desktop::ResumeRequest request) {
    if (running()) {
        return;
    }
    prepare_run();
    const auto arguments = command_builder_.resume(request, cancel_file_);
    launch_run(request.root, arguments);
}

bool MintCliProcess::resolve_approval(bool approved) {
    if (run_process_.state() == QProcess::NotRunning || !awaiting_approval_) {
        return false;
    }
    if (write_interaction(MintCliProtocol::approval_decision(approved, "user"),
                          mint::desktop::RuntimeOperation::approval)) {
        awaiting_approval_ = false;
        return true;
    }
    return false;
}

void MintCliProcess::cancel() {
    if (run_process_.state() == QProcess::NotRunning) {
        return;
    }
    QFile cancel_file(qstring_from_path(cancel_file_));
    if (cancel_file.open(QIODevice::WriteOnly)) {
        cancel_file.close();
    }
    if (awaiting_approval_ &&
        write_interaction(MintCliProtocol::approval_decision(false, "run_cancelled"),
                          mint::desktop::RuntimeOperation::approval)) {
        awaiting_approval_ = false;
    }
}

bool MintCliProcess::running() const noexcept {
    return run_active_;
}

void MintCliProcess::prepare_run() {
    ++status_generation_;
    ++load_generation_;
    run_active_ = true;
    event_stream_.reset();
    pending_exit_.reset();
    final_result_.reset();
    final_error_.clear();
    standard_output_buffer_.clear();
    standard_error_buffer_.clear();
    last_diagnostic_.clear();
    awaiting_approval_ = false;
    cancel_file_ = path_from_qstring(
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(QStringLiteral("mint-gui-%1.cancel")
                          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))));
    QFile::remove(qstring_from_path(cancel_file_));
}

void MintCliProcess::launch_run(const std::filesystem::path& root,
                                const std::vector<std::string>& arguments) {
    run_process_.setWorkingDirectory(qstring_from_path(root));
    run_process_.setProcessChannelMode(QProcess::SeparateChannels);
    run_process_.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    run_process_.start(qstring_from_path(executable_), qstring_arguments(arguments));
}

void MintCliProcess::consume_standard_output() {
    standard_output_buffer_.append(run_process_.readAllStandardOutput());
    consume_lines(standard_output_buffer_, [this](const QByteArray& line) {
        handle_standard_output_line(line);
        return true;
    });
}

void MintCliProcess::consume_standard_error() {
    standard_error_buffer_.append(run_process_.readAllStandardError());
    consume_lines(standard_error_buffer_,
                  [this, lifetime = QPointer(this)](const QByteArray& line) {
                      handle_standard_error_line(line);
                      return !lifetime.isNull();
                  });
}

void MintCliProcess::handle_standard_output_line(const QByteArray& line) {
    if (auto result = MintCliProtocol::parse_result(line.toStdString())) {
        final_result_ = std::move(result);
        final_error_.clear();
    } else if (auto error = MintCliProtocol::parse_error(line.toStdString()); !error.empty()) {
        final_error_ = std::move(error);
        final_result_.reset();
    }
}

void MintCliProcess::handle_standard_error_line(const QByteArray& line) {
    if (!run_active_) {
        return;
    }
    const auto interaction = MintCliProtocol::parse_interaction(line.toStdString());
    if (!interaction.has_value()) {
        last_diagnostic_ = line;
        return;
    }
    if (interaction->kind == InteractionKind::task_ready) {
        const QPointer<MintCliProcess> lifetime(this);
        if (const auto ready = callbacks_.task_ready) {
            ready(interaction->task_id);
        }
        if (lifetime) {
            event_stream_.open(interaction->events_path);
        }
        return;
    }
    awaiting_approval_ = true;
    if (const auto requested = callbacks_.approval_requested;
        interaction->approval.has_value() && requested) {
        requested(std::move(*interaction->approval));
    }
}

void MintCliProcess::handle_run_finished(int exit_code, QProcess::ExitStatus exit_status) {
    if (!run_active_) {
        return;
    }
    consume_standard_output();
    const QPointer<MintCliProcess> lifetime(this);
    consume_standard_error();
    if (!lifetime) {
        return;
    }
    if (!standard_output_buffer_.trimmed().isEmpty()) {
        handle_standard_output_line(standard_output_buffer_.trimmed());
        standard_output_buffer_.clear();
    }
    if (!standard_error_buffer_.trimmed().isEmpty()) {
        const auto line = standard_error_buffer_.trimmed();
        standard_error_buffer_.clear();
        handle_standard_error_line(line);
        if (!lifetime) {
            return;
        }
    }
    QFile::remove(qstring_from_path(cancel_file_));
    awaiting_approval_ = false;
    pending_exit_ = ProcessExit{.code = exit_code, .status = exit_status};
    event_stream_.drain();
}

void MintCliProcess::complete_run() {
    if (!pending_exit_.has_value()) {
        return;
    }
    const auto [exit_code, exit_status] = *pending_exit_;
    pending_exit_.reset();
    run_active_ = false;

    // EN: The CLI returns nonzero for resumable limits, cancellation and task failure. Its
    //     structured result owns the task outcome; QProcess only describes process transport.
    // ZH-CN: CLI 在可恢复的限额、取消和任务失败时返回非零值。任务结果由结构化输出决定，
    //        QProcess 只描述进程是否正常退出。
    if (exit_status == QProcess::NormalExit && final_result_.has_value()) {
        if (const auto finished = callbacks_.run_finished) {
            finished(std::move(*final_result_));
        }
        return;
    }
    if (!final_error_.empty()) {
        emit_error(mint::desktop::RuntimeOperation::task_run, QString::fromStdString(final_error_));
        return;
    }
    if (exit_status == QProcess::NormalExit && !last_diagnostic_.isEmpty()) {
        emit_error(mint::desktop::RuntimeOperation::task_run, QString::fromUtf8(last_diagnostic_));
        return;
    }
    using namespace mint::localization;
    const auto detail =
        message(exit_status == QProcess::CrashExit ? Message::gui_error_process_crashed
                                                   : Message::gui_error_process_result_missing,
                {arg(Placeholder::code, exit_code)});
    emit_error(mint::desktop::RuntimeOperation::task_run, QString::fromStdString(detail));
}

bool MintCliProcess::write_interaction(std::string payload,
                                       mint::desktop::RuntimeOperation failure_operation) {
    const auto bytes = QByteArray::fromStdString(payload);
    if (run_process_.write(bytes) == bytes.size()) {
        return true;
    }
    emit_error(failure_operation, run_process_.errorString());
    return false;
}

void MintCliProcess::emit_error(mint::desktop::RuntimeOperation operation,
                                const QString& detail) const {
    if (const auto error = callbacks_.error) {
        error({.operation = operation, .detail = detail.toUtf8().toStdString()});
    }
}

} // namespace mint::infrastructure::desktop
