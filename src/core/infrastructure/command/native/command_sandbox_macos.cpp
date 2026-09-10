#include "../command_sandbox_backend.hpp"
#include "command_sandbox_posix.hpp"

#include "mint/localization/localization.hpp"
#include "mint/runtime/path.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace mint::command_detail {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

std::string sandbox_string(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char character : value) {
        switch (character) {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            result += character;
            break;
        }
    }
    return result;
}
bool same_scratch_identity(const struct stat& status, dev_t device, ino_t inode) noexcept {
    return S_ISDIR(status.st_mode) && status.st_dev == device && status.st_ino == inode;
}

[[noreturn]] void throw_scratch_identity_changed() {
    throw std::runtime_error(message(Message::command_scratch_identity_changed));
}

class CommandScratchDirectory final {
  public:
    explicit CommandScratchDirectory(const std::filesystem::path& workspace);

    CommandScratchDirectory(const CommandScratchDirectory&) = delete;
    CommandScratchDirectory& operator=(const CommandScratchDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    void verify_cleanup_target() const;
    void confirm_cleanup();

  private:
    struct State {
        std::filesystem::path path;
        dev_t device{};
        ino_t inode{};
    };
    std::optional<State> state_;
};

CommandScratchDirectory::CommandScratchDirectory(const std::filesystem::path& workspace) {
    State state;
    auto pattern = (workspace / ".mint-command-tmp-XXXXXX").string();
    const auto* created = ::mkdtemp(pattern.data());
    if (created == nullptr) {
        throw std::runtime_error(message(Message::command_scratch_create_failed,
                                         {arg(Placeholder::error, std::strerror(errno))}));
    }

    state.path = std::move(pattern);
    struct stat created_status = {};
    if (::lstat(state.path.c_str(), &created_status) != 0 || !S_ISDIR(created_status.st_mode)) {
        throw std::runtime_error(message(Message::command_scratch_identity_failed));
    }
    state.device = created_status.st_dev;
    state.inode = created_status.st_ino;

    std::string setup_error;
    struct stat status = {};
    if (::chmod(state.path.c_str(), S_IRWXU) != 0) {
        setup_error = std::strerror(errno);
    } else if (::lstat(state.path.c_str(), &status) != 0) {
        setup_error = std::strerror(errno);
    } else if (!same_scratch_identity(status, state.device, state.inode) ||
               status.st_uid != ::geteuid() || (status.st_mode & 07777) != S_IRWXU) {
        setup_error = message(Message::command_scratch_permission_check_failed);
    } else {
        state_ = std::move(state);
        return;
    }

    if (::lstat(state.path.c_str(), &status) != 0 ||
        !same_scratch_identity(status, state.device, state.inode)) {
        throw std::runtime_error(message(Message::command_scratch_init_identity_changed,
                                         {arg(Placeholder::error, setup_error)}));
    }
    if (::rmdir(state.path.c_str()) != 0) {
        throw std::runtime_error(message(Message::command_scratch_init_cleanup_failed,
                                         {arg(Placeholder::setup_error, setup_error),
                                          arg(Placeholder::cleanup_error, std::strerror(errno))}));
    }
    throw std::runtime_error(
        message(Message::command_scratch_init_failed, {arg(Placeholder::error, setup_error)}));
}

const std::filesystem::path& CommandScratchDirectory::path() const noexcept {
    return state_->path;
}

void CommandScratchDirectory::verify_cleanup_target() const {
    struct stat status = {};
    if (!state_.has_value() || ::lstat(state_->path.c_str(), &status) != 0 ||
        !same_scratch_identity(status, state_->device, state_->inode)) {
        throw_scratch_identity_changed();
    }
}

void CommandScratchDirectory::confirm_cleanup() {
    if (!state_.has_value()) {
        return;
    }
    struct stat status = {};
    if (::lstat(state_->path.c_str(), &status) == 0) {
        throw std::runtime_error(message(Message::command_scratch_still_exists));
    }
    if (errno != ENOENT) {
        throw std::runtime_error(message(Message::command_scratch_cleanup_confirmation_failed,
                                         {arg(Placeholder::error, std::strerror(errno))}));
    }
    state_.reset();
}

std::string command_exception_message(const std::exception_ptr& error) {
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& exception) {
        return exception.what();
    } catch (...) {
        return localization::message(Message::common_unknown_exception);
    }
}

