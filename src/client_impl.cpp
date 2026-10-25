#include "detail/client_impl.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <future>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket/error.hpp>

namespace livesplit::bridge::detail {
namespace {

constexpr auto sync_queries = std::array{
    query_method::GET_TIMER_STATE,
    query_method::GET_ATTEMPT,
    query_method::GET_RUN,
    query_method::GET_CONTEXT_STATE,
    query_method::GET_COMPLETED_COUNT,
};

constexpr std::uint8_t sync_timer_part = 1U << 0;
constexpr std::uint8_t sync_attempt_part = 1U << 1;
constexpr std::uint8_t sync_run_part = 1U << 2;
constexpr std::uint8_t sync_context_part = 1U << 3;
constexpr std::uint8_t sync_completed_part = 1U << 4;
constexpr std::uint8_t sync_all_parts =
    sync_timer_part | sync_attempt_part | sync_run_part | sync_context_part | sync_completed_part;

[[nodiscard]] bool contains_query(
    const std::deque<query_method>& queue,
    query_method method) {
    return std::find(queue.begin(), queue.end(), method) != queue.end();
}

[[nodiscard]] std::uint8_t part_for_query(query_method method) {
    switch (method) {
        case query_method::GET_TIMER_STATE:
            return sync_timer_part;
        case query_method::GET_ATTEMPT:
            return sync_attempt_part;
        case query_method::GET_RUN:
            return sync_run_part;
        case query_method::GET_CONTEXT_STATE:
            return sync_context_part;
        case query_method::GET_COMPLETED_COUNT:
            return sync_completed_part;
        case query_method::GET_RUN_ICONS:
            return 0;
    }
    return 0;
}

[[nodiscard]] std::uint32_t next_counter(std::uint32_t value) noexcept {
    return value == std::numeric_limits<std::uint32_t>::max() ? 0 : value + 1;
}

[[nodiscard]] std::chrono::milliseconds grow_delay(
    std::chrono::milliseconds current,
    std::chrono::milliseconds initial,
    std::chrono::milliseconds maximum) noexcept {
    if (current <= std::chrono::milliseconds::zero()) return initial;
    const auto doubled = current > maximum / 2 ? maximum : current * 2;
    return std::min(doubled, maximum);
}

[[nodiscard]] bool is_query_response(const rpc_response& response, query_method method) {
    switch (method) {
        case query_method::GET_TIMER_STATE:
            return std::holds_alternative<timer_state_response>(response);
        case query_method::GET_RUN:
            return std::holds_alternative<run_state_response>(response);
        case query_method::GET_RUN_ICONS:
            return std::holds_alternative<run_icons_response>(response);
        case query_method::GET_ATTEMPT:
            return std::holds_alternative<attempt_state_response>(response);
        case query_method::GET_CONTEXT_STATE:
            return std::holds_alternative<context_state_response>(response);
        case query_method::GET_COMPLETED_COUNT:
            return std::holds_alternative<completed_count_response>(response);
    }
    return false;
}

[[nodiscard]] const bridge_error& remote_error(const rpc_response& response) {
    return std::get<error_response>(response).error;
}

[[nodiscard]] std::string describe_response(const rpc_response& response) {
    if (std::holds_alternative<error_response>(response)) return "error_response";
    if (std::holds_alternative<timer_state_response>(response)) return "timer_state_response";
    if (std::holds_alternative<run_state_response>(response)) return "run_state_response";
    if (std::holds_alternative<run_icons_response>(response)) return "run_icons_response";
    if (std::holds_alternative<attempt_state_response>(response)) return "attempt_state_response";
    if (std::holds_alternative<context_state_response>(response)) return "context_state_response";
    if (std::holds_alternative<completed_count_response>(response)) return "completed_count_response";
    return "control_success_response";
}

[[nodiscard]] std::string method_name(query_method method) {
    switch (method) {
        case query_method::GET_TIMER_STATE:
            return "get_timer_state";
        case query_method::GET_RUN:
            return "get_run";
        case query_method::GET_RUN_ICONS:
            return "get_run_icons";
        case query_method::GET_ATTEMPT:
            return "get_attempt";
        case query_method::GET_CONTEXT_STATE:
            return "get_context_state";
        case query_method::GET_COMPLETED_COUNT:
            return "get_completed_count";
    }
    return "unknown";
}

}  // namespace

client_impl::client_impl(client_options options)
    : options_(std::move(options)),
      work_guard_(boost::asio::make_work_guard(io_)),
      reconnect_timer_(io_),
      sync_timer_(io_) {
    if (options_.host.empty()) {
        throw std::invalid_argument("client_options.host must not be empty");
    }
    if (options_.port == 0) {
        throw std::invalid_argument("client_options.port must be non-zero");
    }
    if (options_.rpc_path.empty() || options_.rpc_path.front() != '/') {
        throw std::invalid_argument("client_options.rpc_path must start with '/'");
    }
    if (options_.events_path.empty() || options_.events_path.front() != '/') {
        throw std::invalid_argument("client_options.events_path must start with '/'");
    }
    if (options_.connect_timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("client_options.connect_timeout must be positive");
    }
    if (options_.response_timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("client_options.response_timeout must be positive");
    }
    if (options_.ready_timeout < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("client_options.ready_timeout must be non-negative");
    }
    if (options_.sync_timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("client_options.sync_timeout must be positive");
    }
    if (options_.reconnect_initial_delay < std::chrono::milliseconds::zero() ||
        options_.reconnect_max_delay < options_.reconnect_initial_delay) {
        throw std::invalid_argument("client_options reconnect delays are invalid");
    }
}

client_impl::~client_impl() {
    close();
}

void client_impl::start() {
    thread_ = std::thread([self = shared_from_this()] { self->io_.run(); });
    boost::asio::post(io_, [self = shared_from_this()] { self->begin_connect(); });
}

void client_impl::close() noexcept {
    std::call_once(close_once_, [this] {
        std::lock_guard submit_lock(submit_mutex_);
        closed_.store(true);
        {
            std::lock_guard lock(mutex_);
            public_closed_ = true;
            public_state_ = connection_state::closed;
        }
        state_cv_.notify_all();
        event_cv_.notify_all();

        if (thread_.joinable()) {
            boost::asio::post(io_, [self = shared_from_this()] { self->shutdown(); });
            if (thread_.get_id() != std::this_thread::get_id()) {
                thread_.join();
            }
        } else {
            shutdown();
        }
    });
}

connection_state client_impl::state() const {
    std::lock_guard lock(mutex_);
    return public_state_;
}

bool client_impl::wait_until_ready(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    if (public_closed_) {
        throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    }
    const bool ready = state_cv_.wait_for(lock, timeout, [this] {
        return public_closed_ || public_state_ == connection_state::ready;
    });
    if (public_closed_) {
        throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    }
    if (!ready) {
        throw error(error_kind::timeout, "timed out waiting for LiveSplit.Bridge to become ready");
    }
    return true;
}

std::uint32_t client_impl::session_id() const {
    std::lock_guard lock(mutex_);
    if (public_closed_) {
        throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    }
    return public_session_id_;
}

std::uint64_t client_impl::dropped_event_count() const {
    std::lock_guard lock(mutex_);
    return dropped_event_count_;
}

snapshot client_impl::current_snapshot() const {
    std::lock_guard lock(mutex_);
    if (public_closed_) {
        throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    }
    if (!public_snapshot_ || public_state_ != connection_state::ready) {
        throw error(error_kind::not_ready, "no synchronized LiveSplit.Bridge snapshot is available");
    }
    return *public_snapshot_;
}

void client_impl::set_state(connection_state state) {
    {
        std::lock_guard lock(mutex_);
        if (public_closed_) return;
        public_state_ = state;
    }
    state_cv_.notify_all();
}

void client_impl::publish_session(std::uint32_t session_id) {
    {
        std::lock_guard lock(mutex_);
        public_session_id_ = session_id;
    }
}

void client_impl::publish_snapshot(snapshot state) {
    {
        std::lock_guard lock(mutex_);
        public_snapshot_ = std::move(state);
    }
}

void client_impl::push_event(bridge_event event) {
    {
        std::lock_guard lock(mutex_);
        events_.push_back(std::move(event));
        constexpr std::size_t maximum_events = 4096;
        while (events_.size() > maximum_events) {
            events_.pop_front();
            ++dropped_event_count_;
        }
    }
    event_cv_.notify_one();
}

void client_impl::clear_events() {
    {
        std::lock_guard lock(mutex_);
        events_.clear();
    }
}

void client_impl::shutdown() {
    io_stopping_ = true;
    sync_timer_.cancel();
    const error stopped(error_kind::closed, "LiveSplit.Bridge client is closed");

    auto request = pending_;
    pending_.reset();
    if (request && request->timer) request->timer->cancel();
    if (current_) {
        if (request && request->kind == operation::kind::control && request->write_started) {
            fail_operation(
                current_,
                error(
                    error_kind::control_result_unknown,
                    "client closed after sending a control; the result is unknown and was not retried",
                    request->id));
        } else {
            fail_operation(current_, stopped);
        }
    }
    while (!queue_.empty()) {
        auto op = queue_.front();
        queue_.pop_front();
        fail_operation(op, stopped);
    }

    reconnect_timer_.cancel();
    close_connections();
    event_cv_.notify_all();
    state_cv_.notify_all();
    work_guard_.reset();
}

void client_impl::close_connection(const std::shared_ptr<connection_base>& connection) {
    if (!connection || connection->closed) return;
    connection->closed = true;
    if (connection->deadline) connection->deadline->cancel();
    if (connection->resolver) connection->resolver->cancel();
    boost::system::error_code ignored;
    auto& socket = boost::beast::get_lowest_layer(connection->websocket_stream).socket();
    socket.shutdown(tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

void client_impl::close_connections() {
    close_connection(rpc_);
    close_connection(events_connection_);
    rpc_.reset();
    events_connection_.reset();
    connections_ready_ = false;
}

void client_impl::reset_event_stream(bool clear_queued_events) {
    event_baseline_reset_ = true;
    event_session_.reset();
    last_event_sequence_.reset();
    if (clear_queued_events) clear_events();
}

void client_impl::begin_connect() {
    if (closed_ || io_stopping_) return;
    connect_scheduled_ = false;
    connecting_ = true;
    connections_ready_ = false;
    ready_ = false;
    reconnect_when_idle_ = false;
    sync_needed_ = true;
    known_session_ = 0;
    if (pending_) {
        if (pending_->timer) pending_->timer->cancel();
        pending_.reset();
    }
    close_connections();
    reset_event_stream(true);
    set_state(ever_ready_ ? connection_state::reconnecting : connection_state::connecting);
    connect_events();
}

void client_impl::schedule_connection_attempt() {
    if (closed_ || io_stopping_ || connect_scheduled_) return;
    connect_scheduled_ = true;
    connecting_ = false;
    next_reconnect_delay_ = next_reconnect_delay_ <= std::chrono::milliseconds::zero()
        ? options_.reconnect_initial_delay
        : grow_delay(next_reconnect_delay_, options_.reconnect_initial_delay, options_.reconnect_max_delay);
    set_state(connection_state::reconnecting);

    const auto delay = next_reconnect_delay_;
    if (delay <= std::chrono::milliseconds::zero()) {
        boost::asio::post(io_, [self = shared_from_this()] { self->begin_connect(); });
        return;
    }
    reconnect_timer_.expires_after(delay);
    reconnect_timer_.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
        if (!ec) self->begin_connect();
    });
}

void client_impl::connection_failed(
    const std::shared_ptr<connection_base>& connection,
    std::string message) {
    if (closed_ || io_stopping_) return;
    if (!connection || connection->closed) return;
    close_connection(connection);
    close_connections();
    connecting_ = false;
    connections_ready_ = false;
    ready_ = false;
    (void)message;
    schedule_connection_attempt();
}

void client_impl::connect_events() {
    auto connection = std::make_shared<connection_base>(io_);
    events_connection_ = connection;
    connect_endpoint(connection, options_.events_path, [self = shared_from_this(), connection] {
        if (self->closed_ || connection->closed) return;
        self->start_event_read(connection);
        self->connect_rpc();
    });
}

void client_impl::connect_rpc() {
    auto connection = std::make_shared<connection_base>(io_);
    rpc_ = connection;
    connect_endpoint(connection, options_.rpc_path, [self = shared_from_this(), connection] {
        if (self->closed_ || connection->closed) return;
        self->start_rpc_read(connection);
        self->connecting_ = false;
        self->connections_ready_ = true;
        self->sync_needed_ = true;
        self->pump();
    });
}

void client_impl::connect_endpoint(
    const std::shared_ptr<connection_base>& connection,
    const std::string& path,
    std::function<void()> on_success) {
    if (closed_ || io_stopping_) return;

    connection->deadline = std::make_shared<boost::asio::steady_timer>(io_);
    connection->deadline->expires_after(options_.connect_timeout);
    connection->deadline->async_wait(
        [self = shared_from_this(), connection](const boost::system::error_code& ec) {
            if (ec || connection->established || connection->closed) return;
            self->connection_failed(connection, "WebSocket connection timed out");
        });

    connection->resolver = std::make_shared<tcp::resolver>(io_);
    connection->resolver->async_resolve(
        options_.host,
        std::to_string(options_.port),
        [self = shared_from_this(), connection, path = std::string(path), on_success = std::move(on_success)](
            const boost::system::error_code& ec,
            tcp::resolver::results_type results) mutable {
            if (self->closed_ || connection->closed) return;
            if (ec) {
                self->connection_failed(connection, "failed to resolve Bridge endpoint: " + ec.message());
                return;
            }
            boost::beast::get_lowest_layer(connection->websocket_stream)
                .async_connect(
                    results,
                    [self,
                     connection,
                     path,
                     on_success = std::move(on_success)](
                        const boost::system::error_code& connect_error,
                        const tcp::endpoint&) mutable {
                        if (self->closed_ || connection->closed) return;
                        if (connect_error) {
                            self->connection_failed(
                                connection,
                                "failed to connect to Bridge endpoint: " + connect_error.message());
                            return;
                        }

                        connection->websocket_stream.set_option(
                            boost::beast::websocket::stream_base::timeout::suggested(
                                boost::beast::role_type::client));
                        connection->websocket_stream.set_option(
                            boost::beast::websocket::stream_base::decorator(
                                [origin = self->options_.origin](
                                    boost::beast::websocket::request_type& request) {
                                    request.set(boost::beast::http::field::user_agent,
                                                "livesplit-bridge-client-cpp/0.1");
                                    if (origin) {
                                        request.set(boost::beast::http::field::origin, *origin);
                                    }
                                }));

                        connection->websocket_stream.async_handshake(
                            self->host_header(),
                            path,
                            [self, connection, on_success = std::move(on_success)](
                                const boost::system::error_code& handshake_error) mutable {
                                if (self->closed_ || connection->closed) return;
                                if (handshake_error) {
                                    self->connection_failed(
                                        connection,
                                        "Bridge WebSocket handshake failed: " +
                                            handshake_error.message());
                                    return;
                                }
                                connection->established = true;
                                connection->websocket_stream.read_message_max(
                                    connection_base::max_message_bytes);
                                if (connection->deadline) connection->deadline->cancel();
                                boost::beast::get_lowest_layer(connection->websocket_stream)
                                    .expires_never();
                                on_success();
                            });
                    });
        });
}

std::string client_impl::host_header() const {
    const bool needs_brackets = options_.host.find(':') != std::string::npos &&
        (options_.host.empty() || options_.host.front() != '[');
    std::string host = needs_brackets ? "[" + options_.host + "]" : options_.host;
    if (options_.port == 80) return host;
    return host + ":" + std::to_string(options_.port);
}

void client_impl::start_event_read(const std::shared_ptr<connection_base>& connection) {
    connection->websocket_stream.async_read(
        connection->buffer,
        [self = shared_from_this(), connection](
            const boost::system::error_code& ec,
            std::size_t bytes) {
            if (self->closed_ || connection->closed) return;
            if (ec) {
                if (ec == boost::beast::websocket::error::closed) {
                    self->begin_reconnect("Events connection was closed by the Bridge");
                } else {
                    self->begin_reconnect("Events read failed: " + ec.message());
                }
                return;
            }
            if (!connection->websocket_stream.got_text()) {
                self->begin_reconnect("Bridge sent a non-text Events frame");
                return;
            }
            const std::string text = boost::beast::buffers_to_string(connection->buffer.data());
            connection->buffer.consume(bytes);
            self->handle_event_text(text);
            if (!self->closed_ && !connection->closed) {
                self->start_event_read(connection);
            }
        });
}

void client_impl::start_rpc_read(const std::shared_ptr<connection_base>& connection) {
    connection->websocket_stream.async_read(
        connection->buffer,
        [self = shared_from_this(), connection](
            const boost::system::error_code& ec,
            std::size_t bytes) {
            if (self->closed_ || connection->closed) return;
            if (ec) {
                self->on_rpc_transport_failure(
                    connection,
                    ec == boost::beast::websocket::error::closed
                        ? "RPC connection was closed by the Bridge"
                        : "RPC read failed: " + ec.message());
                return;
            }
            if (!connection->websocket_stream.got_text()) {
                self->on_rpc_protocol_failure("Bridge sent a non-text RPC frame");
                return;
            }
            const std::string text = boost::beast::buffers_to_string(connection->buffer.data());
            connection->buffer.consume(bytes);
            self->handle_rpc_text(text);
            if (!self->closed_ && !connection->closed) {
                self->start_rpc_read(connection);
            }
        });
}

void client_impl::handle_event_text(const std::string& text) {
    if (closed_ || io_stopping_) return;

    bridge_event event;
    try {
        event = parse_bridge_event(text);
    } catch (const std::exception&) {
        if (current_ && current_->type == operation::kind::synchronize) {
            sync_dirty_parts_ |= sync_all_parts;
            sync_saw_events_ = true;
            event_baseline_reset_ = true;
            last_event_sequence_.reset();
        } else {
            begin_resync("Bridge returned a malformed event");
        }
        return;
    }

    const std::uint32_t session_id = std::visit(
        [](const auto& value) { return value.session_id; },
        event);
    const std::uint32_t sequence = std::visit(
        [](const auto& value) { return value.event_sequence; },
        event);

    if (!event_session_) {
        event_session_ = session_id;
    } else if (*event_session_ != session_id) {
        begin_reconnect("Events session changed");
        return;
    }
    if (known_session_ != 0 && session_id != known_session_) {
        begin_reconnect("Events and RPC sessions differ");
        return;
    }

    if (event_baseline_reset_) {
        event_baseline_reset_ = false;
    } else if (last_event_sequence_) {
        const auto expected = *last_event_sequence_ == std::numeric_limits<std::uint32_t>::max()
            ? 1u
            : *last_event_sequence_ + 1u;
        if (sequence != expected) {
            if (current_ && current_->type == operation::kind::synchronize) {
                sync_dirty_parts_ |= sync_all_parts;
                sync_saw_events_ = true;
                event_baseline_reset_ = true;
                last_event_sequence_.reset();
            } else {
                begin_resync("Events sequence gap");
            }
            return;
        }
    }
    last_event_sequence_ = sequence;

    const bool syncing = current_ && current_->type == operation::kind::synchronize;
    if (syncing) sync_saw_events_ = true;
    const bool timer_query_in_flight = pending_ &&
        pending_->kind == operation::kind::synchronize &&
        sync_method_ == query_method::GET_TIMER_STATE;
    const auto invalidate = [this, syncing](query_method method) {
        if (!syncing) return;
        const bool in_flight = pending_ &&
            pending_->kind == operation::kind::synchronize &&
            sync_method_ == method;
        if (in_flight || !contains_query(sync_queue_, method)) {
            sync_dirty_parts_ |= part_for_query(method);
        }
    };

    std::optional<timer_state> timer_update;
    std::visit(
        [&](const auto& value) {
            using value_type = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<value_type, timer_event>) {
                timer_update = value.timer_state;
                if (syncing) {
                    if (timer_query_in_flight) {
                        sync_dirty_parts_ |= sync_timer_part;
                    } else {
                        sync_snapshot_.timer = value.timer_state;
                    }
                }
            } else if constexpr (std::is_same_v<value_type, reset_event>) {
                timer_update = value.timer_state;
                if (syncing) {
                    if (value.timer_state) {
                        if (timer_query_in_flight) sync_dirty_parts_ |= sync_timer_part;
                        else sync_snapshot_.timer = *value.timer_state;
                    } else if (
                        timer_query_in_flight ||
                        !contains_query(sync_queue_, query_method::GET_TIMER_STATE)) {
                        sync_dirty_parts_ |= sync_timer_part;
                    }
                    invalidate(query_method::GET_ATTEMPT);
                    invalidate(query_method::GET_COMPLETED_COUNT);
                }
            } else {
                if (value.type == changed_event_type::RUN_CHANGED) {
                    invalidate(query_method::GET_RUN);
                } else {
                    invalidate(query_method::GET_CONTEXT_STATE);
                }
            }
        },
        event);

