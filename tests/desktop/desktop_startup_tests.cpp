#include "app/gui_options.hpp"
#include "application/workspaces/workspace_catalog.hpp"

#include "mint/infrastructure/model_config_service.hpp"

#include <QByteArray>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <vector>

namespace {

std::optional<mint::desktop::GuiOptions> parse(std::vector<QByteArray> arguments) {
    arguments.insert(arguments.begin(), QByteArray("mint-gui"));
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    return mint::desktop::parse_gui_options(static_cast<int>(argv.size()), argv.data());
}

TEST(DesktopStartup, RequiredPathOptionsRejectMissingEmptyAndFlagValues) {
    for (const auto& option :
         {QByteArray("--root"), QByteArray("--state-dir"), QByteArray("--config")}) {
        EXPECT_FALSE(parse({option}).has_value()) << option.constData();
        EXPECT_FALSE(parse({option, ""}).has_value()) << option.constData();
        EXPECT_FALSE(parse({option, "--demo"}).has_value()) << option.constData();
    }
    const auto unspecified = parse({});
    ASSERT_TRUE(unspecified.has_value());
    EXPECT_TRUE(unspecified->root.empty());
    EXPECT_TRUE(unspecified->state_root.empty());

    const auto specified = parse(
        {"--root", "./project", "--state-dir", "./private-state", "--config", "./profile.json"});
    ASSERT_TRUE(specified.has_value());
    EXPECT_EQ(specified->root, std::filesystem::path("./project"));
    EXPECT_EQ(specified->state_root, std::filesystem::path("./private-state"));
    EXPECT_EQ(specified->config, std::filesystem::path("./profile.json"));
}

TEST(DesktopStartup, ResolvesSymlinkParentsBeforeCollapsingDotDotForWorkspaceAndState) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto base =
        std::filesystem::weakly_canonical(std::filesystem::path(directory.path().toStdString()));
    const auto apparent_parent = base / "apparent";
    const auto real_parent = base / "real";
    std::filesystem::create_directories(apparent_parent / "project");
    std::filesystem::create_directories(real_parent / "project");
    std::filesystem::create_directories(real_parent / "child");
    std::error_code error;
    std::filesystem::create_directory_symlink(real_parent / "child", apparent_parent / "link",
                                              error);
    if (error) {
        GTEST_SKIP() << "Directory symlinks are unavailable: " << error.message();
    }
    const auto requested_root = apparent_parent / "link" / ".." / "project";
    const auto requested_state = apparent_parent / "link" / ".." / "state";
    auto options = parse({"--root", QByteArray::fromStdString(requested_root.string()),
                          "--state-dir", QByteArray::fromStdString(requested_state.string())});
    ASSERT_TRUE(options.has_value());
    mint::desktop::resolve_gui_invocation_paths(*options);
    EXPECT_EQ(options->root, real_parent / "project");
    EXPECT_EQ(options->state_root, real_parent / "state");
    EXPECT_EQ(mint::desktop::select_startup_workspace(options->root, {}, options->state_root),
              real_parent / "project");

    EXPECT_NO_THROW(mint::ModelConfigService(options->state_root).prepare_storage());
    EXPECT_TRUE(std::filesystem::is_directory(real_parent / "state"));
    EXPECT_FALSE(std::filesystem::exists(apparent_parent / "state"));
}

TEST(DesktopStartup, StateDirectoryResolutionKeepsLeafSymlinksVisibleToPrivateStorageChecks) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto base =
        std::filesystem::weakly_canonical(std::filesystem::path(directory.path().toStdString()));
    const auto state = base / "state";
    mint::ModelConfigService(state).prepare_storage();
    const auto alias = base / "state-alias";
    std::error_code error;
    std::filesystem::create_directory_symlink(state, alias, error);
    if (error) {
        GTEST_SKIP() << "Directory symlinks are unavailable: " << error.message();
    }
    for (const auto& requested : {alias, alias / "", alias / "."}) {
        auto options = parse({"--state-dir", QByteArray::fromStdString(requested.string())});
        ASSERT_TRUE(options.has_value());
        mint::desktop::resolve_gui_invocation_paths(*options);
        EXPECT_EQ(options->state_root, alias);
        EXPECT_TRUE(std::filesystem::is_symlink(options->state_root));
        EXPECT_THROW(mint::ModelConfigService(options->state_root).prepare_storage(),
                     mint::ModelConfigError);
    }
}

} // namespace
