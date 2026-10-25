#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <limits>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "livesplit/bridge/client.hpp"
#include "livesplit/bridge/icons.hpp"
#include "mock_bridge_server.hpp"

namespace {

using namespace std::chrono_literals;
using livesplit::bridge::attempt_state_response;
using livesplit::bridge::bridge_event;
using livesplit::bridge::client;
using livesplit::bridge::client_options;
using livesplit::bridge::connection_state;
using livesplit::bridge::error;
using livesplit::bridge::error_kind;
using livesplit::bridge::run_icons_response;
using livesplit::bridge::snapshot;
using livesplit::bridge::timer_event;
using livesplit::bridge::timer_operation;
using livesplit::bridge::timer_phase;
using livesplit::bridge::timer_state;
using livesplit::bridge::timer_state_response;

client_options options_for_port(std::uint16_t port) {
    client_options options;
    options.port = port;
    options.connect_timeout = 2s;
    options.response_timeout = 1s;
    options.ready_timeout = 3s;
    options.reconnect_initial_delay = 10ms;
    options.reconnect_max_delay = 100ms;
    return options;
}

bool wait_until(
    const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

nlohmann::json timer_event_json(
    std::uint32_t session_id,
    std::uint32_t sequence,
    std::string_view type = "timer_started") {
    timer_state state;
    state.phase = timer_phase::RUNNING;
    state.split_index = 0;
    return {
        {"session_id", session_id},
        {"event_sequence", sequence},
        {"type", type},
        {"timer_state", state},
    };
}

nlohmann::json changed_event_json(
    std::uint32_t session_id,
    std::uint32_t sequence,
    std::string_view type) {
    return {
        {"session_id", session_id},
        {"event_sequence", sequence},
        {"type", type},
        {"timer_state", nullptr},
    };
}
error_kind capture_error_kind(const std::function<void()>& operation) {
    try {
        operation();
    } catch (const error& failure) {
        return failure.kind();
    }
    FAIL("expected livesplit::bridge::error");
    return error_kind::internal_error;
}

struct fixture {
    test_support::mock_bridge_server server;
    client bridge;

    fixture() : bridge(options_for_port(server.port())) {
        bridge.wait_until_ready(5s);
    }
};

}  // namespace

TEST_CASE("queries, timer controls, game time and snapshot") {
    fixture f;

    const snapshot initial = f.bridge.current_snapshot();
    REQUIRE(initial.session_id == 42);
    REQUIRE(initial.run.game_name == "Bridge Test Game");
    REQUIRE(initial.run.segments.size() == 1);
    REQUIRE(initial.context.custom_variables.at("host_var") == "host-value");
    REQUIRE(initial.completed.completed_count == 9);

    REQUIRE(f.bridge.get_timer_state().phase == timer_phase::NOT_RUNNING);
    REQUIRE(f.bridge.get_run().category_name == "Any%");
    REQUIRE(f.bridge.get_attempt().attempt_count == 4);
    REQUIRE(f.bridge.get_context_state().current_comparison == "Personal Best");
    REQUIRE(f.bridge.get_completed_count().completed_count == 9);

    f.server.clear_counts();
    f.bridge.start();
    REQUIRE(f.server.method_count("timer_operation") == 1);
    REQUIRE(f.bridge.get_timer_state().phase == timer_phase::RUNNING);

    f.bridge.pause();
    REQUIRE(f.bridge.get_timer_state().phase == timer_phase::PAUSED);
    f.bridge.resume();
    REQUIRE(f.bridge.get_timer_state().phase == timer_phase::RUNNING);

    f.bridge.initialize_game_time();
    f.bridge.pause_game_time();
    f.bridge.set_game_time_ticks(-123456);
    auto timer = f.bridge.get_timer_state();
    REQUIRE(timer.is_game_time_initialized);
    REQUIRE(timer.is_game_time_paused);
    REQUIRE(timer.game_time_ticks == -123456);
    f.bridge.resume_game_time();
    REQUIRE_FALSE(f.bridge.get_timer_state().is_game_time_paused);
}

TEST_CASE("all eight event types are delivered in sequence") {
    fixture f;
    const std::vector<std::string> timer_types = {
        "timer_started",
        "timer_split",
        "timer_skipped",
        "timer_undo",
        "timer_phase_changed",
    };
    std::uint32_t sequence = 1;
    for (const auto& type : timer_types) {
        f.server.send_event(timer_event_json(42, sequence++, type));
        const auto event = f.bridge.next_event(2s);
        REQUIRE(event.has_value());
        REQUIRE(std::holds_alternative<timer_event>(*event));
    }

    f.server.send_event(
        nlohmann::json{
            {"session_id", 42},
            {"event_sequence", sequence++},
            {"type", "timer_reset"},
            {"timer_state", nullptr},
        });
    auto event = f.bridge.next_event(2s);
    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<livesplit::bridge::reset_event>(*event));

