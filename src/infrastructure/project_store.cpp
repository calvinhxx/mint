#include "aiagent/infrastructure/project_store.hpp"

#include "aiagent/domain/task_policy.hpp"
#include "aiagent/infrastructure/session_store.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace aiagent {
namespace {

constexpr int project_schema_version = 1;
constexpr int task_schema_version = 1;
constexpr std::size_t max_question_bytes = 64 * 1024;
std::atomic_uint64_t task_sequence{0};

bool is_inside(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto root_part = root.begin();
    auto candidate_part = candidate.begin();
    for (; root_part != root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == candidate.end() || *root_part != *candidate_part) {
            return false;
        }
    }
    return true;
}

std::string hexadecimal(std::uint64_t value, std::size_t width) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(static_cast<int>(width)) << value;
    return output.str();
}

std::uint64_t fnv1a(std::string_view value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto character : value) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string utc_timestamp(std::chrono::system_clock::time_point point) {
    const auto time = std::chrono::system_clock::to_time_t(point);
    std::tm utc{};
#if defined(_WIN32)
    if (::gmtime_s(&utc, &time) != 0) {
        throw std::runtime_error("无法生成 UTC 时间戳");
    }
#else
    if (::gmtime_r(&time, &utc) == nullptr) {
        throw std::runtime_error("无法生成 UTC 时间戳");
    }
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string task_id(const std::string& question) {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    if (::gmtime_s(&utc, &time) != 0) {
        throw std::runtime_error("无法生成任务 ID");
    }
#else
    if (::gmtime_r(&time, &utc) == nullptr) {
        throw std::runtime_error("无法生成任务 ID");
    }
#endif
    std::ostringstream prefix;
    const auto microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() %
        1000000;
    prefix << std::put_time(&utc, "%Y%m%d-%H%M%S") << '-' << std::setfill('0') << std::setw(6)
           << microseconds;
    const auto sequence = task_sequence.fetch_add(1);
    const auto entropy =
        question + std::to_string(now.time_since_epoch().count()) + std::to_string(sequence);
    return prefix.str() + "-" + hexadecimal(fnv1a(entropy), 16).substr(0, 8);
}

std::filesystem::path normalized_absolute(std::filesystem::path path) {
    std::error_code error;
    path = std::filesystem::absolute(std::move(path), error);
    if (error || path.empty()) {
        throw std::invalid_argument("无法解析 aiagent 状态目录");
    }
    const auto resolved = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : resolved;
}

void restrict_directory_permissions(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
    if (error) {
        throw std::runtime_error("无法把 aiagent 状态目录权限限制为当前用户: " + error.message());
    }
}

void create_private_directory(const std::filesystem::path& path) {
    std::error_code error;
    const bool created = std::filesystem::create_directories(path, error);
    if (error || !std::filesystem::is_directory(path)) {
        throw std::runtime_error("无法创建 aiagent 状态目录: " + path.generic_string());
    }
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || std::filesystem::is_symlink(status)) {
        throw std::runtime_error("aiagent 状态目录不能是符号链接: " + path.generic_string());
    }
    if (created) {
        restrict_directory_permissions(path);
#if !defined(_WIN32)
    } else {
        const auto public_permissions =
            std::filesystem::perms::group_all | std::filesystem::perms::others_all;
        if ((status.permissions() & public_permissions) != std::filesystem::perms::none) {
            throw std::runtime_error("已有 aiagent 状态目录不是当前用户私有目录: " +
                                     path.generic_string());
        }
#endif
    }
}

void validate_task_id(const std::string& id) {
    if (id.empty() || id.size() > 80 ||
        !std::all_of(id.begin(), id.end(), [](unsigned char character) {
            return std::isalnum(character) != 0 || character == '-' || character == '_';
        })) {
        throw std::invalid_argument("任务 ID 格式无效");
    }
}

bool valid_project_kind(const std::string& kind) {
    return !kind.empty() && kind.size() <= 64 &&
           std::all_of(kind.begin(), kind.end(), [](unsigned char character) {
               return std::isalnum(character) != 0 || character == '-' || character == '_';
           });
}

bool valid_json_text(const std::string& text) {
    try {
        (void)Json(text).dump();
        return true;
    } catch (const Json::type_error&) {
        return false;
    }
}

bool is_plain_file(const std::filesystem::path& path, std::error_code& error) {
    const auto status = std::filesystem::symlink_status(path, error);
    return !error && std::filesystem::is_regular_file(status) &&
           !std::filesystem::is_symlink(status);
}

