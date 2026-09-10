#include "mint/platform/file.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

using mint::platform::FileOpenMode;
using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;

class PlatformFileTest : public testing::Test {
  protected:
    void SetUp() override {
        static std::atomic_uint64_t sequence{0};
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            const auto candidate = std::filesystem::temp_directory_path() /
                                   ("mint-platform-files-" + std::to_string(stamp) + "-" +
                                    std::to_string(sequence.fetch_add(1)));
            if (std::filesystem::create_directory(candidate)) {
                directory = candidate;
                return;
            }
        }
        FAIL() << "Could not create the file fixture directory";
    }

    void TearDown() override {
        if (!directory.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        }
    }

    static void write_text(const std::filesystem::path& path, const std::string& text) {
        std::ofstream stream(path, std::ios::binary);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) {
            throw std::runtime_error("Could not write the synthetic file fixture");
        }
    }

    static std::string read_text(const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }

    std::filesystem::path directory;
};

TEST_F(PlatformFileTest, RegularOpenAndExclusiveCollisionPreserveExistingContent) {
    const auto path = directory / std::filesystem::path(u8"配置.bin");
    write_text(path, "existing content");
    std::error_code error;
    for (const auto mode : {FileOpenMode::read_existing, FileOpenMode::update_or_create}) {
        File file(mint::platform::open_regular_file(path, mode, error), &std::fclose);
        ASSERT_NE(file, nullptr) << error.message();
        EXPECT_FALSE(error);
        EXPECT_TRUE(mint::platform::verify_regular_file(file.get(), error));
        EXPECT_EQ(read_text(path), "existing content");
    }
    File collision(mint::platform::open_regular_file(path, FileOpenMode::create_new, error),
                   &std::fclose);
    EXPECT_EQ(collision, nullptr);
    EXPECT_EQ(error, std::errc::file_exists);
    EXPECT_EQ(read_text(path), "existing content");
}

TEST_F(PlatformFileTest, RegularFilesRejectDirectoriesAndNewHardLinkAliases) {
    std::error_code error;
    File folder(mint::platform::open_regular_file(directory, FileOpenMode::read_existing, error),
                &std::fclose);
    EXPECT_EQ(folder, nullptr);
    EXPECT_TRUE(error);

    const auto original = directory / "original.bin";
    const auto alias = directory / "alias.bin";
    write_text(original, "unchanged");
    File opened(mint::platform::open_regular_file(original, FileOpenMode::read_existing, error),
                &std::fclose);
    ASSERT_NE(opened, nullptr) << error.message();
    std::filesystem::create_hard_link(original, alias, error);
    if (error) {
        GTEST_SKIP() << "The fixture filesystem cannot create hard links: " << error.message();
    }
    EXPECT_FALSE(mint::platform::verify_regular_file(opened.get(), error));
    EXPECT_TRUE(error);
    File rejected(mint::platform::open_regular_file(alias, FileOpenMode::read_existing, error),
                  &std::fclose);
    EXPECT_EQ(rejected, nullptr);
    EXPECT_TRUE(error);
    EXPECT_TRUE(mint::platform::same_existing_entry(original, alias));
    EXPECT_EQ(read_text(original), "unchanged");
}

TEST_F(PlatformFileTest, RegularOpenRejectsFifosWithoutWaitingForAPeer) {
#if defined(_WIN32)
    GTEST_SKIP() << "POSIX FIFO open regression; Windows uses a different named-pipe API";
#else
    const auto fifo = directory / "unconnected.fifo";
    ASSERT_EQ(::mkfifo(fifo.c_str(), S_IRUSR | S_IWUSR), 0);
    for (const auto mode : {FileOpenMode::read_existing, FileOpenMode::update_or_create}) {
        SCOPED_TRACE(static_cast<int>(mode));
        // EN: Bound a blocking-open regression in a child process.
        // ZH-CN: 在子进程中限制打开操作发生阻塞回归时的等待。
        EXPECT_EXIT(
            {
                ::alarm(5);
                std::error_code error;
                File opened(mint::platform::open_regular_file(fifo, mode, error), &std::fclose);
                const bool rejected = opened == nullptr && error == std::errc::invalid_argument;
                opened.reset();
                ::_exit(rejected ? 0 : 1);
            },
            testing::ExitedWithCode(0), "");
    }
    EXPECT_TRUE(std::filesystem::is_fifo(fifo));
#endif
}

