#include "gameboy/link_session.hpp"

#include "gameboy/emulator.hpp"

namespace gameboy {

void LinkSession::start(Emulator& first, Emulator& second) noexcept {
    stop();
    state_ = State::starting;
    first_ = &first;
    second_ = &second;
    transport_->attach(first.bus().serial_port(), second.bus().serial_port());
    stalled_cycles_ = 0;
    progress_marker_ = transfers_completed();
    state_ = State::connected;
}

void LinkSession::stop() noexcept {
    transport_->detach();
    first_ = nullptr;
    second_ = nullptr;
    state_ = State::disconnected;
    stalled_cycles_ = 0;
    progress_marker_ = 0;
}

void LinkSession::retry() noexcept {
    if (first_ == nullptr || second_ == nullptr) return;
    transport_->detach();
    first_->bus().serial_port().reset_link();
    second_->bus().serial_port().reset_link();
    transport_->attach(first_->bus().serial_port(),
                       second_->bus().serial_port());
    stalled_cycles_ = 0;
    progress_marker_ = transfers_completed();
    state_ = State::connected;
}

LinkSession::State LinkSession::state() const noexcept {
    if (state_ != State::connected && state_ != State::transferring) {
        return state_;
    }
    if ((first_ != nullptr &&
         first_->bus().serial_port().transfer_active()) ||
        (second_ != nullptr &&
         second_->bus().serial_port().transfer_active())) {
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
    return first_->bus().serial_port().transfers_completed() +
           second_->bus().serial_port().transfers_completed();
}

void LinkSession::advance(const unsigned target_cycles) {
    if (!active() || target_cycles == 0) return;

    unsigned first_cycles = 0;
    unsigned second_cycles = 0;
    while (first_cycles < target_cycles || second_cycles < target_cycles) {
        if (first_cycles <= second_cycles && first_cycles < target_cycles) {
            first_cycles += first_->step();
        } else if (second_cycles < target_cycles) {
            second_cycles += second_->step();
        } else if (first_cycles < target_cycles) {
            first_cycles += first_->step();
        } else {
            break;
        }
    }

    const auto progress =
        transfers_completed() +
        static_cast<std::uint64_t>(first_->bus().serial_port().bits_shifted()) +
        static_cast<std::uint64_t>(second_->bus().serial_port().bits_shifted());
    const auto transferring =
        first_->bus().serial_port().transfer_active() ||
        second_->bus().serial_port().transfer_active();
    if (!transferring) {
        stalled_cycles_ = 0;
    } else if (progress == progress_marker_) {
        stalled_cycles_ += target_cycles;
    } else {
        stalled_cycles_ = 0;
    }
    progress_marker_ = progress;
    if (stalled_cycles_ >= timeout_cycles_) state_ = State::timed_out;
}

void LinkSession::mark_timeout() noexcept {
    if (first_ != nullptr && second_ != nullptr) state_ = State::timed_out;
}

} // namespace gameboy
