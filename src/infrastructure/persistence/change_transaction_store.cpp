#include "mint/infrastructure/change_transaction_store.hpp"

#include "mint/infrastructure/session_store.hpp"

#include "filesystem/output_path.hpp"

#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mint {
namespace {

std::filesystem::path checked_transaction_path(std::filesystem::path path) {
    return infrastructure_detail::validated_output_path(
        std::move(path), "changeset 事务日志", infrastructure_detail::HardLinkPolicy::reject);
}

} // namespace

struct ChangeTransactionStore::LockState {
#if defined(_WIN32)
    HANDLE handle = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped{};

    ~LockState() {
        if (handle != INVALID_HANDLE_VALUE) {
            (void)::UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlapped);
            (void)::CloseHandle(handle);
        }
    }
#else
    int descriptor = -1;

    ~LockState() {
        if (descriptor >= 0) {
            (void)::flock(descriptor, LOCK_UN);
            (void)::close(descriptor);
        }
    }
#endif
};

std::unique_ptr<ChangeTransactionStore::LockState>
ChangeTransactionStore::acquire_lock(const std::filesystem::path& transaction_path) {
    auto state = std::make_unique<ChangeTransactionStore::LockState>();
    const auto path = infrastructure_detail::validated_output_path(
        change_transaction_lock_path(transaction_path), "changeset 事务锁",
        infrastructure_detail::HardLinkPolicy::reject);
#if defined(_WIN32)
    state->handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (state->handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                "无法打开 changeset 事务锁");
    }
    if (!::LockFileEx(state->handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                      MAXDWORD, MAXDWORD, &state->overlapped)) {
        const auto error = ::GetLastError();
        if (error == ERROR_LOCK_VIOLATION) {
            throw std::runtime_error("另一个 mint 进程正在修改该任务的工作区");
        }
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "无法锁定 changeset 事务日志");
    }
#else
    int flags = O_CREAT | O_RDWR;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    state->descriptor = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
    if (state->descriptor < 0) {
        throw std::system_error(errno, std::generic_category(), "无法打开 changeset 事务锁");
    }
    if (::flock(state->descriptor, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            throw std::runtime_error("另一个 mint 进程正在修改该任务的工作区");
        }
        throw std::system_error(errno, std::generic_category(), "无法锁定 changeset 事务日志");
    }
#endif
    return state;
}

ChangeTransactionStore::ChangeTransactionStore(std::filesystem::path path)
    : path_(checked_transaction_path(std::move(path))), lock_(acquire_lock(path_)) {}

ChangeTransactionStore::~ChangeTransactionStore() = default;

const std::filesystem::path& ChangeTransactionStore::path() const noexcept {
    return path_;
}

bool ChangeTransactionStore::exists() const {
    return SessionStore(checked_transaction_path(path_)).exists();
}

Json ChangeTransactionStore::load() const {
    return SessionStore(checked_transaction_path(path_)).load();
}

void ChangeTransactionStore::save(const Json& transaction) const {
    SessionStore(checked_transaction_path(path_)).save(transaction);
}

void ChangeTransactionStore::clear() const {
    const auto checked = checked_transaction_path(path_);
    std::error_code error;
    const bool removed = std::filesystem::remove(checked, error);
    if (error || (!removed && std::filesystem::exists(checked))) {
        throw std::runtime_error("无法删除 changeset 事务日志: " + error.message());
    }
}

std::filesystem::path
change_transaction_path_for_session(const std::filesystem::path& session_path) {
    if (session_path.empty() || session_path.filename().empty()) {
        throw std::invalid_argument("会话路径不能为空");
    }
    const auto filename = session_path.filename();
    if (filename == "session.json") {
        return session_path.parent_path() / "changeset-transaction.json";
    }
    return session_path.parent_path() / (filename.string() + ".changeset-transaction.json");
}

std::filesystem::path change_transaction_lock_path(const std::filesystem::path& transaction_path) {
    if (transaction_path.empty() || transaction_path.filename().empty()) {
        throw std::invalid_argument("changeset 事务日志路径不能为空");
    }
    return transaction_path.parent_path() / (transaction_path.filename().string() + ".lock");
}

} // namespace mint
