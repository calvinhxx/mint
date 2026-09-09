#include "mint_cli_process.hpp"

#include "infrastructure/cli/mint_cli_protocol.hpp"
#include "infrastructure/cli/task_history_reader.hpp"

#include "mint/localization/localization.hpp"

#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QStandardPaths>
#include <QStringList>
#include <QUuid>

#include <QtConcurrentRun>

#include <algorithm>
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

void consume_lines(QByteArray& buffer, const std::function<void(const QByteArray&)>& consume) {
    for (;;) {
        const auto newline = buffer.indexOf('\n');
        if (newline < 0) {
            return;
        }
        const auto line = buffer.left(newline).trimmed();
        buffer.remove(0, newline + 1);
        if (!line.isEmpty()) {
            consume(line);
        }
    }
}

constexpr qint64 event_frame_bytes = 64 * 1024;
constexpr std::size_t event_frame_records = 128;

struct EventBatch {
    std::vector<mint::desktop::RunEvent> events;
    QByteArray remainder;
    qint64 offset = 0;
    bool more = false;
};

EventBatch read_event_batch(const std::filesystem::path& path, qint64 offset, QByteArray buffer,
                            bool terminal) {
    EventBatch batch;
    batch.offset = offset;
    QFile file(qstring_from_path(path));
    if (file.open(QIODevice::ReadOnly) && file.seek(offset)) {
        // EN: Consume buffered records before reading another bounded slice of the file.
        // ZH-CN: 先处理已有完整记录，再读取下一段有界文件内容。
        if (!buffer.contains('\n')) {
            const auto bytes = file.read(event_frame_bytes);
            batch.offset += bytes.size();
            buffer.append(bytes);
        }
        batch.more = batch.offset < file.size();
    }
    qsizetype consumed = 0;
    for (std::size_t count = 0; count < event_frame_records; ++count) {
        const auto newline = buffer.indexOf('\n', consumed);
        if (newline < 0) {
            break;
        }
        if (auto event = MintCliProtocol::parse_event(
                buffer.mid(consumed, newline - consumed).trimmed().toStdString())) {
            batch.events.push_back(std::move(*event));
        }
        consumed = newline + 1;
    }
    buffer.remove(0, consumed);
    batch.more = batch.more || buffer.contains('\n');
    // EN: A finished writer can leave one complete JSON record without a final newline.
    // ZH-CN: 已结束的写入进程可能留下没有末尾换行的完整 JSON 记录。
    if (terminal && !batch.more && !buffer.trimmed().isEmpty()) {
        if (batch.events.size() == event_frame_records) {
            batch.more = true;
        } else {
            if (auto event = MintCliProtocol::parse_event(buffer.trimmed().toStdString())) {
                batch.events.push_back(std::move(*event));
            }
            buffer.clear();
        }
    }
    batch.remainder = std::move(buffer);
    return batch;
}

} // namespace

MintCliProcess::MintCliProcess(std::filesystem::path executable, std::filesystem::path state_root,
                               QObject* parent)
    : QObject(parent), executable_(std::move(executable)), command_builder_(std::move(state_root)) {
    event_timer_.setInterval(120);
    connect(&event_timer_, &QTimer::timeout, this, [this] { tail_events(); });
    connect(&run_process_, &QProcess::readyReadStandardOutput, this,
            [this] { consume_standard_output(); });
    connect(&run_process_, &QProcess::readyReadStandardError, this,
            [this] { consume_standard_error(); });
    connect(&run_process_, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus status) { handle_run_finished(code, status); });
    connect(&run_process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            run_active_ = false;
            ++event_generation_;
            pending_exit_.reset();
            event_timer_.stop();
            QFile::remove(qstring_from_path(cancel_file_));
            emit_error(mint::desktop::RuntimeOperation::task_run, run_process_.errorString());
        }
    });
}

MintCliProcess::~MintCliProcess() {
    callbacks_ = {};
    ++event_generation_;
    run_active_ = false;
    event_timer_.stop();
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
    connect(process, &QProcess::errorOccurred, this,
            [process, output, error, handled, completion](QProcess::ProcessError process_error) {
                if (process_error != QProcess::FailedToStart || *handled) {
                    return;
                }
                *handled = true;
                completion(-1, QProcess::CrashExit, *output, *error, process->errorString());
                process->deleteLater();
            });
    connect(
        process, &QProcess::finished, this,
        [process, output, error, handled, completion](int exit_code, QProcess::ExitStatus status) {
            if (*handled) {
                return;
            }
            *handled = true;
            output->append(process->readAllStandardOutput());
            error->append(process->readAllStandardError());
            completion(exit_code, status, *output, *error, process->errorString());
            process->deleteLater();
        });
    if (!root.empty()) {
        process->setWorkingDirectory(qstring_from_path(root));
    }
    process->start(qstring_from_path(executable_), qstring_arguments(arguments));
}