    for (const auto* type : {"run_changed", "context_changed"}) {
        f.server.send_event(
            nlohmann::json{
                {"session_id", 42},
                {"event_sequence", sequence++},
                {"type", type},
                {"timer_state", nullptr},
            });
        event = f.bridge.next_event(2s);
        REQUIRE(event.has_value());
        REQUIRE(std::holds_alternative<livesplit::bridge::changed_event>(*event));
    }
    REQUIRE(f.bridge.state() == connection_state::ready);
}
TEST_CASE("events are validated and delivered") {
    fixture f;
    f.server.send_event(timer_event_json(42, 1, "timer_started"));

    const auto event = f.bridge.next_event(2s);
    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<timer_event>(*event));
    const auto& timer_event_value = std::get<timer_event>(*event);
    REQUIRE(timer_event_value.session_id == 42);
    REQUIRE(timer_event_value.event_sequence == 1);
    REQUIRE(timer_event_value.timer_state.phase == timer_phase::RUNNING);
}

TEST_CASE("a control timeout is reported as unknown and is not replayed") {
    fixture f;
    f.server.clear_counts();
    f.server.set_behavior(
        "timer_operation",
        {.no_response = 1});

    REQUIRE(
        capture_error_kind([&] { f.bridge.start(); }) ==
        error_kind::control_result_unknown);
    REQUIRE(f.server.method_count("timer_operation") == 1);

    REQUIRE(wait_until([&] { return f.server.method_count("get_timer_state") >= 1; }));
    f.bridge.wait_until_ready(5s);
    REQUIRE(f.server.method_count("timer_operation") == 1);
    REQUIRE(f.bridge.get_timer_state().phase == timer_phase::NOT_RUNNING);
}

TEST_CASE("a query timeout recovers through automatic reconnect and resync") {
    fixture f;
    f.server.clear_counts();
    f.server.set_behavior("get_timer_state", {.no_response = 1});

    REQUIRE(capture_error_kind([&] { (void)f.bridge.get_timer_state(); }) == error_kind::timeout);
    REQUIRE(wait_until([&] { return f.server.method_count("get_timer_state") >= 2; }));
    f.bridge.wait_until_ready(5s);
    REQUIRE_NOTHROW(f.bridge.get_timer_state());
}

TEST_CASE("event sequence wraps from UINT32_MAX to one") {
    fixture f;
    f.server.send_event(timer_event_json(42, std::numeric_limits<std::uint32_t>::max()));
    REQUIRE(f.bridge.next_event(2s).has_value());
    f.server.send_event(timer_event_json(42, 1));
    const auto event = f.bridge.next_event(2s);
    REQUIRE(event.has_value());
    REQUIRE(std::get<timer_event>(*event).event_sequence == 1);
    REQUIRE(f.bridge.state() == connection_state::ready);
}
TEST_CASE("an event sequence gap triggers resync before further events") {
    fixture f;
    f.server.send_event(timer_event_json(42, 5, "timer_started"));
    REQUIRE(f.bridge.next_event(2s).has_value());

    f.server.clear_counts();
    f.server.send_event(timer_event_json(42, 7, "timer_split"));
    REQUIRE(wait_until([&] { return f.server.method_count("get_attempt") >= 1; }));
    f.bridge.wait_until_ready(5s);

    f.server.send_event(timer_event_json(42, 8, "timer_split"));
    const auto event = f.bridge.next_event(2s);
    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<timer_event>(*event));
}

