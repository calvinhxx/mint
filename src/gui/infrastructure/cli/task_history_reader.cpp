#include "task_history_reader.hpp"

#include "mint_cli_protocol.hpp"

#include <deque>
#include <fstream>
#include <iterator>

namespace mint::infrastructure::desktop {

mint::desktop::TaskHistory TaskHistoryReader::read(const std::filesystem::path& task_directory) {
    mint::desktop::TaskHistory history;
    std::deque<mint::desktop::RunEvent> events;
    std::ifstream input(task_directory / "events.jsonl", std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        auto event = MintCliProtocol::parse_event(line);
        if (!event.has_value()) {
            ++history.ignored_event_records;
            continue;
        }
        if (events.size() == mint::desktop::maximum_retained_run_events) {
            events.pop_front();
        }
        events.push_back(std::move(*event));
    }
    history.events.assign(std::make_move_iterator(events.begin()),
                          std::make_move_iterator(events.end()));

    std::ifstream session(task_directory / "session.json", std::ios::binary);
    if (session) {
        const std::string snapshot((std::istreambuf_iterator<char>(session)),
                                   std::istreambuf_iterator<char>());
        history.result = MintCliProtocol::parse_result(snapshot);
        history.invalid_snapshot = !snapshot.empty() && !history.result.has_value();
    }
    return history;
}

} // namespace mint::infrastructure::desktop