void MintCliProcess::refresh_tasks(const std::filesystem::path& root, const std::string& language) {
    const auto generation = ++status_generation_;
    ++load_generation_;
    run_query(
        root, command_builder_.status(root, language),
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
                    auto response =
                        MintCliProtocol::parse_status(output.trimmed().toStdString(), parse_error);
                    if (response.has_value()) {
                        project_directory_ = std::move(response->state_directory);
                        if (callbacks_.tasks_loaded) {
                            callbacks_.tasks_loaded(std::move(response->tasks), project_directory_);
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
                auto history = watcher->result();
                watcher->deleteLater();
                if (generation == load_generation_ && callbacks_.task_loaded) {
                    callbacks_.task_loaded(task_id, std::move(history));
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
    ++event_generation_;
    run_active_ = true;
    event_read_pending_ = false;
    event_read_queued_ = false;
    pending_exit_.reset();
    final_result_.reset();
    final_error_.clear();
    standard_output_buffer_.clear();
    standard_error_buffer_.clear();
    event_buffer_.clear();
    last_diagnostic_.clear();
    event_offset_ = 0;
    awaiting_approval_ = false;
    events_path_.clear();
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
    consume_lines(standard_output_buffer_,
                  [this](const QByteArray& line) { handle_standard_output_line(line); });
}

void MintCliProcess::consume_standard_error() {
    standard_error_buffer_.append(run_process_.readAllStandardError());
    consume_lines(standard_error_buffer_,
                  [this](const QByteArray& line) { handle_standard_error_line(line); });
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
    const auto interaction = MintCliProtocol::parse_interaction(line.toStdString());
    if (!interaction.has_value()) {
        last_diagnostic_ = line;
        return;
    }
    if (interaction->kind == InteractionKind::task_ready) {
        ++event_generation_;
        event_read_pending_ = false;
        event_read_queued_ = false;
        events_path_ = interaction->events_path;
        event_offset_ = 0;
        event_buffer_.clear();
        if (callbacks_.task_ready) {
            callbacks_.task_ready(interaction->task_id);
        }
        event_timer_.start();
        tail_events();
        return;
    }
    awaiting_approval_ = true;
    if (interaction->approval.has_value() && callbacks_.approval_requested) {
        callbacks_.approval_requested(std::move(*interaction->approval));
    }
}

void MintCliProcess::handle_run_finished(int exit_code, QProcess::ExitStatus exit_status) {
    consume_standard_output();
    consume_standard_error();
    if (!standard_output_buffer_.trimmed().isEmpty()) {
        handle_standard_output_line(standard_output_buffer_.trimmed());
        standard_output_buffer_.clear();
    }
    if (!standard_error_buffer_.trimmed().isEmpty()) {
        handle_standard_error_line(standard_error_buffer_.trimmed());
        standard_error_buffer_.clear();
    }
    event_timer_.stop();
    QFile::remove(qstring_from_path(cancel_file_));
    awaiting_approval_ = false;
    pending_exit_ = ProcessExit{.code = exit_code, .status = exit_status};
    tail_events();
}

void MintCliProcess::complete_run() {
    if (!pending_exit_.has_value()) {
        return;
    }
    const auto [exit_code, exit_status] = *pending_exit_;
    pending_exit_.reset();
    run_active_ = false;
    ++event_generation_;

    // EN: The CLI returns nonzero for resumable limits, cancellation and task failure. Its
    //     structured result owns the task outcome; QProcess only describes process transport.
    // ZH-CN: CLI 在可恢复的限额、取消和任务失败时返回非零值。任务结果由结构化输出决定，
    //        QProcess 只描述进程是否正常退出。
    if (exit_status == QProcess::NormalExit && final_result_.has_value()) {
        if (callbacks_.run_finished) {
            callbacks_.run_finished(std::move(*final_result_));
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

void MintCliProcess::tail_events() {
    if (!run_active_ || event_read_pending_) {
        return;
    }
    if (events_path_.empty()) {
        complete_run();
        return;
    }
    event_read_pending_ = true;
    const auto generation = event_generation_;
    const auto terminal = pending_exit_.has_value();
    auto* watcher = new QFutureWatcher<EventBatch>(this);
    connect(watcher, &QFutureWatcher<EventBatch>::finished, this,
            [this, watcher, generation, terminal] {
                auto batch = watcher->future().takeResult();
                watcher->deleteLater();
                if (generation != event_generation_ || !run_active_) {
                    return;
                }
                event_read_pending_ = false;
                event_offset_ = batch.offset;
                event_buffer_ = std::move(batch.remainder);
                if (!batch.events.empty()) {
                    if (callbacks_.events_received) {
                        callbacks_.events_received(std::move(batch.events));
                    } else if (callbacks_.event_received) {
                        for (auto& event : batch.events) {
                            callbacks_.event_received(std::move(event));
                        }
                    }
                }
                if (generation != event_generation_ || !run_active_) {
                    return;
                }
                if (batch.more || (pending_exit_.has_value() && !terminal)) {
                    queue_event_read();
                } else if (pending_exit_.has_value()) {
                    complete_run();
                }
            });
    watcher->setFuture(QtConcurrent::run([path = events_path_, offset = event_offset_,
                                          buffer = std::move(event_buffer_), terminal]() mutable {
        return read_event_batch(path, offset, std::move(buffer), terminal);
    }));
}

void MintCliProcess::queue_event_read() {
    if (event_read_queued_) {
        return;
    }
    event_read_queued_ = true;
    const auto generation = event_generation_;
    QTimer::singleShot(0, this, [this, generation] {
        if (generation == event_generation_ && run_active_) {
            event_read_queued_ = false;
            tail_events();
        }
    });
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
    if (callbacks_.error) {
        callbacks_.error({.operation = operation, .detail = detail.toUtf8().toStdString()});
    }
}

} // namespace mint::infrastructure::desktop
