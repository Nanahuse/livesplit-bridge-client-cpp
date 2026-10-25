#include <chrono>
#include <cstdlib>
#include <type_traits>
#include <utility>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "livesplit/bridge/client.hpp"
#include "livesplit/bridge/icons.hpp"

namespace {

using namespace std::chrono_literals;
using namespace livesplit::bridge;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "testhost integration failed: " << message << '\n';
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) fail(message);
}

timer_state latest_timer(client& bridge) {
    return bridge.get_timer_state();
}

bridge_event wait_for_type(client& bridge, std::string_view type) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto event = bridge.next_event(100ms);
        if (!event) continue;
        const bool matches = std::visit(
            [type](const auto& value) {
                using value_type = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<value_type, timer_event>) {
                    switch (value.type) {
                        case timer_event_type::TIMER_STARTED:
                            return type == "timer_started";
                        case timer_event_type::TIMER_SPLIT:
                            return type == "timer_split";
                        case timer_event_type::TIMER_SKIPPED:
                            return type == "timer_skipped";
                        case timer_event_type::TIMER_UNDO:
                            return type == "timer_undo";
                        case timer_event_type::TIMER_PHASE_CHANGED:
                            return type == "timer_phase_changed";
                    }
                    return false;
                } else if constexpr (std::is_same_v<value_type, reset_event>) {
                    return type == "timer_reset";
                } else {
                    switch (value.type) {
                        case changed_event_type::RUN_CHANGED:
                            return type == "run_changed";
                        case changed_event_type::CONTEXT_CHANGED:
                            return type == "context_changed";
                    }
                    return false;
                }
            },
            *event);
        if (matches) return std::move(*event);
    }
    fail("timed out waiting for event " + std::string(type));
}

std::uint32_t png_width(const std::vector<std::uint8_t>& png) {
    require(png.size() >= 24, "PNG is too short");
    const auto width = (static_cast<std::uint32_t>(png[16]) << 24) |
        (static_cast<std::uint32_t>(png[17]) << 16) |
        (static_cast<std::uint32_t>(png[18]) << 8) |
        static_cast<std::uint32_t>(png[19]);
    return width;
}

std::uint32_t png_height(const std::vector<std::uint8_t>& png) {
    require(png.size() >= 24, "PNG is too short");
    return (static_cast<std::uint32_t>(png[20]) << 24) |
        (static_cast<std::uint32_t>(png[21]) << 16) |
        (static_cast<std::uint32_t>(png[22]) << 8) |
        static_cast<std::uint32_t>(png[23]);
}

