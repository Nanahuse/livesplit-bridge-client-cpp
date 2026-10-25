#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include "livesplit/bridge/client.hpp"
#include "livesplit/bridge/error.hpp"
#include "livesplit/bridge/options.hpp"
#include "livesplit/bridge/protocol.hpp"

namespace livesplit::bridge::detail {

class client_impl : public std::enable_shared_from_this<client_impl> {
public:
    explicit client_impl(client_options options);
    ~client_impl();

    client_impl(const client_impl&) = delete;
    client_impl& operator=(const client_impl&) = delete;

    void start();
    void close() noexcept;

    [[nodiscard]] connection_state state() const;
    [[nodiscard]] bool wait_until_ready(std::chrono::milliseconds timeout) const;
    [[nodiscard]] std::uint32_t session_id() const;
    [[nodiscard]] snapshot current_snapshot() const;

    [[nodiscard]] snapshot synchronize();
    [[nodiscard]] timer_state get_timer_state();
    [[nodiscard]] run_state get_run();
    [[nodiscard]] run_icons get_run_icons();
    [[nodiscard]] attempt_state get_attempt();
    [[nodiscard]] context_state get_context_state();
    [[nodiscard]] completed_count_value get_completed_count();

    void timer_control(timer_operation operation);
    void game_control(game_control_operation operation);
    void set_game_time(std::int64_t ticks);

    [[nodiscard]] std::optional<bridge_event> next_event(std::chrono::milliseconds timeout);
    [[nodiscard]] bridge_event wait_for_event();

private:
    using tcp = boost::asio::ip::tcp;
    using websocket = boost::beast::websocket::stream<boost::beast::tcp_stream>;

    struct connection_base {
        explicit connection_base(boost::asio::io_context& io)
            : websocket_stream(io) {}

        websocket websocket_stream;
        boost::beast::flat_buffer buffer{64 * 1024 * 1024};
        std::shared_ptr<boost::asio::steady_timer> deadline;
        std::shared_ptr<tcp::resolver> resolver;
        bool closed{false};
        bool established{false};
    };

    struct operation {
        enum class kind { query, control, synchronize };

        kind type{};
        query_method query{};
        timer_operation timer{};
        game_control_operation game{};
        bool game_time{false};
        bool game_set{false};
        std::int64_t ticks{0};
        bool pause_events{false};
        bool started{false};
        bool completed{false};
        std::shared_ptr<std::promise<rpc_response>> response;
        std::shared_ptr<std::promise<snapshot>> sync;
        std::shared_ptr<boost::asio::steady_timer> queue_timer;
    };

    struct pending_request {
        std::uint32_t id{};
        operation::kind kind{};
        bool write_started{false};
        std::shared_ptr<boost::asio::steady_timer> timer;
    };

    void begin_connect();
    void connect_events();
    void connect_rpc();
    void connect_endpoint(
        const std::shared_ptr<connection_base>& connection,
        const std::string& path,
        std::function<void()> on_success);
    void schedule_connection_attempt();
    void connection_failed(const std::shared_ptr<connection_base>& connection, std::string message);
    void close_connections();
    void close_connection(const std::shared_ptr<connection_base>& connection);

    void start_event_read(const std::shared_ptr<connection_base>& connection);
    void start_rpc_read(const std::shared_ptr<connection_base>& connection);
    void handle_event_text(const std::string& text);
    void handle_rpc_text(const std::string& text);
    void reset_event_stream(bool clear_queued_events);
    void begin_resync(std::string reason);
    void begin_reconnect(std::string reason);
    void perform_reconnect();
    void pump();

    void enqueue_from_public(const std::shared_ptr<operation>& op);
    void enqueue_operation(const std::shared_ptr<operation>& op);
    void start_operation(const std::shared_ptr<operation>& op);
    void start_sync(const std::shared_ptr<operation>& op);
    void send_next_sync_query(const std::shared_ptr<operation>& op);
    void finish_sync(const std::shared_ptr<operation>& op);
    void fail_sync(const std::shared_ptr<operation>& op, const error& failure);
    void fail_operation(const std::shared_ptr<operation>& op, const error& failure);
    void fail_current(const error& failure);
    void complete_operation(const std::shared_ptr<operation>& op, const rpc_response& response);
    void after_operation();

    void send_query(const std::shared_ptr<operation>& op);
    void send_control(const std::shared_ptr<operation>& op);
    void send_request(const std::shared_ptr<pending_request>& request, std::string payload);
    void handle_normal_response(const std::shared_ptr<operation>& op, const rpc_response& response);
    void handle_sync_response(const std::shared_ptr<operation>& op, const rpc_response& response);
    void on_response_timeout(const std::shared_ptr<pending_request>& request);
    void on_rpc_transport_failure(const std::shared_ptr<connection_base>& connection, std::string message);
    void on_rpc_protocol_failure(std::string message);

    [[nodiscard]] bool observe_normal_session(std::uint32_t session_id);
    [[nodiscard]] std::uint32_t allocate_request_id();
    [[nodiscard]] std::uint32_t response_request_id(const rpc_response& response) const;
    [[nodiscard]] std::uint32_t response_session_id(const rpc_response& response) const;

    void set_state(connection_state state);
    void publish_session(std::uint32_t session_id);
    void publish_snapshot(snapshot state);
    void push_event(bridge_event event);
    void clear_events();
    void shutdown();

    [[nodiscard]] std::string host_header() const;

    client_options options_;
    boost::asio::io_context io_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard_;
    boost::asio::steady_timer reconnect_timer_;
    std::thread thread_;
    std::atomic<bool> closed_{false};
    std::once_flag close_once_;

    mutable std::mutex submit_mutex_;

    mutable std::mutex mutex_;
    mutable std::condition_variable state_cv_;
    mutable std::condition_variable event_cv_;
    connection_state public_state_{connection_state::connecting};
    std::uint32_t public_session_id_{0};
    std::optional<snapshot> public_snapshot_;
    bool public_closed_{false};
    std::deque<bridge_event> events_;

    bool io_stopping_{false};
    bool connecting_{false};
    bool connect_scheduled_{false};
    bool connections_ready_{false};
    bool ready_{false};
    bool sync_needed_{true};
    bool reconnect_when_idle_{false};
    bool ever_ready_{false};
    bool event_paused_{true};
    bool event_baseline_reset_{true};

    std::chrono::milliseconds next_reconnect_delay_{0};
    std::shared_ptr<connection_base> rpc_;
    std::shared_ptr<connection_base> events_connection_;
    std::shared_ptr<pending_request> pending_;
    std::shared_ptr<operation> current_;
    std::deque<std::shared_ptr<operation>> queue_;

    std::uint32_t next_request_id_{1};
    std::uint32_t known_session_{0};
    std::optional<std::uint32_t> event_session_;
    std::optional<std::uint32_t> last_event_sequence_;

    std::uint32_t sync_session_{0};
    int sync_step_{0};
    snapshot sync_snapshot_;
};

}  // namespace livesplit::bridge::detail