TEST_CASE("an events session change triggers a full reconnect") {
    fixture f;
    f.server.send_event(timer_event_json(42, 1));
    REQUIRE(f.bridge.next_event(2s).has_value());

    f.server.set_session_id(99);
    f.server.send_event(timer_event_json(99, 2));
    REQUIRE(wait_until([&] { return f.bridge.session_id() == 99; }));
    f.bridge.wait_until_ready(5s);
    REQUIRE(f.bridge.current_snapshot().session_id == 99);

    f.server.send_event(timer_event_json(99, 1));
    const auto event = f.bridge.next_event(2s);
    REQUIRE(event.has_value());
    REQUIRE(std::get<timer_event>(*event).session_id == 99);
}

TEST_CASE("losing the RPC connection reconnects without losing the session") {
    fixture f;
    const auto baseline = f.server.method_count("get_timer_state");
    f.server.close_rpc_after_responses(1);

    REQUIRE(f.bridge.get_timer_state().phase == timer_phase::NOT_RUNNING);
    REQUIRE(wait_until([&] { return f.server.method_count("get_timer_state") >= baseline + 2; }));
    f.bridge.wait_until_ready(5s);
    REQUIRE(f.bridge.session_id() == 42);
    REQUIRE_NOTHROW(f.bridge.get_timer_state());
}

TEST_CASE("an RPC session change is followed by reconnect and resync") {
    fixture f;
    f.server.set_session_id(99);

    REQUIRE(f.bridge.get_timer_state().phase == timer_phase::NOT_RUNNING);
    REQUIRE(wait_until([&] { return f.bridge.session_id() == 99; }));
    f.bridge.wait_until_ready(5s);
    REQUIRE(f.bridge.current_snapshot().session_id == 99);
    REQUIRE(f.bridge.get_run().game_name == "Bridge Test Game");
}

TEST_CASE("two clients can target different ports independently") {
    test_support::mock_bridge_server first_server;
    test_support::mock_bridge_server second_server;
    first_server.set_session_id(11);
    second_server.set_session_id(22);

    client first(options_for_port(first_server.port()));
    client second(options_for_port(second_server.port()));
    first.wait_until_ready(5s);
    second.wait_until_ready(5s);
    REQUIRE(first.session_id() == 11);
    REQUIRE(second.session_id() == 22);

    first.start();
    REQUIRE(first.get_timer_state().phase == timer_phase::RUNNING);
    REQUIRE(second.get_timer_state().phase == timer_phase::NOT_RUNNING);
}
TEST_CASE("responses larger than the Beast default message limit are accepted") {
    test_support::mock_bridge_server server;
    auto options = options_for_port(server.port());
    options.response_timeout = 15s;
    client bridge(options);
    bridge.wait_until_ready(5s);

    constexpr std::size_t payload_size = 17 * 1024 * 1024;
    server.set_run_game_name(std::string(payload_size, 'x'));
    const auto run = bridge.get_run();
    REQUIRE(run.game_name.size() == payload_size);
    bridge.close();
}
TEST_CASE("responses larger than 64 KiB are accepted and decoded") {
    fixture f;
    f.server.set_icon_bytes(70 * 1024);

    const auto icons = f.bridge.get_run_icons();
    REQUIRE(icons.game_icon.has_value());
    REQUIRE(icons.game_icon->size() > 64 * 1024);
    const auto decoded = livesplit::bridge::decode_run_icons(icons);
    REQUIRE(decoded.game_icon.has_value());
    REQUIRE(decoded.game_icon->size() == 70 * 1024);
    REQUIRE(decoded.segment_icons.size() == 1);
    REQUIRE_FALSE(decoded.segment_icons[0].has_value());
}

