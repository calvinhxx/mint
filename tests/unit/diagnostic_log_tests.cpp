#include "logging/diagnostic_log_internal.hpp"
#include "mint/infrastructure/diagnostic_log.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

long current_process_id() {
#if defined(_WIN32)
    return static_cast<long>(::_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("mint-diagnostic-log-" + std::to_string(current_process_id()) + '-' +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        mint::diagnostics::shutdown();
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

class DiagnosticLogTest : public testing::Test {
  protected:
    void TearDown() override {
        mint::diagnostics::shutdown();
    }
};

TEST_F(DiagnosticLogTest, UsesWarnAsTheQuietDefault) {
    mint::diagnostics::configure({});

    EXPECT_EQ(mint::diagnostics::current_level(), "warn");
}

TEST_F(DiagnosticLogTest, AcceptsSupportedLevelsAndAliases) {
    mint::diagnostics::configure("debug");
    EXPECT_EQ(mint::diagnostics::current_level(), "debug");

    mint::diagnostics::configure("warning");
    EXPECT_EQ(mint::diagnostics::current_level(), "warn");

    mint::diagnostics::configure("off");
    EXPECT_EQ(mint::diagnostics::current_level(), "off");
}

TEST_F(DiagnosticLogTest, AppliesSeverityThresholds) {
    using mint::diagnostics::Level;

    mint::diagnostics::configure("info");

    EXPECT_FALSE(mint::diagnostics::enabled(Level::debug));
    EXPECT_TRUE(mint::diagnostics::enabled(Level::info));
    EXPECT_TRUE(mint::diagnostics::enabled(Level::warning));
}

TEST_F(DiagnosticLogTest, EmitsStructuredEventsWithoutAffectingControlFlow) {
    using mint::diagnostics::Level;

    mint::diagnostics::configure("debug");

    EXPECT_NO_THROW(
        mint::diagnostics::emit(Level::debug, "test.event", {{"count", 2}, {"completed", true}}));
    EXPECT_NO_THROW(mint::diagnostics::flush());
}

TEST_F(DiagnosticLogTest, EscapesConsoleMessageButKeepsStructuredFileValues) {
    TemporaryDirectory temporary;
    mint::diagnostics::LocalLogOptions options;
    options.directory = temporary.path() / "logs";
    options.console_enabled = false;
    options.file_level = "debug";
    const auto status = mint::diagnostics::configure_local(options);
    ASSERT_TRUE(status.file_enabled) << status.error;
    const auto unsafe_name = std::string("tool-") + "\xE2\x80\xAE" + "hidden";
    const auto fields = mint::diagnostics::detail::sanitize_fields(
        "tool.completed", {{"name", unsafe_name}, {"ok", true}});
    const auto console = mint::diagnostics::detail::console_message("tool.completed", fields);

    mint::diagnostics::emit(mint::diagnostics::Level::debug, "tool.completed",
                            {{"name", unsafe_name}, {"ok", true}});
    mint::diagnostics::flush();

    EXPECT_EQ(console.find("\xE2\x80\xAE"), std::string::npos);
    EXPECT_NE(console.find("tool-\\u202Ehidden"), std::string::npos);
    const auto record = mint::Json::parse(read_text(status.file_path));
    EXPECT_EQ(record.at("fields").at("name"), unsafe_name);
}

TEST_F(DiagnosticLogTest, DropsUnknownEventsAndWrongFieldTypes) {
    TemporaryDirectory temporary;
    mint::diagnostics::LocalLogOptions options;
    options.directory = temporary.path() / "logs";
    options.console_enabled = false;
    options.file_level = "debug";

    const auto status = mint::diagnostics::configure_local(options);
    ASSERT_TRUE(status.file_enabled) << status.error;
    mint::diagnostics::emit(mint::diagnostics::Level::info, "api-key-must-not-be-an-event",
                            {{"secret", "must-not-reach-disk"}});
    mint::diagnostics::emit(mint::diagnostics::Level::info, "process.started",
                            {{"mode", "run"}, {"file_enabled", "not-a-boolean"}});
    mint::diagnostics::flush();

    const auto contents = read_text(status.file_path);
    EXPECT_EQ(contents.find("api-key-must-not-be-an-event"), std::string::npos);
    EXPECT_EQ(contents.find("must-not-reach-disk"), std::string::npos);
    const auto record = mint::Json::parse(contents);
    EXPECT_EQ(record.at("fields").at("mode"), "run");
    EXPECT_FALSE(record.at("fields").contains("file_enabled"));
    EXPECT_EQ(record.at("fields").at("omitted_field_count"), 1);
}

TEST_F(DiagnosticLogTest, TruncatesUtf8FieldsWithoutCorruptingJson) {
    TemporaryDirectory temporary;
    mint::diagnostics::LocalLogOptions options;
    options.directory = temporary.path() / "logs";
    options.console_enabled = false;
    options.file_level = "debug";

    const auto status = mint::diagnostics::configure_local(options);
    ASSERT_TRUE(status.file_enabled) << status.error;
    const std::string model = std::string(255, 'a') + "模型";
    mint::diagnostics::emit(mint::diagnostics::Level::info, "model.request.completed",
                            {{"attempt", 1}, {"http_status", 200}, {"duration_ms", 5}});
    mint::diagnostics::emit(mint::diagnostics::Level::debug, "model.request.started",
                            {{"provider", "groq"}, {"adapter", "chat"}, {"model", model}});
    mint::diagnostics::flush();

    std::ifstream input(status.file_path);
    std::string line;
    ASSERT_TRUE(std::getline(input, line));
    mint::Json first_record;
    ASSERT_NO_THROW(first_record = mint::Json::parse(line));
    EXPECT_TRUE(first_record.is_object());
    ASSERT_TRUE(std::getline(input, line));
    const auto record = mint::Json::parse(line);
    EXPECT_NE(record.at("fields").at("model").get<std::string>().find("[truncated]"),
              std::string::npos);
}

TEST_F(DiagnosticLogTest, DefaultFileLevelKeepsRequestResultsButDropsDebugDetails) {
    TemporaryDirectory temporary;
    mint::diagnostics::LocalLogOptions options;
    options.directory = temporary.path() / "logs";
    options.console_enabled = false;

    const auto status = mint::diagnostics::configure_local(options);
    ASSERT_TRUE(status.file_enabled) << status.error;
    mint::diagnostics::emit(mint::diagnostics::Level::debug, "model.request.started",
                            {{"provider", "groq"}, {"adapter", "chat"}, {"model", "test"}});
    mint::diagnostics::emit(mint::diagnostics::Level::info, "model.request.completed",
                            {{"attempt", 1},
                             {"http_status", 200},
                             {"duration_ms", 12},
                             {"usage_available", true},
                             {"input_tokens", 100},
                             {"cached_tokens", 20},
                             {"cache_hit_rate", 0.2},
                             {"output_tokens", 10},
                             {"total_tokens", 110}});
    mint::diagnostics::flush();

    const auto contents = read_text(status.file_path);
    EXPECT_EQ(contents.find("model.request.started"), std::string::npos);
    const auto record = mint::Json::parse(contents);
    EXPECT_EQ(record.at("event"), "model.request.completed");
    EXPECT_EQ(record.at("fields").at("input_tokens"), 100);
    EXPECT_DOUBLE_EQ(record.at("fields").at("cache_hit_rate").get<double>(), 0.2);
    EXPECT_EQ(record.at("fields").at("output_tokens"), 10);
}

TEST_F(DiagnosticLogTest, RejectsUnknownLevels) {
    EXPECT_THROW(mint::diagnostics::configure("verbose"), std::invalid_argument);
}

TEST_F(DiagnosticLogTest, WritesPrivateStructuredJsonLinesAndDropsUnknownFields) {
    TemporaryDirectory temporary;
    mint::diagnostics::LocalLogOptions options;
    options.directory = temporary.path() / "logs";
    options.console_enabled = false;

    const auto status = mint::diagnostics::configure_local(options);
    ASSERT_TRUE(status.file_enabled) << status.error;
    mint::diagnostics::emit(mint::diagnostics::Level::info, "process.started",
                            {{"mode", "run"},
                             {"file_enabled", true},
                             {"api_key", "must-never-reach-disk"},
                             {"request_body", "secret prompt"}});
    mint::diagnostics::flush();

    const auto contents = read_text(status.file_path);
    ASSERT_FALSE(contents.empty());
    EXPECT_EQ(contents.find("must-never-reach-disk"), std::string::npos);
    EXPECT_EQ(contents.find("secret prompt"), std::string::npos);

    const auto record = mint::Json::parse(contents);
    EXPECT_EQ(record.at("schema_version"), 1);
    EXPECT_EQ(record.at("level"), "info");
    EXPECT_EQ(record.at("event"), "process.started");
    EXPECT_EQ(record.at("fields").at("mode"), "run");
    EXPECT_EQ(record.at("fields").at("omitted_field_count"), 2);

#if !defined(_WIN32)
    const auto directory_permissions = std::filesystem::status(options.directory).permissions();
    const auto file_permissions = std::filesystem::status(status.file_path).permissions();
    EXPECT_EQ(directory_permissions &
                  (std::filesystem::perms::group_all | std::filesystem::perms::others_all),
              std::filesystem::perms::none);
    EXPECT_EQ(file_permissions &
                  (std::filesystem::perms::group_all | std::filesystem::perms::others_all),
              std::filesystem::perms::none);
#endif
}

TEST_F(DiagnosticLogTest, RotatesBoundedPerProcessFiles) {
    TemporaryDirectory temporary;
    mint::diagnostics::LocalLogOptions options;
    options.directory = temporary.path() / "logs";
    options.console_enabled = false;
    options.file_level = "debug";
    options.max_file_bytes = 512;
    options.rotated_files = 2;

    const auto status = mint::diagnostics::configure_local(options);
    ASSERT_TRUE(status.file_enabled) << status.error;
    for (int attempt = 0; attempt < 40; ++attempt) {
        mint::diagnostics::emit(mint::diagnostics::Level::debug, "model.request.attempt",
                                {{"attempt", attempt + 1}, {"max_attempts", 40}});
    }
    mint::diagnostics::flush();
    EXPECT_TRUE(mint::diagnostics::current_status().file_enabled);

    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(options.directory)) {
        if (entry.is_regular_file()) {
            ++files;
            EXPECT_LE(entry.file_size(), options.max_file_bytes);
#if !defined(_WIN32)
            EXPECT_EQ(std::filesystem::status(entry.path()).permissions() &
                          (std::filesystem::perms::group_all | std::filesystem::perms::others_all),
                      std::filesystem::perms::none);
#endif
        }
    }
    EXPECT_GT(files, 1U);
    EXPECT_LE(files, options.rotated_files + 1);
}

TEST_F(DiagnosticLogTest, FileFailureFallsBackWithoutBreakingControlFlow) {
    TemporaryDirectory temporary;
    const auto blocker = temporary.path() / "not-a-directory";
    {
        std::ofstream output(blocker);
        output << "occupied";
    }
    mint::diagnostics::LocalLogOptions options;
    options.directory = blocker / "logs";
    options.console_enabled = false;

    mint::diagnostics::LogStatus status;
    EXPECT_NO_THROW(status = mint::diagnostics::configure_local(options));
    EXPECT_FALSE(status.file_enabled);
    EXPECT_FALSE(status.error.empty());
    EXPECT_NO_THROW(mint::diagnostics::emit(mint::diagnostics::Level::error, "process.failed"));
}

TEST_F(DiagnosticLogTest, InteractionModeCanKeepStderrReservedForControlMessages) {
    mint::diagnostics::LocalLogOptions options;
    options.console_enabled = false;
    options.file_level = "off";
    (void)mint::diagnostics::configure_local(options);

    testing::internal::CaptureStderr();
    mint::diagnostics::emit(mint::diagnostics::Level::critical, "process.failed",
                            {{"exit_code", 1}});
    mint::diagnostics::flush();
    EXPECT_TRUE(testing::internal::GetCapturedStderr().empty());
}

TEST_F(DiagnosticLogTest, RemovesExpiredMintLogsButLeavesOtherFilesAlone) {
    TemporaryDirectory temporary;
    const auto directory = temporary.path() / "logs";
    mint::diagnostics::LocalLogOptions options;
    options.directory = directory;
    options.console_enabled = false;
    const auto seed = mint::diagnostics::configure_local(options);
    ASSERT_TRUE(seed.file_enabled) << seed.error;
    mint::diagnostics::shutdown();
    std::filesystem::remove(seed.file_path);
    const auto expired = directory / "mint-20260101T000000000Z-p123-1.jsonl";
    const auto rotated = directory / "mint-20260101T000000000Z-p123-1.1.jsonl";
    const auto similar = directory / "mint-personal.jsonl";
    const auto unrelated = directory / "notes.jsonl";
    {
        std::ofstream(expired) << "old mint log";
        std::ofstream(rotated) << "old rotated mint log";
        std::ofstream(similar) << "belongs to the user";
        std::ofstream(unrelated) << "belongs to the user";
    }
    std::filesystem::last_write_time(expired, std::filesystem::file_time_type::clock::now() -
                                                  std::chrono::hours(24 * 8));
    std::filesystem::last_write_time(rotated, std::filesystem::file_time_type::clock::now() -
                                                  std::chrono::hours(24 * 8));
    std::filesystem::last_write_time(similar, std::filesystem::file_time_type::clock::now() -
                                                  std::chrono::hours(24 * 8));

    const auto status = mint::diagnostics::configure_local(options);

    ASSERT_TRUE(status.file_enabled) << status.error;
    EXPECT_FALSE(std::filesystem::exists(expired));
    EXPECT_FALSE(std::filesystem::exists(rotated));
    EXPECT_TRUE(std::filesystem::exists(similar));
    EXPECT_TRUE(std::filesystem::exists(unrelated));
}

TEST_F(DiagnosticLogTest, RefusesToChangePermissionsOnAnExistingSharedDirectory) {
#if defined(_WIN32)
    GTEST_SKIP() << "Windows ACL behavior is exercised by configure_local on Windows";
#else
    TemporaryDirectory temporary;
    const auto directory = temporary.path() / "shared";
    std::filesystem::create_directories(directory);
    std::filesystem::permissions(
        directory, std::filesystem::perms::owner_all | std::filesystem::perms::group_read,
        std::filesystem::perm_options::replace);
    const auto before = std::filesystem::status(directory).permissions();
    mint::diagnostics::LocalLogOptions options;
    options.directory = directory;
    options.console_enabled = false;

    const auto status = mint::diagnostics::configure_local(options);

    EXPECT_FALSE(status.file_enabled);
    EXPECT_FALSE(status.error.empty());
    EXPECT_EQ(std::filesystem::status(directory).permissions(), before);
#endif
}

TEST_F(DiagnosticLogTest, BoundsTheDirectoryWithoutDeletingRecentFiles) {
    TemporaryDirectory temporary;
    const auto directory = temporary.path() / "logs";
    mint::diagnostics::LocalLogOptions options;
    options.directory = directory;
    options.console_enabled = false;
    const auto seed = mint::diagnostics::configure_local(options);
    ASSERT_TRUE(seed.file_enabled) << seed.error;
    mint::diagnostics::shutdown();
    std::filesystem::remove(seed.file_path);

    const auto older = directory / "mint-20260101T000000000Z-p123-1.jsonl";
    const auto newer = directory / "mint-20260101T000001000Z-p123-2.jsonl";
    std::ofstream(older) << std::string(80, 'a');
    std::ofstream(newer) << std::string(80, 'b');
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(older, now - std::chrono::hours(72));
    std::filesystem::last_write_time(newer, now - std::chrono::hours(48));
    options.retention_days = 30;
    options.max_directory_bytes = 100;

    const auto status = mint::diagnostics::configure_local(options);

    ASSERT_TRUE(status.file_enabled) << status.error;
    EXPECT_FALSE(std::filesystem::exists(older));
    EXPECT_TRUE(std::filesystem::exists(newer));
}

TEST_F(DiagnosticLogTest, RefusesLogDirectoriesReachedThroughSymlinks) {
#if defined(_WIN32)
    GTEST_SKIP() << "Creating symlinks requires optional Windows privileges";
#else
    TemporaryDirectory temporary;
    const auto real = temporary.path() / "real";
    const auto alias = temporary.path() / "alias";
    std::filesystem::create_directories(real);
    std::filesystem::create_directory_symlink(real, alias);
    mint::diagnostics::LocalLogOptions options;
    options.directory = alias;
    options.console_enabled = false;

    const auto status = mint::diagnostics::configure_local(options);

    EXPECT_FALSE(status.file_enabled);
    EXPECT_NE(status.error.find("符号链接"), std::string::npos);
#endif
}

TEST_F(DiagnosticLogTest, FileLevelOffDoesNotCreateAPlaceholderFile) {
    TemporaryDirectory temporary;
    mint::diagnostics::LocalLogOptions options;
    options.directory = temporary.path() / "logs";
    options.console_enabled = false;
    options.file_level = "off";

    const auto status = mint::diagnostics::configure_local(options);

    EXPECT_FALSE(status.file_enabled);
    EXPECT_TRUE(status.error.empty());
    EXPECT_FALSE(std::filesystem::exists(options.directory));
}

} // namespace
