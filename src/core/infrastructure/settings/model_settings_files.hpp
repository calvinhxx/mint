#pragma once

#include "mint/infrastructure/model_config_service.hpp"

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace mint::model_settings_detail {

inline constexpr std::size_t max_document_bytes = 256U * 1024U;

void scrub_memory(char* value, std::size_t bytes) noexcept;
void scrub(std::string& value) noexcept;
void scrub(Json& document) noexcept;

// EN: Erase owned temporary secrets on successful and exceptional exits.
// ZH-CN: 在正常返回和异常退出时擦除临时持有的密钥。
class SecretGuard final {
  public:
    explicit SecretGuard(std::string& value) noexcept : value_(&value) {}
    explicit SecretGuard(std::string* value) noexcept : value_(value) {}
    ~SecretGuard() {
        if (value_ != nullptr) {
            scrub(*value_);
        }
    }
    SecretGuard(const SecretGuard&) = delete;
    SecretGuard& operator=(const SecretGuard&) = delete;
    void release() noexcept {
        value_ = nullptr;
    }

  private:
    std::string* value_;
};

struct Document {
    Json value;
    std::string revision;
    Document(Json content, std::string version)
        : value(std::move(content)), revision(std::move(version)) {}
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&& other) noexcept = default;
    Document& operator=(Document&& other) noexcept {
        if (this != &other) {
            scrub(value);
            value = std::move(other.value);
            revision = std::move(other.revision);
        }
        return *this;
    }
    ~Document() {
        scrub(value);
    }
};

class FileLock final {
  public:
    explicit FileLock(const std::filesystem::path& directory);
    ~FileLock();
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

  private:
    std::FILE* stream_ = nullptr;
};

[[nodiscard]] std::optional<Document> read_document(const std::filesystem::path& path,
                                                    bool require_private);
void write_document(const std::filesystem::path& path, const Json& document);
[[nodiscard]] std::string unique_reference();

} // namespace mint::model_settings_detail
