#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace livesplit::bridge {

struct client_options {
    std::string host{"127.0.0.1"};
    std::uint16_t port{54000};
    std::string rpc_path{"/bridge/v4/rpc"};
    std::string events_path{"/bridge/v4/events"};

    std::chrono::milliseconds connect_timeout{3000};
    std::chrono::milliseconds response_timeout{3000};
    std::chrono::milliseconds ready_timeout{10000};
    std::chrono::milliseconds reconnect_initial_delay{250};
    std::chrono::milliseconds reconnect_max_delay{5000};

    // The Bridge treats a missing Origin as a local client. Set this only when
    // the client must emulate a browser Origin; Control is then restricted to
    // localhost origins by the Bridge.
    std::optional<std::string> origin{};
};

}  // namespace livesplit::bridge