TEST_F(PlatformFileTest, FinalSymlinkIsNeitherOpenedNorConfusedWithItsTarget) {
    const auto original = directory / "original.bin";
    const auto alias = directory / "alias.bin";
    write_text(original, "unchanged");
    std::error_code error;
    std::filesystem::create_symlink(original, alias, error);
    if (error) {
        GTEST_SKIP() << "The fixture filesystem cannot create symlinks: " << error.message();
    }
    for (const auto mode :
         {FileOpenMode::read_existing, FileOpenMode::update_or_create, FileOpenMode::create_new}) {
        File rejected(mint::platform::open_regular_file(alias, mode, error), &std::fclose);
        EXPECT_EQ(rejected, nullptr);
        EXPECT_TRUE(error);
    }
    EXPECT_TRUE(mint::platform::same_existing_entry(alias, alias));
    EXPECT_FALSE(mint::platform::same_existing_entry(original, alias));
    EXPECT_EQ(read_text(original), "unchanged");
}

TEST_F(PlatformFileTest, LockRemainsBusyUntilItsOwningHandleCloses) {
    const auto path = directory / "transaction.lock";
    std::error_code error;
    File first(mint::platform::open_lock_file(path, error), &std::fclose);
    ASSERT_NE(first, nullptr) << error.message();
    File second(mint::platform::open_lock_file(path, error), &std::fclose);
    ASSERT_NE(second, nullptr) << error.message();
    ASSERT_TRUE(mint::platform::try_lock_file(first.get(), error)) << error.message();
    error = std::make_error_code(std::errc::io_error);
    EXPECT_FALSE(mint::platform::try_lock_file(second.get(), error));
    EXPECT_FALSE(error);
    {
        File unrelated(mint::platform::open_lock_file(path, error), &std::fclose);
        ASSERT_NE(unrelated, nullptr) << error.message();
    }
    EXPECT_FALSE(mint::platform::try_lock_file(second.get(), error));
    EXPECT_FALSE(error);
    first.reset();
    EXPECT_TRUE(mint::platform::try_lock_file(second.get(), error)) << error.message();
    EXPECT_FALSE(error);
}

TEST_F(PlatformFileTest, DirectWritesAndDurableReplacementPreserveBinaryContent) {
    const auto source = directory / "temporary.bin";
    const auto target = directory / "target.bin";
    write_text(target, "previous");
    std::error_code error;
    File file(mint::platform::open_regular_file(source, FileOpenMode::create_new, error),
              &std::fclose);
    ASSERT_NE(file, nullptr) << error.message();
    const std::string payload("first\0second\n", 13);
    ASSERT_TRUE(mint::platform::write_file(file.get(), payload, error)) << error.message();
    ASSERT_TRUE(mint::platform::sync_file(file.get(), error)) << error.message();
    file.reset();
    EXPECT_TRUE(mint::platform::replace_file_durably(source, target, error)) << error.message();
    EXPECT_FALSE(std::filesystem::exists(source));
    EXPECT_EQ(read_text(target), payload);
}

TEST_F(PlatformFileTest, ExclusiveTemporaryCreationDoesNotReplaceAnotherEntry) {
    const auto target = directory / "exclusive.bin";
    std::error_code error;
    File first(mint::platform::create_exclusive_file(target, error), &std::fclose);
    ASSERT_NE(first, nullptr) << error.message();
    ASSERT_EQ(std::fwrite("first", 1, 5, first.get()), 5U);
    first.reset();
    File collision(mint::platform::create_exclusive_file(target, error), &std::fclose);
    EXPECT_EQ(collision, nullptr);
    EXPECT_EQ(error, std::errc::file_exists);
    EXPECT_EQ(read_text(target), "first");
}

TEST_F(PlatformFileTest, ExistingEntryIdentityUsesTheFilesystemCaseRules) {
    const auto original = directory / "MixedCase.txt";
    const auto lowercase = directory / "mixedcase.txt";
    write_text(original, "first");
    std::error_code error;
    const auto aliases_existing = std::filesystem::exists(lowercase, error);
    ASSERT_FALSE(error) << error.message();
    EXPECT_EQ(mint::platform::same_existing_entry(original, lowercase), aliases_existing);
    if (!aliases_existing) {
        write_text(lowercase, "second");
        EXPECT_FALSE(mint::platform::same_existing_entry(original, lowercase));
    }
}

} // namespace
