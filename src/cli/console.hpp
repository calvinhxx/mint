#pragma once

#include <istream>
#include <ostream>
#include <string>
#include <utility>

namespace mint::cli {

class Console final {
  public:
    Console(std::istream& input, std::ostream& output, std::ostream& error) noexcept;

    [[nodiscard]] bool read_line(std::string& value);

    template <typename... Parts> void write(Parts&&... parts) {
        ((output_ << std::forward<Parts>(parts)), ...);
    }

    template <typename... Parts> void write_line(Parts&&... parts) {
        write(std::forward<Parts>(parts)...);
        output_.put('\n');
    }

    template <typename... Parts> void write_error(Parts&&... parts) {
        ((error_ << std::forward<Parts>(parts)), ...);
    }

    template <typename... Parts> void write_error_line(Parts&&... parts) {
        write_error(std::forward<Parts>(parts)...);
        error_.put('\n');
    }

    void flush_output();
    void flush_error();

    [[nodiscard]] std::ostream& output_stream() noexcept;

  private:
    std::istream& input_;
    std::ostream& output_;
    std::ostream& error_;
};

[[nodiscard]] Console system_console() noexcept;

} // namespace mint::cli
