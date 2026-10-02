#pragma once

#include <cstdint>

namespace gameboy {

class SnesApuBus;

// Incremental SPC700 interpreter for the IPL and bounded driver probes.
// Unsupported opcodes
// stop explicitly; they are never treated as NOPs or as synthesized audio.
class SnesSpc700 final {
public:
    struct Registers {
        std::uint16_t pc{0xFFC0};
        std::uint8_t a{};
        std::uint8_t x{};
        std::uint8_t y{};
        std::uint8_t sp{};
        std::uint8_t psw{};
    };

    struct StepResult {
        unsigned cycles{};
        std::uint8_t opcode{};
        bool supported{};
    };

    explicit SnesSpc700(SnesApuBus& bus) noexcept : bus_(bus) {}
    void reset() noexcept { registers_ = {}; cycles_ = 0; }
    [[nodiscard]] const Registers& registers() const noexcept { return registers_; }
    [[nodiscard]] std::uint64_t cycles() const noexcept { return cycles_; }
    // Opt-in diagnostic bus execution. Each read/write/idle advances timers
    // separately; the legacy instruction-granular path remains the default.
    void set_cycle_bus_enabled(bool enabled) noexcept { cycle_bus_ = enabled; }
    // T: completed clock, before the access; R/W: accepted access; I: idle.
    // Observer installation alone does not opt into cycle-level execution.
    using BusCycleObserver = void (*)(void*, std::uint64_t, char,
                                      std::uint16_t, std::uint8_t) noexcept;
    void set_bus_cycle_observer(BusCycleObserver observer,
                                void* context = nullptr) noexcept {
        bus_observer_ = observer;
        bus_context_ = context;
    }
    // Optional write-boundary diagnostic. Called before/after the accepted
    // bus write, at its instruction-relative completed cycle (not instruction
    // end). A zero cycle means an unclassified write; consumers must reject it.
    // This observer only reports writes. Reads/timers remain instruction-
    // granular unless cycle-level bus execution is explicitly enabled.
    using WriteCycleObserver = void (*)(void*, std::uint64_t, std::uint8_t,
                                        std::uint16_t, std::uint8_t, bool) noexcept;
    void set_write_cycle_observer(WriteCycleObserver observer,
                                  void* context = nullptr) noexcept {
        write_observer_ = observer;
        write_context_ = context;
    }
    [[nodiscard]] StepResult step() noexcept;

private:
    [[nodiscard]] std::uint8_t fetch() noexcept;
    [[nodiscard]] std::uint8_t read_memory(std::uint16_t address) noexcept;
    void idle_cycle() noexcept;
    void clock_bus() noexcept;
    void begin_bus_instruction() noexcept;
    [[nodiscard]] std::uint16_t direct_address(std::uint8_t offset) const noexcept;
    [[nodiscard]] std::uint8_t read_direct(std::uint8_t offset) noexcept;
    void write_direct(std::uint8_t offset, std::uint8_t value) noexcept;
    void write_memory(std::uint16_t address, std::uint8_t value) noexcept;
    [[nodiscard]] unsigned write_cycle_offset() const noexcept;
    void set_nz8(std::uint8_t value) noexcept;
    void set_nz16(std::uint16_t value) noexcept;
    void compare(std::uint8_t lhs, std::uint8_t rhs) noexcept;
    void add_with_carry(std::uint8_t rhs) noexcept;
    void subtract_with_carry(std::uint8_t rhs) noexcept;
    [[nodiscard]] unsigned branch(bool take) noexcept;
    void push(std::uint8_t value) noexcept;
    [[nodiscard]] std::uint8_t pop() noexcept;

    SnesApuBus& bus_;
    Registers registers_{};
    std::uint64_t cycles_{};
    std::uint8_t opcode_{};
    unsigned write_index_{};
    WriteCycleObserver write_observer_{};
    void* write_context_{};
    bool cycle_bus_{};
    unsigned instruction_cycle_{};
    BusCycleObserver bus_observer_{};
    void* bus_context_{};
};

} // namespace gameboy