enum class StoredFileState { missing, plain, invalid };

StoredFileState stored_file_state(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && status.type() == std::filesystem::file_type::not_found)) {
        return StoredFileState::missing;
    }
    if (error || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
        return StoredFileState::invalid;
    }
    return StoredFileState::plain;
}

bool is_resumable_status(const std::string& status) {
    return status == "running" || status == "max_turns" || status == "cancelled" ||
           status == "timed_out";
}

std::optional<ManagedTaskMode> parse_task_mode(const Json& task) {
    if (!task.contains("mode") || !task.at("mode").is_string()) {
        return std::nullopt;
    }
    const auto mode = task.at("mode").get<std::string>();
    if (mode == "model") {
        return ManagedTaskMode::model;
    }
    if (mode == "demo") {
        return ManagedTaskMode::demo;
    }
    return std::nullopt;
}

ManagedTaskSummary summary_from(const ManagedTaskPaths& paths,
                                const std::filesystem::path& workspace_root) {
    const SessionStore metadata(paths.metadata);
    const auto task = metadata.load();
    if (!task.is_object() || task.value("schema_version", 0) != task_schema_version ||
        task.value("id", "") != paths.id || !task.contains("question") ||
        !task.at("question").is_string() || !task.contains("created_at") ||
        !task.at("created_at").is_string() || !task.contains("workspace_root") ||
        !task.at("workspace_root").is_string() ||
        task.at("workspace_root").get<std::string>() != workspace_root.generic_string() ||
        task.value("policy_file", "") != "policy.json" ||
        task.value("session_file", "") != "session.json" ||
        task.value("events_file", "") != "events.jsonl") {
        throw std::runtime_error("任务元数据无效: " + paths.id);
    }
    const auto mode = parse_task_mode(task);
    if (!mode.has_value()) {
        throw std::runtime_error("任务模式无效: " + paths.id);
    }
    ManagedTaskSummary summary;
    summary.id = paths.id;
    summary.question = task.at("question").get<std::string>();
    summary.created_at = task.at("created_at").get<std::string>();
    summary.mode = *mode;
    if (summary.question.empty() || summary.question.size() > max_question_bytes ||
        summary.question.find('\0') != std::string::npos || summary.created_at.empty() ||
        summary.created_at.size() > 64 || summary.created_at.find('\0') != std::string::npos) {
        throw std::runtime_error("任务元数据无效: " + paths.id);
    }

    const SessionStore session(paths.session);
    if (session.exists()) {
        const auto snapshot = session.load();
        const auto schema = snapshot.is_object() ? snapshot.value("schema_version", 0) : 0;
        if (!snapshot.is_object() || (schema != 2 && schema != 3) ||
            snapshot.value("workspace_root", "") != task.at("workspace_root").get<std::string>() ||
            !snapshot.contains("status") || !snapshot.at("status").is_string() ||
            !snapshot.contains("verification_status") ||
            !snapshot.at("verification_status").is_string() || !snapshot.contains("turns") ||
            !snapshot.at("turns").is_number_unsigned()) {
            throw std::runtime_error("任务会话无效: " + paths.id);
        }
        summary.status = snapshot.value("status", "unknown");
        if (summary.status != "completed" && !is_resumable_status(summary.status)) {
            throw std::runtime_error("任务会话终态无效: " + paths.id);
        }
        summary.verification_status = snapshot.value("verification_status", "not_required");
        summary.turns = snapshot.value("turns", std::size_t{0});
        summary.completed = summary.status == "completed";
        summary.resumable =
            summary.mode == ManagedTaskMode::model && is_resumable_status(summary.status);
    }
    return summary;
}

} // namespace

std::string_view managed_task_mode_name(ManagedTaskMode mode) noexcept {
    switch (mode) {
    case ManagedTaskMode::model:
        return "model";
    case ManagedTaskMode::demo:
        return "demo";
    }
    return "unknown";
}

std::filesystem::path default_aiagent_state_directory() {
#if defined(_WIN32)
    const char* base = std::getenv("LOCALAPPDATA");
    if (base == nullptr || std::string_view(base).empty()) {
        throw std::runtime_error("无法确定状态目录；请显式传入 --state-dir");
    }
    return std::filesystem::path(base) / "aiagent";
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (home == nullptr || std::string_view(home).empty()) {
        throw std::runtime_error("无法确定状态目录；请显式传入 --state-dir");
    }
    return std::filesystem::path(home) / "Library" / "Application Support" / "aiagent";
#else
    if (const char* xdg = std::getenv("XDG_STATE_HOME");
        xdg != nullptr && !std::string_view(xdg).empty()) {
        return std::filesystem::path(xdg) / "aiagent";
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr || std::string_view(home).empty()) {
        throw std::runtime_error("无法确定状态目录；请显式传入 --state-dir");
    }
    return std::filesystem::path(home) / ".local" / "state" / "aiagent";
#endif
}

