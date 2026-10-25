#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "livesplit/bridge/protocol.hpp"

namespace livesplit::bridge {

struct decoded_run_icons {
    std::optional<std::vector<std::uint8_t>> game_icon;
    std::vector<std::optional<std::vector<std::uint8_t>>> segment_icons;
};

// Strict RFC 4648 Base64 with padding. Throws error_kind::protocol_error for
// malformed input.
[[nodiscard]] std::vector<std::uint8_t> decode_base64(std::string_view text);

[[nodiscard]] std::optional<std::vector<std::uint8_t>> decode_icon(
    const std::optional<std::string>& icon);

[[nodiscard]] decoded_run_icons decode_run_icons(const run_icons& icons);

}  // namespace livesplit::bridge
