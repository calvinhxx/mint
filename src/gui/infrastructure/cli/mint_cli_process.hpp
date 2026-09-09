#pragma once

#include "application/agent_runtime.hpp"
#include "infrastructure/cli/mint_cli_command_builder.hpp"

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>

#include <optional>

namespace mint::infrastructure::desktop {

class MintCliProcess final : public QObject, public mint::desktop::AgentRuntime {
  public:
    explicit MintCliProcess(std::filesystem::path executable, std::filesystem::path state_root = {},
                            QObject* parent = nullptr);
    ~MintCliProcess() override;

    void set_callbacks(mint::desktop::AgentRuntimeCallbacks callbacks) override;
    void refresh_tasks(const std::filesystem::path& root, const std::string& language) override;
    void load_task(const std::string& task_id) override;
    void start_task(mint::desktop::RunRequest request) override;
    void resume_task(mint::desktop::ResumeRequest request) override;
    [[nodiscard]] bool resolve_approval(bool approved) override;
    void cancel() override;
    [[nodiscard]] bool running() const noexcept override;

  private:
    using QueryCompletion =
        std::function<void(int, QProcess::ExitStatus, QByteArray, QByteArray, QString)>;

    void run_query(const std::filesystem::path& root, const std::vector<std::string>& arguments,
                   QueryCompletion completion);
    void consume_standard_output();
    void consume_standard_error();
    void handle_standard_output_line(const QByteArray& line);
    void handle_standard_error_line(const QByteArray& line);
    void handle_run_finished(int exit_code, QProcess::ExitStatus exit_status);
    void prepare_run();
    void launch_run(const std::filesystem::path& root, const std::vector<std::string>& arguments);
    void tail_events();
    void queue_event_read();
    void complete_run();
    bool write_interaction(std::string payload, mint::desktop::RuntimeOperation failure_operation);
    void emit_error(mint::desktop::RuntimeOperation operation, const QString& detail) const;

    std::filesystem::path executable_;
    std::filesystem::path project_directory_;
    std::filesystem::path events_path_;
    std::filesystem::path cancel_file_;
    MintCliCommandBuilder command_builder_;
    mint::desktop::AgentRuntimeCallbacks callbacks_;
    QProcess run_process_;
    QTimer event_timer_;
    QByteArray standard_output_buffer_;
    QByteArray standard_error_buffer_;
    QByteArray event_buffer_;
    QByteArray last_diagnostic_;
    std::optional<mint::desktop::TaskResult> final_result_;
    struct ProcessExit {
        int code = 0;
        QProcess::ExitStatus status = QProcess::NormalExit;
    };
    std::optional<ProcessExit> pending_exit_;
    std::string final_error_;
    qint64 event_offset_ = 0;
    bool awaiting_approval_ = false;
    bool run_active_ = false;
    bool event_read_pending_ = false;
    bool event_read_queued_ = false;
    std::size_t event_generation_ = 0;
    std::size_t status_generation_ = 0;
    std::size_t load_generation_ = 0;
};

} // namespace mint::infrastructure::desktop
