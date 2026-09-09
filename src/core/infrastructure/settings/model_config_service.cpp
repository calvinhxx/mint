#include "mint/infrastructure/model_config_service.hpp"

#include "mint/infrastructure/config.hpp"
#include "mint/infrastructure/project_store.hpp"

#include "model_settings_files.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace mint {
namespace {

using model_settings_detail::Document;
using model_settings_detail::FileLock;
using model_settings_detail::read_document;
using model_settings_detail::scrub;
using model_settings_detail::write_document;

constexpr std::size_t max_key_bytes = 32U * 1024U;
constexpr std::size_t max_url_bytes = 8192;
constexpr std::size_t max_model_bytes = 512;
// EN: Room for 64 legacy entries and the previous/new managed keys.
// ZH-CN: 为 64 个旧条目以及前后两个托管密钥预留空间。
constexpr std::size_t max_credentials = 66;

[[noreturn]] void invalid_config() {
    throw ModelConfigError(ModelConfigErrorCode::invalid_config,
                           "The model configuration is invalid.");
}

[[noreturn]] void invalid_input() {
    throw ModelConfigError(ModelConfigErrorCode::invalid_input,
                           "Check the API type, HTTPS or local Base URL, model name and API key.");
}

bool valid_text(std::string_view value, std::size_t limit) {
    if (value.empty() || value.size() > limit ||
        std::any_of(value.begin(), value.end(),
                    [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; })) {
        return false;
    }
    try {
        auto encoded = Json(value).dump();
        scrub(encoded);
        return true;
    } catch (...) {
        return false;
    }
}

std::string trim(std::string value) {
    const auto space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), space).base(), value.end());
    return value;
}

void validate_endpoint_text(std::string_view endpoint) {
    if (!valid_text(endpoint, max_url_bytes) || endpoint.find_first_of("?#\\") != endpoint.npos ||
        std::any_of(endpoint.begin(), endpoint.end(),
                    [](unsigned char ch) { return std::isspace(ch) != 0; })) {
        invalid_config();
    }
    const auto scheme = endpoint.find("://");
    if (scheme == endpoint.npos) {
        invalid_config();
    }
    const auto authority_end = endpoint.find('/', scheme + 3);
    const auto authority = endpoint.substr(scheme + 3, authority_end - scheme - 3);
    if (authority.empty() || authority.find('@') != authority.npos ||
        authority.find('%') != authority.npos) {
        invalid_config();
    }
}

