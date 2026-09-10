#include "mint/infrastructure/change_transaction_store.hpp"

#include "mint/infrastructure/session_store.hpp"
#include "mint/localization/localization.hpp"
#include "mint/platform/file.hpp"

#include "filesystem/output_path.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace mint {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

std::filesystem::path checked_transaction_path(std::filesystem::path path) {
    return infrastructure_detail::validated_output_path(
        std::move(path), message(Message::label_changeset_journal),
        infrastructure_detail::HardLinkPolicy::reject);
}

} // namespace

struct ChangeTransactionStore::LockState {
    std::unique_ptr<std::FILE, decltype(&std::fclose)> stream{nullptr, &std::fclose};
};

std::unique_ptr<ChangeTransactionStore::LockState>
ChangeTransactionStore::acquire_lock(const std::filesystem::path& transaction_path) {
    auto state = std::make_unique<ChangeTransactionStore::LockState>();
    const auto path = infrastructure_detail::validated_output_path(
        change_transaction_lock_path(transaction_path), message(Message::label_changeset_lock),
        infrastructure_detail::HardLinkPolicy::reject);
    std::error_code error;
    state->stream.reset(platform::open_lock_file(path, error));
    if (!state->stream) {
        throw std::system_error(error, message(Message::persistence_transaction_open_lock_failed));
    }
    if (!platform::try_lock_file(state->stream.get(), error)) {
        if (!error) {
            throw std::runtime_error(message(Message::persistence_transaction_locked));
        }
        throw std::system_error(error, message(Message::persistence_transaction_lock_failed));
    }
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
        throw std::runtime_error(message(Message::persistence_transaction_remove_failed,
                                         {arg(Placeholder::error, error.message())}));
    }
}

std::filesystem::path
change_transaction_path_for_session(const std::filesystem::path& session_path) {
    if (session_path.empty() || session_path.filename().empty()) {
        throw std::invalid_argument(message(Message::persistence_transaction_session_path_empty));
    }
    const auto filename = session_path.filename();
    if (filename == "session.json") {
        return session_path.parent_path() / "changeset-transaction.json";
    }
    return session_path.parent_path() / (filename.string() + ".changeset-transaction.json");
}

std::filesystem::path change_transaction_lock_path(const std::filesystem::path& transaction_path) {
    if (transaction_path.empty() || transaction_path.filename().empty()) {
        throw std::invalid_argument(message(Message::persistence_transaction_path_empty));
    }
    return transaction_path.parent_path() / (transaction_path.filename().string() + ".lock");
}

} // namespace mint
