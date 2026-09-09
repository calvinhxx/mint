#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "status") {
            std::cout << R"({"state_directory":"fixture-state","tasks":[]})" << '\n';
            return 0;
        }
    }
    const std::string scenario = argc > 1 ? argv[argc - 1] : "";
    if (scenario == "multiple_interactions" || scenario == "unterminated_interaction" ||
        scenario == "exited_interactions") {
        std::string output =
            R"({"channel":"mint_interaction","type":"task_ready","data":{"task_id":"fixture-task","events_path":""}})";
        if (scenario != "unterminated_interaction") {
            output +=
                "\n"
                R"({"channel":"mint_interaction","type":"command_approval","data":{"program":"fixture"}})"
                "\n";
        }
        std::cerr << output;
        std::cout << R"({"status":"completed","answer":"Interaction delivered"})" << '\n'
                  << std::flush;
        if (scenario == "exited_interactions") {
            std::ofstream(std::filesystem::current_path() / "fixture-exit-ready").put('1');
        }
        return 0;
    }
    if (scenario == "long_events" || scenario == "one_event") {
        const auto path = std::filesystem::current_path() / "fixture-events.jsonl";
        const auto count = scenario == "long_events" ? 8193 : 1;
        std::ofstream log(path, std::ios::binary | std::ios::trunc);
        const std::string padding(scenario == "long_events" ? 1024 : 0, 'x');
        for (int index = 1; index <= count; ++index) {
            log << R"({"seq":)" << (scenario == "one_event" ? 90'001 : index)
                << R"(,"type":"tool_completed","data":{"name":"read_file","padding":")" << padding
                << R"("}})";
            if (index != count) {
                log << '\n';
            }
        }
        log.close();
        std::cerr
            << R"({"channel":"mint_interaction","type":"task_ready","data":{"task_id":"fixture-task","events_path":)"
            << std::quoted(path.generic_string()) << "}}\n";
        std::cout << R"({"status":"completed","answer":"Events saved"})" << '\n';
        return 0;
    }
    if (scenario == "closed_approval_input") {
        std::filesystem::path cancel_file;
        for (int index = 1; index + 1 < argc; ++index) {
            if (std::string(argv[index]) == "--cancel-file") {
                cancel_file = argv[index + 1];
            }
        }
        std::fclose(stdin);
        std::cerr
            << R"({"channel":"mint_interaction","type":"command_approval","data":{"program":"fixture"}})"
            << '\n';
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!std::filesystem::exists(cancel_file) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        std::cout << R"({"status":"cancelled","answer":"Cancelled through the file channel"})"
                  << '\n';
        return 130;
    }
    if (scenario == "crash_after_result") {
        std::cout << R"({"status":"completed","answer":"Do not accept a crashed transport"})"
                  << '\n'
                  << std::flush;
#if defined(_WIN32)
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(-1));
#else
        std::raise(SIGTERM);
#endif
        return 99;
    }
    if (scenario == "missing_result") {
        std::cout << "{}\n";
        return 2;
    }
    if (scenario == "error") {
        std::cout << R"({"error":"fixture failure"})" << '\n';
        return 2;
    }
    const auto status =
        scenario == "partial" || scenario == "trailing_noise" ? "completed" : scenario;
    std::cout << R"({"status":")" << status
              << R"(","answer":"Saved task outcome","model":{"calls":1}})";
    if (scenario != "partial") {
        std::cout << '\n';
    }
    if (scenario == "trailing_noise") {
        std::cout << "{}\n";
    }
    if (status == "completed") {
        return 0;
    }
    if (status == "cancelled") {
        return 130;
    }
    return status == "timed_out" ? 124 : 2;
}