std::string normalized_url(std::string base_url) {
    base_url = trim(std::move(base_url));
    validate_endpoint_text(base_url);
    const auto scheme_end = base_url.find("://");
    const auto authority_end = base_url.find('/', scheme_end + 3);
    const auto prefix_end = authority_end == base_url.npos ? base_url.size() : authority_end;
    std::transform(base_url.begin(), base_url.begin() + static_cast<std::ptrdiff_t>(prefix_end),
                   base_url.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const auto authority = base_url.substr(scheme_end + 3, prefix_end - scheme_end - 3);
    const auto default_port = base_url.starts_with("https://") ? ":443" : ":80";
    if (authority.ends_with(default_port)) {
        base_url.erase(prefix_end - std::char_traits<char>::length(default_port),
                       std::char_traits<char>::length(default_port));
    }
    while (base_url.ends_with('/')) {
        base_url.pop_back();
    }
    return base_url;
}

std::string normalized_endpoint(std::string base_url, ModelAdapter adapter) {
    base_url = normalized_url(std::move(base_url));
    std::string_view suffix;
    switch (adapter) {
    case ModelAdapter::chat_completions:
        suffix = "/chat/completions";
        break;
    case ModelAdapter::responses:
        suffix = "/responses";
        break;
    case ModelAdapter::anthropic_messages:
        suffix = "/messages";
        break;
    default:
        invalid_input();
    }
    if (!base_url.ends_with(suffix)) {
        base_url += suffix;
    }
    return base_url;
}

ModelProviderConfig validated_config(const Json& document) {
    try {
        auto config = parse_model_provider_config(document, "model.json");
        validate_endpoint_text(config.api_url);
        if (!valid_text(config.model, max_model_bytes) ||
            (!config.api_key.empty() && !valid_text(config.api_key, max_key_bytes))) {
            scrub(config.api_key);
            invalid_config();
        }
        return config;
    } catch (...) {
        invalid_config();
    }
}

ModelProviderConfig override_config(const Json& document) {
    try {
        // EN: Explicit CLI profiles retain their protocol/escaping semantics.
        // ZH-CN: 显式 CLI 配置保留现有协议和转义语义。
        return parse_model_provider_config(document, "model override");
    } catch (...) {
        invalid_config();
    }
}

std::optional<Document> override_shared_revision(const ModelConfigService& service) {
    try {
        // EN: Only a revision is used; do not hydrate credentials or alter Windows ACLs.
        // ZH-CN: 仅读取版本号；不加载凭据，也不更改 Windows 文件 ACL。
        return read_document(service.config_path(), false);
    } catch (const ModelConfigError&) {
        // EN: Missing or unavailable shared storage must not disable a standalone profile.
        // ZH-CN: 缺失或不可用的共享存储不能禁用独立配置。
        return std::nullopt;
    }
}

std::string reference(const Json& document) {
    const auto found = document.find("credential_ref");
    if (found == document.end()) {
        return {};
    }
    if (!found->is_string()) {
        invalid_config();
    }
    const auto value = found->get<std::string>();
    if (!value.starts_with("model:") || value.size() != 38 ||
        !std::all_of(value.begin() + 6, value.end(),
                     [](unsigned char ch) { return std::isxdigit(ch) != 0; })) {
        invalid_config();
    }
    return value;
}

Document credentials_document(const std::filesystem::path& path) {
    auto existing = read_document(path, true);
    Document result{existing ? std::move(existing->value)
                             : Json{{"schema_version", 1}, {"credentials", Json::object()}},
                    {}};
    const auto version = result.value.find("schema_version");
    const auto entries = result.value.find("credentials");
    if (version == result.value.end() || !version->is_number_integer() || *version != 1 ||
        entries == result.value.end() || !entries->is_object() ||
        entries->size() > max_credentials) {
        invalid_config();
    }
    for (auto item = entries->begin(); item != entries->end(); ++item) {
        if (!valid_text(item.key(), 4096) || !item.value().is_string() ||
            !valid_text(item.value().get_ref<const std::string&>(), max_key_bytes)) {
            invalid_config();
        }
    }
    const auto bindings = result.value.find("bindings");
    if (bindings != result.value.end() && !bindings->is_object()) {
        invalid_config();
    }
    return result;
}

std::string stored_key(const Json& credentials, const std::string& key_reference,
                       const ModelProviderConfig& config) {
    if (key_reference.empty()) {
        return {};
    }
    const auto& entries = credentials.at("credentials");
    const auto found = entries.find(key_reference);
    if (found == entries.end()) {
        throw ModelConfigError(ModelConfigErrorCode::invalid_config,
                               "The saved API key is missing. Save the key again.");
    }
    const auto bindings = credentials.find("bindings");
    if (bindings == credentials.end() || !bindings->contains(key_reference)) {
        invalid_config();
    }
    const auto& binding = bindings->at(key_reference);
    if (!binding.is_object() || !binding.contains("adapter") || !binding.contains("endpoint") ||
        binding.at("adapter") != model_adapter_name(config.adapter) ||
        binding.at("endpoint") != config.api_url) {
        throw ModelConfigError(ModelConfigErrorCode::invalid_config,
                               "The saved API key does not match this endpoint. Save it again.");
    }
    return found->get<std::string>();
}

std::string legacy_key(const Json& credentials, const ModelProviderConfig& config) {
    const auto provider = resolve_model_provider_profile(config).provider;
    const auto scope = "v1:" + std::string(model_provider_name(provider)) + ':' + config.api_url;
    const auto& entries = credentials.at("credentials");
    const auto found = entries.find(scope);
    return found == entries.end() ? std::string{} : found->get<std::string>();
}

bool environment_available(const ModelProviderConfig& config) {
    if (config.api_key_env.empty()) {
        return false;
    }
    const auto* value = std::getenv(config.api_key_env.c_str());
    return value != nullptr && *value != '\0';
}

ModelConfigSnapshot snapshot(const std::optional<Document>& shared, const Document* selected,
                             const std::filesystem::path& path, const Json& credentials,
                             bool is_override) {
    ModelConfigSnapshot result;
    result.path = path;
    result.revision = shared ? shared->revision : std::string{};
    result.is_override = is_override;
    if (selected == nullptr) {
        return result;
    }
    result.config =
        is_override ? override_config(selected->value) : validated_config(selected->value);
    result.configured = true;
    std::string key = result.config.api_key;
    if (!is_override) {
        const auto selected_reference = reference(selected->value);
        if (!selected_reference.empty()) {
            if (!key.empty() || !result.config.api_key_env.empty()) {
                scrub(key);
                scrub(result.config.api_key);
                invalid_config();
            }
            key = stored_key(credentials, selected_reference, result.config);
        }
    }
    result.saved_api_key = !key.empty();
    result.credential_available = result.saved_api_key || environment_available(result.config);
    scrub(key);
    scrub(result.config.api_key);
    result.config.api_key.clear();
    if (is_override) {
        // EN: Legacy URL query parameters may contain credentials; never expose them.
        // ZH-CN: 旧 URL 的查询参数可能包含凭据，禁止在检查结果中展示。
        result.config.api_url.erase(result.config.api_url.find_first_of("?#") == std::string::npos
                                        ? result.config.api_url.size()
                                        : result.config.api_url.find_first_of("?#"));
    }
    return result;
}

void prune_managed_keys(Json& document, const std::string& keep) {
    auto& entries = document.at("credentials");
    for (auto entry = entries.begin(); entry != entries.end();) {
        if (entry.key().starts_with("model:") && entry.key() != keep) {
            if (document.contains("bindings")) {
                document.at("bindings").erase(entry.key());
            }
            scrub(entry.value());
            entry = entries.erase(entry);
        } else {
            ++entry;
        }
    }
}

void commit(const std::filesystem::path& config_path, const std::filesystem::path& credentials_path,
            Json& document, Json& credentials, const std::string& key,
            const std::string& previous_reference) {
    prune_managed_keys(credentials, previous_reference);
    std::string next_reference;
    if (!key.empty()) {
        next_reference = model_settings_detail::unique_reference();
        credentials.at("credentials")[next_reference] = key;
        auto config = validated_config(document);
        credentials["bindings"][next_reference] = {{"adapter", model_adapter_name(config.adapter)},
                                                   {"endpoint", config.api_url}};
        scrub(config.api_key);
        document["credential_ref"] = next_reference;
        document.erase("api_key_env");
        // EN: Copy-on-write keeps the old reference/key valid until model.json commits.
        // ZH-CN: 写时复制确保 model.json 提交前旧引用和旧密钥始终有效。
        write_document(credentials_path, credentials);
    } else {
        document.erase("credential_ref");
    }
    document.erase("api_key");
    write_document(config_path, document);
    // EN: Cleanup is not the commit point. A failure leaves an unused private key, not
    //     a model referring to an absent key. The next successful write retries cleanup.
    // ZH-CN: 清理不是提交点；清理失败只遗留未使用的私有密钥，不会产生悬空引用。
    //        后续成功写入会再次尝试清理。
    prune_managed_keys(credentials, next_reference);
    try {
        if (std::filesystem::exists(credentials_path)) {
            write_document(credentials_path, credentials);
        }
    } catch (...) {
    }
}

ModelConfigSnapshot migrate_locked(const std::filesystem::path& config_path,
                                   const std::filesystem::path& credentials_path,
                                   const std::filesystem::path& legacy_path,
                                   const std::optional<std::string>& expected_revision) {
    auto shared = read_document(config_path, true);
    if (expected_revision && (!shared || *expected_revision != shared->revision)) {
        throw ModelConfigError(ModelConfigErrorCode::conflict,
                               "Model settings changed elsewhere. Reload before importing.");
    }
    if (!expected_revision && shared) {
        throw ModelConfigError(ModelConfigErrorCode::conflict,
                               "Shared model settings already exist; import was not applied.");
    }
    // EN: Importing a managed destination as its own source would alter the original.
    // ZH-CN: 不能将托管目标文件作为导入源，否则会修改原文件。
    for (const auto& destination : {config_path, credentials_path}) {
        std::error_code error;
        if (std::filesystem::equivalent(legacy_path, destination, error)) {
            invalid_input();
        }
    }
    auto credentials = credentials_document(credentials_path);
    auto legacy = read_document(legacy_path, false);
    if (!legacy) {
        throw ModelConfigError(ModelConfigErrorCode::unavailable,
                               "The previous model configuration is unavailable.");
    }
    auto config = validated_config(legacy->value);
    std::string key =
        config.api_key.empty() ? legacy_key(credentials.value, config) : config.api_key;
    struct Erase {
        std::string& first;
        std::string& second;
        ~Erase() {
            scrub(first);
            scrub(second);
        }
    } erase{key, config.api_key};
    // EN: A supplied legacy reference must not select an unrelated managed key.
    // ZH-CN: 传入的旧引用不允许选取无关的托管密钥。
    legacy->value.erase("credential_ref");
    const auto previous_reference = shared ? reference(shared->value) : std::string{};
    commit(config_path, credentials_path, legacy->value, credentials.value, key,
           previous_reference);
    shared = read_document(config_path, true);
    return snapshot(shared, &*shared, config_path, credentials.value, false);
}

} // namespace