    if (timer_update && ready_) {
        std::lock_guard lock(mutex_);
        if (public_snapshot_ && public_snapshot_->session_id == session_id) {
            public_snapshot_->timer = *timer_update;
        }
    }
    push_event(std::move(event));
}

void client_impl::handle_rpc_text(const std::string& text) {
    if (closed_ || io_stopping_) return;
    if (!pending_) {
        on_rpc_protocol_failure("RPC response received without a pending request");
        return;
    }
    if (!pending_->write_complete) {
        pending_->deferred_response = text;
        pending_->has_deferred_response = true;
        return;
    }

    rpc_response response;
    try {
        response = parse_rpc_response(text);
    } catch (const std::exception& ex) {
        on_rpc_protocol_failure(std::string("malformed RPC response: ") + ex.what());
        return;
    }

    auto request = pending_;
    const auto request_id = response_request_id(response);
    if (!request || request_id != request->id) {
        on_rpc_protocol_failure("RPC response request_id does not match the pending request");
        return;
    }

    pending_.reset();
    if (request->timer) request->timer->cancel();
    const auto session_id = response_session_id(response);

    if (std::holds_alternative<error_response>(response)) {
        const auto& remote = remote_error(response);
        error failure(
            error_kind::remote_error,
            "Bridge error " + nlohmann::json(remote.code).get<std::string>() + ": " +
                remote.message,
            remote,
            request_id);

        if (request->kind == operation::kind::synchronize) {
            fail_sync(current_, failure);
            begin_reconnect("RPC error during synchronization");
            return;
        }

        const bool session_ok = observe_normal_session(session_id);
        fail_current(failure);
        if (!session_ok) reconnect_when_idle_ = true;
        after_operation();
        return;
    }

    if (request->kind == operation::kind::synchronize) {
        handle_sync_response(current_, response);
    } else {
        handle_normal_response(current_, response);
    }
}

