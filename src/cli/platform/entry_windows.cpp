#include "support/console.hpp"
#include "support/entrypoint.hpp"

#include "mint/localization/localization.hpp"
#include "mint/runtime/terminal_text.hpp"

#include <windows.h>

#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using mint::localization::Message;
using mint::localization::Placeholder;

std::string to_utf8(std::wstring_view argument) {
    if (argument.empty()) {
        return {};
    }
    if (argument.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error(
            mint::localization::message(Message::cli_windows_argument_too_long));
    }

    const auto input_size = static_cast<int>(argument.size());
    const auto output_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument.data(),
                                                 input_size, nullptr, 0, nullptr, nullptr);
    if (output_size <= 0) {
        throw std::runtime_error(
            mint::localization::message(Message::cli_windows_utf8_conversion_failed));
    }

    std::string result(static_cast<std::size_t>(output_size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument.data(), input_size,
                            result.data(), output_size, nullptr, nullptr) != output_size) {
        throw std::runtime_error(
            mint::localization::message(Message::cli_windows_utf8_conversion_failed));
    }
    return result;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::string> storage;
        storage.reserve(static_cast<std::size_t>(argc));
        for (int index = 0; index < argc; ++index) {
            storage.push_back(to_utf8(argv[index]));
        }

        std::vector<char*> arguments;
        arguments.reserve(storage.size());
        for (auto& argument : storage) {
            arguments.push_back(argument.data());
        }
        return mint::cli::run_cli(argc, arguments.data());
    } catch (const std::exception& error) {
        auto console = mint::cli::system_console();
        console.write_error_line(mint::localization::message(
            Message::cli_output_error,
            {mint::localization::arg(Placeholder::message,
                                     mint::escape_terminal_field(error.what()))}));
        return 1;
    }
}
