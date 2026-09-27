#include "gameboy/snes_audio_host.hpp"
#include "gameboy/snes_spc700.hpp"

#include <array>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

struct Event {
    std::uint64_t completed_cycle{};
    std::uint8_t address{};
    std::uint8_t value{};
};

// Development-only bridge. Writes are stamped at instruction completion,
// because the current interpreter does not expose intra-instruction bus cycles.
// MOV dp,#imm writes on its fifth/final cycle. Only that known bus-write
// phase is accepted. The clock fixture can replay its sub-sample volume writes;
// the older whole-sample fixture still rejects off-boundary events.
class Timeline final {
public:
    enum class Error { none, unsupported_opcode, multiple_writes, off_boundary,
                       unknown_write_phase, overflow };

    Timeline(gameboy::SnesApuBus& bus, gameboy::SnesSpc700& cpu,
             const bool allow_subsample = false) noexcept
        : bus_(bus), cpu_(cpu), allow_subsample_(allow_subsample) {
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
        if (result.opcode != 0x8f) return error_ = Error::unknown_write_phase;
        if (!allow_subsample_ && completed_cycles_ % 32 != 0) {
            return error_ = Error::off_boundary;
        }
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
    bool allow_subsample_{};
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

gameboy::SnesApuBus::IplRom subsample_program(const bool echo = false) {
    gameboy::SnesApuBus::IplRom image{};
    std::size_t cursor = 0;
    const auto write = [&](std::uint8_t address, std::uint8_t value) {
        image[cursor++] = 0x8f; image[cursor++] = address;
        image[cursor++] = 0xf2;
        image[cursor++] = 0x8f; image[cursor++] = value;
        image[cursor++] = 0xf3;
    };
    for (unsigned i = 0; i < 8; ++i) image[cursor++] = 0x00;
    write(echo ? 0x2c : 0x0c, 0x7f); // clock 26, before left poll
    for (unsigned event = 0; event < 2; ++event) {
        image[cursor++] = 0xe4; image[cursor++] = 0x20; // MOV A,$20: 3 clocks
        for (unsigned i = 0; i < 10; ++i) image[cursor++] = 0x00;
        write(event == 0 ? (echo ? 0x3c : 0x1c) : (echo ? 0x2c : 0x0c),
              event == 0 ? 0x7f : 0x00);
    }
    return image;
}

gameboy::SnesApuBus::IplRom key_program(const bool key_off,
                                       const bool multi_voice = false) {
    gameboy::SnesApuBus::IplRom image{};
    std::size_t cursor = 0;
    const auto write = [&](std::uint8_t address, std::uint8_t value) {
        image[cursor++] = 0x8f; image[cursor++] = address;
        image[cursor++] = 0xf2;
        image[cursor++] = 0x8f; image[cursor++] = value;
        image[cursor++] = 0xf3;
    };
    for (unsigned i = 0; i < 8; ++i) image[cursor++] = 0x00;
    write(key_off ? 0x5c : 0x4c, multi_voice ? 2 : 1);
    for (unsigned event = 0; event < 2; ++event) {
        image[cursor++] = 0xe4; image[cursor++] = 0x20;
        for (unsigned i = 0; i < 10; ++i) image[cursor++] = 0x00;
        write(multi_voice ? (event == 0 ? 0x5c : 0x4c)
                          : (key_off ? 0x5c : 0x4c),
              multi_voice ? (event == 0 ? 1 : 2) : (event == 0 ? 0 : 1));
    }
    return image;
}

gameboy::SnesApuBus::IplRom voice_register_program() {
    gameboy::SnesApuBus::IplRom image{};
    constexpr std::array<std::array<std::uint8_t, 2>, 3> writes{{
        {{0x00, 0x20}}, {{0x13, 0x1b}}, {{0x07, 0x40}},
    }};
    std::size_t cursor = 0;
    for (unsigned i = 0; i < 8; ++i) image[cursor++] = 0x00;
    for (unsigned event = 0; event < writes.size(); ++event) {
        if (event != 0) {
            image[cursor++] = 0xe4; image[cursor++] = 0x20;
            for (unsigned i = 0; i < 10; ++i) image[cursor++] = 0x00;
        }
        image[cursor++] = 0x8f; image[cursor++] = writes[event][0];
        image[cursor++] = 0xf2;
        image[cursor++] = 0x8f; image[cursor++] = writes[event][1];
        image[cursor++] = 0xf3;
    }
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

bool capture_subsample_and_check(std::array<Event, 3>& captured,
                                 const bool echo = false) {
    gameboy::SnesApuBus bus;
    bus.install_ipl(subsample_program(echo));
    gameboy::SnesSpc700 cpu(bus);
    Timeline timeline(bus, cpu, true);
    // 10 + 13 + 13 instructions, respectively.
    for (unsigned i = 0; i < 36; ++i) {
        if (timeline.step() != Timeline::Error::none) return false;
    }
    const std::array<Event, 3> expected{{
        {26, static_cast<std::uint8_t>(echo ? 0x2c : 0x0c), 0x7f},
        {59, static_cast<std::uint8_t>(echo ? 0x3c : 0x1c), 0x7f},
        {92, static_cast<std::uint8_t>(echo ? 0x2c : 0x0c), 0x00},
    }};
    if (timeline.size() != captured.size()) return false;
    for (std::size_t i = 0; i < captured.size(); ++i) {
        captured[i] = timeline.event(i);
        if (captured[i].completed_cycle != expected[i].completed_cycle ||
            captured[i].address != expected[i].address ||
            captured[i].value != expected[i].value) return false;
    }
    return true;
}

bool capture_key_and_check(std::array<Event, 3>& captured,
                           const bool key_off,
                           const bool multi_voice = false) {
    gameboy::SnesApuBus bus;
    bus.install_ipl(key_program(key_off, multi_voice));
    gameboy::SnesSpc700 cpu(bus);
    Timeline timeline(bus, cpu, true);
    for (unsigned i = 0; i < 36; ++i) {
        if (timeline.step() != Timeline::Error::none) return false;
    }
    const auto address = static_cast<std::uint8_t>(key_off ? 0x5c : 0x4c);
    const std::array<Event, 3> expected = multi_voice
        ? std::array<Event, 3>{{{26, 0x4c, 2}, {59, 0x5c, 1}, {92, 0x4c, 2}}}
        : std::array<Event, 3>{{{26, address, 1}, {59, address, 0},
                                {92, address, 1}}};
    if (timeline.size() != captured.size()) return false;
    for (std::size_t i = 0; i < captured.size(); ++i) {
        captured[i] = timeline.event(i);
        if (captured[i].completed_cycle != expected[i].completed_cycle ||
            captured[i].address != expected[i].address ||
            captured[i].value != expected[i].value) return false;
    }
    return true;
}

bool capture_voice_register_and_check(std::array<Event, 3>& captured) {
    gameboy::SnesApuBus bus;
    bus.install_ipl(voice_register_program());
    gameboy::SnesSpc700 cpu(bus);
    Timeline timeline(bus, cpu, true);
    for (unsigned i = 0; i < 36; ++i) {
        if (timeline.step() != Timeline::Error::none) return false;
    }
    constexpr std::array<Event, 3> expected{{
        {26, 0x00, 0x20}, {59, 0x13, 0x1b}, {92, 0x07, 0x40},
    }};
    if (timeline.size() != captured.size()) return false;
    for (std::size_t i = 0; i < captured.size(); ++i) {
        captured[i] = timeline.event(i);
        if (captured[i].completed_cycle != expected[i].completed_cycle ||
            captured[i].address != expected[i].address ||
            captured[i].value != expected[i].value) return false;
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

bool rejects_unknown_write_phase() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom image{};
    image[0] = 0xe8; image[1] = 0x7f; // MOV A,#$7f
    image[2] = 0x8f; image[3] = 0x0c; image[4] = 0xf2;
    image[5] = 0xc4; image[6] = 0xf3; // MOV $F3,A: phase not modeled
    bus.install_ipl(image);
    gameboy::SnesSpc700 cpu(bus);
    Timeline timeline(bus, cpu, true);
    return timeline.step() == Timeline::Error::none &&
           timeline.step() == Timeline::Error::none &&
           timeline.step() == Timeline::Error::unknown_write_phase;
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

void emit_clock_fixture(const std::array<Event, 3>& events) {
    // Prime an audible looping voice, then replay only SPC700-produced master
    // volume writes. No real ROM or external source is embedded in this test.
    std::cout << "# MOV dp,#imm DSP writes at clocks 26, 59, 92\n"
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
                 "reg 0x07 0x7f\nreg 0x0c 0x00\n"
                 "reg 0x1c 0x00\nreg 0x4c 0x01\n"
                 "clock 2048\n";
    std::uint64_t emitted_clocks = 0;
    for (const auto& event : events) {
        std::cout << "clock " << event.completed_cycle - emitted_clocks << '\n'
                  << "reg " << static_cast<unsigned>(event.address) << ' '
                  << static_cast<unsigned>(event.value) << '\n';
        emitted_clocks = event.completed_cycle;
    }
    std::cout << "clock 256\n";
}

void emit_echo_clock_fixture(const std::array<Event, 3>& events) {
    // Static echo-only input at ESA=$40, EDL=0, FIR tap 7. Echo writes are
    // disabled so the same nonzero stereo word is read every sample.
    std::cout << "# SPC700 echo-output volume writes at clocks 26, 59, 92\n"
                 "ram 0x4000 0x00\nram 0x4001 0x40\n"
                 "ram 0x4002 0x00\nram 0x4003 0x20\n"
                 "reg 0x6c 0x20\nreg 0x6d 0x40\n"
                 "reg 0x7d 0x00\nreg 0x7f 0x40\n"
                 "reg 0x0c 0x00\nreg 0x1c 0x00\n"
                 "reg 0x2c 0x00\nreg 0x3c 0x00\n"
                 "clock 2048\n";
    std::uint64_t emitted_clocks = 0;
    for (const auto& event : events) {
        std::cout << "clock " << event.completed_cycle - emitted_clocks << '\n'
                  << "reg " << static_cast<unsigned>(event.address) << ' '
                  << static_cast<unsigned>(event.value) << '\n';
        emitted_clocks = event.completed_cycle;
    }
    std::cout << "clock 256\n";
}

void emit_key_clock_fixture(const std::array<Event, 3>& events,
                             const bool key_off) {
    std::cout << "# SPC700 KON/KOFF writes at clocks 26, 59, 92\n"
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
                 "reg 0x07 0x7f\nreg 0x0c 0x7f\n"
                 "reg 0x1c 0x7f\nreg 0x5c 0x00\n";
    std::cout << "reg 0x4c " << (key_off ? "0x01" : "0x00") << '\n'
              << "clock 2048\n";
    std::uint64_t emitted_clocks = 0;
    for (const auto& event : events) {
        std::cout << "clock " << event.completed_cycle - emitted_clocks << '\n'
                  << "reg " << static_cast<unsigned>(event.address) << ' '
                  << static_cast<unsigned>(event.value) << '\n';
        emitted_clocks = event.completed_cycle;
    }
    std::cout << "clock 512\n";
}

void emit_multi_key_clock_fixture(const std::array<Event, 3>& events) {
    // Two distinct looping BRR voices. All dynamic writes come from the
    // synthetic SPC700 program; the static setup needs no firmware bytes.
    std::cout << "# SPC700 two-voice DSP writes at clocks 26, 59, 92\n"
                 "ram 0x2800 0x00\nram 0x2801 0x80\n"
                 "ram 0x2802 0x00\nram 0x2803 0x80\n"
                 "ram 0x2804 0x00\nram 0x2805 0x81\n"
                 "ram 0x2806 0x00\nram 0x2807 0x81\n"
                 "ram 0x8000 0x83\nram 0x8001 0x40\n"
                 "ram 0x8002 0x00\nram 0x8003 0x44\n"
                 "ram 0x8004 0x44\nram 0x8005 0x44\n"
                 "ram 0x8006 0x44\nram 0x8007 0x44\n"
                 "ram 0x8008 0x44\n"
                 "ram 0x8100 0x83\nram 0x8101 0x25\n"
                 "ram 0x8102 0x52\nram 0x8103 0x25\n"
                 "ram 0x8104 0x52\nram 0x8105 0x25\n"
                 "ram 0x8106 0x52\nram 0x8107 0x25\n"
                 "ram 0x8108 0x52\n"
                 "reg 0x5d 0x28\nreg 0x6c 0x20\n"
                 "reg 0x00 0x7f\nreg 0x01 0x40\n"
                 "reg 0x02 0x00\nreg 0x03 0x10\n"
                 "reg 0x04 0x00\nreg 0x07 0x7f\n"
                 "reg 0x10 0x38\nreg 0x11 0x7f\n"
                 "reg 0x12 0x00\nreg 0x13 0x12\n"
                 "reg 0x14 0x01\nreg 0x17 0x60\n"
                 "reg 0x0c 0x7f\nreg 0x1c 0x7f\n"
                 "reg 0x5c 0x00\nreg 0x4c 0x03\n"
                 "clock 2048\n";
    std::uint64_t emitted_clocks = 0;
    for (const auto& event : events) {
        std::cout << "clock " << event.completed_cycle - emitted_clocks << '\n'
                  << "reg " << static_cast<unsigned>(event.address) << ' '
                  << static_cast<unsigned>(event.value) << '\n';
        emitted_clocks = event.completed_cycle;
    }
    std::cout << "clock 512\n";
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Fixture consumers compare exact LF-delimited bytes on every platform.
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 2;
#endif
    std::array<Event, 3> events{};
    std::array<Event, 3> subsample_events{};
    std::array<Event, 3> echo_events{};
    std::array<Event, 3> kon_events{};
    std::array<Event, 3> koff_events{};
    std::array<Event, 3> multi_key_events{};
    std::array<Event, 3> voice_register_events{};
    if (!capture_and_check(events) || !rejects_off_boundary() ||
        !capture_subsample_and_check(subsample_events) ||
        !capture_subsample_and_check(echo_events, true) ||
        !capture_key_and_check(kon_events, false) ||
        !capture_key_and_check(koff_events, true) ||
        !capture_key_and_check(multi_key_events, false, true) ||
        !capture_voice_register_and_check(voice_register_events) ||
        !rejects_unsupported_opcode() || !rejects_unknown_write_phase() ||
        !ignores_invalid_dsp_address()) {
        std::cerr << "SPC700 DSP timeline contract failed\n";
        return 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--fixture") {
        emit_fixture(events);
    } else if (argc == 2 && std::string_view(argv[1]) == "--clock-fixture") {
        emit_clock_fixture(subsample_events);
    } else if (argc == 2 && std::string_view(argv[1]) == "--echo-clock-fixture") {
        emit_echo_clock_fixture(echo_events);
    } else if (argc == 2 && std::string_view(argv[1]) == "--kon-clock-fixture") {
        emit_key_clock_fixture(kon_events, false);
    } else if (argc == 2 && std::string_view(argv[1]) == "--koff-clock-fixture") {
        emit_key_clock_fixture(koff_events, true);
    } else if (argc == 2 && std::string_view(argv[1]) == "--multi-key-clock-fixture") {
        emit_multi_key_clock_fixture(multi_key_events);
    } else if (argc == 2 && std::string_view(argv[1]) == "--voice-register-clock-fixture") {
        emit_multi_key_clock_fixture(voice_register_events);
    } else if (argc != 1) {
        std::cerr << "usage: snes_spc700_dsp_timeline_tests "
                     "[--fixture|--clock-fixture|--echo-clock-fixture|"
                     "--kon-clock-fixture|--koff-clock-fixture|"
                     "--multi-key-clock-fixture|--voice-register-clock-fixture]\n";
        return 2;
    }
    return 0;
}