bool client_impl::observe_normal_session(std::uint32_t session_id) {
    if (known_session_ == 0) {
        known_session_ = session_id;
        publish_session(session_id);
        return true;
    }
    if (known_session_ != session_id) {
        known_session_ = session_id;
        publish_session(session_id);
        reconnect_when_idle_ = true;
        set_state(connection_state::reconnecting);
        return false;
    }
    return true;
}

std::uint32_t client_impl::response_request_id(const rpc_response& response) const {
    return std::visit([](const auto& value) { return value.request_id; }, response);
}

std::uint32_t client_impl::response_session_id(const rpc_response& response) const {
    return std::visit([](const auto& value) { return value.session_id; }, response);
}

void client_impl::handle_normal_response(
    const std::shared_ptr<operation>& op,
    const rpc_response& response) {
    if (!op) {
        on_rpc_protocol_failure("RPC response received without an active operation");
        return;
    }

    const bool valid = op->type == operation::kind::query
        ? is_query_response(response, op->query)
        : std::holds_alternative<control_success_response>(response);
    if (!valid) {
        on_rpc_protocol_failure(
            "RPC response type does not match the requested method: " + describe_response(response));
        return;
    }

    const bool session_ok = observe_normal_session(response_session_id(response));
    complete_operation(op, response);
    if (!session_ok) reconnect_when_idle_ = true;
    after_operation();
}

