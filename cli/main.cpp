#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "livesplit/bridge/client.hpp"
#include "livesplit/bridge/icons.hpp"

namespace {

using livesplit::bridge::bridge_event;
using livesplit::bridge::client;
using livesplit::bridge::client_options;
using livesplit::bridge::error;
using livesplit::bridge::snapshot;

[[noreturn]] void usage() {
    std::cerr
        << "Usage: livesplit_bridge_cli [--host HOST] [--port PORT] [--timeout MS]\n"
        << "                             [--ready-timeout MS] COMMAND [ARG]\n"
        << "\n"
        << "Queries:\n"
        << "  sync | timer | run | icons [--output DIR] | attempt | context | completed\n"
        << "\n"
        << "Timer controls:\n"
        << "  start | split | skip | undo | reset | pause | resume\n"
        << "\n"
        << "Game time:\n"
        << "  game-init | game-set TICKS | game-pause | game-resume\n"
        << "\n"
        << "Events:\n"
        << "  events [--count N]\n";
    std::exit(2);
}

std::int64_t parse_int64(std::string_view text, const char* name) {
    try {
        std::size_t consumed = 0;
        const auto value = std::stoll(std::string(text), &consumed, 10);
        if (consumed != text.size()) throw std::invalid_argument("trailing characters");
        return value;
    } catch (const std::exception&) {
        std::cerr << "Invalid " << name << ": " << text << "\n";
        std::exit(2);
    }
}

nlohmann::json event_json(const bridge_event& event) {
    return std::visit([](const auto& value) { return nlohmann::json(value); }, event);
}

nlohmann::json snapshot_json(const snapshot& state) {
    return {
        {"session_id", state.session_id},
        {"timer", state.timer},
        {"attempt", state.attempt},
        {"run", state.run},
        {"context", state.context},
        {"completed", state.completed},
    };
}

void write_icon(
    const std::filesystem::path& directory,
    const std::string& name,
    const std::optional<std::vector<std::uint8_t>>& bytes) {
    if (!bytes) return;
    const auto path = directory / name;
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot open " + path.string());
    output.write(
        reinterpret_cast<const char*>(bytes->data()),
        static_cast<std::streamsize>(bytes->size()));
    if (!output) throw std::runtime_error("cannot write " + path.string());
}

template <typename Operation>
nlohmann::json run_query(Operation&& operation) {
    return nlohmann::json(operation());
}

}  // namespace

