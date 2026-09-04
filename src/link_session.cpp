#include "gameboy/link_session.hpp"

#include "gbb/link_scheduler.hpp"
#include "gbb/log.hpp"

#include <algorithm>
#include <atomic>

namespace gameboy {
namespace {

std::atomic<std::uint64_t> next_diagnostic_session{1};

unsigned step_endpoint(void* context) {
    return static_cast<LinkEndpoint*>(context)->step();
}

} // namespace

void LinkSession::start(LinkEndpoint& first, LinkEndpoint& second) noexcept {
    stop();
    state_ = State::starting;
    first_ = &first;
    second_ = &second;
    transport_->attach(first.serial_port(), second.serial_port());
    watchdog_.reset(transfers_completed());
    diagnostic_session_ =
        next_diagnostic_session.fetch_add(1, std::memory_order_relaxed);
    state_ = State::connected;
    gbb::Logger::instance().write(gbb::LogLevel::info,
                                  gbb::LogCategory::link,
                                  "local link session started",
                                  {diagnostic_session_, 0, 0});
}

void LinkSession::stop() noexcept {
    const auto was_attached = first_ != nullptr || second_ != nullptr;
    const auto diagnostic_session = diagnostic_session_;
    transport_->detach();
    first_ = nullptr;
    second_ = nullptr;
    state_ = State::disconnected;
    watchdog_.reset();
    diagnostic_session_ = 0;
    if (was_attached) {
        gbb::Logger::instance().write(gbb::LogLevel::info,
                                      gbb::LogCategory::link,
                                      "local link session stopped",
                                      {diagnostic_session, 0, 0});
    }
}

void LinkSession::retry() noexcept {
    if (first_ == nullptr || second_ == nullptr) return;
    transport_->detach();
    first_->serial_port().reset_link();
    second_->serial_port().reset_link();
    transport_->attach(first_->serial_port(), second_->serial_port());
    watchdog_.reset(transfers_completed());
    state_ = State::connected;
    gbb::Logger::instance().write(gbb::LogLevel::info,
                                  gbb::LogCategory::link,
                                  "local link session retried",
                                  {diagnostic_session_, 0, 0});
}

LinkSession::State LinkSession::state() const noexcept {
    if (state_ != State::connected && state_ != State::transferring) {
        return state_;
    }
    if ((first_ != nullptr &&
         first_->serial_port().transfer_active()) ||
        (second_ != nullptr &&
         second_->serial_port().transfer_active())) {
        return State::transferring;
    }
    return State::connected;
}

bool LinkSession::active() const noexcept {
    const auto current = state();
    return current == State::connected || current == State::transferring;
}

std::uint64_t LinkSession::transfers_completed() const noexcept {
    if (first_ == nullptr || second_ == nullptr) return 0;
    return first_->serial_port().transfers_completed() +
           second_->serial_port().transfers_completed();
}

void LinkSession::advance(const unsigned target_cycles) {
    if (!active() || target_cycles == 0) return;

    const auto diagnostic_cycles = std::max(first_->emulated_cycles(),
                                            second_->emulated_cycles());
    gbb::LogContextScope diagnostic_context{
        {diagnostic_session_, 0, diagnostic_cycles}};
    static_cast<void>(gbb::advance_balanced(
        first_, step_endpoint, second_, step_endpoint, target_cycles));

    const auto progress =
        transfers_completed() +
        static_cast<std::uint64_t>(first_->serial_port().bits_shifted()) +
        static_cast<std::uint64_t>(second_->serial_port().bits_shifted());
    const auto transferring =
        first_->serial_port().transfer_active() ||
        second_->serial_port().transfer_active();
    watchdog_.observe(target_cycles, transferring, progress);
    if (watchdog_.timed_out() && state_ != State::timed_out) {
        state_ = State::timed_out;
        gbb::Logger::instance().write(
            gbb::LogLevel::warning, gbb::LogCategory::link,
            "local link session timed out",
            {diagnostic_session_, 0, watchdog_.stalled_cycles()});
    }
}

void LinkSession::mark_timeout() noexcept {
    if (first_ != nullptr && second_ != nullptr &&
        state_ != State::timed_out) {
        watchdog_.mark_timeout();
        state_ = State::timed_out;
        gbb::Logger::instance().write(gbb::LogLevel::warning,
                                      gbb::LogCategory::link,
                                      "local link session marked timed out",
                                      {diagnostic_session_, 0, 0});
    }
}

} // namespace gameboy