void client_impl::handle_sync_response(
    const std::shared_ptr<operation>& op,
    const rpc_response& response) {
    if (!op || op->type != operation::kind::synchronize) {
        on_rpc_protocol_failure("synchronization response received without a sync operation");
        return;
    }

    const auto expected = sync_method_;
    if (!is_query_response(response, expected)) {
        fail_sync(
            op,
            error(
                error_kind::protocol_error,
                "synchronization response type mismatch for " + method_name(expected)));
        begin_reconnect("invalid synchronization response");
        return;
    }

    const auto session_id = response_session_id(response);
    if (sync_session_ == 0) {
        sync_session_ = session_id;
    } else if (sync_session_ != session_id) {
        fail_sync(op, error(error_kind::connection_lost, "RPC session changed during synchronization"));
        begin_reconnect("RPC session changed during synchronization");
        return;
    }

    switch (expected) {
        case query_method::GET_TIMER_STATE:
            sync_snapshot_.timer = std::get<timer_state_response>(response).result;
            break;
        case query_method::GET_ATTEMPT:
            sync_snapshot_.attempt = std::get<attempt_state_response>(response).result;
            break;
        case query_method::GET_RUN:
            sync_snapshot_.run = std::get<run_state_response>(response).result;
            break;
        case query_method::GET_CONTEXT_STATE:
            sync_snapshot_.context = std::get<context_state_response>(response).result;
            break;
        case query_method::GET_COMPLETED_COUNT:
            sync_snapshot_.completed = std::get<completed_count_response>(response).result;
            break;
        case query_method::GET_RUN_ICONS:
            fail_sync(op, error(error_kind::internal_error, "run icons are not part of synchronization"));
            begin_reconnect("invalid synchronization step");
            return;
    }

    send_next_sync_query(op);
}

