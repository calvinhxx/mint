#include "command_process_tree.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include <csignal>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#endif

namespace mint::command_detail {
namespace {

struct ProcessSnapshot {
    pid_t pid = 0;
    pid_t parent = 0;
    pid_t group = 0;
    std::uint64_t identity = 0;
};

#if defined(__linux__)
std::optional<ProcessSnapshot> linux_process_snapshot(const std::filesystem::path& entry) {
    const auto name = entry.filename().string();
    if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char character) {
            return std::isdigit(character) != 0;
        })) {
        return std::nullopt;
    }

    std::ifstream input(entry / "stat");
    std::string line;
    if (!std::getline(input, line)) {
        return std::nullopt;
    }
    const auto command_end = line.rfind(')');
    if (command_end == std::string::npos || command_end + 2 >= line.size()) {
        return std::nullopt;
    }

    ProcessSnapshot process;
    try {
        process.pid = static_cast<pid_t>(std::stol(name));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    char state = 0;
    long long session = 0;
    long long tty = 0;
    long long terminal_group = 0;
    unsigned long long flags = 0;
    unsigned long long minor_faults = 0;
    unsigned long long child_minor_faults = 0;
    unsigned long long major_faults = 0;
    unsigned long long child_major_faults = 0;
    unsigned long long user_time = 0;
    unsigned long long system_time = 0;
    long long child_user_time = 0;
    long long child_system_time = 0;
    long long priority = 0;
    long long nice = 0;
    long long thread_count = 0;
    long long interval_timer = 0;
    std::istringstream fields(line.substr(command_end + 2));
    fields >> state >> process.parent >> process.group >> session >> tty >> terminal_group >>
        flags >> minor_faults >> child_minor_faults >> major_faults >> child_major_faults >>
        user_time >> system_time >> child_user_time >> child_system_time >> priority >> nice >>
        thread_count >> interval_timer >> process.identity;
    return fields ? std::optional(process) : std::nullopt;
}

std::vector<ProcessSnapshot> process_snapshot() {
    std::vector<ProcessSnapshot> result;
    std::error_code error;
    for (std::filesystem::directory_iterator entry("/proc", error), end; !error && entry != end;
         entry.increment(error)) {
        if (const auto process = linux_process_snapshot(entry->path())) {
            result.push_back(*process);
        }
        error.clear();
    }
    return result;
}
#elif defined(__APPLE__)
std::vector<ProcessSnapshot> process_snapshot() {
    const auto estimate = ::proc_listallpids(nullptr, 0);
    if (estimate <= 0) {
        return {};
    }
    std::vector<pid_t> process_ids(static_cast<std::size_t>(estimate) + 64, 0);
    const auto returned = ::proc_listallpids(process_ids.data(),
                                             static_cast<int>(process_ids.size() * sizeof(pid_t)));
    if (returned <= 0) {
        return {};
    }

    std::vector<ProcessSnapshot> result;
    result.reserve(process_ids.size());
    for (const auto process_id : process_ids) {
        if (process_id <= 0) {
            continue;
        }
        proc_bsdinfo information{};
        if (::proc_pidinfo(process_id, PROC_PIDTBSDINFO, 0, &information,
                           static_cast<int>(sizeof(information))) != sizeof(information)) {
            continue;
        }
        const auto identity =
            information.pbi_start_tvsec * 1'000'000ULL + information.pbi_start_tvusec;
        result.push_back({.pid = process_id,
                          .parent = static_cast<pid_t>(information.pbi_ppid),
                          .group = static_cast<pid_t>(information.pbi_pgid),
                          .identity = identity});
    }
    return result;
}
#else
std::vector<ProcessSnapshot> process_snapshot() {
    return {};
}
#endif

} // namespace

struct ProcessTreeMonitor::State {
    explicit State(pid_t process) : root(process) {
        tracked.emplace(root, 0);
    }

    pid_t root;
    std::unordered_map<pid_t, std::uint64_t> tracked;
    std::vector<pid_t> live;
};

ProcessTreeMonitor::ProcessTreeMonitor(pid_t root) : state_(std::make_unique<State>(root)) {
    (void)refresh();
}

ProcessTreeMonitor::~ProcessTreeMonitor() = default;

std::size_t ProcessTreeMonitor::refresh() {
    const auto processes = process_snapshot();
    std::unordered_map<pid_t, const ProcessSnapshot*> current;
    current.reserve(processes.size());
    for (const auto& process : processes) {
        current.emplace(process.pid, &process);
    }

    if (const auto root = current.find(state_->root);
        root != current.end() && state_->tracked.at(state_->root) == 0) {
        state_->tracked[state_->root] = root->second->identity;
    }
    for (const auto& process : processes) {
        if (process.group == state_->root) {
            state_->tracked.try_emplace(process.pid, process.identity);
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& process : processes) {
            if (state_->tracked.contains(process.pid)) {
                continue;
            }
            const auto parent = state_->tracked.find(process.parent);
            const auto current_parent = current.find(process.parent);
            if (parent == state_->tracked.end() || current_parent == current.end() ||
                (parent->second != 0 && parent->second != current_parent->second->identity)) {
                continue;
            }
            state_->tracked.emplace(process.pid, process.identity);
            changed = true;
        }
    }

    std::vector<pid_t> live;
    live.reserve(state_->tracked.size());
    for (const auto& [pid, identity] : state_->tracked) {
        const auto process = current.find(pid);
        if (process != current.end() && (identity == 0 || identity == process->second->identity)) {
            live.push_back(pid);
        }
    }
    state_->live = std::move(live);
    return state_->live.size();
}

void ProcessTreeMonitor::signal_all(int signal) {
    (void)refresh();
    (void)::kill(-state_->root, signal);
    for (const auto process : state_->live) {
        (void)::kill(process, signal);
    }
}

} // namespace mint::command_detail