ModelConfigService::ModelConfigService(std::filesystem::path state_directory) {
    if (state_directory.empty()) {
        return;
    }
    try {
        state_directory_ = std::filesystem::absolute(std::move(state_directory)).lexically_normal();
    } catch (...) {
        throw ModelConfigError(ModelConfigErrorCode::unavailable,
                               "The model settings directory is unavailable.");
    }
}

ModelConfigService::StoragePaths ModelConfigService::storage_paths() const {
    try {
        const auto directory =
            state_directory_.empty()
                ? std::filesystem::absolute(default_mint_state_directory()).lexically_normal()
                : state_directory_;
        return {.directory = directory,
                .config = directory / "model.json",
                .credentials = directory / "credentials.json"};
    } catch (...) {
        throw ModelConfigError(ModelConfigErrorCode::unavailable,
                               "The model settings directory is unavailable.");
    }
}

std::filesystem::path ModelConfigService::config_path() const {
    return storage_paths().config;
}

void ModelConfigService::prepare_storage() const {
    const FileLock lock(storage_paths().directory);
}

std::filesystem::path ModelConfigService::credentials_path() const {
    return storage_paths().credentials;
}

ModelConfigSnapshot ModelConfigService::inspect(const std::filesystem::path& override_path) const {
    if (!override_path.empty()) {
        const auto selected = read_document(override_path, false);
        if (!selected) {
            throw ModelConfigError(ModelConfigErrorCode::unavailable,
                                   "The selected model configuration is unavailable.");
        }
        return snapshot(override_shared_revision(*this), &*selected, override_path, {}, true);
    }
    const auto paths = storage_paths();
    const FileLock lock(paths.directory);
    const auto shared = read_document(paths.config, true);
    const auto credentials = credentials_document(paths.credentials);
    return snapshot(shared, shared ? &*shared : nullptr, paths.config, credentials.value, false);
}

