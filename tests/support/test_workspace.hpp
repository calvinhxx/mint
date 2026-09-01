#pragma once

#include "mint/domain/model.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace mint::test {

class TemporaryDirectory final {
  public:
    explicit TemporaryDirectory(
        std::filesystem::path base = std::filesystem::temp_directory_path()) {
        static std::atomic<std::uint64_t> sequence{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
        const auto process_id = static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
        const auto process_id = static_cast<std::uint64_t>(::getpid());
#endif
        const auto suffix = std::to_string(process_id) + '-' + std::to_string(stamp) + '-' +
                            std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
        path_ = std::move(base) / ("mint-tests-" + suffix);
        std::filesystem::create_directories(path_ / "workspace" / "src");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

inline void write_text(const std::filesystem::path& path, const std::string& content) {
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    if (error) {
        throw std::runtime_error("test setup could not create parent for " + path.string());
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("test setup could not create " + path.string());
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) {
        throw std::runtime_error("test setup could not finish writing " + path.string());
    }
}

[[nodiscard]] inline std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("test could not read " + path.string());
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

template <typename Callable> void expect_failure(Callable&& callable, const std::string& message) {
    EXPECT_ANY_THROW(std::forward<Callable>(callable)()) << message;
}

[[nodiscard]] inline bool has_entry(const Json& entries, const std::string& path) {
    for (const auto& entry : entries) {
        if (entry.value("path", "") == path) {
            return true;
        }
    }
    return false;
}

} // namespace mint::test
