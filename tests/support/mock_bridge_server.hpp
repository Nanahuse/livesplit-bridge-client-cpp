#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include "livesplit/bridge/protocol.hpp"

namespace test_support {

class mock_bridge_server {
public:
    using tcp = boost::asio::ip::tcp;
    using websocket = boost::beast::websocket::stream<tcp::socket>;

    struct behavior {
        int no_response{0};
        int malformed{0};
        int binary{0};
        int wrong_request_id{0};
        int remote_error{0};
        int close_before_response{0};
        int delay_ms{0};
        livesplit::bridge::bridge_error_code error_code{
            livesplit::bridge::bridge_error_code::OPERATION_FAILED};
        std::string error_message{"operation failed"};
    };

    explicit mock_bridge_server(std::uint16_t port = 0) {
        tcp::endpoint endpoint{boost::asio::ip::make_address("127.0.0.1"), port};
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen();
        port_ = acceptor_.local_endpoint().port();
        accept_thread_ = std::thread([this] { accept_loop(); });
    }

    ~mock_bridge_server() { stop(); }

    mock_bridge_server(const mock_bridge_server&) = delete;
    mock_bridge_server& operator=(const mock_bridge_server&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    void stop() noexcept {
        if (stopped_.exchange(true)) return;
        boost::system::error_code ignored;
        acceptor_.close(ignored);
        if (accept_thread_.joinable()) accept_thread_.join();
        {
            std::lock_guard lock(connections_mutex_);
            for (const auto& connection : connections_) {
                boost::system::error_code close_error;
                connection->ws.next_layer().shutdown(tcp::socket::shutdown_both, close_error);
                connection->ws.next_layer().close(close_error);
            }
        }
        {
            std::lock_guard lock(events_mutex_);
            for (const auto& sink : event_sinks_) {
                std::lock_guard sink_lock(sink->mutex);
                sink->closed = true;
            }
            for (const auto& sink : event_sinks_) sink->cv.notify_all();
        }
        std::lock_guard lock(connections_mutex_);
        for (auto& thread : connection_threads_) {
            if (thread.joinable()) thread.join();
        }
        connection_threads_.clear();
    }

    void set_session_id(std::uint32_t session_id) noexcept {
        session_id_.store(session_id);
    }

    [[nodiscard]] std::uint32_t session_id() const noexcept { return session_id_.load(); }

    void set_icon_bytes(std::size_t count) {
        std::lock_guard lock(state_mutex_);
        icon_bytes_ = count;
    }
    void set_run_game_name(std::string name) {
        std::lock_guard lock(state_mutex_);
        run_.game_name = std::move(name);
    }

    void set_timer_phase(livesplit::bridge::timer_phase phase) {
        std::lock_guard lock(state_mutex_);
        timer_.phase = phase;
    }

    void set_behavior(const std::string& method, behavior value) {
        std::lock_guard lock(state_mutex_);
        behaviors_[method] = std::move(value);
    }

    void clear_behaviors() {
        std::lock_guard lock(state_mutex_);
        behaviors_.clear();
    }

    void close_rpc_after_responses(int count) {
        std::lock_guard lock(state_mutex_);
        close_after_responses_ = count;
    }

    void clear_counts() {
        std::lock_guard lock(state_mutex_);
        method_counts_.clear();
        requests_.clear();
    }

    [[nodiscard]] int method_count(const std::string& method) {
        std::lock_guard lock(state_mutex_);
        const auto iterator = method_counts_.find(method);
        return iterator == method_counts_.end() ? 0 : iterator->second;
    }

    [[nodiscard]] std::vector<std::string> methods() {
        std::lock_guard lock(state_mutex_);
        std::vector<std::string> result;
        result.reserve(requests_.size());
        for (const auto& request : requests_) result.push_back(request.value("method", ""));
        return result;
    }

    [[nodiscard]] std::vector<nlohmann::json> requests() {
        std::lock_guard lock(state_mutex_);
        return requests_;
    }

    void disconnect_events() {
        std::lock_guard lock(connections_mutex_);
        for (const auto& connection : connections_) {
            if (!connection->is_events.load()) continue;
            boost::system::error_code ignored;
            connection->ws.next_layer().shutdown(tcp::socket::shutdown_both, ignored);
            connection->ws.next_layer().close(ignored);
        }
    }
    void send_event(nlohmann::json event) { broadcast(event.dump()); }
    void send_raw_event(std::string event) { broadcast(std::move(event)); }

private:
    struct connection {
        explicit connection(tcp::socket socket) : ws(std::move(socket)) {}
        websocket ws;
        std::atomic<bool> is_events{false};
    };

    struct event_sink {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::string> messages;
        bool closed{false};
    };

    enum class fault { none, no_response, malformed, binary, wrong_request_id, remote_error, close };

    void accept_loop() {
        while (!stopped_.load()) {
            boost::system::error_code error;
            tcp::socket socket(io_);
            acceptor_.accept(socket, error);
            if (error || stopped_.load()) return;
            auto connection = std::make_shared<struct connection>(std::move(socket));
            {
                std::lock_guard lock(connections_mutex_);
                connections_.push_back(connection);
                connection_threads_.emplace_back([this, connection] { serve(connection); });
            }
        }
    }

    void serve(const std::shared_ptr<connection>& connection) {
        boost::beast::flat_buffer buffer;
        boost::beast::http::request<boost::beast::http::string_body> request;
        boost::system::error_code error;
        boost::beast::http::read(connection->ws.next_layer(), buffer, request, error);
        if (error) return;

        const std::string target = std::string(request.target());
        connection->ws.accept(request, error);
        if (error) return;
        connection->ws.text(true);

        if (target.ends_with("/bridge/v4/events")) {
            connection->is_events.store(true);
            serve_events(connection);
        } else if (target.ends_with("/bridge/v4/rpc")) {
            serve_rpc(connection);
        }
        boost::system::error_code ignored;
        connection->ws.next_layer().shutdown(tcp::socket::shutdown_both, ignored);
        connection->ws.next_layer().close(ignored);
    }

    void serve_events(const std::shared_ptr<connection>& connection) {
        auto sink = std::make_shared<event_sink>();
        {
            std::lock_guard lock(events_mutex_);
            event_sinks_.push_back(sink);
        }

        while (!stopped_.load()) {
            std::string message;
            {
                std::unique_lock lock(sink->mutex);
                sink->cv.wait_for(lock, std::chrono::milliseconds{50}, [&] {
                    return sink->closed || !sink->messages.empty();
                });
                if (sink->closed || stopped_.load()) break;
                if (sink->messages.empty()) continue;
                message = std::move(sink->messages.front());
                sink->messages.pop_front();
            }
            boost::system::error_code error;
            connection->ws.write(boost::asio::buffer(message), error);
            if (error) break;
        }

        {
            std::lock_guard lock(events_mutex_);
            event_sinks_.erase(
                std::remove(event_sinks_.begin(), event_sinks_.end(), sink),
                event_sinks_.end());
        }
    }

    void serve_rpc(const std::shared_ptr<connection>& connection) {
        boost::beast::flat_buffer buffer;
        while (!stopped_.load()) {
            boost::system::error_code error;
            connection->ws.read(buffer, error);
            if (error) return;
            if (!connection->ws.got_text()) {
                return;
            }
            const std::string text = boost::beast::buffers_to_string(buffer.data());
            buffer.consume(buffer.size());

            nlohmann::json request;
            try {
                request = nlohmann::json::parse(text);
            } catch (const std::exception&) {
                return;
            }

            const std::string method = request.value("method", "");
            const std::uint32_t request_id = request.value("request_id", 0u);
            const auto selected = take_fault(method);
            {
                std::lock_guard lock(state_mutex_);
                ++method_counts_[method];
                requests_.push_back(request);
            }

            const auto selected_delay = selected.delay_ms;
            if (selected_delay > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds{selected_delay});
            }
            if (selected.fault == fault::no_response) continue;
            if (selected.fault == fault::close) return;
            if (selected.fault == fault::binary) {
                const std::string payload = "binary";
                connection->ws.binary(true);
                connection->ws.write(boost::asio::buffer(payload), error);
                return;
            }
            if (selected.fault == fault::malformed) {
                const std::string payload = "not json";
                connection->ws.write(boost::asio::buffer(payload), error);
                if (error) return;
                if (take_close_after_response()) return;
                continue;
            }

            nlohmann::json response = make_response(request, selected);
            const std::string payload = response.dump();
            connection->ws.write(boost::asio::buffer(payload), error);
            if (error) return;
            if (take_close_after_response()) return;
        }
    }

