#include "gameboy/snes_audio_host.hpp"
#include "gameboy/snes_spc700.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

struct Event {
    std::uint64_t completed_cycle{};
    std::uint8_t address{};
    std::uint8_t value{};
};

// Development-only bridge. Writes are stamped at instruction completion,
// because the current interpreter does not expose intra-instruction bus cycles.
// Only sample-aligned, single-write instructions can be replayed faithfully by
// the 32-clock fixture protocol. Everything else fails closed.
class Timeline final {
public:
    enum class Error { none, unsupported_opcode, multiple_writes, off_boundary,
                       overflow };

    Timeline(gameboy::SnesApuBus& bus, gameboy::SnesSpc700& cpu) noexcept
        : bus_(bus), cpu_(cpu) {
        bus_.set_dsp_write_observer(&Timeline::observe, this);
    }
    ~Timeline() { bus_.set_dsp_write_observer(nullptr); }
    Timeline(const Timeline&) = delete;
    Timeline& operator=(const Timeline&) = delete;

    Error step() noexcept {
        if (error_ != Error::none) return error_;
        pending_count_ = 0;
        const auto result = cpu_.step();
        if (!result.supported || result.cycles == 0) {
            return error_ = Error::unsupported_opcode;
        }
        completed_cycles_ += result.cycles;
        if (pending_count_ > 1) return error_ = Error::multiple_writes;
        if (pending_count_ == 0) return Error::none;
        if (completed_cycles_ % 32 != 0) return error_ = Error::off_boundary;
        if (count_ == events_.size()) return error_ = Error::overflow;
        events_[count_++] = {completed_cycles_, pending_address_, pending_value_};
        return Error::none;
    }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] Event event(const std::size_t index) const noexcept {
        return events_[index];
    }

private:
    static void observe(void* context, const std::uint8_t address,
                        const std::uint8_t value) noexcept {
        auto& self = *static_cast<Timeline*>(context);
        ++self.pending_count_;
        self.pending_address_ = address;
        self.pending_value_ = value;
    }

    gameboy::SnesApuBus& bus_;
    gameboy::SnesSpc700& cpu_;
    std::array<Event, 16> events_{};
    std::size_t count_{};
    std::uint64_t completed_cycles_{};
    unsigned pending_count_{};
    std::uint8_t pending_address_{};
    std::uint8_t pending_value_{};
    Error error_{Error::none};
};

// Each slot takes exactly 32 SPC clocks: 11 NOPs + two 5-cycle MOV dp,#imm.
// The data-port write is the last instruction in each slot. No firmware or
// copyrighted program bytes are embedded here.
gameboy::SnesApuBus::IplRom synthetic_program() {
    gameboy::SnesApuBus::IplRom image{};
    constexpr std::array<std::array<std::uint8_t, 2>, 3> writes{{
        {{0x0c, 0x7f}}, // master left volume
        {{0x1c, 0x7f}}, // master right volume
        {{0x4c, 0x01}}, // voice 0 key-on
    }};
    std::size_t cursor = 0;
    for (const auto& write : writes) {
        for (unsigned i = 0; i < 11; ++i) image[cursor++] = 0x00;
        image[cursor++] = 0x8f;
        image[cursor++] = write[0];
        image[cursor++] = 0xf2;
        image[cursor++] = 0x8f;
        image[cursor++] = write[1];
        image[cursor++] = 0xf3;
    }
    // Unused tail is NOP; execute exactly the three slots.
    return image;
}

bool capture_and_check(std::array<Event, 3>& captured) {
    gameboy::SnesApuBus bus;
    bus.install_ipl(synthetic_program());
    gameboy::SnesSpc700 cpu(bus);
    Timeline timeline(bus, cpu);
    for (unsigned i = 0; i < 39; ++i) {
        if (timeline.step() != Timeline::Error::none) return false;
    }
    if (timeline.size() != captured.size()) return false;
    constexpr std::array<Event, 3> expected{{
        {32, 0x0c, 0x7f}, {64, 0x1c, 0x7f}, {96, 0x4c, 0x01},
    }};
    for (std::size_t i = 0; i < captured.size(); ++i) {
        captured[i] = timeline.event(i);
        if (captured[i].completed_cycle != expected[i].completed_cycle ||
            captured[i].address != expected[i].address ||
            captured[i].value != expected[i].value ||
            bus.dsp_register(captured[i].address) != captured[i].value) return false;
    }
    return true;
}

