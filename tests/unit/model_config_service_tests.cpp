#include "mint/infrastructure/model_config_service.hpp"

#include "test_workspace.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#if !defined(_WIN32)
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using mint::Json;
using mint::ModelAdapter;
using mint::ModelConfigError;
using mint::ModelConfigErrorCode;
using mint::ModelConfigService;
using mint::ModelConfigUpdate;
using mint::test::read_text;
using mint::test::write_text;

class ScopedEnvironment final {
  public:
    ScopedEnvironment(std::string name, const char* value) : name_(std::move(name)) {
        if (const auto* previous = std::getenv(name_.c_str())) {
            previous_ = previous;
        }
        set(value);
    }

    ~ScopedEnvironment() {
        set(previous_ ? previous_->c_str() : nullptr);
    }

  private:
    void set(const char* value) const {
#if defined(_WIN32)
        (void)::_putenv_s(name_.c_str(), value == nullptr ? "" : value);
#else
        if (value == nullptr) {
            (void)::unsetenv(name_.c_str());
        } else {
            (void)::setenv(name_.c_str(), value, 1);
        }
#endif
    }

    std::string name_;
    std::optional<std::string> previous_;
};

class ModelConfigServiceTest : public testing::Test {
  protected:
    mint::test::TemporaryDirectory temporary;
    std::filesystem::path state = temporary.path() / "state";
    ModelConfigService service{state};

    static ModelConfigUpdate request(std::string revision = {}) {
        return {.expected_revision = std::move(revision),
                .adapter = ModelAdapter::chat_completions,
                .base_url = "https://model.example/v1",
                .model = "test-model"};
    }

    std::filesystem::path legacy(Json document) {
        const auto path = temporary.path() / "legacy.json";
        write_text(path, document.dump(2));
        return path;
    }

    static void make_private(const std::filesystem::path& path) {
        std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                               std::filesystem::perms::owner_write);
    }

    template <typename Function>
    static void expect_error(ModelConfigErrorCode expected, Function function) {
        try {
            function();
            FAIL() << "Expected a model configuration error";
        } catch (const ModelConfigError& error) {
            EXPECT_EQ(error.code(), expected);
        }
    }
};

TEST_F(ModelConfigServiceTest, EmptyStateAndFixedPath) {
    const auto current = service.inspect();
    EXPECT_FALSE(current.configured);
    EXPECT_TRUE(current.revision.empty());
    EXPECT_EQ(current.path, state / "model.json");
    EXPECT_EQ(service.credentials_path(), state / "credentials.json");
    EXPECT_FALSE(std::filesystem::exists(service.config_path()));
}

