#include "livesplit/bridge/icons.hpp"

#include <array>
#include <string>
#include <utility>

#include "livesplit/bridge/error.hpp"

namespace livesplit::bridge {
namespace {

constexpr std::array<int, 256> make_decode_table() {
    std::array<int, 256> table{};
    for (auto& entry : table) entry = -1;
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t index = 0; index < alphabet.size(); ++index) {
        table[static_cast<unsigned char>(alphabet[index])] = static_cast<int>(index);
    }
    return table;
}

constexpr auto decode_table = make_decode_table();

}  // namespace

std::vector<std::uint8_t> decode_base64(std::string_view text) {
    if (text.size() % 4 != 0) {
        throw error(error_kind::protocol_error, "Base64 length is not a multiple of four");
    }

    std::vector<std::uint8_t> output;
    output.reserve(text.size() / 4 * 3);

    for (std::size_t offset = 0; offset < text.size(); offset += 4) {
        const auto final_block = offset + 4 == text.size();
        std::uint32_t value = 0;
        int padding = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            const auto character = static_cast<unsigned char>(text[offset + index]);
            if (character == '=') {
                if (!final_block || index < 2) {
                    throw error(error_kind::protocol_error, "Base64 padding is invalid");
                }
                ++padding;
                value <<= 6;
                continue;
            }
            if (padding != 0) {
                throw error(error_kind::protocol_error, "Base64 padding is invalid");
            }
            const auto decoded = decode_table[character];
            if (decoded < 0) {
                throw error(error_kind::protocol_error, "Base64 contains an invalid character");
            }
            value = (value << 6) | static_cast<std::uint32_t>(decoded);
        }

        output.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
        if (padding < 2) output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        if (padding < 1) output.push_back(static_cast<std::uint8_t>(value & 0xFF));
    }

    return output;
}

std::optional<std::vector<std::uint8_t>> decode_icon(const std::optional<std::string>& icon) {
    if (!icon) return std::nullopt;
    return decode_base64(*icon);
}

decoded_run_icons decode_run_icons(const run_icons& icons) {
    decoded_run_icons decoded;
    decoded.game_icon = decode_icon(icons.game_icon);
    decoded.segment_icons.reserve(icons.segment_icons.size());
    for (const auto& icon : icons.segment_icons) {
        decoded.segment_icons.push_back(decode_icon(icon));
    }
    return decoded;
}

}  // namespace livesplit::bridge