bool rejects_off_boundary() {
    gameboy::SnesApuBus bus;
    auto image = synthetic_program();
    image[0] = 0x8f; image[1] = 0x0c; image[2] = 0xf2;
    image[3] = 0x8f; image[4] = 0x7f; image[5] = 0xf3;
    bus.install_ipl(image);
    gameboy::SnesSpc700 cpu(bus);
    Timeline timeline(bus, cpu);
    return timeline.step() == Timeline::Error::none &&
           timeline.step() == Timeline::Error::off_boundary;
}

bool rejects_unsupported_opcode() {
    gameboy::SnesApuBus bus;
    auto image = synthetic_program();
    image[0] = 0xff;
    bus.install_ipl(image);
    gameboy::SnesSpc700 cpu(bus);
    Timeline timeline(bus, cpu);
    return timeline.step() == Timeline::Error::unsupported_opcode;
}

bool ignores_invalid_dsp_address() {
    gameboy::SnesApuBus bus;
    bus.spc_write(0xf2, 0x8c);
    unsigned notifications = 0;
    bus.set_dsp_write_observer([](void* context, std::uint8_t,
                                  std::uint8_t) noexcept {
        ++*static_cast<unsigned*>(context);
    }, &notifications);
    bus.spc_write(0xf3, 0x7f);
    if (notifications != 0 || bus.dsp_register(0x0c) != 0) return false;
    bus.spc_write(0xf2, 0x0c);
    bus.spc_write(0xf3, 0x55);
    if (notifications != 1 || bus.dsp_register(0x0c) != 0x55) return false;
    bus.reset();
    bus.spc_write(0xf3, 0x7f);
    return notifications == 1; // reset detaches the observer
}

void emit_fixture(const std::array<Event, 3>& events) {
    // Static RAM/voice setup is independent of the CPU program. Only the
    // three observed DSP writes below are generated by SPC700 execution.
    std::cout << "# synthetic SPC700 writes stamped at instruction completion\n"
                 "ram 0x2800 0x00\nram 0x2801 0x80\n"
                 "ram 0x2802 0x00\nram 0x2803 0x80\n"
                 "ram 0x8000 0x83\nram 0x8001 0x40\n"
                 "ram 0x8002 0x00\nram 0x8003 0x44\n"
                 "ram 0x8004 0x44\nram 0x8005 0x44\n"
                 "ram 0x8006 0x44\nram 0x8007 0x44\n"
                 "ram 0x8008 0x44\n"
                 "reg 0x5d 0x28\nreg 0x6c 0x20\n"
                 "reg 0x00 0x7f\nreg 0x01 0x7f\n"
                 "reg 0x02 0x00\nreg 0x03 0x10\n"
                 "reg 0x04 0x00\nreg 0x05 0x00\n"
                 "reg 0x07 0x7f\n";
    std::uint64_t emitted_samples = 0;
    for (const auto& event : events) {
        const auto target_samples = event.completed_cycle / 32;
        std::cout << "step " << target_samples - emitted_samples << '\n'
                  << "reg " << static_cast<unsigned>(event.address) << ' '
                  << static_cast<unsigned>(event.value) << '\n';
        emitted_samples = target_samples;
    }
    std::cout << "step " << 128 - emitted_samples << '\n';
}

} // namespace

int main(int argc, char** argv) {
    std::array<Event, 3> events{};
    if (!capture_and_check(events) || !rejects_off_boundary() ||
        !rejects_unsupported_opcode() || !ignores_invalid_dsp_address()) {
        std::cerr << "SPC700 DSP timeline contract failed\n";
        return 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--fixture") {
        emit_fixture(events);
    } else if (argc != 1) {
        std::cerr << "usage: snes_spc700_dsp_timeline_tests [--fixture]\n";
        return 2;
    }
    return 0;
}