void client_impl::begin_resync(std::string reason) {
    (void)reason;
    if (closed_ || io_stopping_ || connecting_ || connect_scheduled_) return;
    if (current_ && current_->type == operation::kind::synchronize) {
        sync_dirty_parts_ |= sync_all_parts;
        event_baseline_reset_ = true;
        last_event_sequence_.reset();
        return;
    }
    if (sync_needed_) return;
    ready_ = false;
    sync_needed_ = true;
    event_baseline_reset_ = true;
    last_event_sequence_.reset();
    set_state(connection_state::synchronizing);
    pump();
}

void client_impl::begin_reconnect(std::string reason) {
    if (closed_ || io_stopping_) return;
    if (connecting_ || connect_scheduled_) {
        ready_ = false;
        sync_needed_ = true;
        if (!connect_scheduled_) {
            close_connections();
            connecting_ = false;
            schedule_connection_attempt();
        }
        return;
    }
    ready_ = false;
    sync_needed_ = true;
    set_state(connection_state::reconnecting);
    if (current_) {
        if (current_->type == operation::kind::synchronize) {
            fail_current(error(
                error_kind::connection_lost,
                reason.empty() ? "reconnect required during synchronization" : reason));
        } else {
            reconnect_when_idle_ = true;
            return;
        }
    }
    perform_reconnect();
}

void client_impl::perform_reconnect() {
    if (closed_ || io_stopping_) return;
    reconnect_when_idle_ = false;
    close_connections();
    if (pending_) {
        if (pending_->timer) pending_->timer->cancel();
        pending_.reset();
    }
    connecting_ = false;
    connect_scheduled_ = true;
    reset_event_stream(true);
    known_session_ = 0;
    if (next_reconnect_delay_ <= std::chrono::milliseconds::zero()) {
        next_reconnect_delay_ = options_.reconnect_initial_delay;
    }
    set_state(connection_state::reconnecting);
    const auto delay = next_reconnect_delay_;
    if (delay <= std::chrono::milliseconds::zero()) {
        connect_scheduled_ = false;
        boost::asio::post(io_, [self = shared_from_this()] { self->begin_connect(); });
        return;
    }
    reconnect_timer_.expires_after(delay);
    reconnect_timer_.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
        if (!ec) self->begin_connect();
    });
}

void client_impl::pump() {
    if (closed_ || io_stopping_ || connecting_ || !connections_ready_ || current_) return;

    if (reconnect_when_idle_) {
        perform_reconnect();
        return;
    }

    if (sync_needed_) {
        auto op = std::make_shared<operation>();
        op->type = operation::kind::synchronize;
        current_ = op;
        start_sync(op);
        return;
    }

    if (!ready_) return;

    while (!queue_.empty()) {
        auto op = queue_.front();
        queue_.pop_front();
        if (op->completed) continue;
        current_ = op;
        start_operation(op);
        return;
    }
}

void client_impl::enqueue_from_public(const std::shared_ptr<operation>& op) {
    std::lock_guard submit_lock(submit_mutex_);
    if (closed_.load()) {
        fail_operation(op, error(error_kind::closed, "LiveSplit.Bridge client is closed"));
        return;
    }
    boost::asio::post(io_, [self = shared_from_this(), op] { self->enqueue_operation(op); });
}

void client_impl::enqueue_operation(const std::shared_ptr<operation>& op) {
    if (closed_ || io_stopping_) {
        fail_operation(op, error(error_kind::closed, "LiveSplit.Bridge client is closed"));
        return;
    }

    op->queue_timer = std::make_shared<boost::asio::steady_timer>(io_);
    op->queue_timer->expires_after(options_.ready_timeout);
    op->queue_timer->async_wait(
        [self = shared_from_this(), op](const boost::system::error_code& ec) {
            if (ec || op->completed) return;
            if (op->started) return;

            const auto iterator = std::find(self->queue_.begin(), self->queue_.end(), op);
            if (iterator != self->queue_.end()) self->queue_.erase(iterator);
            self->fail_operation(
                op,
                error(error_kind::timeout, "operation timed out waiting for the Bridge to become ready"));
        });

    queue_.push_back(op);
    pump();
}

void client_impl::start_operation(const std::shared_ptr<operation>& op) {
    if (!op || op->completed) return;
    op->started = true;
    if (op->queue_timer) op->queue_timer->cancel();

    if (op->type == operation::kind::query) {
        send_query(op);
    } else if (op->type == operation::kind::control) {
        send_control(op);
    } else {
        start_sync(op);
    }
}

