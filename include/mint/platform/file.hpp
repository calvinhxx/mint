#pragma once

#include <cstdio>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace mint::platform {

enum class FileOpenMode { read_existing, update_or_create, create_new };

// EN: Open without following the final link and accept only a single-link regular file.
//     Privacy is enforced separately; callers own the returned stream.
// ZH-CN: 打开时不跟随末尾链接，且只接受单硬链的普通文件。
//        私有权限由独立策略负责；返回的流由调用者关闭。
[[nodiscard]] std::FILE* open_regular_file(const std::filesystem::path& path, FileOpenMode mode,
                                           std::error_code& error) noexcept;

// EN: Lock-file handles also deny removal while open on platforms supporting that restriction.
// ZH-CN: 在支持此限制的平台上，锁文件句柄还会阻止文件在打开期间被删除。
[[nodiscard]] std::FILE* open_lock_file(const std::filesystem::path& path,
                                        std::error_code& error) noexcept;

[[nodiscard]] bool verify_regular_file(std::FILE* stream, std::error_code& error) noexcept;

// EN: A busy lock returns false without an error; interrupted and failed calls retain their code.
// ZH-CN: 锁被占用时返回 false 且不设置错误；调用中断或失败时保留对应错误码。
[[nodiscard]] bool try_lock_file(std::FILE* stream, std::error_code& error) noexcept;

// EN: Write directly to the descriptor so temporary secrets do not enter a stdio output buffer.
// ZH-CN: 直接写入描述符，避免临时密钥进入 stdio 输出缓冲区。
[[nodiscard]] bool write_file(std::FILE* stream, std::string_view content,
                              std::error_code& error) noexcept;
[[nodiscard]] bool sync_file(std::FILE* stream, std::error_code& error) noexcept;
[[nodiscard]] bool replace_file_durably(const std::filesystem::path& source,
                                        const std::filesystem::path& destination,
                                        std::error_code& error);

// EN: Exclusive binary creation keeps the ordinary file permissions selected by the process.
// ZH-CN: 独占创建二进制文件，保留进程对普通文件使用的权限规则。
[[nodiscard]] std::FILE* create_exclusive_file(const std::filesystem::path& path,
                                               std::error_code& error) noexcept;

// EN: Compare existing file IDs without following final symbolic links.
// ZH-CN: 比较已有文件对象的身份，不跟随末尾符号链接。
[[nodiscard]] bool same_existing_entry(const std::filesystem::path& left,
                                       const std::filesystem::path& right) noexcept;

} // namespace mint::platform