int main(int argc, char** argv) {
    client_options options;
    std::chrono::milliseconds timeout{3000};
    std::chrono::milliseconds ready_timeout{10000};
    std::string command;
    std::string argument;
    std::optional<std::filesystem::path> icon_output;
    std::optional<int> event_count;
    bool icon_command = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view value = argv[index];
        auto next = [&](const char* name) -> std::string {
            if (index + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++index];
        };

        if (value == "--host") {
            options.host = next("--host");
        } else if (value == "--port") {
            const auto port = parse_int64(next("--port"), "port");
            if (port < 1 || port > 65535) {
                std::cerr << "Port must be between 1 and 65535\n";
                return 2;
            }
            options.port = static_cast<std::uint16_t>(port);
        } else if (value == "--timeout") {
            const auto milliseconds = parse_int64(next("--timeout"), "timeout");
            if (milliseconds < 1) {
                std::cerr << "Timeout must be positive\n";
                return 2;
            }
            timeout = std::chrono::milliseconds{milliseconds};
        } else if (value == "--ready-timeout") {
            const auto milliseconds = parse_int64(next("--ready-timeout"), "ready timeout");
            if (milliseconds < 0) {
                std::cerr << "Ready timeout must be non-negative\n";
                return 2;
            }
            ready_timeout = std::chrono::milliseconds{milliseconds};
        } else if (value == "--output") {
            icon_output = next("--output");
        } else if (value == "--count") {
            const auto count = parse_int64(next("--count"), "count");
            if (count < 0) {
                std::cerr << "Count must be non-negative\n";
                return 2;
            }
            event_count = static_cast<int>(count);
        } else if (value == "--help" || value == "-h") {
            usage();
        } else if (!value.empty() && value.front() == '-') {
            std::cerr << "Unknown option: " << value << "\n";
            usage();
        } else if (command.empty()) {
            command = std::string(value);
            icon_command = command == "icons";
            if (icon_command) continue;
        } else if (argument.empty()) {
            argument = std::string(value);
        } else {
            usage();
        }
    }

    if (command.empty()) usage();
    if (command == "icons" && icon_output == std::nullopt) {
        icon_output = std::filesystem::path(".");
    }

    options.response_timeout = timeout;
    options.ready_timeout = ready_timeout;

    try {
        client bridge(options);
        bridge.wait_until_ready(ready_timeout);

        if (command == "sync") {
            std::cout << snapshot_json(bridge.synchronize()).dump() << '\n';
        } else if (command == "timer") {
            std::cout << run_query([&] { return bridge.get_timer_state(); }).dump() << '\n';
        } else if (command == "run") {
            std::cout << run_query([&] { return bridge.get_run(); }).dump() << '\n';
        } else if (command == "attempt") {
            std::cout << run_query([&] { return bridge.get_attempt(); }).dump() << '\n';
        } else if (command == "context") {
            std::cout << run_query([&] { return bridge.get_context_state(); }).dump() << '\n';
        } else if (command == "completed") {
            std::cout << run_query([&] { return bridge.get_completed_count(); }).dump() << '\n';
        } else if (command == "icons") {
            const auto decoded = livesplit::bridge::decode_run_icons(bridge.get_run_icons());
            std::filesystem::create_directories(*icon_output);
            write_icon(*icon_output, "game.png", decoded.game_icon);
            for (std::size_t index = 0; index < decoded.segment_icons.size(); ++index) {
                write_icon(
                    *icon_output,
                    "segment-" + std::to_string(index) + ".png",
                    decoded.segment_icons[index]);
            }
            nlohmann::json result{
                {"game_icon", decoded.game_icon.has_value()},
                {"segments", nlohmann::json::array()},
            };
            for (const auto& icon : decoded.segment_icons) {
                result["segments"].push_back(icon.has_value());
            }
            std::cout << result.dump() << '\n';
        } else if (command == "start") {
            bridge.start();
            std::cout << "{\"ok\":true}\n";
        } else if (command == "split") {
            bridge.split();
            std::cout << "{\"ok\":true}\n";
        } else if (command == "skip") {
            bridge.skip();
            std::cout << "{\"ok\":true}\n";
        } else if (command == "undo") {
            bridge.undo();
            std::cout << "{\"ok\":true}\n";
        } else if (command == "reset") {
            bridge.reset();
            std::cout << "{\"ok\":true}\n";
        } else if (command == "pause") {
            bridge.pause();
            std::cout << "{\"ok\":true}\n";
        } else if (command == "resume") {
            bridge.resume();
            std::cout << "{\"ok\":true}\n";
        } else if (command == "game-init") {
            bridge.initialize_game_time();
            std::cout << "{\"ok\":true}\n";
        } else if (command == "game-set") {
            if (argument.empty()) usage();
            bridge.set_game_time_ticks(parse_int64(argument, "ticks"));
            std::cout << "{\"ok\":true}\n";
        } else if (command == "game-pause") {
            bridge.pause_game_time();
            std::cout << "{\"ok\":true}\n";
        } else if (command == "game-resume") {
            bridge.resume_game_time();
            std::cout << "{\"ok\":true}\n";
        } else if (command == "events") {
            int received = 0;
            while (!event_count || received < *event_count) {
                const auto event = bridge.wait_for_event();
                std::cout << event_json(event).dump() << '\n';
                std::cout.flush();
                ++received;
            }
        } else {
            std::cerr << "Unknown command: " << command << "\n";
            usage();
        }
    } catch (const error& failure) {
        nlohmann::json body{
            {"kind", std::string(livesplit::bridge::to_string(failure.kind()))},
            {"message", failure.what()},
        };
        if (failure.remote()) {
            body["bridge_error"] = *failure.remote();
        }
        if (failure.request_id()) body["request_id"] = *failure.request_id();
        std::cerr << body.dump() << '\n';
        return 1;
    } catch (const std::exception& failure) {
        std::cerr << "{\"kind\":\"internal_error\",\"message\":" << nlohmann::json(failure.what()).dump()
                  << "}\n";
        return 1;
    }

    return 0;
}
