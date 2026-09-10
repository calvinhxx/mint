#include "model_settings_files.hpp"

#include "mint/platform/file.hpp"

#include "filesystem/output_path.hpp"
#include "filesystem/private_path.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>
#include <thread>

namespace mint::model_settings_detail {
namespace {

[[noreturn]] void file_error() {
    throw ModelConfigError(ModelConfigErrorCode::unavailable,
                           "Cannot access the private model settings files.");
}

[[noreturn]] void document_error() {
    throw ModelConfigError(ModelConfigErrorCode::invalid_config,
                           "The model settings file is invalid or too large.");
}

using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;

File open_file(const std::filesystem::path& path, platform::FileOpenMode mode) {
    std::error_code error;
    File stream(platform::open_regular_file(path, mode, error), &std::fclose);
    if (!stream) {
        file_error();
    }
    return stream;
}

void verify_file(std::FILE* stream, bool require_private) {
    std::error_code error;
    if (!platform::verify_regular_file(stream, error)) {
        file_error();
    }
    if (require_private) {
        try {
            private_path::verify_open_file(stream);
        } catch (...) {
            file_error();
        }
    }
}

std::filesystem::path checked_path(const std::filesystem::path& path) {
    try {
        return infrastructure_detail::validated_output_path(
            path, "model settings", infrastructure_detail::HardLinkPolicy::reject);
    } catch (...) {
        file_error();
    }
}

std::string revision_for(std::string_view text) {
    // EN: Change detection only, not a credential hash or authentication mechanism.
    // ZH-CN: 仅用于变更检测，不是凭据哈希或身份认证机制。
    std::uint64_t first = 14695981039346656037ULL;
    std::uint64_t second = 7809847782465536322ULL;
    for (const unsigned char byte : text) {
        first = (first ^ byte) * 1099511628211ULL;
        second = (second ^ byte) * 14029467366897019727ULL;
    }
    std::ostringstream result;
    result << std::hex << std::setfill('0') << std::setw(16) << first << std::setw(16) << second;
    return result.str();
}

bool lock_file(std::FILE* stream) {
    std::error_code error;
    if (platform::try_lock_file(stream, error)) {
        return true;
    }
    if (error && error != std::errc::interrupted) {
        file_error();
    }
    return false;
}

void flush_file(std::FILE* stream) {
    std::error_code error;
    if (!platform::sync_file(stream, error)) {
        file_error();
    }
}

void write_private_file(std::FILE* stream, std::string_view text) {
    verify_file(stream, true);
    std::error_code error;
    if (!platform::write_file(stream, text, error)) {
        file_error();
    }
}

} // namespace

void scrub_memory(char* value, std::size_t bytes) noexcept {
    // EN: Volatile writes keep owned secret erasure observable under optimization.
    // ZH-CN: 使用 volatile 写入，避免优化器省略持有密钥的擦除操作。
    volatile char* cursor = value;
    while (bytes-- != 0) {
        *cursor++ = '\0';
    }
}

void scrub(std::string& value) noexcept {
    scrub_memory(value.data(), value.size());
}

void scrub(Json& document) noexcept {
    if (document.is_string()) {
        scrub(document.get_ref<std::string&>());
    } else if (document.is_structured()) {
        for (auto& value : document) {
            scrub(value);
        }
    }
}

FileLock::FileLock(const std::filesystem::path& directory) {
    try {
        private_path::ensure_directory(directory, "model settings directory");
        const auto path = checked_path(directory / "model.lock");
        auto stream = open_file(path, platform::FileOpenMode::update_or_create);
        verify_file(stream.get(), true);
        private_path::secure_open_file(path, stream.get());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!lock_file(stream.get())) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw ModelConfigError(ModelConfigErrorCode::unavailable,
                                       "Model settings are busy. Try again.");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        stream_ = stream.release();
    } catch (const ModelConfigError&) {
        throw;
    } catch (...) {
        file_error();
    }
}

FileLock::~FileLock() {
    if (stream_ != nullptr) {
        // EN: Closing the handle releases the cross-process advisory lock.
        // ZH-CN: 关闭句柄会释放跨进程建议锁。
        std::fclose(stream_);
    }
}

std::optional<Document> read_document(const std::filesystem::path& path, bool require_private) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && status.type() == std::filesystem::file_type::not_found)) {
        return std::nullopt;
    }
    if (error) {
        file_error();
    }
    const auto checked = checked_path(path);
    auto stream = open_file(checked, platform::FileOpenMode::read_existing);
    verify_file(stream.get(), require_private);
    if (std::setvbuf(stream.get(), nullptr, _IONBF, 0) != 0) {
        file_error();
    }
    std::string source;
    const SecretGuard erase(source);
    // EN: The bounded reserve avoids leaving secret fragments in reallocated buffers.
    // ZH-CN: 按文件上限预留空间，避免扩容释放的缓冲区残留密钥片段。
    source.reserve(max_document_bytes);
    std::array<char, 8192> buffer{};
    struct EraseBuffer {
        std::array<char, 8192>& value;
        ~EraseBuffer() {
            scrub_memory(value.data(), value.size());
        }
    } erase_buffer{buffer};
    for (;;) {
        const auto bytes = std::fread(buffer.data(), 1, buffer.size(), stream.get());
        if (source.size() + bytes > max_document_bytes) {
            document_error();
        }
        source.append(buffer.data(), bytes);
        scrub_memory(buffer.data(), buffer.size());
        if (bytes < buffer.size()) {
            if (std::ferror(stream.get()) != 0) {
                file_error();
            }
            break;
        }
    }
    Document document{Json::parse(source, nullptr, false), {}};
    if (!document.value.is_object()) {
        document_error();
    }
    document.revision = revision_for(source);
    return document;
}

std::string unique_reference() {
    std::random_device random;
    std::ostringstream result;
    result << "model:" << std::hex << std::setfill('0');
    for (int index = 0; index < 4; ++index) {
        result << std::setw(8) << static_cast<std::uint32_t>(random());
    }
    return result.str();
}

void write_document(const std::filesystem::path& path, const Json& document) {
    const auto destination = checked_path(path);
    const auto staging = destination.parent_path() / ".model-staging";
    try {
        // EN: Keep in-flight credential copies inside one protected private directory.
        // ZH-CN: 将提交中的凭据副本限制在一个受保护的私有目录内。
        private_path::ensure_directory(staging, "model settings staging directory");
    } catch (...) {
        file_error();
    }
    auto temporary = staging / (".model-" + unique_reference().substr(6) + ".tmp");
    struct RemoveTemporary {
        const std::filesystem::path& path;
        bool created = false;
        ~RemoveTemporary() {
            if (created) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        }
    } remove{temporary};
    auto stream = open_file(temporary, platform::FileOpenMode::create_new);
    remove.created = true;
    private_path::secure_open_file(temporary, stream.get());
    auto source = document.dump(2);
    const SecretGuard erase(source);
    if (source.size() >= max_document_bytes) {
        document_error();
    }
    write_private_file(stream.get(), source);
    write_private_file(stream.get(), "\n");
    flush_file(stream.get());
    stream.reset();
    (void)checked_path(destination);
    std::error_code error;
    if (!platform::replace_file_durably(temporary, destination, error)) {
        file_error();
    }
}

} // namespace mint::model_settings_detail