    struct selected_behavior {
        fault fault{fault::none};
        int delay_ms{0};
        livesplit::bridge::bridge_error_code error_code{
            livesplit::bridge::bridge_error_code::OPERATION_FAILED};
        std::string error_message;
    };

    [[nodiscard]] selected_behavior take_fault(const std::string& method) {
        std::lock_guard lock(state_mutex_);
        auto& value = behaviors_[method];
        selected_behavior selected;
        selected.delay_ms = value.delay_ms;
        selected.error_code = value.error_code;
        selected.error_message = value.error_message;
        if (value.no_response > 0) {
            --value.no_response;
            selected.fault = fault::no_response;
        } else if (value.malformed > 0) {
            --value.malformed;
            selected.fault = fault::malformed;
        } else if (value.binary > 0) {
            --value.binary;
            selected.fault = fault::binary;
        } else if (value.wrong_request_id > 0) {
            --value.wrong_request_id;
            selected.fault = fault::wrong_request_id;
        } else if (value.remote_error > 0) {
            --value.remote_error;
            selected.fault = fault::remote_error;
        } else if (value.close_before_response > 0) {
            --value.close_before_response;
            selected.fault = fault::close;
        }
        return selected;
    }

    [[nodiscard]] bool take_close_after_response() {
        std::lock_guard lock(state_mutex_);
        if (close_after_responses_ <= 0) return false;
        --close_after_responses_;
        return true;
    }