Json managed_task_summary_to_json(const ManagedTaskSummary& summary) {
    return {{"id", summary.id},
            {"question", summary.question},
            {"created_at", summary.created_at},
            {"status", summary.status},
            {"verification_status", summary.verification_status},
            {"mode", managed_task_mode_name(summary.mode)},
            {"turns", summary.turns},
            {"completed", summary.completed},
            {"resumable", summary.resumable}};
}

ProjectStore::ProjectStore(std::filesystem::path workspace_root, std::filesystem::path state_root) {
    std::error_code error;
    workspace_root_ = std::filesystem::weakly_canonical(std::move(workspace_root), error);
    if (error || !std::filesystem::is_directory(workspace_root_)) {
        throw std::invalid_argument("项目根目录不存在或不是目录");
    }
    state_root_ = normalized_absolute(state_root.empty() ? default_aiagent_state_directory()
                                                         : std::move(state_root));
    if (state_root_ == workspace_root_ || is_inside(workspace_root_, state_root_) ||
        is_inside(state_root_, workspace_root_)) {
        throw std::invalid_argument("--state-dir 必须与项目工作区彼此独立，不能互相包含");
    }
    const auto workspace_hash = hexadecimal(fnv1a(workspace_root_.generic_string()), 16);
    project_directory_ = state_root_ / "projects" / workspace_hash;
}

const std::filesystem::path& ProjectStore::workspace_root() const noexcept {
    return workspace_root_;
}

const std::filesystem::path& ProjectStore::state_root() const noexcept {
    return state_root_;
}

const std::filesystem::path& ProjectStore::project_directory() const noexcept {
    return project_directory_;
}

std::filesystem::path ProjectStore::profile_path() const {
    return project_directory_ / "project.json";
}

std::filesystem::path ProjectStore::project_policy_path() const {
    return project_directory_ / "policy.json";
}

bool ProjectStore::initialized() const {
    return stored_file_state(profile_path()) == StoredFileState::plain &&
           stored_file_state(project_policy_path()) == StoredFileState::plain;
}

Json ProjectStore::load_profile() const {
    if (!initialized()) {
        throw std::runtime_error("项目尚未初始化；先运行 aiagent init");
    }
    const auto profile = SessionStore(profile_path()).load();
    if (!profile.is_object() || profile.value("schema_version", 0) != project_schema_version ||
        profile.value("workspace_root", "") != workspace_root_.generic_string() ||
        profile.value("policy_file", "") != "policy.json" || !profile.contains("project_kind") ||
        !profile.at("project_kind").is_string()) {
        throw std::runtime_error("aiagent 项目配置损坏或与当前工作区不匹配");
    }
    return profile;
}

void ProjectStore::initialize(const std::string& project_kind, const Json& policy,
                              bool force) const {
    if (!valid_project_kind(project_kind)) {
        throw std::invalid_argument("项目类型只能包含字母、数字、连字符和下划线");
    }
    try {
        (void)parse_task_policy(policy);
    } catch (const std::exception& error) {
        throw std::invalid_argument("项目 policy 无效: " + std::string(error.what()));
    }
    const auto profile_state = stored_file_state(profile_path());
    const auto policy_state = stored_file_state(project_policy_path());
    if (profile_state == StoredFileState::invalid || policy_state == StoredFileState::invalid ||
        (profile_state == StoredFileState::missing) != (policy_state == StoredFileState::missing)) {
        throw std::runtime_error("已有 aiagent 项目状态不完整或包含非普通文件");
    }
    const bool has_existing_project = profile_state == StoredFileState::plain;
    if (has_existing_project) {
        (void)load_profile();
    }
    if (has_existing_project && !force) {
        throw std::runtime_error("项目已经初始化；如需重新生成，请显式使用 --force");
    }
    create_private_directory(state_root_);
    create_private_directory(state_root_ / "projects");
    create_private_directory(project_directory_);
    create_private_directory(project_directory_ / "tasks");

    SessionStore(project_policy_path()).save(policy);
    SessionStore(profile_path())
        .save({{"schema_version", project_schema_version},
               {"workspace_root", workspace_root_.generic_string()},
               {"project_kind", project_kind},
               {"policy_file", "policy.json"},
               {"initialized_at", utc_timestamp(std::chrono::system_clock::now())}});
}

