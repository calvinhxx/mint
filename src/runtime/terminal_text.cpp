#include "mint/runtime/terminal_text.hpp"

#include <cstddef>
#include <cstdint>

namespace mint {
namespace {

constexpr char hex_digit(unsigned value) noexcept {
    return "0123456789ABCDEF"[value & 0xFU];
}

void append_byte_escape(std::string& output, unsigned char byte) {
    output += "\\x";
    output += hex_digit(byte >> 4U);
    output += hex_digit(byte);
}

void append_code_point_escape(std::string& output, std::uint32_t code_point) {
    output += "\\u";
    if (code_point > 0xFFFFU) {
        output += '{';
        bool started = false;
        for (int shift = 20; shift >= 0; shift -= 4) {
            const auto digit = static_cast<unsigned>(code_point >> shift) & 0xFU;
            if (digit != 0U || started || shift == 0) {
                output += hex_digit(digit);
                started = true;
            }
        }
        output += '}';
        return;
    }
    for (int shift = 12; shift >= 0; shift -= 4) {
        output += hex_digit(static_cast<unsigned>(code_point >> shift));
    }
}

bool is_continuation(unsigned char byte) noexcept {
    return (byte & 0xC0U) == 0x80U;
}

bool decode_utf8(std::string_view text, std::size_t offset, std::uint32_t& code_point,
                 std::size_t& width) noexcept {
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80U) {
        code_point = first;
        width = 1;
        return true;
    }

    if (first >= 0xC2U && first <= 0xDFU && offset + 1 < text.size()) {
        const auto second = static_cast<unsigned char>(text[offset + 1]);
        if (is_continuation(second)) {
            code_point = (static_cast<std::uint32_t>(first & 0x1FU) << 6U) | (second & 0x3FU);
            width = 2;
            return true;
        }
    }

    if (first >= 0xE0U && first <= 0xEFU && offset + 2 < text.size()) {
        const auto second = static_cast<unsigned char>(text[offset + 1]);
        const auto third = static_cast<unsigned char>(text[offset + 2]);
        const bool valid_second = is_continuation(second) && (first != 0xE0U || second >= 0xA0U) &&
                                  (first != 0xEDU || second <= 0x9FU);
        if (valid_second && is_continuation(third)) {
            code_point = (static_cast<std::uint32_t>(first & 0x0FU) << 12U) |
                         (static_cast<std::uint32_t>(second & 0x3FU) << 6U) | (third & 0x3FU);
            width = 3;
            return true;
        }
    }

    if (first >= 0xF0U && first <= 0xF4U && offset + 3 < text.size()) {
        const auto second = static_cast<unsigned char>(text[offset + 1]);
        const auto third = static_cast<unsigned char>(text[offset + 2]);
        const auto fourth = static_cast<unsigned char>(text[offset + 3]);
        const bool valid_second = is_continuation(second) && (first != 0xF0U || second >= 0x90U) &&
                                  (first != 0xF4U || second <= 0x8FU);
        if (valid_second && is_continuation(third) && is_continuation(fourth)) {
            code_point = (static_cast<std::uint32_t>(first & 0x07U) << 18U) |
                         (static_cast<std::uint32_t>(second & 0x3FU) << 12U) |
                         (static_cast<std::uint32_t>(third & 0x3FU) << 6U) | (fourth & 0x3FU);
            width = 4;
            return true;
        }
    }

    return false;
}

bool is_bidirectional_control(std::uint32_t code_point) noexcept {
    return code_point == 0x061CU || code_point == 0x200EU || code_point == 0x200FU ||
           (code_point >= 0x202AU && code_point <= 0x202EU) ||
           (code_point >= 0x2066U && code_point <= 0x206FU);
}

bool is_terminal_control(std::uint32_t code_point, bool preserve_layout) noexcept {
    return (code_point < 0x20U &&
            (!preserve_layout || (code_point != '\n' && code_point != '\t'))) ||
           (code_point >= 0x7FU && code_point <= 0x9FU) || is_bidirectional_control(code_point);
}

std::string escape_terminal(std::string_view text, bool preserve_layout) {
    std::string output;
    output.reserve(text.size());

    for (std::size_t offset = 0; offset < text.size();) {
        std::uint32_t code_point = 0;
        std::size_t width = 0;
        if (!decode_utf8(text, offset, code_point, width)) {
            append_byte_escape(output, static_cast<unsigned char>(text[offset]));
            ++offset;
            continue;
        }
        if (is_terminal_control(code_point, preserve_layout)) {
            append_code_point_escape(output, code_point);
        } else {
            output.append(text.substr(offset, width));
        }
        offset += width;
    }
    return output;
}

} // namespace

std::string escape_terminal_text(std::string_view text) {
    return escape_terminal(text, true);
}

std::string escape_terminal_field(std::string_view text) {
    return escape_terminal(text, false);
}

} // namespace mint