    void broadcast(std::string message) {
        std::vector<std::shared_ptr<event_sink>> sinks;
        {
            std::lock_guard lock(events_mutex_);
            sinks = event_sinks_;
        }
        for (const auto& sink : sinks) {
            {
                std::lock_guard lock(sink->mutex);
                if (sink->closed) continue;
                sink->messages.push_back(message);
            }
            sink->cv.notify_one();
        }
    }

    [[nodiscard]] nlohmann::json make_response(
        const nlohmann::json& request,
        const selected_behavior& selected) {
        using namespace livesplit::bridge;
        const std::string method = request.value("method", "");
        const std::uint32_t request_id = request.value("request_id", 0u);
        const std::uint32_t session_id = session_id_.load();
        nlohmann::json response;
        response["session_id"] = session_id;
        response["request_id"] =
            selected.fault == fault::wrong_request_id ? request_id + 1 : request_id;

        if (selected.fault == fault::remote_error) {
            response["error"] = bridge_error{
                .code = selected.error_code,
                .message = selected.error_message,
            };
            return response;
        }

        std::lock_guard lock(state_mutex_);
        if (method == "get_timer_state") {
            response["result"] = timer_;
        } else if (method == "get_run") {
            response["result"] = run_;
        } else if (method == "get_run_icons") {
            run_icons icons;
            icons.segment_icons.resize(run_.segments.size());
            if (icon_bytes_ > 0) {
                icons.game_icon = base64_encode(std::vector<std::uint8_t>(icon_bytes_, 0xAB));
            }
            response["result"] = icons;
        } else if (method == "get_attempt") {
            response["result"] = attempt_;
        } else if (method == "get_context_state") {
            response["result"] = context_;
        } else if (method == "get_completed_count") {
            response["result"] = completed_;
        } else if (method == "timer_operation") {
            update_timer(request.at("params").value("operation", ""));
            response["result"] = nullptr;
        } else if (method == "game_time_operation") {
            update_game_time(request.at("params"));
            response["result"] = nullptr;
        } else {
            response.erase("result");
            response["error"] = bridge_error{
                .code = bridge_error_code::INVALID_REQUEST,
                .message = "unsupported method",
            };
        }
        return response;
    }

