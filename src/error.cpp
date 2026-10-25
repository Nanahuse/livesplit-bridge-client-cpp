#include "livesplit/bridge/error.hpp"

namespace livesplit::bridge {

std::string_view to_string(error_kind kind) noexcept {
    switch (kind) {
        case error_kind::closed:
            return "closed";
        case error_kind::timeout:
            return "timeout";
        case error_kind::not_ready:
            return "not_ready";
        case error_kind::connection_lost:
            return "connection_lost";
        case error_kind::protocol_error:
            return "protocol_error";
        case error_kind::remote_error:
            return "remote_error";
        case error_kind::control_result_unknown:
            return "control_result_unknown";
        case error_kind::internal_error:
            return "internal_error";
    }
    return "unknown";
}

}  // namespace livesplit::bridge