TEST_CASE("malformed responses require reconnect and recover") {
    fixture f;
    const auto baseline = f.server.method_count("get_run");
    f.server.set_behavior("get_run", {.malformed = 1});

    REQUIRE(capture_error_kind([&] { (void)f.bridge.get_run(); }) == error_kind::protocol_error);
    REQUIRE(wait_until([&] { return f.server.method_count("get_run") >= baseline + 2; }));
    f.bridge.wait_until_ready(5s);
    REQUIRE(f.bridge.get_run().game_name == "Bridge Test Game");
}

TEST_CASE("an RPC request id mismatch is a protocol error") {
    fixture f;
    f.server.set_behavior("get_run", {.wrong_request_id = 1});
    REQUIRE(capture_error_kind([&] { (void)f.bridge.get_run(); }) == error_kind::protocol_error);
    f.bridge.wait_until_ready(5s);
}

TEST_CASE("a binary RPC response is a protocol error") {
    fixture f;
    f.server.set_behavior("get_run", {.binary = 1});
    REQUIRE(capture_error_kind([&] { (void)f.bridge.get_run(); }) == error_kind::protocol_error);
    f.bridge.wait_until_ready(5s);
}

TEST_CASE("initial connection retries until the Bridge appears") {
    std::uint16_t port = 0;
    {
        test_support::mock_bridge_server probe;
        port = probe.port();
    }

    client bridge(options_for_port(port));
    std::this_thread::sleep_for(50ms);
    {
        test_support::mock_bridge_server server(port);
        bridge.wait_until_ready(5s);
        REQUIRE(bridge.state() == connection_state::ready);
        REQUIRE(bridge.session_id() == 42);
        bridge.close();
    }
}
TEST_CASE("zero sync_timeout is rejected") {
    client_options options;
    options.sync_timeout = 0ms;
    REQUIRE_THROWS_AS(client{options}, std::invalid_argument);
}

TEST_CASE("sync_timeout also bounds the last Query response") {
    test_support::mock_bridge_server server;
    auto options = options_for_port(server.port());
    options.response_timeout = 5s;
    options.sync_timeout = 200ms;
    client bridge(options);
    bridge.wait_until_ready(5s);
    server.set_behavior("get_completed_count", {.delay_ms = 700});

    const auto started = std::chrono::steady_clock::now();
    auto future = bridge.synchronize_async();
    std::optional<error_kind> failure_kind;
    try {
        (void)future.get();
    } catch (const error& failure) {
        failure_kind = failure.kind();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(failure_kind == error_kind::timeout);
    REQUIRE(elapsed < 500ms);
    bridge.close();
}

TEST_CASE("operations time out while no Bridge is available") {
    std::uint16_t port = 0;
    {
        test_support::mock_bridge_server probe;
        port = probe.port();
        probe.stop();
    }

    auto options = options_for_port(port);
    options.ready_timeout = 100ms;
    options.connect_timeout = 100ms;
    options.reconnect_initial_delay = 10ms;
    options.reconnect_max_delay = 20ms;
    client bridge(options);

    REQUIRE(capture_error_kind([&] { (void)bridge.get_timer_state(); }) == error_kind::timeout);
    bridge.close();
}
TEST_CASE("async queries return without blocking the caller") {
    fixture f;
    f.server.set_behavior("get_timer_state", {.delay_ms = 300});

    const auto started = std::chrono::steady_clock::now();
    auto future = f.bridge.get_timer_state_async();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    REQUIRE(elapsed < 150ms);
    REQUIRE(future.wait_for(0ms) == std::future_status::timeout);
    REQUIRE(future.get().phase == timer_phase::NOT_RUNNING);
}

TEST_CASE("a run change during synchronization is not lost") {
    fixture f;
    f.server.clear_counts();
    f.server.set_behavior("get_context_state", {.delay_ms = 300});

    auto future = f.bridge.synchronize_async();
    REQUIRE(wait_until([&] { return f.server.method_count("get_run") >= 1; }));
    f.server.set_run_game_name("Changed Game");
    f.server.send_event(changed_event_json(42, 1, "run_changed"));

    const auto state = future.get();
    REQUIRE(state.run.game_name == "Changed Game");
    REQUIRE(f.server.method_count("get_run") >= 2);

    const auto event = f.bridge.next_event(1s);
    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<livesplit::bridge::changed_event>(*event));
}