ModelConfigSnapshot ModelConfigService::update(ModelConfigUpdate update) {
    struct Erase {
        ModelConfigUpdate& update;
        ~Erase() {
            if (update.api_key)
                scrub(*update.api_key);
        }
    } erase{update};
    if (update.api_key && (update.remove_api_key || !valid_text(*update.api_key, max_key_bytes))) {
        invalid_input();
    }
    update.model = trim(std::move(update.model));
    std::string endpoint;
    try {
        endpoint = normalized_endpoint(std::move(update.base_url), update.adapter);
        (void)validated_config({{"provider", "custom"},
                                {"adapter", model_adapter_name(update.adapter)},
                                {"endpoint", endpoint},
                                {"model", update.model}});
    } catch (...) {
        invalid_input();
    }
    const auto paths = storage_paths();
    const FileLock lock(paths.directory);
    const auto shared = read_document(paths.config, true);
    if (update.expected_revision != (shared ? shared->revision : std::string{})) {
        throw ModelConfigError(ModelConfigErrorCode::conflict,
                               "Model settings changed elsewhere. Reload before saving.");
    }
    auto credentials = credentials_document(paths.credentials);
    Document next{shared ? shared->value : Json{{"stream", true}}, {}};
    std::string key;
    struct EraseKey {
        std::string& key;
        ~EraseKey() {
            scrub(key);
        }
    } erase_key{key};
    std::string previous_reference;
    bool same_endpoint = false;
    bool same_adapter = false;
    if (shared) {
        auto previous = validated_config(shared->value);
        previous_reference = reference(shared->value);
        same_adapter = previous.adapter == update.adapter;
        same_endpoint = same_adapter && normalized_url(previous.api_url) == endpoint;
        if (same_endpoint && !update.remove_api_key && !update.api_key) {
            key = previous_reference.empty()
                      ? previous.api_key
                      : stored_key(credentials.value, previous_reference, previous);
        }
        scrub(previous.api_key);
    }
    if (update.api_key) {
        key = *update.api_key;
    }
    if (!same_endpoint || update.remove_api_key || update.api_key) {
        next.value.erase("api_key_env");
    }
    if (!same_adapter) {
        // EN: Protocol-specific capabilities cannot be carried to another API type.
        // ZH-CN: 协议专属的能力设置不能沿用到另一种 API 类型。
        next.value.erase("capabilities");
    }
    if (!same_endpoint) {
        next.value["provider"] = "custom";
    }
    next.value["adapter"] = model_adapter_name(update.adapter);
    next.value["endpoint"] = endpoint;
    next.value.erase("api_url");
    next.value["model"] = update.model;
    next.value.erase("api_key");
    next.value.erase("credential_ref");
    (void)validated_config(next.value);
    commit(paths.config, paths.credentials, next.value, credentials.value, key, previous_reference);
    const auto committed = read_document(paths.config, true);
    return snapshot(committed, &*committed, paths.config, credentials.value, false);
}

