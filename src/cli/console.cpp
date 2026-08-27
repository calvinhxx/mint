#include "console.hpp"

#include <iostream>

namespace mint::cli {

Console::Console(std::istream& input, std::ostream& output, std::ostream& error) noexcept
    : input_(input), output_(output), error_(error) {}

bool Console::read_line(std::string& value) {
    std::string line;
    if (!std::getline(input_, line)) {
        return false;
    }
    value = std::move(line);
    return true;
}

void Console::flush_output() {
    output_.flush();
}

void Console::flush_error() {
    error_.flush();
}

std::ostream& Console::output_stream() noexcept {
    return output_;
}

Console system_console() noexcept {
    return {std::cin, std::cout, std::cerr};
}

} // namespace mint::cli