TEST_CASE("a timer change during synchronization is not lost") {
    fixture f;
    f.server.clear_counts();
    f.server.set_behavior("get_context_state", {.delay_ms = 300});

    auto future = f.bridge.synchronize_async();
    REQUIRE(wait_until([&] { return f.server.method_count("get_run") >= 1; }));
    f.server.set_timer_phase(timer_phase::RUNNING);
    f.server.send_event(timer_event_json(42, 1, "timer_started"));

    const auto state = future.get();
    REQUIRE(state.timer.phase == timer_phase::RUNNING);
}

TEST_CASE("continuous events terminate synchronization with unstable_state") {
    test_support::mock_bridge_server server;
    auto options = options_for_port(server.port());
    options.sync_timeout = 200ms;
    client bridge(options);
    bridge.wait_until_ready(5s);

    server.set_behavior("get_run", {.delay_ms = 50});
    std::atomic<bool> stop{false};
    std::thread sender([&] {
        std::uint32_t sequence = 1;
        while (!stop.load()) {
            server.send_event(changed_event_json(42, sequence++, "run_changed"));
            std::this_thread::sleep_for(5ms);
        }
    });

    auto future = bridge.synchronize_async();
    std::optional<error_kind> failure_kind;
    try {
        (void)future.get();
    } catch (const error& failure) {
        failure_kind = failure.kind();
    }

    stop.store(true);
    sender.join();
    REQUIRE(failure_kind.has_value());
    REQUIRE(*failure_kind == error_kind::unstable_state);
    bridge.close();
}
TEST_CASE("event queue overflow is observable") {
    fixture f;
    for (std::uint32_t sequence = 1; sequence <= 4200; ++sequence) {
        f.server.send_event(timer_event_json(42, sequence));
    }

    REQUIRE(wait_until([&] { return f.bridge.dropped_event_count() > 0; }, 10s));
    REQUIRE(f.bridge.next_event(1s).has_value());
}

TEST_CASE("closing with an in-flight control reports an unknown result") {
    fixture f;
    f.server.clear_counts();
    f.server.set_behavior("timer_operation", {.no_response = 1});

    auto future = f.bridge.start_async();
    REQUIRE(wait_until([&] { return f.server.method_count("timer_operation") >= 1; }));
    f.bridge.close();

    REQUIRE(
        capture_error_kind([&] { future.get(); }) ==
        error_kind::control_result_unknown);
}
TEST_CASE("remote errors do not break the connection") {
    fixture f;
    f.server.set_behavior(
        "timer_operation",
        {
            .remote_error = 1,
            .error_code = livesplit::bridge::bridge_error_code::PERMISSION_DENIED,
            .error_message = "denied",
        });

    try {
        f.bridge.start();
        FAIL("expected a remote error");
    } catch (const error& failure) {
        REQUIRE(failure.kind() == error_kind::remote_error);
        REQUIRE(failure.remote().has_value());
        REQUIRE(failure.remote()->code == livesplit::bridge::bridge_error_code::PERMISSION_DENIED);
        REQUIRE(failure.remote()->message == "denied");
    }
    REQUIRE(f.bridge.state() == connection_state::ready);
    REQUIRE_NOTHROW(f.bridge.get_timer_state());
}