ModelConfigSnapshot
ModelConfigService::migrate_legacy(const std::filesystem::path& legacy_path,
                                   std::optional<std::string> expected_revision) {
    if (legacy_path.empty() || (expected_revision && expected_revision->empty())) {
        invalid_input();
    }
    const auto paths = storage_paths();
    const FileLock lock(paths.directory);
    return migrate_locked(paths.config, paths.credentials, legacy_path, expected_revision);
}

ModelProviderConfig ModelConfigService::load_runtime(const std::filesystem::path& override_path,
                                                     ModelConfigSnapshot* inspected) const {
    if (!override_path.empty()) {
        const auto selected = read_document(override_path, false);
        if (!selected) {
            throw ModelConfigError(ModelConfigErrorCode::unavailable,
                                   "The selected model configuration is unavailable.");
        }
        auto config = override_config(selected->value);
        if (inspected != nullptr) {
            *inspected =
                snapshot(override_shared_revision(*this), &*selected, override_path, {}, true);
        }
        return config;
    }
    const auto paths = storage_paths();
    const FileLock lock(paths.directory);
    const auto selected = read_document(paths.config, true);
    if (!selected) {
        throw ModelConfigError(ModelConfigErrorCode::unavailable,
                               "No model is configured. Save model settings or use mint config "
                               "import --config path.");
    }
    auto config = validated_config(selected->value);
    const auto key_reference = reference(selected->value);
    std::optional<Document> credentials;
    if (!key_reference.empty()) {
        credentials = credentials_document(paths.credentials);
    }
    if (inspected != nullptr) {
        *inspected = snapshot(selected, &*selected, paths.config,
                              credentials ? credentials->value : Json{}, false);
    }
    if (!key_reference.empty()) {
        if (!config.api_key.empty() || !config.api_key_env.empty()) {
            scrub(config.api_key);
            invalid_config();
        }
        config.api_key = stored_key(credentials->value, key_reference, config);
    }
    return config;
}

} // namespace mint
