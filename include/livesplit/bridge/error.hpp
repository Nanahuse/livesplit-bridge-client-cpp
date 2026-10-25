#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "livesplit/bridge/protocol.hpp"

namespace livesplit::bridge {

enum class error_kind {
    closed,
    timeout,
    not_ready,
    connection_lost,
    protocol_error,
    remote_error,
    control_result_unknown,
    internal_error,
};

[[nodiscard]] std::string_view to_string(error_kind kind) noexcept;

class error : public std::runtime_error {
public:
    error(error_kind kind, std::string message)
        : std::runtime_error(std::move(message)), kind_(kind) {}

    error(error_kind kind, std::string message, bridge_error remote, std::uint32_t request_id)
        : std::runtime_error(std::move(message)),
          kind_(kind),
          remote_(std::move(remote)),
          request_id_(request_id) {}

    error(error_kind kind, std::string message, std::uint32_t request_id)
        : std::runtime_error(std::move(message)), kind_(kind), request_id_(request_id) {}

    [[nodiscard]] error_kind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::optional<bridge_error>& remote() const noexcept { return remote_; }
    [[nodiscard]] std::optional<std::uint32_t> request_id() const noexcept { return request_id_; }

private:
    error_kind kind_;
    std::optional<bridge_error> remote_;
    std::optional<std::uint32_t> request_id_;
};

}  // namespace livesplit::bridge
