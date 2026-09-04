#include "gbb/core_runtime.hpp"
#include "gbb/link_scheduler.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class ScriptedCore final : public gbb::EmulatorCore {
public:
    const gbb::CoreDescriptor& descriptor() const noexcept override {
        static const gbb::CoreDescriptor descriptor{
            "test", "Scripted test core", gbb::SystemId::game_boy,
            160, 144, 59.7, 4194304.0, 70224, 44100, 2, nullptr, 0,
            gbb::CoreCapability::none};
        return descriptor;
    }
    void reset() noexcept override { ready_ = false; steps_ = 0; }
    unsigned step_instruction() override {
        ++steps_;
        if (steps_ == 2) ready_ = true;
        return 4;
    }
    bool frame_ready() const noexcept override { return ready_; }
    void consume_frame() noexcept override { ready_ = false; }
    gbb::VideoFrameView video_frame() const noexcept override { return {}; }
    std::vector<std::int16_t> take_audio_samples() override { return {}; }
    void set_input(gbb::InputId, bool) noexcept override {}
    std::vector<std::uint8_t> save_state() const override { return {}; }
    void load_state(const std::vector<std::uint8_t>&) override {}
    std::uint64_t rom_fingerprint() const noexcept override { return 0; }
    void flush_persistent_data() override {}
    bool has_persistent_data(gbb::PersistentDataKind) const noexcept override {
        return false;
    }
    std::vector<std::uint8_t> export_persistent_data(
        gbb::PersistentDataKind) const override { return {}; }
    void import_persistent_data(
        gbb::PersistentDataKind,
        const std::vector<std::uint8_t>&) override {}

private:
    unsigned steps_{};
    bool ready_{};
};

struct SchedulerProbe {
    unsigned calls{};
    unsigned cost{};
};

unsigned scheduler_step(void* context) {
    auto& probe = *static_cast<SchedulerProbe*>(context);
    ++probe.calls;
    return probe.cost;
}

} // namespace

int main() {
    ScriptedCore core;
    const auto no_work = gbb::advance_to_frame(core, 0);
    check(no_work.cycles == 0 && !no_work.frame_ready,
          "bounded core stepping treats a zero budget as a no-op");

    const auto advanced = gbb::advance_to_frame(core, 7);
    check(advanced.cycles == 8 && advanced.frame_ready && core.frame_ready(),
          "bounded core stepping stops at a frame and reports indivisible cycles");
    core.consume_frame();
    check(!core.frame_ready(),
          "bounded core stepping leaves frame consumption to the caller");

    SchedulerProbe first{0, 4};
    SchedulerProbe second{0, 8};
    const auto result = gbb::advance_balanced(
        &first, scheduler_step, &second, scheduler_step, 20);
    const auto difference = result.first_cycles > result.second_cycles
                                ? result.first_cycles - result.second_cycles
                                : result.second_cycles - result.first_cycles;
    check(result.first_cycles >= 20 && result.second_cycles >= 20 &&
              difference <= 8,
          "balanced link scheduling keeps two callback timelines close");
    check(first.calls != 0 && second.calls != 0,
          "balanced link scheduling advances both participants");

    bool invalid_rejected = false;
    try {
        static_cast<void>(gbb::advance_balanced(nullptr, scheduler_step,
                                                &second, scheduler_step, 1));
    } catch (const std::invalid_argument&) {
        invalid_rejected = true;
    }
    check(invalid_rejected,
          "balanced link scheduling rejects incomplete participant callbacks");

    gbb::LinkWatchdog watchdog(10);
    watchdog.reset(7);
    watchdog.observe(4, true, 7);
    check(!watchdog.timed_out() && watchdog.stalled_cycles() == 4,
          "link watchdog accumulates active cycles without progress");
    watchdog.observe(6, true, 7);
    check(watchdog.timed_out() && watchdog.stalled_cycles() == 10,
          "link watchdog reports a timeout at its configured threshold");
    watchdog.reset(7);
    watchdog.observe(100, false, 7);
    check(!watchdog.timed_out() && watchdog.stalled_cycles() == 0,
          "link watchdog clears stalls while a transfer is inactive");
    watchdog.observe(3, true, 8);
    check(!watchdog.timed_out() && watchdog.stalled_cycles() == 0,
          "link watchdog treats progress as a stall reset");
    watchdog.mark_timeout();
    check(watchdog.timed_out(),
          "link watchdog supports explicit guest-level timeout marking");

    return failures == 0 ? 0 : 1;
}