void client_impl::start_sync(const std::shared_ptr<operation>& op) {
    if (!op || closed_ || !connections_ready_ || !rpc_ || rpc_->closed) {
        fail_sync(op, error(error_kind::not_ready, "RPC connection is not available"));
        if (!closed_) begin_reconnect("RPC connection is not available");
        return;
    }
    sync_session_ = 0;
    sync_snapshot_ = {};
    sync_queue_.clear();
    for (const auto method : sync_queries) sync_queue_.push_back(method);
    sync_dirty_parts_ = 0;
    sync_saw_events_ = false;
    sync_timer_.cancel();
    sync_deadline_ = std::chrono::steady_clock::now() + options_.sync_timeout;
    sync_timer_.expires_at(sync_deadline_);
    sync_timer_.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
        if (ec || self->closed_ || self->io_stopping_) return;
        if (!self->current_ || self->current_->type != operation::kind::synchronize) return;
        const auto kind = self->sync_saw_events_
            ? error_kind::unstable_state
            : error_kind::timeout;
        self->sync_queue_.clear();
        self->sync_dirty_parts_ = 0;
        self->fail_sync(
            self->current_,
            error(kind, "synchronization exceeded sync_timeout"));
        self->begin_reconnect("synchronization timed out");
    });
    sync_needed_ = false;
    set_state(connection_state::synchronizing);
    send_next_sync_query(op);
}

void client_impl::send_next_sync_query(const std::shared_ptr<operation>& op) {
    if (sync_queue_.empty()) {
        // Let already-queued Event handlers run before deciding that the pass is
        // clean. This narrows the window in which a change can be observed after
        // the last Query but before Ready.
        boost::asio::post(io_, [self = shared_from_this(), op] {
            if (!self->closed_ && !self->io_stopping_) self->finish_sync(op);
        });
        return;
    }

    if (std::chrono::steady_clock::now() >= sync_deadline_) {
        sync_queue_.clear();
        sync_dirty_parts_ = 0;
        fail_sync(
            op,
            error(
                sync_saw_events_ ? error_kind::unstable_state : error_kind::timeout,
                "synchronization timed out"));
        begin_reconnect("synchronization timed out");
        return;
    }

    const auto method = sync_queue_.front();
    sync_queue_.pop_front();
    sync_method_ = method;
    const auto request_id = allocate_request_id();
    query_request request;
    request.request_id = request_id;
    request.method = method;
    nlohmann::json document = request;

    auto pending = std::make_shared<pending_request>();
    pending->id = request_id;
    pending->kind = operation::kind::synchronize;
    send_request(pending, document.dump());
}

void client_impl::finish_sync(const std::shared_ptr<operation>& op) {
    if (!op || op->completed || current_ != op) return;
    if (sync_session_ == 0) {
        fail_sync(op, error(error_kind::internal_error, "synchronization did not complete"));
        begin_reconnect("synchronization did not complete");
        return;
    }
    if (event_session_ && *event_session_ != sync_session_) {
        fail_sync(op, error(error_kind::connection_lost, "Events and RPC sessions differ"));
        begin_reconnect("Events and RPC sessions differ");
        return;
    }
    if (sync_dirty_parts_ != 0) {
        if (std::chrono::steady_clock::now() >= sync_deadline_) {
            sync_dirty_parts_ = 0;
            sync_queue_.clear();
            fail_sync(
                op,
                error(
                    error_kind::unstable_state,
                    "state changed continuously during synchronization"));
            begin_reconnect("synchronization did not stabilize");
            return;
        }

        const auto dirty = sync_dirty_parts_;
        sync_dirty_parts_ = 0;
        if ((dirty & sync_timer_part) != 0) {
            sync_queue_.push_back(query_method::GET_TIMER_STATE);
        }
        if ((dirty & sync_attempt_part) != 0) {
            sync_queue_.push_back(query_method::GET_ATTEMPT);
        }
        if ((dirty & sync_run_part) != 0) {
            sync_queue_.push_back(query_method::GET_RUN);
        }
        if ((dirty & sync_context_part) != 0) {
            sync_queue_.push_back(query_method::GET_CONTEXT_STATE);
        }
        if ((dirty & sync_completed_part) != 0) {
            sync_queue_.push_back(query_method::GET_COMPLETED_COUNT);
        }
        send_next_sync_query(op);
        return;
    }

    sync_timer_.cancel();
    sync_snapshot_.session_id = sync_session_;
    known_session_ = sync_session_;
    publish_session(sync_session_);
    publish_snapshot(sync_snapshot_);
    ready_ = true;
    next_reconnect_delay_ = std::chrono::milliseconds::zero();
    ever_ready_ = true;
    set_state(connection_state::ready);

    if (op->on_sync) {
        try {
            op->on_sync(sync_snapshot_);
        } catch (const std::future_error&) {
        }
    }
    op->completed = true;
    current_.reset();
    pump();
}

void client_impl::fail_sync(const std::shared_ptr<operation>& op, const error& failure) {
    sync_timer_.cancel();
    if (!op || op->completed) return;
    op->completed = true;
    if (op->on_error) {
        try {
            op->on_error(failure);
        } catch (const std::future_error&) {
        }
    }
    if (current_ == op) current_.reset();
}

void client_impl::fail_operation(const std::shared_ptr<operation>& op, const error& failure) {
    if (!op || op->completed) return;
    if (op->type == operation::kind::synchronize) sync_timer_.cancel();
    op->completed = true;
    if (op->queue_timer) op->queue_timer->cancel();
    if (op->on_error) {
        try {
            op->on_error(failure);
        } catch (const std::future_error&) {
        }
    }
    if (current_ == op) current_.reset();
}

void client_impl::fail_current(const error& failure) {
    if (!current_) return;
    fail_operation(current_, failure);
}

void client_impl::complete_operation(const std::shared_ptr<operation>& op, const rpc_response& response) {
    if (!op || op->completed) return;
    op->completed = true;
    if (op->queue_timer) op->queue_timer->cancel();
    if (op->on_response) {
        try {
            op->on_response(response);
        } catch (const std::future_error&) {
        }
    }
    if (current_ == op) current_.reset();
}

