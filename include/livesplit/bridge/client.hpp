#pragma once

#include <chrono>
#include <cstdint>
#include <future>
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

    // Stops the worker thread. Queued operations receive error_kind::closed.
    // An in-flight Control that was already sent receives
    // error_kind::control_result_unknown because it may have executed.
    void close() noexcept;

    [[nodiscard]] connection_state state() const;
    [[nodiscard]] bool is_ready() const;
    [[nodiscard]] std::uint32_t session_id() const;

    // Number of validated Events dropped because the local delivery queue
    // reached its 4096-entry limit. A growing value means the caller is not
    // consuming Events fast enough; Query the current state to resynchronize.
    [[nodiscard]] std::uint64_t dropped_event_count() const;

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
    [[nodiscard]] std::future<snapshot> synchronize_async();

    [[nodiscard]] timer_state get_timer_state();
    [[nodiscard]] std::future<timer_state> get_timer_state_async();
    [[nodiscard]] run_state get_run();
    [[nodiscard]] std::future<run_state> get_run_async();
    [[nodiscard]] run_icons get_run_icons();
    [[nodiscard]] std::future<run_icons> get_run_icons_async();
    [[nodiscard]] attempt_state get_attempt();
    [[nodiscard]] std::future<attempt_state> get_attempt_async();
    [[nodiscard]] context_state get_context_state();
    [[nodiscard]] std::future<context_state> get_context_state_async();
    [[nodiscard]] completed_count_value get_completed_count();
    [[nodiscard]] std::future<completed_count_value> get_completed_count_async();

    // Async calls enqueue the operation and return immediately. All operations
    // on one client are serialized on the worker thread. The synchronous
    // variants above are convenience wrappers around the async variants.
    void start();
    [[nodiscard]] std::future<void> start_async();
    void split();
    [[nodiscard]] std::future<void> split_async();
    void skip();
    [[nodiscard]] std::future<void> skip_async();
    void undo();
    [[nodiscard]] std::future<void> undo_async();
    void reset();
    [[nodiscard]] std::future<void> reset_async();
    void pause();
    [[nodiscard]] std::future<void> pause_async();
    void resume();
    [[nodiscard]] std::future<void> resume_async();

    void initialize_game_time();
    [[nodiscard]] std::future<void> initialize_game_time_async();
    void set_game_time_ticks(std::int64_t ticks);
    [[nodiscard]] std::future<void> set_game_time_ticks_async(std::int64_t ticks);
    void pause_game_time();
    [[nodiscard]] std::future<void> pause_game_time_async();
    void resume_game_time();
    [[nodiscard]] std::future<void> resume_game_time_async();

    // Returns nullopt on timeout. Events are validated for session and sequence
    // continuity; a gap or malformed event triggers an automatic resync, and the
    // affected event is not delivered. Events received during synchronization
    // are still delivered and cause the synchronization pass to be retried.
    [[nodiscard]] std::optional<bridge_event> next_event(std::chrono::milliseconds timeout);
    [[nodiscard]] bridge_event wait_for_event();
    [[nodiscard]] std::optional<bridge_event> try_next_event();

private:
    std::shared_ptr<detail::client_impl> impl_;
};

}  // namespace livesplit::bridge