void check_png(const std::vector<std::uint8_t>& png, std::uint32_t width, std::uint32_t height) {
    require(png.size() > 8, "PNG is empty");
    static constexpr std::uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    require(std::memcmp(png.data(), signature, 8) == 0, "PNG signature is invalid");
    require(png_width(png) == width, "PNG width is unexpected");
    require(png_height(png) == height, "PNG height is unexpected");
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 54000;
    bool with_icons = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--port" && index + 1 < argc) {
            port = static_cast<std::uint16_t>(std::stoul(argv[++index]));
        } else if (argument == "--with-icons") {
            with_icons = true;
        } else {
            std::cerr << "Usage: testhost_integration --port PORT [--with-icons]\n";
            return 2;
        }
    }

    client_options options;
    options.port = port;
    options.response_timeout = 3s;
    options.ready_timeout = 10s;
    options.reconnect_initial_delay = 100ms;
    options.reconnect_max_delay = 1s;

    try {
        client bridge(options);
        bridge.wait_until_ready(20s);
        const auto initial = bridge.current_snapshot();
        require(initial.timer.phase == timer_phase::NOT_RUNNING, "initial phase is not not_running");
        require(initial.run.game_name == "Bridge Test Game", "game name differs");
        require(initial.run.category_name == "Any%", "category name differs");
        require(initial.run.segments.size() == 2, "segment count differs");
        require(initial.run.segments[0].name == "First", "first segment differs");
        require(initial.run.segments[1].name == "Second", "second segment differs");
        require(
            initial.context.custom_variables.at("host_var") == "host-value",
            "host custom variable differs");
        require(bridge.session_id() != 0, "session id is zero");

        const auto icons = bridge.get_run_icons();
        if (with_icons) {
            require(icons.game_icon.has_value(), "game icon is missing");
            require(icons.segment_icons.size() == 2, "segment icon count differs");
            require(icons.segment_icons[0].has_value(), "first segment icon is missing");
            require(!icons.segment_icons[1].has_value(), "second segment icon is not null");
            require(
                icons.game_icon->size() > 64 * 1024,
                "Base64 game icon is not larger than 64 KiB");
            const auto decoded = decode_run_icons(icons);
            check_png(*decoded.game_icon, 256, 256);
            check_png(*decoded.segment_icons[0], 16, 16);
        } else {
            require(!icons.game_icon.has_value(), "unexpected game icon");
            require(icons.segment_icons.size() == 2, "segment icon count differs");
            require(!icons.segment_icons[0].has_value(), "unexpected first segment icon");
            require(!icons.segment_icons[1].has_value(), "unexpected second segment icon");
        }

        // Continue using the same RPC connection after a potentially large response.
        require(bridge.get_run() == initial.run, "run changed after get_run_icons");

        bridge.start();
        wait_for_type(bridge, "timer_started");
        require(latest_timer(bridge).phase == timer_phase::RUNNING, "timer did not start");

        bridge.initialize_game_time();
        bridge.pause_game_time();
        bridge.set_game_time_ticks(-123456);
        auto timer = latest_timer(bridge);
        require(timer.is_game_time_initialized, "game time was not initialized");
        require(timer.is_game_time_paused, "game time was not paused");
        require(timer.game_time_ticks == -123456, "game time ticks differ");
        bridge.resume_game_time();
        require(!latest_timer(bridge).is_game_time_paused, "game time did not resume");

        bridge.pause();
        wait_for_type(bridge, "timer_phase_changed");
        require(latest_timer(bridge).phase == timer_phase::PAUSED, "timer did not pause");
        bridge.resume();
        require(latest_timer(bridge).phase == timer_phase::RUNNING, "timer did not resume");

        bridge.split();
        wait_for_type(bridge, "timer_split");
        require(latest_timer(bridge).split_index == 1, "split index differs");
        const auto attempt = bridge.get_attempt();
        require(attempt.segments.size() >= 1, "attempt segment is missing");
        require(
            attempt.segments[0].split_time.real_time_ticks.has_value(),
            "attempt split time is missing");

        bridge.undo();
        wait_for_type(bridge, "timer_undo");
        require(latest_timer(bridge).split_index == 0, "undo split index differs");

        bridge.skip();
        wait_for_type(bridge, "timer_skipped");
        require(latest_timer(bridge).split_index == 1, "skip split index differs");

        bridge.split();
        wait_for_type(bridge, "timer_split");
        require(latest_timer(bridge).phase == timer_phase::ENDED, "timer did not end");

        bridge.reset();
        wait_for_type(bridge, "timer_reset");
        require(latest_timer(bridge).phase == timer_phase::NOT_RUNNING, "timer did not reset");
        require(
            bridge.get_completed_count().completed_count ==
                initial.completed.completed_count + 1,
            "completed count did not increase");

        // Exercise the remaining public query methods after all controls.
        (void)bridge.get_context_state();
        (void)bridge.get_timer_state();

        // A fresh synchronization must remain in the same live session.
        const auto resynchronized = bridge.synchronize();
        require(resynchronized.session_id == initial.session_id, "session changed during sync");
        require(
            resynchronized.timer.phase == timer_phase::NOT_RUNNING,
            "resynchronized phase differs");

        // Control from a non-local Origin must fail without closing the RPC
        // connection. Query and Events remain available.
        {
            auto restricted_options = options;
            restricted_options.origin = "https://example.com";
            client restricted(restricted_options);
            restricted.wait_until_ready(10s);
            try {
                restricted.start();
                fail("non-local origin unexpectedly allowed Control");
            } catch (const error& failure) {
                require(failure.kind() == error_kind::remote_error, "expected a remote error");
                require(failure.remote().has_value(), "remote error details are missing");
                require(
                    failure.remote()->code == bridge_error_code::PERMISSION_DENIED,
                    "expected permission_denied");
            }
            require(
                restricted.get_timer_state().phase == timer_phase::NOT_RUNNING,
                "restricted client lost Query access");
            restricted.close();
        }

        bridge.close();
        std::cout << "testhost integration passed\n";
        return 0;
    } catch (const error& failure) {
        std::cerr << "client error (" << to_string(failure.kind()) << "): " << failure.what() << '\n';
        return 1;
    } catch (const std::exception& failure) {
        std::cerr << "unexpected error: " << failure.what() << '\n';
        return 1;
    }
}