void client_impl::after_operation() {
    if (closed_ || io_stopping_) return;
    pump();
}

std::uint32_t client_impl::allocate_request_id() {
    const auto id = next_request_id_;
    next_request_id_ = next_counter(next_request_id_);
    return id;
}

void client_impl::send_query(const std::shared_ptr<operation>& op) {
    if (!rpc_ || rpc_->closed) {
        fail_operation(op, error(error_kind::connection_lost, "RPC connection is not available"));
        after_operation();
        return;
    }
    const auto request_id = allocate_request_id();
    query_request request;
    request.request_id = request_id;
    request.method = op->query;

    auto pending = std::make_shared<pending_request>();
    pending->id = request_id;
    pending->kind = operation::kind::query;
    nlohmann::json document = request;
    send_request(pending, document.dump());
}

void client_impl::send_control(const std::shared_ptr<operation>& op) {
    if (!rpc_ || rpc_->closed) {
        fail_operation(op, error(error_kind::connection_lost, "RPC connection is not available"));
        after_operation();
        return;
    }

    const auto request_id = allocate_request_id();
    nlohmann::json document;
    if (op->game_set) {
        game_set_request request;
        request.request_id = request_id;
        request.params.ticks = op->ticks;
        document = request;
    } else if (op->game_time) {
        game_control_request request;
        request.request_id = request_id;
        request.params.operation = op->game;
        document = request;
    } else {
        timer_request request;
        request.request_id = request_id;
        request.params.operation = op->timer;
        document = request;
    }

    auto pending = std::make_shared<pending_request>();
    pending->id = request_id;
    pending->kind = operation::kind::control;
    send_request(pending, document.dump());
}

void client_impl::send_request(
    const std::shared_ptr<pending_request>& request,
    std::string payload) {
    if (!rpc_ || rpc_->closed) {
        fail_current(error(error_kind::connection_lost, "RPC connection is not available"));
        after_operation();
        return;
    }

    pending_ = request;
    request->timer = std::make_shared<boost::asio::steady_timer>(io_);
    request->timer->expires_after(options_.response_timeout);
    request->timer->async_wait(
        [self = shared_from_this(), request](const boost::system::error_code& ec) {
            if (!ec) self->on_response_timeout(request);
        });

    auto data = std::make_shared<std::string>(std::move(payload));
    request->write_started = true;
    request->write_complete = false;
    request->has_deferred_response = false;
    rpc_->websocket_stream.async_write(
        boost::asio::buffer(*data),
        [self = shared_from_this(), connection = rpc_, data, request](
            const boost::system::error_code& ec,
            std::size_t) {
            if (self->closed_ || connection->closed || self->pending_ != request) return;
            if (ec) {
                self->on_rpc_transport_failure(
                    connection,
                    "RPC write failed: " + ec.message());
                return;
            }
            request->write_complete = true;
            if (request->has_deferred_response) {
                auto deferred = std::move(request->deferred_response);
                request->has_deferred_response = false;
                self->handle_rpc_text(deferred);
            }
        });
}

void client_impl::on_response_timeout(const std::shared_ptr<pending_request>& request) {
    if (closed_ || pending_ != request) return;

    pending_.reset();
    const bool control_unknown = request->kind == operation::kind::control && request->write_started;
    const auto failure = control_unknown
        ? error(
              error_kind::control_result_unknown,
              "control response timed out; the operation was not retried",
              request->id)
        : error(error_kind::timeout, "timed out waiting for a Bridge RPC response");

    fail_current(failure);
    close_connections();
    begin_reconnect("RPC response timed out");
}

void client_impl::on_rpc_transport_failure(
    const std::shared_ptr<connection_base>& connection,
    std::string message) {
    if (closed_ || io_stopping_ || connection->closed) return;
    close_connection(connection);

    auto request = pending_;
    pending_.reset();
    if (request && request->timer) request->timer->cancel();

    if (current_) {
        const bool control_unknown =
            request && request->kind == operation::kind::control && request->write_started;
        const auto failure = control_unknown
            ? error(
                  error_kind::control_result_unknown,
                  "control transport failed after sending; the result is unknown and was not retried: " +
                      message,
                  request->id)
            : error(error_kind::connection_lost, std::move(message));
        fail_current(failure);
    }

    close_connections();
    begin_reconnect("RPC transport failed");
}

void client_impl::on_rpc_protocol_failure(std::string message) {
    if (closed_ || io_stopping_) return;
    auto request = pending_;
    pending_.reset();
    if (request && request->timer) request->timer->cancel();

    if (current_) {
        const bool control_unknown = request && request->kind == operation::kind::control;
        const auto failure = control_unknown
            ? error(
                  error_kind::control_result_unknown,
                  "control response was invalid; the result is unknown and was not retried: " + message,
                  request->id)
            : error(error_kind::protocol_error, std::move(message));
        fail_current(failure);
    }

    close_connections();
    begin_reconnect("RPC protocol violation");
}

std::future<snapshot> client_impl::synchronize_async() {
    auto op = std::make_shared<operation>();
    op->type = operation::kind::synchronize;
    auto promise = std::make_shared<std::promise<snapshot>>();
    auto future = promise->get_future();
    op->on_sync = [promise](const snapshot& state) { promise->set_value(state); };
    op->on_error = [promise](const error& failure) {
        promise->set_exception(std::make_exception_ptr(failure));
    };
    enqueue_from_public(op);
    return future;
}

snapshot client_impl::synchronize() {
    return synchronize_async().get();
}

std::future<timer_state> client_impl::get_timer_state_async() {
    auto op = std::make_shared<operation>();
    op->type = operation::kind::query;
    op->query = query_method::GET_TIMER_STATE;
    auto promise = std::make_shared<std::promise<timer_state>>();
    auto future = promise->get_future();
    op->on_response = [promise](const rpc_response& response) {
        promise->set_value(std::get<timer_state_response>(response).result);
    };
    op->on_error = [promise](const error& failure) {
        promise->set_exception(std::make_exception_ptr(failure));
    };
    enqueue_from_public(op);
    return future;
}

