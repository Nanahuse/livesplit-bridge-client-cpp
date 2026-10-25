#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "livesplit/bridge/error.hpp"
#include "livesplit/bridge/options.hpp"
#include "livesplit/bridge/protocol.hpp"

namespace livesplit::bridge {

namespace detail {
class client_impl;
}

enum class connection_state {
    connecting,
    synchronizing,
    ready,
    reconnecting,
    closed,
};

struct snapshot {
    std::uint32_t session_id{};
    timer_state timer;
    attempt_state attempt;
    run_state run;
    context_state context;
    completed_count_value completed;

    bool operator==(const snapshot&) const = default;
};

class client {
public:
    explicit client(client_options options = {});
    ~client();

    client(const client&) = delete;
    client& operator=(const client&) = delete;
    client(client&& other) noexcept;
    client& operator=(client&& other) noexcept;

    // Stops the worker thread. Blocking calls that are already waiting receive
    // error_kind::closed. Subsequent operations throw error_kind::closed.
    void close() noexcept;

    [[nodiscard]] connection_state state() const;
    [[nodiscard]] bool is_ready() const;
    [[nodiscard]] std::uint32_t session_id() const;

    // Waits for the initial connection and state synchronization, or for an
    // automatic reconnect/resynchronization to finish.
    void wait_until_ready(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{10000}) const;

    // Last synchronized state. Timer events update the timer member while the
    // client is ready. Throws error_kind::not_ready before the first sync and
    // while recovering.
    [[nodiscard]] snapshot current_snapshot() const;

    // Runs the five snapshot Query RPCs in one session and replaces the cached
    // snapshot. Run icons are fetched separately because their response can be
    // larger than 64 KiB.
    [[nodiscard]] snapshot synchronize();

    [[nodiscard]] timer_state get_timer_state();
    [[nodiscard]] run_state get_run();
    [[nodiscard]] run_icons get_run_icons();
    [[nodiscard]] attempt_state get_attempt();
    [[nodiscard]] context_state get_context_state();
    [[nodiscard]] completed_count_value get_completed_count();

    void start();
    void split();
    void skip();
    void undo();
    void reset();
    void pause();
    void resume();

    void initialize_game_time();
    void set_game_time_ticks(std::int64_t ticks);
    void pause_game_time();
    void resume_game_time();

    // Returns nullopt on timeout. Events are validated for session and sequence
    // continuity; a gap or malformed event triggers an automatic resync, and the
    // affected event is not delivered.
    [[nodiscard]] std::optional<bridge_event> next_event(std::chrono::milliseconds timeout);
    [[nodiscard]] bridge_event wait_for_event();
    [[nodiscard]] std::optional<bridge_event> try_next_event();

private:
    std::shared_ptr<detail::client_impl> impl_;
};

}  // namespace livesplit::bridge