    void update_timer(const std::string& operation) {
        if (operation == "start") {
            timer_.phase = livesplit::bridge::timer_phase::RUNNING;
        } else if (operation == "pause") {
            timer_.phase = livesplit::bridge::timer_phase::PAUSED;
        } else if (operation == "resume") {
            timer_.phase = livesplit::bridge::timer_phase::RUNNING;
        } else if (operation == "reset") {
            timer_.phase = livesplit::bridge::timer_phase::NOT_RUNNING;
            timer_.split_index = -1;
        } else if (operation == "split" || operation == "skip") {
            ++timer_.split_index;
        } else if (operation == "undo") {
            --timer_.split_index;
        }
    }

    void update_game_time(const nlohmann::json& params) {
        const std::string operation = params.value("operation", "");
        if (operation == "initialize") {
            timer_.is_game_time_initialized = true;
            timer_.game_time_ticks = 0;
        } else if (operation == "pause") {
            timer_.is_game_time_paused = true;
        } else if (operation == "resume") {
            timer_.is_game_time_paused = false;
        } else if (operation == "set") {
            timer_.game_time_ticks = params.value("ticks", std::int64_t{0});
        }
    }

    static std::string base64_encode(const std::vector<std::uint8_t>& bytes) {
        static constexpr char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string output;
        output.reserve((bytes.size() + 2) / 3 * 4);
        for (std::size_t index = 0; index < bytes.size(); index += 3) {
            const std::uint32_t a = bytes[index];
            const std::uint32_t b = index + 1 < bytes.size() ? bytes[index + 1] : 0;
            const std::uint32_t c = index + 2 < bytes.size() ? bytes[index + 2] : 0;
            const std::uint32_t value = (a << 16) | (b << 8) | c;
            output.push_back(alphabet[(value >> 18) & 0x3F]);
            output.push_back(alphabet[(value >> 12) & 0x3F]);
            output.push_back(index + 1 < bytes.size() ? alphabet[(value >> 6) & 0x3F] : '=');
            output.push_back(index + 2 < bytes.size() ? alphabet[value & 0x3F] : '=');
        }
        return output;
    }

    boost::asio::io_context io_;
    tcp::acceptor acceptor_{io_};
    std::uint16_t port_{0};
    std::atomic<bool> stopped_{false};
    std::thread accept_thread_;

    std::mutex connections_mutex_;
    std::vector<std::shared_ptr<connection>> connections_;
    std::vector<std::thread> connection_threads_;

    mutable std::mutex state_mutex_;
    std::atomic<std::uint32_t> session_id_{42};
    std::size_t icon_bytes_{0};
    int close_after_responses_{0};
    std::map<std::string, behavior> behaviors_;
    std::map<std::string, int> method_counts_;
    std::vector<nlohmann::json> requests_;

    livesplit::bridge::timer_state timer_{
        .phase = livesplit::bridge::timer_phase::NOT_RUNNING,
        .split_index = -1,
        .real_time_ticks = 0,
        .game_time_ticks = std::nullopt,
        .is_game_time_initialized = false,
        .is_game_time_paused = false,
    };
    livesplit::bridge::attempt_state attempt_{
        .attempt_count = 4,
        .segments = {},
    };
    livesplit::bridge::run_state run_{
        .game_name = "Bridge Test Game",
        .category_name = "Any%",
        .offset_ticks = -100,
        .file_path = std::nullopt,
        .layout_path = std::nullopt,
        .metadata = std::nullopt,
        .comparisons = {"Personal Best"},
        .segments = {
            {
                .index = 0,
                .name = "First",
                .comparisons = {},
                .best_segment_time = {},
            },
        },
    };
    livesplit::bridge::context_state context_{
        .current_timing_method = livesplit::bridge::timing_method::REAL_TIME,
        .current_comparison = "Personal Best",
        .custom_variables = {{"host_var", "host-value"}},
    };
    livesplit::bridge::completed_count_value completed_{.completed_count = 9};

    std::mutex events_mutex_;
    std::vector<std::shared_ptr<event_sink>> event_sinks_;
};

}  // namespace test_support