ManagedTaskPaths ProjectStore::create_task(const std::string& question,
                                           ManagedTaskMode mode) const {
    (void)load_profile();
    if (question.empty() || question.size() > max_question_bytes ||
        question.find('\0') != std::string::npos || !valid_json_text(question)) {
        throw std::invalid_argument("任务内容必须是 1 到 65536 字节的有效 UTF-8 文本");
    }
    const auto policy_snapshot = SessionStore(project_policy_path()).load();
    (void)parse_task_policy(policy_snapshot, project_policy_path());
    create_private_directory(project_directory_ / "tasks");
    std::string id;
    std::filesystem::path directory;
    for (int attempt = 0; attempt < 8; ++attempt) {
        id = task_id(question);
        directory = project_directory_ / "tasks" / id;
        std::error_code error;
        if (std::filesystem::create_directory(directory, error)) {
            restrict_directory_permissions(directory);
            break;
        }
        if (error) {
            throw std::runtime_error("无法创建任务目录: " + error.message());
        }
        directory.clear();
    }
    if (directory.empty()) {
        throw std::runtime_error("无法生成唯一任务 ID");
    }
    create_private_directory(directory);
    const ManagedTaskPaths paths{id,
                                 directory,
                                 directory / "task.json",
                                 directory / "policy.json",
                                 directory / "session.json",
                                 directory / "events.jsonl"};
    SessionStore(paths.policy).save(policy_snapshot);
    SessionStore(paths.metadata)
        .save({{"schema_version", task_schema_version},
               {"id", id},
               {"workspace_root", workspace_root_.generic_string()},
               {"question", question},
               {"mode", managed_task_mode_name(mode)},
               {"created_at", utc_timestamp(std::chrono::system_clock::now())},
               {"policy_file", "policy.json"},
               {"session_file", "session.json"},
               {"events_file", "events.jsonl"}});
    return paths;
}

ManagedTaskPaths ProjectStore::task_paths(const std::string& id) const {
    (void)load_profile();
    validate_task_id(id);
    const auto directory = project_directory_ / "tasks" / id;
    std::error_code error;
    const auto directory_status = std::filesystem::symlink_status(directory, error);
    if (error || !std::filesystem::is_directory(directory_status) ||
        std::filesystem::is_symlink(directory_status)) {
        throw std::runtime_error("找不到任务: " + id);
    }
    const ManagedTaskPaths paths{id,
                                 directory,
                                 directory / "task.json",
                                 directory / "policy.json",
                                 directory / "session.json",
                                 directory / "events.jsonl"};
    if (!is_plain_file(paths.metadata, error) || !is_plain_file(paths.policy, error)) {
        throw std::runtime_error("任务元数据不完整: " + id);
    }
    return paths;
}

std::vector<ManagedTaskSummary> ProjectStore::list_tasks() const {
    (void)load_profile();
    std::vector<ManagedTaskSummary> result;
    const auto tasks = project_directory_ / "tasks";
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(tasks, error), end; !error && iterator != end;
         iterator.increment(error)) {
        const auto status = iterator->symlink_status(error);
        if (error || !std::filesystem::is_directory(status) ||
            std::filesystem::is_symlink(status)) {
            error.clear();
            continue;
        }
        const auto id = iterator->path().filename().string();
        try {
            validate_task_id(id);
            result.push_back(summary_from(task_paths(id), workspace_root_));
        } catch (const std::exception&) {
            ManagedTaskSummary corrupt;
            corrupt.id = id;
            corrupt.status = "corrupt";
            result.push_back(std::move(corrupt));
        }
    }
    if (error) {
        throw std::runtime_error("无法列出任务目录: " + error.message());
    }
    std::sort(result.begin(), result.end(),
              [](const ManagedTaskSummary& left, const ManagedTaskSummary& right) {
                  return left.id > right.id;
              });
    return result;
}

std::optional<ManagedTaskSummary> ProjectStore::task_summary(const std::string& id) const {
    try {
        return summary_from(task_paths(id), workspace_root_);
    } catch (const std::runtime_error& error) {
        if (std::string(error.what()).starts_with("找不到任务")) {
            return std::nullopt;
        }
        throw;
    }
}

std::optional<ManagedTaskPaths> ProjectStore::latest_resumable_task() const {
    for (const auto& summary : list_tasks()) {
        if (summary.resumable) {
            return task_paths(summary.id);
        }
    }
    return std::nullopt;
}

} // namespace aiagent