timer_state client_impl::get_timer_state() {
    return get_timer_state_async().get();
}

std::future<run_state> client_impl::get_run_async() {
    auto op = std::make_shared<operation>();
    op->type = operation::kind::query;
    op->query = query_method::GET_RUN;
    auto promise = std::make_shared<std::promise<run_state>>();
    auto future = promise->get_future();
    op->on_response = [promise](const rpc_response& response) {
        promise->set_value(std::get<run_state_response>(response).result);
    };
    op->on_error = [promise](const error& failure) {
        promise->set_exception(std::make_exception_ptr(failure));
    };
    enqueue_from_public(op);
    return future;
}

run_state client_impl::get_run() {
    return get_run_async().get();
}

std::future<run_icons> client_impl::get_run_icons_async() {
    auto op = std::make_shared<operation>();
    op->type = operation::kind::query;
    op->query = query_method::GET_RUN_ICONS;
    auto promise = std::make_shared<std::promise<run_icons>>();
    auto future = promise->get_future();
    op->on_response = [promise](const rpc_response& response) {
        promise->set_value(std::get<run_icons_response>(response).result);
    };
    op->on_error = [promise](const error& failure) {
        promise->set_exception(std::make_exception_ptr(failure));
    };
    enqueue_from_public(op);
    return future;
}

run_icons client_impl::get_run_icons() {
    return get_run_icons_async().get();
}

std::future<attempt_state> client_impl::get_attempt_async() {
    auto op = std::make_shared<operation>();
    op->type = operation::kind::query;
    op->query = query_method::GET_ATTEMPT;
    auto promise = std::make_shared<std::promise<attempt_state>>();
    auto future = promise->get_future();
    op->on_response = [promise](const rpc_response& response) {
        promise->set_value(std::get<attempt_state_response>(response).result);
    };
    op->on_error = [promise](const error& failure) {
        promise->set_exception(std::make_exception_ptr(failure));
    };
    enqueue_from_public(op);
    return future;
}

attempt_state client_impl::get_attempt() {
    return get_attempt_async().get();
}

std::future<context_state> client_impl::get_context_state_async() {
    auto op = std::make_shared<operation>();
    op->type = operation::kind::query;
    op->query = query_method::GET_CONTEXT_STATE;
    auto promise = std::make_shared<std::promise<context_state>>();
    auto future = promise->get_future();
    op->on_response = [promise](const rpc_response& response) {
        promise->set_value(std::get<context_state_response>(response).result);
    };
    op->on_error = [promise](const error& failure) {
        promise->set_exception(std::make_exception_ptr(failure));
    };
    enqueue_from_public(op);
    return future;
}

context_state client_impl::get_context_state() {
    return get_context_state_async().get();
}

std::future<completed_count_value> client_impl::get_completed_count_async() {
    auto op = std::make_shared<operation>();
    op->type = operation::kind::query;
    op->query = query_method::GET_COMPLETED_COUNT;
    auto promise = std::make_shared<std::promise<completed_count_value>>();
    auto future = promise->get_future();
    op->on_response = [promise](const rpc_response& response) {
        promise->set_value(std::get<completed_count_response>(response).result);
    };
    op->on_error = [promise](const error& failure) {
        promise->set_exception(std::make_exception_ptr(failure));
    };
    enqueue_from_public(op);
    return future;
}

completed_count_value client_impl::get_completed_count() {
    return get_completed_count_async().get();
}

std::future<void> client_impl::timer_control_async(timer_operation value) {
    auto op = std::make_shared<operation>();
    op->type = operation::kind::control;
    op->timer = value;
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    op->on_response = [promise](const rpc_response& response) {
        if (!std::holds_alternative<control_success_response>(response)) {
            throw error(error_kind::internal_error, "unexpected response for timer control");
        }
        promise->set_value();
    };
    op->on_error = [promise](const error& failure) {
        promise->set_exception(std::make_exception_ptr(failure));
    };
    enqueue_from_public(op);
    return future;
}

void client_impl::timer_control(timer_operation value) {
    timer_control_async(value).get();
}

std::future<void> client_impl::game_control_async(game_control_operation value) {
    auto op = std::make_shared<operation>();
    op->type = operation::kind::control;
    op->game_time = true;
    op->game = value;
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    op->on_response = [promise](const rpc_response& response) {
        if (!std::holds_alternative<control_success_response>(response)) {
            throw error(error_kind::internal_error, "unexpected response for game-time control");
        }
        promise->set_value();
    };
    op->on_error = [promise](const error& failure) {
        promise->set_exception(std::make_exception_ptr(failure));
    };
    enqueue_from_public(op);
    return future;
}

void client_impl::game_control(game_control_operation value) {
    game_control_async(value).get();
}

std::future<void> client_impl::set_game_time_async(std::int64_t ticks) {
    auto op = std::make_shared<operation>();
    op->type = operation::kind::control;
    op->game_time = true;
    op->game_set = true;
    op->ticks = ticks;
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    op->on_response = [promise](const rpc_response& response) {
        if (!std::holds_alternative<control_success_response>(response)) {
            throw error(error_kind::internal_error, "unexpected response for set_game_time");
        }
        promise->set_value();
    };
    op->on_error = [promise](const error& failure) {
        promise->set_exception(std::make_exception_ptr(failure));
    };
    enqueue_from_public(op);
    return future;
}

void client_impl::set_game_time(std::int64_t ticks) {
    set_game_time_async(ticks).get();
}
std::optional<bridge_event> client_impl::next_event(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    if (public_closed_) {
        throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    }
    const bool available = event_cv_.wait_for(lock, timeout, [this] {
        return public_closed_ || !events_.empty();
    });
    if (public_closed_) {
        throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    }
    if (!available || events_.empty()) return std::nullopt;
    auto event = std::move(events_.front());
    events_.pop_front();
    return event;
}

bridge_event client_impl::wait_for_event() {
    std::unique_lock lock(mutex_);
    event_cv_.wait(lock, [this] { return public_closed_ || !events_.empty(); });
    if (public_closed_) {
        throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    }
    auto event = std::move(events_.front());
    events_.pop_front();
    return event;
}

}  // namespace livesplit::bridge::detail
