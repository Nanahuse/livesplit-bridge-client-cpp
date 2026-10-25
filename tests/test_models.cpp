#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "livesplit/bridge/error.hpp"
#include "livesplit/bridge/icons.hpp"
#include "livesplit/bridge/protocol.hpp"

namespace {

using namespace livesplit::bridge;

nlohmann::json load_contract_cases() {
    const auto path =
        std::filesystem::path(LIVESPLIT_BRIDGE_TEST_FIXTURES) / "csharp-serialized-cases.json";
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open " + path.string());
    return nlohmann::json::parse(input);
}

nlohmann::json to_json_value(const rpc_request& request) {
    return std::visit([](const auto& value) { return nlohmann::json(value); }, request);
}

nlohmann::json to_json_value(const rpc_response& response) {
    return std::visit([](const auto& value) { return nlohmann::json(value); }, response);
}

nlohmann::json to_json_value(const bridge_event& event) {
    return std::visit([](const auto& value) { return nlohmann::json(value); }, event);
}

timer_state timer() {
    return timer_state{
        .phase = timer_phase::NOT_RUNNING,
        .split_index = -1,
        .real_time_ticks = 0,
        .game_time_ticks = std::nullopt,
        .is_game_time_initialized = false,
        .is_game_time_paused = false,
    };
}

nlohmann::json timer_event_json(
    std::uint32_t session,
    std::uint32_t sequence,
    std::string_view type = "timer_started") {
    return nlohmann::json{
        {"session_id", session},
        {"event_sequence", sequence},
        {"type", type},
        {"timer_state", timer()},
    };
}

}  // namespace

TEST_CASE("official C# serialized cases round-trip through generated models") {
    const auto cases = load_contract_cases();
    REQUIRE(cases.is_array());
    for (const auto& test_case : cases) {
        INFO(test_case.value("name", "") << " / " << test_case.value("schema", ""));
        REQUIRE(test_case.value("valid", false));
        const auto& data = test_case.at("data");
        const auto schema = test_case.at("schema").get<std::string>();
        if (schema == "request") {
            const auto parsed = parse_rpc_request(data.dump());
            REQUIRE(to_json_value(parsed) == data);
        } else if (schema == "response") {
            const auto parsed = parse_rpc_response(data.dump());
            REQUIRE(to_json_value(parsed) == data);
        } else if (schema == "event") {
            const auto parsed = parse_bridge_event(data.dump());
            REQUIRE(to_json_value(parsed) == data);
        } else {
            FAIL("unknown fixture schema");
        }
    }
}

TEST_CASE("all event discriminators parse") {
    for (const auto* type : {
             "timer_started",
             "timer_split",
             "timer_skipped",
             "timer_undo",
             "timer_phase_changed",
         }) {
        const auto parsed = parse_bridge_event(timer_event_json(7, 1, type).dump());
        REQUIRE(std::holds_alternative<timer_event>(parsed));
        REQUIRE(std::get<timer_event>(parsed).type == [&] {
            if (std::string_view(type) == "timer_started") return timer_event_type::TIMER_STARTED;
            if (std::string_view(type) == "timer_split") return timer_event_type::TIMER_SPLIT;
            if (std::string_view(type) == "timer_skipped") return timer_event_type::TIMER_SKIPPED;
            if (std::string_view(type) == "timer_undo") return timer_event_type::TIMER_UNDO;
            return timer_event_type::TIMER_PHASE_CHANGED;
        }());
    }
    for (const auto* type : {"run_changed", "context_changed"}) {
        const auto parsed = parse_bridge_event(
            nlohmann::json{
                {"session_id", 7},
                {"event_sequence", 2},
                {"type", type},
                {"timer_state", nullptr},
            }
            .dump());
        REQUIRE(std::holds_alternative<changed_event>(parsed));
    }
    const auto reset = parse_bridge_event(
        nlohmann::json{
            {"session_id", 7},
            {"event_sequence", 3},
            {"type", "timer_reset"},
            {"timer_state", nullptr},
        }
        .dump());
    REQUIRE(std::holds_alternative<reset_event>(reset));
}