ProcessResult execute_macos_sandboxed_process(ProcessRequest request,
                                              const std::filesystem::path& workspace_root,
                                              const std::vector<std::string>& sandbox_arguments) {
    if (sandbox_arguments.size() != 3 || sandbox_arguments[0] != "sandbox-exec" ||
        sandbox_arguments[1] != "-p") {
        throw std::logic_error(
            localization::message(Message::command_macos_invalid_sandbox_arguments));
    }

    const auto cleanup_executable = request.executable;
    CommandScratchDirectory scratch(workspace_root);
    std::optional<ProcessResult> result;
    std::exception_ptr execution_error;
    try {
        const auto scratch_path = scratch.path().generic_string();
        request.environment_overrides = {
            {"TMPDIR", scratch_path}, {"TMP", scratch_path}, {"TEMP", scratch_path}};
        result.emplace(execute_process(std::move(request)));
    } catch (...) {
        execution_error = std::current_exception();
    }

    try {
        scratch.verify_cleanup_target();
        ProcessRequest cleanup_request;
        cleanup_request.executable = cleanup_executable;
        cleanup_request.argv = sandbox_arguments;
        cleanup_request.argv.insert(cleanup_request.argv.end(),
                                    {"/bin/rm", "-rf", "--", scratch.path().generic_string()});
        cleanup_request.workspace_root = workspace_root;
        cleanup_request.cwd = workspace_root;
        cleanup_request.timeout_seconds = 5;
        cleanup_request.max_output_bytes = 4 * 1024;
        const auto cleanup = execute_process(std::move(cleanup_request));
        if (cleanup.status != "exited" || cleanup.exit_code.value_or(-1) != 0) {
            auto message = localization::message(Message::command_scratch_cleanup_process_failed,
                                                 {arg(Placeholder::status, cleanup.status)});
            if (!cleanup.output.empty()) {
                message = localization::message(
                    Message::command_scratch_cleanup_with_output,
                    {arg(Placeholder::message, message), arg(Placeholder::output, cleanup.output)});
            }
            throw std::runtime_error(std::move(message));
        }
        scratch.confirm_cleanup();
    } catch (const std::exception& cleanup_error) {
        if (execution_error != nullptr) {
            throw std::runtime_error(localization::message(
                Message::command_scratch_execution_and_cleanup_failed,
                {arg(Placeholder::execution_error, command_exception_message(execution_error)),
                 arg(Placeholder::cleanup_error, cleanup_error.what())}));
        }
        throw;
    }
    if (execution_error != nullptr) {
        std::rethrow_exception(execution_error);
    }
    return std::move(*result);
}
} // namespace

namespace native {

SandboxConfig build_sandbox_config(
    const std::filesystem::path& root,
    const std::unordered_map<std::string, std::filesystem::path>& resolved_programs,
    std::vector<std::filesystem::path> read_only_paths,
    std::vector<std::filesystem::path> denied_read_paths) {
    read_only_paths =
        normalize_read_only_paths(root, std::move(read_only_paths), denied_read_paths);
    SandboxConfig config{.executable = "/usr/bin/sandbox-exec", .backend = "macos-seatbelt"};
    if (!is_executable_file(config.executable)) {
        throw std::invalid_argument(message(Message::command_sandbox_seatbelt_missing));
    }

    const auto escaped_root = sandbox_string(root.generic_string());
    std::string profile = "(version 1) "
                          "(allow default) "
                          "(deny network*) "
                          "(deny file-write* (require-all (require-not (subpath \"" +
                          escaped_root + "\")) (require-not (literal \"/dev/null\"))))";

    if (const char* home_value = std::getenv("HOME"); home_value != nullptr) {
        std::error_code error;
        const auto home = std::filesystem::weakly_canonical(home_value, error);
        if (!error && home.is_absolute() && home != root) {
            profile += " (deny file-read* (require-all (subpath \"" +
                       sandbox_string(home.generic_string()) + "\") (require-not (subpath \"" +
                       escaped_root + "\"))";
            for (const auto& [label, executable] : resolved_programs) {
                (void)label;
                if (is_path_within(home, executable) && !is_path_within(root, executable)) {
                    profile += " (require-not (literal \"" +
                               sandbox_string(executable.generic_string()) + "\"))";
                }
            }
            for (const auto& path : read_only_paths) {
                if (is_path_within(home, path)) {
                    profile +=
                        " (require-not (" +
                        std::string(std::filesystem::is_directory(path) ? "subpath" : "literal") +
                        " \"" + sandbox_string(path.generic_string()) + "\"))";
                }
            }
            profile += "))";
        }
    }

    for (auto& denied : denied_read_paths) {
        std::error_code error;
        denied = std::filesystem::weakly_canonical(std::move(denied), error);
        if (error || denied.empty()) {
            continue;
        }
        const bool directory = std::filesystem::is_directory(denied, error) && !error;
        profile += " (deny file-read* file-write* (" +
                   std::string(directory ? "subpath" : "literal") + " \"" +
                   sandbox_string(denied.generic_string()) + "\"))";
    }
    config.arguments = {"sandbox-exec", "-p", std::move(profile)};
    return config;
}

} // namespace native

ProcessResult execute_sandboxed_process(ProcessRequest request, const SandboxConfig& sandbox) {
    if (sandbox.executable.empty()) {
        return execute_process(std::move(request));
    }
    const auto workspace_root = request.workspace_root;
    return execute_macos_sandboxed_process(std::move(request), workspace_root, sandbox.arguments);
}

} // namespace mint::command_detail
