#pragma once

#include "gameboy/hardware_model.hpp"

#include <cstdint>
#include <stdexcept>

namespace gameboy {

class MemoryBus;
class SaveStateCodec;
class SaveStateCpuCodec;

struct CpuRegisters {
    std::uint8_t a{};
    std::uint8_t f{};
    std::uint8_t b{};
    std::uint8_t c{};
    std::uint8_t d{};
    std::uint8_t e{};
    std::uint8_t h{};
    std::uint8_t l{};
    std::uint16_t sp{};
    std::uint16_t pc{};
};

class UnsupportedOpcode final : public std::runtime_error {
public:
    UnsupportedOpcode(std::uint8_t opcode, std::uint16_t address);
};

class Cpu {
public:
    Cpu();

    void reset(bool cgb_mode = false) noexcept;
    void reset(HardwareModel model) noexcept;
    void load_registers(CpuRegisters registers) noexcept;
    [[nodiscard]] unsigned step(MemoryBus& bus);
    [[nodiscard]] const CpuRegisters& registers() const noexcept;
    [[nodiscard]] bool halted() const noexcept;
    [[nodiscard]] bool stopped() const noexcept;
    [[nodiscard]] bool interrupts_enabled() const noexcept;
    [[nodiscard]] std::uint64_t total_cycles() const noexcept;

private:
    friend class SaveStateCodec;
    friend class SaveStateCpuCodec;

    static constexpr std::uint8_t zero_flag = 0x80;
    static constexpr std::uint8_t subtract_flag = 0x40;
    static constexpr std::uint8_t half_carry_flag = 0x20;
    static constexpr std::uint8_t carry_flag = 0x10;

    [[nodiscard]] unsigned execute_instruction(MemoryBus& bus);
    [[nodiscard]] unsigned execute_cb(MemoryBus& bus);
    [[nodiscard]] std::uint8_t pending_interrupts(const MemoryBus& bus) const noexcept;
    [[nodiscard]] unsigned service_interrupt(MemoryBus& bus,
                                             std::uint8_t pending) noexcept;
    [[nodiscard]] bool condition(unsigned index) const noexcept;
    void idle(MemoryBus& bus, unsigned cycles) noexcept;
    [[nodiscard]] std::uint8_t read8(MemoryBus& bus,
                                     std::uint16_t address) noexcept;
    void write8(MemoryBus& bus, std::uint16_t address,
                std::uint8_t value) noexcept;
    void push(MemoryBus& bus, std::uint16_t value) noexcept;
    [[nodiscard]] std::uint16_t pop(MemoryBus& bus) noexcept;
    [[nodiscard]] std::uint8_t fetch8(MemoryBus& bus) noexcept;
    [[nodiscard]] std::uint16_t fetch16(MemoryBus& bus) noexcept;
    [[nodiscard]] std::uint8_t read_register(unsigned index,
                                             MemoryBus& bus) noexcept;
    void write_register(unsigned index, std::uint8_t value,
                        MemoryBus& bus) noexcept;
    [[nodiscard]] std::uint16_t bc() const noexcept;
    [[nodiscard]] std::uint16_t de() const noexcept;
    [[nodiscard]] std::uint16_t hl() const noexcept;
    [[nodiscard]] std::uint16_t register_pair(unsigned index) const noexcept;
    [[nodiscard]] std::uint16_t stack_register_pair(unsigned index) const noexcept;
    void set_bc(std::uint16_t value) noexcept;
    void set_de(std::uint16_t value) noexcept;
    void set_hl(std::uint16_t value) noexcept;
    void set_register_pair(unsigned index, std::uint16_t value) noexcept;
    void set_stack_register_pair(unsigned index, std::uint16_t value) noexcept;
    [[nodiscard]] bool flag(std::uint8_t mask) const noexcept;
    void add(std::uint8_t value, bool with_carry) noexcept;
    void subtract(std::uint8_t value, bool with_carry) noexcept;
    void compare(std::uint8_t value) noexcept;
    [[nodiscard]] std::uint8_t increment(std::uint8_t value) noexcept;
    [[nodiscard]] std::uint8_t decrement(std::uint8_t value) noexcept;
    void decimal_adjust() noexcept;

    CpuRegisters registers_{};
    bool ime_{};
    bool halted_{};
    bool stopped_{};
    bool halt_bug_{};
    unsigned ime_enable_delay_{};
    unsigned step_cycles_{};
    std::uint64_t total_cycles_{};
};

} // namespace gameboy