TEST_CASE("strict response validation") {
    const auto base = nlohmann::json{
        {"request_id", 1},
        {"session_id", 42},
        {"result", timer()},
    };

    SECTION("duplicate properties") {
        const std::string text =
            R"({"request_id":1,"request_id":1,"session_id":42,"result":{"phase":"not_running","split_index":-1,"real_time_ticks":0,"game_time_ticks":null,"is_game_time_initialized":false,"is_game_time_paused":false}})";
        REQUIRE_THROWS_AS(parse_rpc_response(text), detail::schema_error);
    }
    SECTION("unknown property") {
        auto value = base;
        value["extra"] = true;
        REQUIRE_THROWS_AS(parse_rpc_response(value.dump()), detail::schema_error);
        auto nested = base;
        nested["result"]["extra"] = 1;
        REQUIRE_THROWS_AS(parse_rpc_response(nested.dump()), detail::schema_error);
    }
    SECTION("missing property") {
        auto value = base;
        value["result"].erase("split_index");
        REQUIRE_THROWS_AS(parse_rpc_response(value.dump()), detail::schema_error);
    }
    SECTION("session and event counters are non-zero") {
        auto zero_session = base;
        zero_session["session_id"] = 0;
        REQUIRE_THROWS_AS(parse_rpc_response(zero_session.dump()), detail::schema_error);

        auto zero_sequence = timer_event_json(42, 0);
        REQUIRE_THROWS_AS(parse_bridge_event(zero_sequence.dump()), detail::schema_error);
    }
    SECTION("integer ranges") {
        auto value = base;
        value["request_id"] = 4294967296ULL;
        REQUIRE_THROWS_AS(parse_rpc_response(value.dump()), detail::schema_error);

        value = base;
        value["result"]["split_index"] = 2147483648LL;
        REQUIRE_THROWS_AS(parse_rpc_response(value.dump()), detail::schema_error);

        value = base;
        value["result"]["real_time_ticks"] = std::numeric_limits<std::int64_t>::min();
        REQUIRE_NOTHROW(parse_rpc_response(value.dump()));
        value["result"]["real_time_ticks"] = std::numeric_limits<std::int64_t>::max();
        REQUIRE_NOTHROW(parse_rpc_response(value.dump()));
    }
    SECTION("enum values") {
        auto value = base;
        value["result"]["phase"] = "unknown";
        REQUIRE_THROWS_AS(parse_rpc_response(value.dump()), detail::schema_error);
    }
    SECTION("event timer_state is branch-specific") {
        auto changed = nlohmann::json{
            {"session_id", 1},
            {"event_sequence", 1},
            {"type", "run_changed"},
            {"timer_state", timer()},
        };
        REQUIRE_THROWS_AS(parse_bridge_event(changed.dump()), detail::schema_error);

        auto timer_reset = nlohmann::json{
            {"session_id", 1},
            {"event_sequence", 1},
            {"type", "timer_reset"},
            {"timer_state", timer()},
        };
        REQUIRE_NOTHROW(parse_bridge_event(timer_reset.dump()));
    }
    SECTION("control success has a null result") {
        REQUIRE_NOTHROW(parse_rpc_response(
            R"({"request_id":1,"session_id":42,"result":null})"));
        REQUIRE_THROWS_AS(
            parse_rpc_response(R"({"request_id":1,"session_id":42,"result":{}})"),
            detail::schema_error);
    }
    SECTION("invalid JSON and duplicate top-level properties") {
        REQUIRE_THROWS_AS(parse_rpc_response("not json"), detail::schema_error);
        REQUIRE_THROWS_AS(parse_rpc_response("[]"), detail::schema_error);
        REQUIRE_THROWS_AS(
            parse_rpc_response(R"({"request_id":1,"request_id":2,"session_id":1,"result":null})"),
            detail::schema_error);
    }
}

TEST_CASE("RunIcons keeps segment positions and rejects a null array") {
    auto value = nlohmann::json{
        {"request_id", 1},
        {"session_id", 42},
        {"result", {{"game_icon", nullptr}, {"segment_icons", {nullptr, "TQ=="}}}},
    };
    const auto parsed = parse_rpc_response(value.dump());
    const auto& result = std::get<run_icons_response>(parsed).result;
    REQUIRE(result.segment_icons.size() == 2);
    REQUIRE_FALSE(result.segment_icons[0].has_value());
    REQUIRE(result.segment_icons[1] == "TQ==");

    value["result"]["segment_icons"] = nullptr;
    REQUIRE_THROWS_AS(parse_rpc_response(value.dump()), detail::schema_error);
}

TEST_CASE("requests serialize with the generated wire fields") {
    query_request query{
        .request_id = 7,
        .method = query_method::GET_RUN,
    };
    REQUIRE(
        nlohmann::json(query) ==
        nlohmann::json{{"request_id", 7}, {"method", "get_run"}});

    game_set_request set{
        .request_id = 8,
        .params = {.operation = "set", .ticks = -123456},
    };
    REQUIRE(
        nlohmann::json(set) ==
        nlohmann::json{
            {"request_id", 8},
            {"method", "game_time_operation"},
            {"params", {{"operation", "set"}, {"ticks", -123456}}},
        });
}

TEST_CASE("Base64 decoding is strict") {
    REQUIRE(decode_base64("TQ==") == std::vector<std::uint8_t>{'M'});
    REQUIRE(decode_base64("TWE=") == std::vector<std::uint8_t>{'M', 'a'});
    REQUIRE(decode_base64("TWFu") == std::vector<std::uint8_t>{'M', 'a', 'n'});
    REQUIRE(decode_base64("") == std::vector<std::uint8_t>{});

    REQUIRE_THROWS_AS(decode_base64("TQ="), livesplit::bridge::error);
    REQUIRE_THROWS_AS(decode_base64("TQ==="), livesplit::bridge::error);
    REQUIRE_THROWS_AS(decode_base64("T!=="), livesplit::bridge::error);
    REQUIRE_THROWS_AS(decode_base64("===="), livesplit::bridge::error);
    REQUIRE_THROWS_AS(decode_base64("TQ==TQ=="), livesplit::bridge::error);
}
