#include "livesplit/bridge/client.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

#include "detail/client_impl.hpp"

namespace livesplit::bridge {

client::client(client_options options)
    : impl_(std::make_shared<detail::client_impl>(std::move(options))) {
    impl_->start();
}

client::~client() {
    close();
}

client::client(client&& other) noexcept : impl_(std::move(other.impl_)) {}

client& client::operator=(client&& other) noexcept {
    if (this != &other) {
        close();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

void client::close() noexcept {
    if (impl_) impl_->close();
}

connection_state client::state() const {
    if (!impl_) return connection_state::closed;
    return impl_->state();
}

bool client::is_ready() const {
    return state() == connection_state::ready;
}

std::uint32_t client::session_id() const {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->session_id();
}

std::uint64_t client::dropped_event_count() const {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->dropped_event_count();
}

void client::wait_until_ready(std::chrono::milliseconds timeout) const {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    (void)impl_->wait_until_ready(timeout);
}

snapshot client::current_snapshot() const {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->current_snapshot();
}

snapshot client::synchronize() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->synchronize();
}

std::future<snapshot> client::synchronize_async() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->synchronize_async();
}

timer_state client::get_timer_state() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->get_timer_state();
}

std::future<timer_state> client::get_timer_state_async() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->get_timer_state_async();
}

run_state client::get_run() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->get_run();
}

std::future<run_state> client::get_run_async() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->get_run_async();
}

run_icons client::get_run_icons() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->get_run_icons();
}

std::future<run_icons> client::get_run_icons_async() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->get_run_icons_async();
}

attempt_state client::get_attempt() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->get_attempt();
}

std::future<attempt_state> client::get_attempt_async() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->get_attempt_async();
}

context_state client::get_context_state() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->get_context_state();
}

std::future<context_state> client::get_context_state_async() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->get_context_state_async();
}

completed_count_value client::get_completed_count() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->get_completed_count();
}

std::future<completed_count_value> client::get_completed_count_async() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->get_completed_count_async();
}

void client::start() {
    start_async().get();
}

std::future<void> client::start_async() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->timer_control_async(timer_operation::START);
}

void client::split() {
    split_async().get();
}

std::future<void> client::split_async() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->timer_control_async(timer_operation::SPLIT);
}

void client::skip() {
    skip_async().get();
}

std::future<void> client::skip_async() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->timer_control_async(timer_operation::SKIP);
}

void client::undo() {
    undo_async().get();
}

std::future<void> client::undo_async() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->timer_control_async(timer_operation::UNDO);
}

void client::reset() {
    reset_async().get();
}

std::future<void> client::reset_async() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->timer_control_async(timer_operation::RESET);
}

void client::pause() {
    pause_async().get();
}

std::future<void> client::pause_async() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->timer_control_async(timer_operation::PAUSE);
}

void client::resume() {
    resume_async().get();
}

std::future<void> client::resume_async() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->timer_control_async(timer_operation::RESUME);
}

void client::initialize_game_time() {
    initialize_game_time_async().get();
}

std::future<void> client::initialize_game_time_async() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->game_control_async(game_control_operation::INITIALIZE);
}

void client::set_game_time_ticks(std::int64_t ticks) {
    set_game_time_ticks_async(ticks).get();
}

std::future<void> client::set_game_time_ticks_async(std::int64_t ticks) {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->set_game_time_async(ticks);
}

void client::pause_game_time() {
    pause_game_time_async().get();
}

std::future<void> client::pause_game_time_async() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->game_control_async(game_control_operation::PAUSE);
}

void client::resume_game_time() {
    resume_game_time_async().get();
}

std::future<void> client::resume_game_time_async() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->game_control_async(game_control_operation::RESUME);
}
std::optional<bridge_event> client::next_event(std::chrono::milliseconds timeout) {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->next_event(timeout);
}

bridge_event client::wait_for_event() {
    if (!impl_) throw error(error_kind::closed, "LiveSplit.Bridge client is closed");
    return impl_->wait_for_event();
}

std::optional<bridge_event> client::try_next_event() {
    return next_event(std::chrono::milliseconds::zero());
}

}  // namespace livesplit::bridge