TEST_F(ModelConfigServiceTest, NewStateDirectoryAcceptsATrailingSeparatorAndStaysPrivate) {
    ModelConfigService trailing(state / "");
    EXPECT_FALSE(trailing.inspect().configured);
    const auto saved = trailing.update(request());
    EXPECT_TRUE(saved.configured);
    EXPECT_EQ(service.inspect().revision, saved.revision);
    EXPECT_EQ(service.load_runtime().model, "test-model");
#if !defined(_WIN32)
    struct stat info = {};
    ASSERT_EQ(::stat(state.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0700);
#endif
}

TEST_F(ModelConfigServiceTest, ParentComponentsResolveAfterSymlinksWithoutChangingTheTarget) {
    const auto lexical = temporary.path() / "lexical";
    const auto physical = temporary.path() / "physical";
    std::filesystem::create_directories(lexical);
    std::filesystem::create_directories(physical / "child");
    std::error_code error;
    std::filesystem::create_directory_symlink(physical / "child", lexical / "alias", error);
    if (error) {
        GTEST_SKIP() << "Directory symlinks are unavailable";
    }
    ModelConfigService selected(lexical / "alias" / ".." / "state");
    auto change = request();
    change.api_key = "synthetic-path-key";
    (void)selected.update(change);
    EXPECT_TRUE(std::filesystem::exists(physical / "state" / "model.json"));
    EXPECT_FALSE(std::filesystem::exists(lexical / "state"));
    EXPECT_EQ(ModelConfigService(physical / "state").load_runtime().api_key, "synthetic-path-key");

    std::filesystem::create_directory_symlink(physical / "state", lexical / "linked-state", error);
    ASSERT_FALSE(error);
    ModelConfigService linked_leaf(lexical / "linked-state");
    expect_error(ModelConfigErrorCode::unavailable, [&] { (void)linked_leaf.load_runtime(); });
}

TEST_F(ModelConfigServiceTest, EmptyAndEnvironmentProfilesDoNotReadUnreferencedCredentials) {
    const ScopedEnvironment environment("MINT_TEST_ISOLATED_ENV_KEY", "synthetic-environment-key");
    (void)service.inspect();
    write_text(service.credentials_path(), "unrelated invalid credential document");
    make_private(service.credentials_path());
    EXPECT_FALSE(service.inspect().configured);
    write_text(service.config_path(),
               Json{{"endpoint", "https://model.example/v1/chat/completions"},
                    {"model", "environment-only"},
                    {"api_key_env", "MINT_TEST_ISOLATED_ENV_KEY"}}
                   .dump());
    make_private(service.config_path());
    const auto inspected = service.inspect();
    EXPECT_TRUE(inspected.credential_available);
    EXPECT_FALSE(inspected.saved_api_key);
    EXPECT_TRUE(inspected.config.api_key.empty());
    mint::ModelConfigSnapshot metadata;
    EXPECT_EQ(service.load_runtime({}, &metadata).api_key_env, "MINT_TEST_ISOLATED_ENV_KEY");
    EXPECT_EQ(metadata.revision, inspected.revision);
    EXPECT_EQ(read_text(service.credentials_path()), "unrelated invalid credential document");
}

TEST_F(ModelConfigServiceTest, ReferencedCredentialsFailClosedWhenTheStoreIsInvalid) {
    auto change = request();
    change.api_key = "synthetic-referenced-key";
    (void)service.update(change);
    write_text(service.credentials_path(), "invalid referenced credential document");
    expect_error(ModelConfigErrorCode::invalid_config, [&] { (void)service.inspect(); });
    expect_error(ModelConfigErrorCode::invalid_config, [&] { (void)service.load_runtime(); });
}

TEST_F(ModelConfigServiceTest, SaveIsRedactedAndRuntimeHydratesPrivateKey) {
    auto change = request();
    change.api_key = "unit-secret-do-not-display";
    const auto saved = service.update(change);
    ASSERT_TRUE(saved.configured);
    EXPECT_TRUE(saved.saved_api_key);
    EXPECT_TRUE(saved.credential_available);
    EXPECT_TRUE(saved.config.api_key.empty());
    EXPECT_EQ(saved.config.api_url, "https://model.example/v1/chat/completions");
    EXPECT_EQ(service.load_runtime().api_key, *change.api_key);
    EXPECT_EQ(service.inspect().revision, saved.revision);
    EXPECT_EQ(read_text(service.config_path()).find(*change.api_key), std::string::npos);
    EXPECT_NE(read_text(service.credentials_path()).find(*change.api_key), std::string::npos);
    const auto credentials = Json::parse(read_text(service.credentials_path()));
    EXPECT_EQ(credentials.at("schema_version"), 1);
    EXPECT_EQ(credentials.at("credentials").size(), 1);
#if !defined(_WIN32)
    struct stat info = {};
    ASSERT_EQ(::stat(service.config_path().c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600);
    ASSERT_EQ(::stat(service.credentials_path().c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600);
    ASSERT_EQ(::stat(state.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0700);
#endif
}

TEST_F(ModelConfigServiceTest, CrossInstanceConflictAndManualEditConflict) {
    const auto initial = service.update(request());
    ModelConfigService second(state);
    auto changed = request(initial.revision);
    changed.model = "new-model";
    const auto next = second.update(changed);
    expect_error(ModelConfigErrorCode::conflict,
                 [&] { (void)service.update(request(initial.revision)); });
    auto document = Json::parse(read_text(service.config_path()));
    document["max_retries"] = 1;
    write_text(service.config_path(), document.dump());
    expect_error(ModelConfigErrorCode::conflict,
                 [&] { (void)service.update(request(next.revision)); });
}

TEST_F(ModelConfigServiceTest, ConcurrentWritersHaveOneWinner) {
    const auto initial = service.update(request());
    auto writer = [&](const char* model) {
        ModelConfigService another(state);
        auto change = request(initial.revision);
        change.model = model;
        try {
            (void)another.update(change);
            return true;
        } catch (const ModelConfigError& error) {
            EXPECT_EQ(error.code(), ModelConfigErrorCode::conflict);
            return false;
        }
    };
    auto first = std::async(std::launch::async, writer, "first");
    auto second = std::async(std::launch::async, writer, "second");
    EXPECT_NE(first.get(), second.get());
}

TEST_F(ModelConfigServiceTest, ModelChangePreservesKeyEndpointChangeDoesNot) {
    auto change = request();
    change.api_key = "endpoint-one-secret";
    auto saved = service.update(change);
    auto model_only = request(saved.revision);
    model_only.model = "second-model";
    saved = service.update(model_only);
    EXPECT_EQ(service.load_runtime().api_key, "endpoint-one-secret");
    auto other_endpoint = request(saved.revision);
    other_endpoint.base_url = "https://other.example/v1";
    saved = service.update(other_endpoint);
    EXPECT_FALSE(saved.saved_api_key);
    EXPECT_TRUE(service.load_runtime().api_key.empty());
    EXPECT_EQ(read_text(service.credentials_path()).find("endpoint-one-secret"), std::string::npos);
}

TEST_F(ModelConfigServiceTest, AdapterChangeDoesNotTransferKeyOrProtocolCapabilities) {
    auto change = request();
    change.api_key = "chat-secret";
    const auto saved = service.update(change);
    auto response = request(saved.revision);
    response.adapter = ModelAdapter::responses;
    const auto changed = service.update(response);
    EXPECT_EQ(changed.config.api_url, "https://model.example/v1/responses");
    EXPECT_FALSE(changed.saved_api_key);
}

TEST_F(ModelConfigServiceTest, ManualEndpointChangeCannotTransferReferencedKey) {
    auto change = request();
    change.api_key = "never-transfer";
    (void)service.update(change);
    auto document = Json::parse(read_text(service.config_path()));
    document["endpoint"] = "https://other.example/chat/completions";
    write_text(service.config_path(), document.dump());
    expect_error(ModelConfigErrorCode::invalid_config, [&] { (void)service.load_runtime(); });
}

TEST_F(ModelConfigServiceTest, RemoveKeyAndRetainEarlierRuntimeSnapshot) {
    auto change = request();
    change.api_key = "snapshot-secret";
    const auto saved = service.update(change);
    const auto running = service.load_runtime();
    auto remove = request(saved.revision);
    remove.remove_api_key = true;
    const auto cleared = service.update(remove);
    EXPECT_FALSE(cleared.saved_api_key);
    EXPECT_TRUE(service.load_runtime().api_key.empty());
    EXPECT_EQ(running.api_key, "snapshot-secret");
    EXPECT_EQ(read_text(service.credentials_path()).find("snapshot-secret"), std::string::npos);
}

TEST_F(ModelConfigServiceTest, MigrationMovesInlineKeyPreservesAdvancedFieldsAndOriginal) {
    const auto path =
        legacy({{"provider", "custom"},
                {"adapter", "responses"},
                {"endpoint", "https://model.example/v1/responses"},
                {"model", "legacy-model"},
                {"api_key", "legacy-inline-secret"},
                {"max_retries", 7},
                {"request_timeout_seconds", 245},
                {"response_limits", {{"max_tool_calls", 17}}},
                {"capabilities",
                 {{"stream_usage", false}, {"token_limit_parameter", "max_output_tokens"}}}});
    const auto original = read_text(path);
    const auto imported = service.migrate_legacy(path);
    EXPECT_TRUE(imported.saved_api_key);
    EXPECT_TRUE(imported.config.api_key.empty());
    EXPECT_EQ(imported.config.max_retries, 7);
    EXPECT_EQ(service.load_runtime().api_key, "legacy-inline-secret");
    EXPECT_EQ(read_text(path), original);
    EXPECT_EQ(read_text(service.config_path()).find("legacy-inline-secret"), std::string::npos);
    auto change = request(imported.revision);
    change.adapter = ModelAdapter::responses;
    change.model = "updated-model";
    const auto updated = service.update(change);
    EXPECT_EQ(updated.config.max_retries, 7);
    EXPECT_EQ(updated.config.request_timeout_seconds, 245);
    EXPECT_EQ(updated.config.response_limits.max_tool_calls, 17U);
    ASSERT_TRUE(updated.config.capabilities.has_value());
    EXPECT_EQ(updated.config.capabilities->token_limit_parameter,
              mint::ModelTokenLimitParameter::max_output_tokens);
    const auto revision = updated.revision;
    const auto credentials = read_text(service.credentials_path());
    expect_error(ModelConfigErrorCode::conflict,
                 [&] { (void)service.migrate_legacy(temporary.path() / "missing.json"); });
    EXPECT_EQ(service.inspect().revision, revision);
    EXPECT_EQ(read_text(service.credentials_path()), credentials);
}

TEST_F(ModelConfigServiceTest, MigrationFindsOnlyMatchingLegacyCredentialAndPreservesOtherKeys) {
    (void)service.inspect();
    write_text(service.credentials_path(),
               Json{{"schema_version", 1},
                    {"credentials",
                     {{"v1:custom:https://model.example/v1/chat/completions", "matching-key"},
                      {"v1:custom:https://other.example/v1/chat/completions", "other-key"}}}}
                   .dump());
    make_private(service.credentials_path());
    const auto path = legacy({{"provider", "custom"},
                              {"endpoint", "https://model.example/v1/chat/completions"},
                              {"model", "legacy-model"},
                              {"api_key_env", "MINT_MISSING_TEST_KEY"}});
    EXPECT_TRUE(service.migrate_legacy(path).saved_api_key);
    EXPECT_EQ(service.load_runtime().api_key, "matching-key");
    EXPECT_NE(read_text(service.credentials_path()).find("other-key"), std::string::npos);
    EXPECT_EQ(Json::parse(read_text(service.credentials_path())).at("credentials").size(), 3);
}

TEST_F(ModelConfigServiceTest, ReplacementMigratesMatchingLegacyKeyAndPreservesSource) {
    auto change = request();
    change.api_key = "previous-managed-secret";
    const auto previous = service.update(change);
    const auto previous_reference =
        Json::parse(read_text(service.config_path())).at("credential_ref").get<std::string>();
    auto credentials = Json::parse(read_text(service.credentials_path()));
    credentials["credentials"]["v1:custom:https://legacy.example/v1/chat/completions"] =
        "matching-legacy-secret";
    credentials["credentials"]["v1:deepseek:https://legacy.example/v1/chat/completions"] =
        "other-provider-secret";
    credentials["credentials"]["v1:custom:https://other.example/v1/chat/completions"] =
        "other-endpoint-secret";
    write_text(service.credentials_path(), credentials.dump());
    const auto path = legacy({{"provider", "custom"},
                              {"endpoint", "https://legacy.example/v1/chat/completions"},
                              {"model", "replacement-model"},
                              {"api_key_env", "MINT_MISSING_TEST_LEGACY_KEY"},
                              {"max_retries", 3},
                              {"max_request_tokens", 16000},
                              {"response_limits", {{"max_tool_calls", 11}}}});
    const auto original = read_text(path);

    const auto imported = service.migrate_legacy(path, previous.revision);
    EXPECT_NE(imported.revision, previous.revision);
    EXPECT_FALSE(imported.is_override);
    EXPECT_TRUE(imported.saved_api_key);
    EXPECT_TRUE(imported.config.api_key.empty());
    EXPECT_TRUE(imported.config.api_key_env.empty());
    EXPECT_EQ(imported.config.model, "replacement-model");
    EXPECT_EQ(imported.config.max_retries, 3);
    EXPECT_EQ(imported.config.max_request_tokens, 16000U);
    EXPECT_EQ(imported.config.response_limits.max_tool_calls, 11U);
    EXPECT_EQ(service.load_runtime().api_key, "matching-legacy-secret");
    EXPECT_EQ(read_text(path), original);
    EXPECT_EQ(read_text(service.config_path()).find("secret"), std::string::npos);
    const auto committed = Json::parse(read_text(service.config_path()));
    const auto next_reference = committed.at("credential_ref").get<std::string>();
    EXPECT_NE(next_reference, previous_reference);
    credentials = Json::parse(read_text(service.credentials_path()));
    EXPECT_FALSE(credentials.at("credentials").contains(previous_reference));
    EXPECT_FALSE(credentials.at("bindings").contains(previous_reference));
    EXPECT_EQ(credentials.at("bindings").at(next_reference),
              (Json{{"adapter", "chat_completions"},
                    {"endpoint", "https://legacy.example/v1/chat/completions"}}));
    EXPECT_EQ(
        credentials.at("credentials").at("v1:deepseek:https://legacy.example/v1/chat/completions"),
        "other-provider-secret");
    EXPECT_EQ(
        credentials.at("credentials").at("v1:custom:https://other.example/v1/chat/completions"),
        "other-endpoint-secret");
}

TEST_F(ModelConfigServiceTest, ReplacementRequiresCurrentNonemptyRevisionWithoutWriting) {
    auto change = request();
    change.api_key = "unchanged-secret";
    const auto initial = service.update(change);
    change = request(initial.revision);
    change.model = "changed-elsewhere";
    const auto current = service.update(change);
    const auto model_before = read_text(service.config_path());
    const auto credentials_before = read_text(service.credentials_path());
    const auto path = legacy({{"endpoint", "https://legacy.example/v1/chat/completions"},
                              {"model", "replacement-model"},
                              {"api_key", "replacement-secret"}});
    const auto original = read_text(path);
    expect_error(ModelConfigErrorCode::conflict,
                 [&] { (void)service.migrate_legacy(path, initial.revision); });
    expect_error(ModelConfigErrorCode::conflict, [&] {
        (void)service.migrate_legacy(temporary.path() / "missing.json", initial.revision);
    });
    expect_error(ModelConfigErrorCode::conflict, [&] { (void)service.migrate_legacy(path); });
    expect_error(ModelConfigErrorCode::invalid_input,
                 [&] { (void)service.migrate_legacy(path, std::string{}); });
    EXPECT_EQ(read_text(service.config_path()), model_before);
    EXPECT_EQ(read_text(service.credentials_path()), credentials_before);
    EXPECT_EQ(read_text(path), original);
    ModelConfigService missing(temporary.path() / "missing-state");
    expect_error(ModelConfigErrorCode::conflict,
                 [&] { (void)missing.migrate_legacy(path, current.revision); });
    EXPECT_FALSE(std::filesystem::exists(missing.config_path()));
    EXPECT_FALSE(std::filesystem::exists(missing.credentials_path()));
}

TEST_F(ModelConfigServiceTest, ReplacementNeverReusesManagedKeysForAnotherEndpointOrAdapter) {
    std::string revision;
    for (const auto& [adapter, endpoint] :
         {std::pair{"chat_completions", "https://other.example/v1/chat/completions"},
          std::pair{"responses", "https://model.example/v1/chat/completions"}}) {
        auto change = request(revision);
        change.api_key = "never-inherit-managed-secret";
        const auto current = service.update(change);
        const auto key_reference =
            Json::parse(read_text(service.config_path())).at("credential_ref");
        auto credentials = Json::parse(read_text(service.credentials_path()));
        credentials["credentials"][std::string("v1:deepseek:") + endpoint] =
            "never-inherit-other-provider-secret";
        write_text(service.credentials_path(), credentials.dump());
        const auto path = legacy({{"provider", "custom"},
                                  {"adapter", adapter},
                                  {"endpoint", endpoint},
                                  {"model", "replacement-model"},
                                  {"credential_ref", key_reference}});
        const auto original = read_text(path);
        const auto replaced = service.migrate_legacy(path, current.revision);
        revision = replaced.revision;
        EXPECT_FALSE(replaced.saved_api_key);
        EXPECT_FALSE(replaced.credential_available);
        EXPECT_TRUE(service.load_runtime().api_key.empty());
        EXPECT_FALSE(Json::parse(read_text(service.config_path())).contains("credential_ref"));
        EXPECT_EQ(read_text(path), original);
    }
}

TEST_F(ModelConfigServiceTest, ReplacementRejectsInvalidLegacyFieldsAndManagedSources) {
    auto change = request();
    change.api_key = "retained-secret";
    const auto current = service.update(change);
    const auto model_before = read_text(service.config_path());
    const auto credentials_before = read_text(service.credentials_path());
    const Json valid = {{"endpoint", "https://legacy.example/v1/chat/completions"},
                        {"model", "replacement-model"}};
    for (const auto& [field, value] :
         {std::pair{"model", std::string(513, 'm')}, std::pair{"api_key", std::string(32769, 'k')},
          std::pair{"api_key", std::string("invalid\nsecret")},
          std::pair{"endpoint", std::string("https://legacy.example/?token=secret")}}) {
        auto invalid = valid;
        invalid[field] = value;
        const auto path = legacy(invalid);
        const auto original = read_text(path);
        expect_error(ModelConfigErrorCode::invalid_config,
                     [&] { (void)service.migrate_legacy(path, current.revision); });
        EXPECT_EQ(read_text(path), original);
    }
    const auto oversized = legacy(valid);
    write_text(oversized, std::string(256 * 1024 + 1, ' '));
    expect_error(ModelConfigErrorCode::invalid_config,
                 [&] { (void)service.migrate_legacy(oversized, current.revision); });
    for (const auto& path : {service.config_path(), service.credentials_path()}) {
        expect_error(ModelConfigErrorCode::invalid_input,
                     [&] { (void)service.migrate_legacy(path, current.revision); });
    }
    EXPECT_EQ(read_text(service.config_path()), model_before);
    EXPECT_EQ(read_text(service.credentials_path()), credentials_before);
}

TEST_F(ModelConfigServiceTest, ConcurrentReplacementsHaveOneCompleteWinner) {
    auto change = request();
    change.api_key = "previous-secret";
    const auto current = service.update(change);
    const auto first_path = temporary.path() / "first.json";
    const auto second_path = temporary.path() / "second.json";
    for (const auto& path : {first_path, second_path}) {
        write_text(path, Json{{"endpoint", "https://legacy.example/v1/chat/completions"},
                              {"model", path.stem().string()},
                              {"api_key", path.stem().string() + "-secret"}}
                             .dump());
    }
    const auto replace = [&](const std::filesystem::path& path) {
        try {
            (void)ModelConfigService(state).migrate_legacy(path, current.revision);
            return true;
        } catch (const ModelConfigError& error) {
            EXPECT_EQ(error.code(), ModelConfigErrorCode::conflict);
            return false;
        }
    };
    auto first = std::async(std::launch::async, replace, first_path);
    auto second = std::async(std::launch::async, replace, second_path);
    const auto first_won = first.get();
    EXPECT_NE(first_won, second.get());
    const auto runtime = service.load_runtime();
    EXPECT_EQ(runtime.model, first_won ? "first" : "second");
    EXPECT_EQ(runtime.api_key, runtime.model + "-secret");
    EXPECT_EQ(Json::parse(read_text(service.credentials_path())).at("credentials").size(), 1U);
    EXPECT_EQ(Json::parse(read_text(first_path)).at("api_key"), "first-secret");
    EXPECT_EQ(Json::parse(read_text(second_path)).at("api_key"), "second-secret");
}

TEST_F(ModelConfigServiceTest, EditingEquivalentLegacyUrlPreservesItsCredential) {
    const auto path = legacy({{"provider", "custom"},
                              {"endpoint", "https://MODEL.EXAMPLE:443/v1/chat/completions/"},
                              {"model", "legacy-model"},
                              {"api_key", "equivalent-endpoint-key"}});
    const auto imported = service.migrate_legacy(path);
    EXPECT_TRUE(service.update(request(imported.revision)).saved_api_key);
    EXPECT_EQ(service.load_runtime().api_key, "equivalent-endpoint-key");
}

TEST_F(ModelConfigServiceTest, EnvironmentMigrationDoesNotPersistTheValue) {
    const auto path = legacy({{"endpoint", "https://model.example/v1/chat/completions"},
                              {"model", "environment-model"},
                              {"api_key_env", "MINT_MISSING_TEST_ENVIRONMENT"}});
    const auto imported = service.migrate_legacy(path);
    EXPECT_FALSE(imported.saved_api_key);
    EXPECT_EQ(imported.config.api_key_env, "MINT_MISSING_TEST_ENVIRONMENT");
    EXPECT_EQ(service.load_runtime().api_key_env, "MINT_MISSING_TEST_ENVIRONMENT");
    EXPECT_FALSE(std::filesystem::exists(service.credentials_path()));
}

TEST_F(ModelConfigServiceTest, ExplicitOverrideNeverImportsOrUsesDefaultKey) {
    auto change = request();
    change.api_key = "shared-key";
    const auto shared = service.update(change);
    const auto path = legacy({{"endpoint", "https://override.example/v1/responses"},
                              {"adapter", "responses"},
                              {"model", "override-model"}});
    const auto override = service.inspect(path);
    EXPECT_TRUE(override.is_override);
    EXPECT_EQ(override.revision, shared.revision);
    EXPECT_FALSE(override.saved_api_key);
    EXPECT_EQ(service.load_runtime(path).model, "override-model");
    EXPECT_TRUE(service.load_runtime(path).api_key.empty());
    EXPECT_EQ(service.inspect().revision, shared.revision);
    EXPECT_EQ(service.load_runtime().api_key, "shared-key");
}

TEST_F(ModelConfigServiceTest, StandaloneOverridesNeverCreateSharedStorage) {
    const ScopedEnvironment key_environment("MINT_TEST_STANDALONE_KEY", "synthetic-env-key");
    for (const auto* field : {"api_key", "api_key_env"}) {
        const auto value = std::string_view(field) == "api_key" ? "synthetic-inline-key"
                                                                : "MINT_TEST_STANDALONE_KEY";
        const auto path = legacy({{"endpoint", "https://override.example/v1/chat/completions"},
                                  {"model", "standalone-model"},
                                  {field, value}});
        mint::ModelConfigSnapshot metadata;
        const auto runtime = service.load_runtime(path, &metadata);
        EXPECT_EQ(runtime.model, "standalone-model");
        EXPECT_EQ(runtime.api_key, std::string_view(field) == "api_key" ? value : "");
        EXPECT_EQ(runtime.api_key_env, std::string_view(field) == "api_key_env" ? value : "");
        EXPECT_TRUE(metadata.is_override);
        EXPECT_TRUE(metadata.credential_available);
        EXPECT_TRUE(metadata.config.api_key.empty());
        EXPECT_TRUE(metadata.revision.empty());
        EXPECT_TRUE(service.inspect(path).credential_available);
        EXPECT_FALSE(std::filesystem::exists(state));
    }
}

TEST_F(ModelConfigServiceTest, StandaloneOverridesWorkWithoutGlobalStateEnvironment) {
    const auto path = legacy({{"endpoint", "https://override.example/v1/chat/completions"},
                              {"model", "standalone-model"},
                              {"api_key", "synthetic-standalone-key"}});
    const ScopedEnvironment home("HOME", nullptr);
    const ScopedEnvironment local_app_data("LOCALAPPDATA", nullptr);
    const ScopedEnvironment xdg_state("XDG_STATE_HOME", nullptr);
    ModelConfigService unspecified;
    mint::ModelConfigSnapshot metadata;
    EXPECT_EQ(unspecified.load_runtime(path, &metadata).api_key, "synthetic-standalone-key");
    EXPECT_TRUE(metadata.is_override);
    EXPECT_TRUE(metadata.revision.empty());
    EXPECT_TRUE(unspecified.inspect(path).credential_available);
    expect_error(ModelConfigErrorCode::unavailable, [&] { (void)unspecified.config_path(); });
    expect_error(ModelConfigErrorCode::unavailable, [&] { (void)unspecified.load_runtime(); });
    expect_error(ModelConfigErrorCode::unavailable, [&] { unspecified.prepare_storage(); });

    auto change = request();
    change.api_key = "synthetic-explicit-state-key";
    const auto saved = service.update(change);
    EXPECT_TRUE(saved.saved_api_key);
    EXPECT_EQ(service.load_runtime().api_key, "synthetic-explicit-state-key");
    EXPECT_EQ(service.config_path(), state / "model.json");
}

TEST_F(ModelConfigServiceTest, StandaloneOverridesIgnoreAnUnusableSharedDirectory) {
    write_text(state, "an unrelated file blocks the state directory");
    const auto path = legacy({{"endpoint", "https://override.example/v1/chat/completions"},
                              {"model", "standalone-model"},
                              {"api_key", "synthetic-inline-key"}});
    mint::ModelConfigSnapshot metadata;
    EXPECT_EQ(service.load_runtime(path, &metadata).api_key, "synthetic-inline-key");
    EXPECT_TRUE(service.inspect(path).credential_available);
    EXPECT_TRUE(metadata.revision.empty());
    EXPECT_EQ(read_text(state), "an unrelated file blocks the state directory");
    expect_error(ModelConfigErrorCode::unavailable, [&] { (void)service.load_runtime(); });
}

TEST_F(ModelConfigServiceTest, AtomicWritesUseOnePrivateStagingDirectory) {
    auto change = request();
    change.api_key = "synthetic-staging-key";
    (void)service.update(change);
    const auto staging = state / ".model-staging";
    ASSERT_TRUE(std::filesystem::is_directory(staging));
    EXPECT_TRUE(std::filesystem::is_empty(staging));
    for (const auto& entry : std::filesystem::directory_iterator(state)) {
        if (entry.path() != staging) {
            EXPECT_FALSE(entry.path().filename().string().starts_with(".model-"));
        }
    }
    EXPECT_EQ(service.load_runtime().api_key, "synthetic-staging-key");
#if !defined(_WIN32)
    struct stat info = {};
    ASSERT_EQ(::stat(staging.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0700);
#endif
}

TEST_F(ModelConfigServiceTest, AtomicWritesRejectAStagingDirectorySymlink) {
    (void)service.inspect();
    const auto external = temporary.path() / "external";
    std::filesystem::create_directory(external);
    std::error_code error;
    std::filesystem::create_directory_symlink(external, state / ".model-staging", error);
    if (error) {
        GTEST_SKIP() << "Directory symlinks are unavailable";
    }
    auto change = request();
    change.api_key = "never-write-through-link";
    expect_error(ModelConfigErrorCode::unavailable, [&] { (void)service.update(change); });
    EXPECT_TRUE(std::filesystem::is_empty(external));
    EXPECT_FALSE(std::filesystem::exists(service.config_path()));
    EXPECT_FALSE(std::filesystem::exists(service.credentials_path()));
}

#if !defined(_WIN32)
TEST_F(ModelConfigServiceTest, OverridesDoNotAcquireTheSharedCredentialLock) {
    auto change = request();
    change.api_key = "synthetic-shared-key";
    const auto current = service.update(change);
    std::unique_ptr<std::FILE, decltype(&std::fclose)> held(
        std::fopen((state / "model.lock").c_str(), "r+b"), &std::fclose);
    ASSERT_NE(held, nullptr);
    ASSERT_EQ(::flock(::fileno(held.get()), LOCK_EX | LOCK_NB), 0);
    const auto path = legacy({{"endpoint", "https://override.example/v1/chat/completions"},
                              {"model", "standalone-model"},
                              {"api_key", "synthetic-independent-key"}});
    mint::ModelConfigSnapshot metadata;
    EXPECT_EQ(service.load_runtime(path, &metadata).api_key, "synthetic-independent-key");
    EXPECT_EQ(metadata.revision, current.revision);
    EXPECT_TRUE(service.inspect(path).is_override);
    expect_error(ModelConfigErrorCode::unavailable, [&] { (void)service.load_runtime(); });
}
#endif

TEST_F(ModelConfigServiceTest, LegacyFileRequiresExplicitImportAndNeverBecomesAFallback) {
    const auto path = legacy(
        {{"endpoint", "https://model.example/v1/chat/completions"}, {"model", "legacy-model"}});
    expect_error(ModelConfigErrorCode::unavailable, [&] { (void)service.load_runtime(); });
    EXPECT_FALSE(std::filesystem::exists(service.config_path()));
    (void)service.migrate_legacy(path);
    EXPECT_EQ(service.load_runtime().model, "legacy-model");
    write_text(path, "invalid new legacy text");
    EXPECT_EQ(service.load_runtime().model, "legacy-model");
    std::filesystem::remove(service.config_path());
    expect_error(ModelConfigErrorCode::unavailable, [&] { (void)service.load_runtime(); });
    EXPECT_FALSE(std::filesystem::exists(service.config_path()));
}

TEST_F(ModelConfigServiceTest, EmptyImportIsRejectedWithoutCreatingModelFiles) {
    expect_error(ModelConfigErrorCode::invalid_input, [&] { (void)service.migrate_legacy({}); });
    EXPECT_FALSE(std::filesystem::exists(service.config_path()));
    EXPECT_FALSE(std::filesystem::exists(service.credentials_path()));
}

TEST_F(ModelConfigServiceTest, ConcurrentImportsCannotReplaceAnExistingConfiguration) {
    const auto path = legacy({{"endpoint", "https://model.example/v1/chat/completions"},
                              {"model", "legacy-model"},
                              {"api_key", "concurrent-import-secret"}});
    std::atomic<int> imported{0};
    std::atomic<int> conflicts{0};
    const auto import = [&] {
        try {
            (void)ModelConfigService(state).migrate_legacy(path);
            ++imported;
        } catch (const ModelConfigError& error) {
            if (error.code() != ModelConfigErrorCode::conflict) {
                throw;
            }
            ++conflicts;
        }
    };
    auto first = std::async(std::launch::async, import);
    auto second = std::async(std::launch::async, import);
    EXPECT_NO_THROW(first.get());
    EXPECT_NO_THROW(second.get());
    EXPECT_EQ(imported.load(), 1);
    EXPECT_EQ(conflicts.load(), 1);
    EXPECT_EQ(service.load_runtime().api_key, "concurrent-import-secret");
}

TEST_F(ModelConfigServiceTest, ExplicitOverridesKeepLegacySemanticsAndIgnoreUnrelatedCredentials) {
    const auto path = legacy({{"endpoint", "https://override.example/v1/responses?token=legacy"},
                              {"adapter", "responses"},
                              {"model", "model\nlegacy"},
                              {"api_key_env", "MINT_MISSING_TEST_ENVIRONMENT"}});
    (void)service.inspect();
    write_text(service.credentials_path(), "invalid unrelated credentials");
    make_private(service.credentials_path());
    write_text(service.config_path(), "invalid unrelated shared model");
    make_private(service.config_path());
    const auto selected = service.inspect(path);
    EXPECT_EQ(selected.config.model, "model\nlegacy");
    EXPECT_EQ(selected.config.api_url, "https://override.example/v1/responses");
    EXPECT_EQ(service.load_runtime(path).api_url,
              "https://override.example/v1/responses?token=legacy");
    EXPECT_EQ(service.load_runtime(path).api_key_env, "MINT_MISSING_TEST_ENVIRONMENT");
    EXPECT_TRUE(selected.revision.empty());
    mint::ModelConfigSnapshot runtime_snapshot;
    EXPECT_EQ(service.load_runtime(path, &runtime_snapshot).model, "model\nlegacy");
    EXPECT_EQ(runtime_snapshot.config.api_url, "https://override.example/v1/responses");
    EXPECT_EQ(read_text(service.config_path()), "invalid unrelated shared model");
    expect_error(ModelConfigErrorCode::invalid_config,
                 [&] { (void)service.update(request(runtime_snapshot.revision)); });
}

TEST_F(ModelConfigServiceTest, RuntimeMetadataAndCredentialComeFromTheSameSnapshot) {
    auto change = request();
    change.api_key = "runtime-metadata-secret";
    const auto saved = service.update(change);
    mint::ModelConfigSnapshot metadata;
    const auto runtime = service.load_runtime({}, &metadata);
    EXPECT_EQ(metadata.revision, saved.revision);
    EXPECT_EQ(metadata.config.model, runtime.model);
    EXPECT_EQ(metadata.config.api_url, runtime.api_url);
    EXPECT_TRUE(metadata.config.api_key.empty());
    EXPECT_EQ(runtime.api_key, "runtime-metadata-secret");
    auto next = request(saved.revision);
    next.model = "next-model";
    (void)service.update(next);
    EXPECT_EQ(metadata.config.model, "test-model");
    EXPECT_EQ(runtime.model, "test-model");
}

TEST_F(ModelConfigServiceTest, RejectsInvalidInputsWithoutLeakingSubmittedValues) {
    for (const auto* url : {"http://remote.example", "https://user:secret@example.com/v1",
                            "https://model.example?token=secret", "https://model.example/#secret",
                            "file:///tmp/secret", "https://model.example\\secret"}) {
        auto change = request();
        change.base_url = url;
        change.api_key = "secret-do-not-display";
        try {
            (void)service.update(change);
            FAIL();
        } catch (const ModelConfigError& error) {
            EXPECT_EQ(error.code(), ModelConfigErrorCode::invalid_input);
            EXPECT_EQ(std::string(error.what()).find("secret"), std::string::npos);
        }
    }
    auto change = request();
    change.model = std::string(513, 'm');
    expect_error(ModelConfigErrorCode::invalid_input, [&] { (void)service.update(change); });
    change = request();
    change.api_key = std::string("key\nheader");
    expect_error(ModelConfigErrorCode::invalid_input, [&] { (void)service.update(change); });
    change.api_key = "key";
    change.remove_api_key = true;
    expect_error(ModelConfigErrorCode::invalid_input, [&] { (void)service.update(change); });
    EXPECT_FALSE(std::filesystem::exists(service.config_path()));
}

TEST_F(ModelConfigServiceTest, SupportsLoopbackAndNormalizesDefaultPorts) {
    auto change = request();
    change.base_url = " HTTPS://MODEL.EXAMPLE:443/v1/ ";
    auto saved = service.update(change);
    EXPECT_EQ(saved.config.api_url, "https://model.example/v1/chat/completions");
    change = request(saved.revision);
    change.base_url = "http://127.0.0.1:8080/v1";
    EXPECT_EQ(service.update(change).config.api_url, "http://127.0.0.1:8080/v1/chat/completions");
}

TEST_F(ModelConfigServiceTest, RejectsInvalidOrOversizedDocumentsWithoutSourceInError) {
    (void)service.inspect();
    write_text(service.config_path(), "{\"api_key\":\"secret\" invalid}");
    make_private(service.config_path());
    try {
        (void)service.inspect();
        FAIL();
    } catch (const ModelConfigError& error) {
        EXPECT_EQ(std::string(error.what()).find("secret"), std::string::npos);
    }
    write_text(service.config_path(), std::string(256 * 1024 + 1, ' '));
    expect_error(ModelConfigErrorCode::invalid_config, [&] { (void)service.inspect(); });
}

TEST_F(ModelConfigServiceTest, RejectsSymlinksAndHardlinks) {
    (void)service.inspect();
    const auto path =
        legacy({{"endpoint", "https://model.example/v1/chat/completions"}, {"model", "model"}});
    std::error_code error;
    std::filesystem::create_symlink(path, service.config_path(), error);
    if (!error) {
        expect_error(ModelConfigErrorCode::unavailable, [&] { (void)service.inspect(); });
        std::filesystem::remove(service.config_path());
    }
    std::filesystem::create_hard_link(path, service.config_path(), error);
    if (!error) {
        expect_error(ModelConfigErrorCode::unavailable, [&] { (void)service.inspect(); });
    }
}

#if !defined(_WIN32)
TEST_F(ModelConfigServiceTest, RejectsPublicStateAndCredentialFilePermissions) {
    std::filesystem::create_directory(state);
    std::filesystem::permissions(state, std::filesystem::perms::owner_all |
                                            std::filesystem::perms::others_read);
    expect_error(ModelConfigErrorCode::unavailable, [&] { (void)service.inspect(); });
    std::filesystem::permissions(state, std::filesystem::perms::owner_all);
    auto change = request();
    change.api_key = "synthetic-private-credential";
    (void)service.update(change);
    std::filesystem::permissions(service.credentials_path(),
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write |
                                     std::filesystem::perms::others_read);
    expect_error(ModelConfigErrorCode::unavailable, [&] { (void)service.inspect(); });
}

TEST_F(ModelConfigServiceTest, SeparateProcessCannotOverwriteStaleRevision) {
    const auto initial = service.update(request());
    const auto child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        try {
            auto change = request(initial.revision);
            change.model = "child-model";
            (void)ModelConfigService(state).update(change);
            ::_exit(0);
        } catch (...) {
            ::_exit(1);
        }
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    expect_error(ModelConfigErrorCode::conflict,
                 [&] { (void)service.update(request(initial.revision)); });
    EXPECT_EQ(service.inspect().config.model, "child-model");
}
#endif

} // namespace
