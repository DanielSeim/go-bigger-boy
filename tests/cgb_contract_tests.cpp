#include "gameboy/emulator.hpp"
#include "gameboy/memory_bus.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::uint16_t program_address = 0x0100;
int failures = 0;

void check(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> test_rom(
    const std::vector<std::uint8_t>& program = {}) {
    std::vector<std::uint8_t> rom(0x8000, 0);
    const std::string title = "CORE TEST";
    for (std::size_t i = 0; i < title.size(); ++i) {
        rom[0x134 + i] = static_cast<std::uint8_t>(title[i]);
    }
    for (std::size_t i = 0; i < program.size(); ++i) {
        rom[program_address + i] = program[i];
    }
    return rom;
}

std::vector<std::uint8_t> cgb_test_rom(
    const std::vector<std::uint8_t>& program = {}) {
    auto rom = test_rom(program);
    rom[0x143] = 0x80;
    return rom;
}

// Independently authored standard HRAM DMA delay: 40 DEC/JR iterations.
void test_ff74_native_and_compatibility() {
    for (const bool color : {false, true}) {
        gameboy::MemoryBus bus{gameboy::Cartridge{color ? cgb_test_rom() : test_rom()}};
        bus.initialize_post_boot(gameboy::HardwareModel::cgb_e);
        check(bus.read8(0xFF74) == (color ? 0 : 255), "FF74 reset value depends on CGB mode");
        bus.write8(0xFF74, 0x5A);
        check(bus.read8(0xFF74) == (color ? 0x5A : 255), "FF74 is writable only in CGB mode");
    }
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.write8(0xFF74, 0x5A);
    check(bus.read8(0xFF74) == 255, "FF74 is unmapped on monochrome hardware");
}

void test_dma_instruction_bus_and_stack() {
    for (const auto model : {gameboy::HardwareModel::dmg, gameboy::HardwareModel::cgb_e}) {
        auto image = test_rom();
        image[0x143] = model == gameboy::HardwareModel::cgb_e ? 0x80 : 0;
        // Source stream: LD A,42; PUSH AF; three trap bytes consumed while
        // PUSH runs; INC A. DMA advances on internal and write cycles too.
        const std::vector<std::uint8_t> stream{0x3E, 0x42, 0xF5, 0xFF, 0xFF, 0xFF, 0x3C};
        std::copy(stream.begin(), stream.end(), image.begin());
        gameboy::MemoryBus bus{gameboy::Cartridge{image}};
        bus.initialize_post_boot(model);
        bus.write8(0xC0FE, 0x99); bus.write8(0xC0FF, 0x99);
        gameboy::Cpu cpu;
        auto registers = cpu.registers();
        registers.pc = 0x100; registers.sp = 0xC100;
        cpu.load_registers(registers);
        bus.write8(0xFF46, 0);
        bus.tick(4);
        check(cpu.step(bus) == 8 && cpu.registers().a == 0x42,
              "instruction and operand fetches see successive DMA source bytes");
        check(cpu.step(bus) == 16 && cpu.registers().sp == 0xC0FE,
              "DMA-driven PUSH consumes its internal and write cycles");
        check(cpu.step(bus) == 4 && cpu.registers().a == 0x43,
              "multi-cycle instruction skips DMA bytes instead of executing traps");
        check(bus.read8(0xC0FF) == (model == gameboy::HardwareModel::dmg ? 0x99 : 0x42),
              "ROM-source DMA blocks DMG WRAM writes but permits CGB stack writes");
    }
}

void test_hram_dma_return() {
    for (const auto model : {gameboy::HardwareModel::dmg, gameboy::HardwareModel::sgb,
                             gameboy::HardwareModel::sgb2, gameboy::HardwareModel::cgb_e}) {
        for (const bool fast : {false, true}) {
            if (fast && model != gameboy::HardwareModel::cgb_e) continue;
            auto rom = cgb_test_rom({0xCD, 0x80, 0xFF}); // CALL FF80; return to 0103
            gameboy::Emulator emulator{gameboy::Cartridge{std::move(rom)}, model};
            auto& bus = emulator.bus();
            if (fast) {
                bus.write8(0xFF4D, 1);
                // Execute STOP in HRAM so ROM contents and return site stay fixed.
                bus.write8(0xFF90, 0x10); bus.write8(0xFF91, 0);
                auto r = emulator.cpu().registers(); r.pc = 0xFF90;
                emulator.set_cpu_registers(r);
                static_cast<void>(emulator.step());
                check(bus.double_speed(), "DMA return fixture enters double speed");
                while (emulator.cpu().debug_speed_switch_cycles_remaining())
                    static_cast<void>(emulator.step());
                static_cast<void>(emulator.step()); // Resume through FF92 NOP.
            }
            bus.write8(0xFF46, 0xC0);
            bus.tick(8); // Existing startup delay, expressed in CPU T-cycles.
            bus.tick(639);
            check(bus.debug_oam_dma().active && bus.debug_oam_dma().bytes_copied == 159,
                  "OAM DMA remains active until the 160th CPU machine cycle");
            bus.tick(1);
            check(!bus.debug_oam_dma().active && bus.debug_oam_dma().bytes_copied == 160,
                  "OAM DMA completes after 640 CPU T-cycles at either speed");
            // Finish even with the old bug, isolating the CALL/RET reproduction.
            bus.tick(648);
            const std::uint8_t routine[]{0x3E,0xC0,0xE0,0x46,0x3E,40,0x3D,0x20,0xFD,0xC9};
            for (unsigned i = 0; i < sizeof(routine); ++i) bus.write8(0xFF80 + i, routine[i]);
            auto r = emulator.cpu().registers(); r.pc = 0x0100; r.sp = 0xCFE1;
            emulator.set_cpu_registers(r);
            static_cast<void>(emulator.step());
            check(bus.read8(0xCFDF) == 3 && bus.read8(0xCFE0) == 1,
                  "CALL stores little-endian return word in WRAM");
            unsigned steps = 0;
            while (emulator.cpu().registers().pc != 0xFF89 && steps++ < 100)
                static_cast<void>(emulator.step());
            check(steps < 100, "DMA delay reaches HRAM RET");
            check(bus.read8(0xCFDF) == 3 && bus.read8(0xCFE0) == 1,
                  "OAM DMA never modifies the physical return word");
            static_cast<void>(emulator.step());
            check(!bus.debug_oam_dma().active, "DMA is complete after the standard delay and RET");
            check(emulator.cpu().registers().pc == 0x0103 && emulator.cpu().registers().sp == 0xCFE1,
                  fast ? "double-speed RET returns to caller, not FFFF" : "normal-speed RET returns to caller");
        }
    }
}

void test_native_interrupt_fetch_sample() {
    for (const auto model : {gameboy::HardwareModel::cgb0, gameboy::HardwareModel::cgb_c,
                             gameboy::HardwareModel::cgb_e, gameboy::HardwareModel::cgb}) {
        for (const bool native : {false,true}) for (const bool fast : {false,true}) {
            if (fast && !native) continue;
            for (const bool timer : {false,true}) {
                std::vector<std::uint8_t> program;
                if (fast) program={0x3E,1,0xE0,0x4D,0x10,0,0};
                program.insert(program.end(),{0xFB,0,0x03,0x76}); // EI; NOP; INC BC; HALT
                auto image=native?cgb_test_rom(program):test_rom(program);
                gameboy::Emulator emulator{gameboy::Cartridge{image},model};
                auto& bus=emulator.bus();
                const auto mask=static_cast<std::uint8_t>(timer?4:2);
                bus.write8(0xFF40,0); bus.write8(0xFF41,0);
                bus.write8(0xFF07,0); bus.write8(0xFF0F,0); bus.write8(0xFFFF,mask);
                if (fast) {
                    for (unsigned n=0;n<3;++n) (void)emulator.step();
                    while (emulator.cpu().debug_speed_switch_cycles_remaining()) (void)emulator.step();
                    (void)emulator.step();
                }
                (void)emulator.step(); (void)emulator.step();
                check(emulator.cpu().interrupts_enabled(),"IRQ fetch witness arms IME before the request");
                const auto pc=emulator.cpu().registers().pc;
                if (timer) {
                    bus.write8(0xFF04,0); bus.write8(0xFF05,0xFF); bus.write8(0xFF06,0);
                    bus.write8(0xFF07,5); bus.tick(18); // overflow at 16T, reload/IRQ at 20T
                } else {
                    bus.write8(0xFF41,0x20); bus.write8(0xFF40,0x87);
                    bus.tick((fast?904:452)-2); // first mode-2 edge two CPU clocks into the fetch
                }
                check((bus.read8(0xFF0F)&mask)==0,"IRQ is not pending at the instruction boundary");
                struct Capture {
                    const gameboy::Emulator* emulator{};
                    unsigned instructions{}, interrupts{};
                    std::uint16_t pc{}; std::uint8_t pending{}; bool after_fetch{};
                    std::uint64_t clock{};
                } observed{&emulator};
                emulator.debug_set_cpu_instruction_observer(
                    [](void* context,std::uint16_t,std::uint8_t) noexcept {
                        ++static_cast<Capture*>(context)->instructions;
                    },&observed);
                emulator.debug_set_cpu_interrupt_observer(
                    [](void* context,std::uint16_t address,std::uint8_t pending,bool after_fetch) noexcept {
                        auto& out=*static_cast<Capture*>(context);
                        ++out.interrupts; out.pc=address; out.pending=pending; out.after_fetch=after_fetch;
                        out.clock=out.emulator->bus().debug_normalized_clocks();
                    },&observed);
                const auto clock=bus.debug_normalized_clocks();
                const auto before=emulator.save_state();
                check(emulator.step()==(native?20U:8U),"discarded native fetch is part of the twenty entry clocks");
                if (native) {
                    check(emulator.cpu().registers().pc==(timer?0x50:0x48) &&
                              emulator.cpu().registers().b==0 && emulator.cpu().registers().c==0,
                          "native IRQ discards INC BC and enters the selected vector");
                    check(bus.read16(0xFFFC)==pc && emulator.cpu().registers().sp==0xFFFC,
                          "discarded fetch stacks its original PC exactly once");
                    check(observed.instructions==0 && observed.interrupts==1 && observed.pc==pc &&
                              observed.pending==mask && observed.after_fetch &&
                              observed.clock-clock==(fast?4U:8U),
                          "observers distinguish discarded fetches from executed instructions");
                } else {
                    check(emulator.cpu().registers().pc==pc+1 && emulator.cpu().registers().c==1 &&
                              observed.instructions==1 && observed.interrupts==0,
                          "monochrome compatibility keeps its existing recognition boundary");
                }
                const auto after=emulator.save_state();
                emulator.debug_set_cpu_instruction_observer(nullptr,nullptr);
                emulator.debug_set_cpu_interrupt_observer(nullptr,nullptr);
                emulator.load_state(before);
                (void)emulator.step();
                check(emulator.save_state()==after,
                      "IRQ recognition and observer-disabled replay preserve serialized state");
            }
        }
    }
}

void test_native_pixel_access_boundaries() {
    // Independently authored line-one access witnesses. The paired external
    // sweep covers executed accesses; these add both palette ports, index wrap,
    // serialized replay and CPU-versus-direct VRAM arbitration controls.
    for (const auto model : {gameboy::HardwareModel::cgb0, gameboy::HardwareModel::cgb_c,
                             gameboy::HardwareModel::cgb_e, gameboy::HardwareModel::cgb,
                             gameboy::HardwareModel::agb0, gameboy::HardwareModel::agb}) {
        for (const bool fast : {false, true}) {
            for (const bool palette : {false, true}) {
                for (const bool object : {false, true}) {
                    if (!palette && object) continue;
                    for (const unsigned index : {2U, 63U}) {
                        if (!palette && index != 2) continue;
                        for (const bool write : {false, true}) {
                            for (const unsigned dot : {79U, 80U, 81U, 252U, 253U}) {
                                auto image = cgb_test_rom({0x3e, 1, 0xe0, 0x4d, 0x10, 0});
                                const auto port = static_cast<std::uint16_t>(object ? 0xff6b : 0xff69);
                                image[0x200] = palette ? (write ? 0xe0 : 0xf0) : (write ? 0x70 : 0x7e);
                                image[0x201] = static_cast<std::uint8_t>(port);
                                gameboy::Emulator e{gameboy::Cartridge{std::move(image)}, model};
                                if (fast) {
                                    for (unsigned n=0;n<3;++n) (void)e.step();
                                    while (e.cpu().debug_speed_switch_cycles_remaining()) (void)e.step();
                                    (void)e.step(); // Complete resumption through the cartridge NOP.
                                    check(e.bus().double_speed(), "access witness enters double speed");
                                }
                                auto& bus=e.bus();
                                bus.write8(0xff40,0); bus.write8(0xff4f,0);
                                bus.write8(0x8100,0xa5);
                                bus.write8(static_cast<std::uint16_t>(port-1),static_cast<std::uint8_t>(index));
                                bus.write8(port,3);
                                bus.write8(static_cast<std::uint16_t>(port-1),static_cast<std::uint8_t>(0x80|index));
                                auto r=e.cpu().registers();r.pc=0x200;r.a=0x91;r.b=0x5a;r.h=0x81;r.l=0;
                                e.set_cpu_registers(r);
                                bus.write8(0xff40,0x91);
                                const unsigned scale=fast?2:1;
                                bus.tick((452+dot)*scale-8);
                                check(bus.read8(0xff44)==1,"access witness measures an ordinary visible line");
                                bus.debug_enable_io_trace(true);
                                const auto before=e.save_state();
                                const auto clock=bus.debug_bus_cycles();
                                const auto cycles=e.cpu().total_cycles();
                                check(e.step()==(palette?12U:8U) &&
                                          e.cpu().total_cycles()-cycles==(palette?12U:8U),
                                      "access sampling preserves the instruction's complete CPU duration");
                                const bool allowed=palette?(dot<80 || dot>=253 || (fast && dot==80))
                                                          :(dot<=80 || dot>=253);
                                if (write) {
                                    const auto value=palette?(object?bus.debug_read_cgb_object_palette(static_cast<std::uint8_t>(index))
                                                                   :bus.debug_read_cgb_bg_palette(static_cast<std::uint8_t>(index)))
                                                            :bus.debug_read_vram(0,0x100);
                                    check(value==(allowed?(palette?0x91:0x5a):(palette?3:0xa5)),
                                          "native write observes the independently measured access boundary");
                                } else check(e.cpu().registers().a==(allowed?(palette?3:0xa5):0xff),
                                             "native read observes the independently measured access boundary");
                                if (!palette && dot==80) {
                                    check(bus.read8(0x8100)==0xff,
                                          "native CPU handoff access does not relax direct/debug VRAM arbitration");
                                }
                                if (palette) {
                                    const auto expected=write?((index+1)&63):index;
                                    check((bus.read8(static_cast<std::uint16_t>(port-1))&0xbf)==(0x80|expected),
                                          "blocked palette writes still auto-increment and wrap; reads do not");
                                }
                                const auto trace=bus.debug_take_io_trace();
                                if (palette && write) {
                                    check(trace.size()==1 && trace[0].address==port && trace[0].value==0x91 &&
                                              trace[0].cycle==clock+8,
                                          "native palette IO diagnostics record the actual earlier write phase once");
                                } else check(trace.empty(),"non-IO access does not invent an IO write");
                                const auto after=e.save_state();
                                e.load_state(before); (void)e.step();
                                check(e.save_state()==after,"access boundaries and palette index replay through save/load");
                            }
                        }
                    }
                }
            }
        }
    }
    for (const auto model : {gameboy::HardwareModel::dmg, gameboy::HardwareModel::mgb,
                             gameboy::HardwareModel::cgb_e}) {
        for (const bool write : {false,true}) {
            auto image=test_rom({static_cast<std::uint8_t>(write?0x70:0x7e)});
            gameboy::Emulator e{gameboy::Cartridge{std::move(image)},model};
            auto& bus=e.bus();bus.write8(0xff40,0);bus.write8(0x8100,0xa5);
            auto r=e.cpu().registers();r.h=0x81;r.l=0;r.b=0x5a;e.set_cpu_registers(r);
            bus.write8(0xff40,0x91);bus.tick(452+72);
            check(e.step()==8,"compatibility handoff retains its instruction duration");
            if(write) check(bus.debug_read_vram(0,0x100)==(model==gameboy::HardwareModel::cgb_e?0xa5:0x5a),
                            "monochrome and CGB compatibility writes retain their separate handoff rules");
            else check(e.cpu().registers().a==0xff,"compatibility reads retain internal VRAM arbitration");
        }
    }
}

void test_split_double_speed_clocks() {
    const auto image = cgb_test_rom({0x3e, 1, 0xe0, 0x4d, 0x10, 0, 0, 0xf0, 0x0f});
    const auto prepare = [](gameboy::Emulator& emulator) {
        for (unsigned instruction = 0; instruction < 3; ++instruction) (void)emulator.step();
        while (emulator.cpu().debug_speed_switch_cycles_remaining()) (void)emulator.step();
        (void)emulator.step(); // Complete resumption with a NOP before measuring IF.
        check(emulator.bus().double_speed(), "split-clock witness enters double speed");
    };
    for (const auto model : {gameboy::HardwareModel::cgb0, gameboy::HardwareModel::cgb_c,
                             gameboy::HardwareModel::cgb_e, gameboy::HardwareModel::cgb}) {
        gameboy::Emulator batch{gameboy::Cartridge{image}, model};
        gameboy::Emulator split{gameboy::Cartridge{image}, model};
        prepare(batch);
        prepare(split);
        const auto dot = batch.bus().debug_ppu_dot();
        const auto clocks = batch.bus().debug_normalized_clocks();
        check(batch.step() == 12 && batch.bus().debug_ppu_dot() == dot + 6 &&
                  batch.bus().debug_normalized_clocks() == clocks + 12,
              "double-speed IF read advances six PPU dots despite its 3+1 clock split");
        (void)split.step();
        batch.bus().tick(4);
        split.bus().tick(3);
        split.bus().tick(1);
        check(batch.save_state() == split.save_state(),
              "one double-speed machine cycle matches split 3+1 ticks");

        const auto half_dot = split.bus().debug_ppu_dot();
        split.bus().tick(1);
        const auto half_state = split.save_state();
        check(split.bus().debug_ppu_dot() == half_dot,
              "one double-speed CPU tick retains its fractional PPU clock");
        split.bus().tick(1);
        const auto full_state = split.save_state();
        check(split.bus().debug_ppu_dot() == half_dot + 1,
              "two separate double-speed CPU ticks complete one PPU clock");
        split.load_state(half_state);
        split.bus().tick(1);
        check(split.save_state() == full_state,
              "save/load preserves the pending half clock through normalized bus time");

        split.bus().tick(1);
        const auto reset_dot = split.bus().debug_ppu_dot();
        split.reset(); // The post-boot reset is a warm CPU reset.
        split.bus().tick(1);
        check(split.bus().debug_ppu_dot() == reset_dot + 1,
              "warm CPU reset retains the pending peripheral half clock");
        split.bus().initialize_cgb_power_on(model);
        check(split.bus().debug_normalized_clocks() == 0,
              "cold initialization resets the peripheral half-clock epoch");
    }
}

void test_cgb_memory_and_rendering() {
    gameboy::Emulator speed{gameboy::Cartridge{cgb_test_rom({
        0x3E, 0x01, // LD A,1
        0xE0, 0x4D, // LDH (KEY1),A
        0x10, 0x00, // STOP: switch to double speed
        0x3E, 0x01, 0xE0, 0x4D, 0x10, 0x00,
    })}};
    static_cast<void>(speed.step());
    static_cast<void>(speed.step());
    check(!speed.bus().debug_apu_cycle_phase(),
          "CGB APU 1 MHz phase remains stable during normal-speed CPU cycles");
    speed.bus().tick(1);
    check(!speed.bus().debug_apu_cycle_phase(),
          "CGB APU 1 MHz phase ignores an odd normal-speed bus tick");
    check(speed.bus().read8(0xFF4D) == 0x7F,
          "KEY1 exposes a requested CGB speed switch");
    static_cast<void>(speed.step());
    check(speed.bus().double_speed() && !speed.cpu().stopped() &&
              speed.bus().read8(0xFF4D) == 0xFE,
          "CGB STOP performs an armed double-speed switch");
    check(!speed.bus().debug_apu_cycle_phase(),
          "CGB speed switch starts the APU at its retained 1 MHz phase");
    while (speed.cpu().debug_speed_switch_cycles_remaining())
        static_cast<void>(speed.step());
    speed.bus().tick(1);
    check(speed.bus().debug_apu_cycle_phase(),
          "CGB double-speed advances the APU phase after one half-cycle");
    speed.bus().tick(1);
    check(!speed.bus().debug_apu_cycle_phase(),
          "CGB double-speed returns to the APU phase after one full cycle");
    const auto apu_phase_state = speed.save_state();
    speed.bus().tick(1);
    check(speed.bus().debug_apu_cycle_phase(),
          "CGB APU phase changes on a split double-speed bus tick");
    speed.load_state(apu_phase_state);
    check(!speed.bus().debug_apu_cycle_phase(),
          "save states preserve the CGB APU 1 MHz phase");
    static_cast<void>(speed.step());
    static_cast<void>(speed.step());
    static_cast<void>(speed.step());
    check(!speed.bus().double_speed() && !speed.cpu().stopped(),
          "a second armed STOP returns the CGB CPU to normal speed");

    gameboy::Emulator emulator{gameboy::Cartridge{cgb_test_rom()}};
    auto& bus = emulator.bus();
    check(bus.cgb_mode() && emulator.cpu().registers().a == 0x11 &&
              emulator.cpu().registers().f == 0x80 &&
              emulator.cpu().registers().d == 0xFF &&
              emulator.cpu().registers().e == 0x56 &&
              emulator.cpu().registers().h == 0 &&
              emulator.cpu().registers().l == 0x0D,
          "CGB cartridges start with CGB hardware detection state");

    auto edited_registers = emulator.cpu().registers();
    edited_registers.a = 0x42;
    edited_registers.f = 0xAF;
    edited_registers.sp = 0xC123;
    edited_registers.pc = 0x4567;
    emulator.set_cpu_registers(edited_registers);
    check(emulator.cpu().registers().a == 0x42 &&
              emulator.cpu().registers().f == 0xA0 &&
              emulator.cpu().registers().sp == 0xC123 &&
              emulator.cpu().registers().pc == 0x4567,
          "debugger register edits update CPU state and sanitize F");

    bus.write8(0xFF40, 0);
    bus.write8(0x8000, 0x12);
    bus.write8(0xFF4F, 1);
    bus.write8(0x8000, 0x34);
    check(bus.read8(0xFF4F) == 0xFF && bus.read8(0x8000) == 0x34,
          "VBK selects the second CGB VRAM bank");
    bus.write8(0xFF4F, 0);
    check(bus.read8(0x8000) == 0x12,
          "switching VBK restores the first CGB VRAM bank");
    bus.debug_write_vram(0, 0x0020, 0x56);
    bus.debug_write_vram(1, 0x0020, 0x78);
    check(bus.debug_read_vram(0, 0x0020) == 0x56 &&
              bus.debug_read_vram(1, 0x0020) == 0x78,
          "debugger VRAM access reads and edits either CGB bank directly");

    bus.write8(0xD000, 0x11);
    bus.write8(0xFF70, 2);
    bus.write8(0xD000, 0x22);
    bus.write8(0xFF70, 0);
    check(bus.read8(0xFF70) == 0xF9 && bus.read8(0xD000) == 0x11,
          "SVBK zero aliases bank one and preserves switched WRAM banks");
    bus.write8(0xFF70, 2);
    check(bus.read8(0xD000) == 0x22 && bus.read8(0xF000) == 0x22,
          "CGB switched WRAM is mirrored through echo RAM");

    bus.write8(0xFF4F, 1);
    bus.write8(0x8000, 0x80);
    bus.write8(0x8001, 0x00); // Bank 1 tile 0, first pixel color 1.
    bus.write8(0x9800, 0x08); // Tile attribute selects VRAM bank 1.
    bus.write8(0xFF4F, 0);
    bus.write8(0x9800, 0);
    bus.write8(0xFF68, 0x82); // Palette 0, color 1, auto-increment.
    bus.write8(0xFF69, 0x1F);
    bus.write8(0xFF69, 0x00); // RGB555 red.
    check(bus.read8(0xFF68) == 0xC4,
          "CGB palette writes auto-increment their six-bit index");
    bus.write8(0xFF40, 0x91);
    bus.tick(254);
    check(bus.framebuffer()[0] == 0xFFFF0000,
          "CGB tile attributes select VRAM banks and RGB555 palettes");

    // CGB/AGB expose SCY through a two-T-cycle latch. A write must not be
    // visible on the register readback until that delay has elapsed.
    bus.write8(0xFF42, 0x37);
    bus.write8(0xFF40, 0x91);
    check(bus.read8(0xFF42) == 0x00,
          "CGB SCY writes remain pending for the documented delay");
    bus.tick(1);
    check(bus.read8(0xFF42) == 0x00,
          "CGB SCY remains old after one T-cycle");
    bus.tick(1);
    check(bus.read8(0xFF42) == 0x37,
          "CGB SCY becomes visible after two T-cycles");
    bus.write8(0xFF40, 0);

    bus.write8(0xFF42, 0x52);
    const auto scy_latch_state = emulator.save_state();
    bus.write8(0xFF40, 0x91);
    bus.tick(2);
    check(bus.read8(0xFF42) == 0x52,
          "CGB SCY pending writes complete before the next sample");
    emulator.load_state(scy_latch_state);
    bus.write8(0xFF40, 0x91);
    bus.tick(2);
    check(bus.read8(0xFF42) == 0x52,
          "save states preserve an in-flight CGB SCY latch");
    bus.write8(0xFF40, 0);

    for (unsigned byte = 0; byte < 0x30; ++byte) {
        bus.write8(static_cast<std::uint16_t>(0xC000 + byte),
                   static_cast<std::uint8_t>(0x40 + byte));
    }
    bus.write8(0xFF4F, 1);
    bus.write8(0xFF51, 0xC0);
    bus.write8(0xFF52, 0x00);
    bus.write8(0xFF53, 0x01);
    bus.write8(0xFF54, 0x00);
    bus.write8(0xFF55, 0x00);
    check(bus.read8(0x8100) == 0x40 && bus.read8(0x810F) == 0x4F &&
              bus.read8(0xFF55) == 0xFF,
          "CGB general-purpose VRAM DMA copies complete 16-byte blocks");

    bus.write8(0xFF51, 0xC0);
    bus.write8(0xFF52, 0x10);
    bus.write8(0xFF53, 0x01);
    bus.write8(0xFF54, 0x20);
    bus.write8(0xFF40, 0x91);
    bus.write8(0xFF55, 0x81);
    bus.tick(254);
    check(bus.read8(0xFF55) == 0x00 && bus.read8(0x8120) == 0x50,
          "CGB HBlank DMA transfers one block at each HBlank");
    bus.tick(456);
    check(bus.read8(0xFF55) == 0xFF && bus.read8(0x8130) == 0x60,
          "CGB HBlank DMA completes after its requested block count");

    gameboy::MemoryBus batched_hdma{gameboy::Cartridge{cgb_test_rom()}};
    for (unsigned byte = 0; byte < 0x30; ++byte) {
        batched_hdma.write8(static_cast<std::uint16_t>(0xC000 + byte),
                            static_cast<std::uint8_t>(0x70 + byte));
    }
    batched_hdma.write8(0xFF51, 0xC0);
    batched_hdma.write8(0xFF52, 0x00);
    batched_hdma.write8(0xFF53, 0x01);
    batched_hdma.write8(0xFF54, 0x40);
    batched_hdma.write8(0xFF40, 0x91);
    batched_hdma.write8(0xFF55, 0x82); // Three HBlank blocks.
    batched_hdma.tick(720); // Crosses two HBlanks in one bus batch.
    check(batched_hdma.read8(0x8140) == 0x70 &&
              batched_hdma.read8(0x8150) == 0x80 &&
              batched_hdma.read8(0xFF55) == 0x00,
          "CGB HBlank DMA services every HBlank crossed by a batched tick");

    gameboy::MemoryBus lcd_off_hdma{gameboy::Cartridge{cgb_test_rom()}};
    for (unsigned byte = 0; byte < 0x30; ++byte) {
        lcd_off_hdma.write8(static_cast<std::uint16_t>(0xC000 + byte),
                            static_cast<std::uint8_t>(0x90 + byte));
    }
    lcd_off_hdma.write8(0xFF51, 0xC0);
    lcd_off_hdma.write8(0xFF52, 0x00);
    lcd_off_hdma.write8(0xFF53, 0x01);
    lcd_off_hdma.write8(0xFF54, 0x60);
    lcd_off_hdma.write8(0xFF55, 0x82); // Three blocks, HBlank mode.
    check(lcd_off_hdma.read8(0x8160) == 0x90 &&
              lcd_off_hdma.read8(0x816F) == 0x9F &&
              lcd_off_hdma.read8(0x8170) == 0x00 &&
              lcd_off_hdma.read8(0xFF55) == 0x01,
          "CGB HBlank DMA starts with one block when the LCD is off");
    lcd_off_hdma.write8(0xFF55, 0x00);
    check(lcd_off_hdma.read8(0xFF55) == 0x80 &&
              lcd_off_hdma.read8(0x8170) == 0x00,
          "cancelling LCD-off HBlank DMA stops before the next block");

    const auto cgb_state = emulator.save_state();
    bus.write8(0xFF40, 0);
    bus.write8(0xFF4F, 1);
    bus.write8(0x8100, 0);
    bus.write8(0xFF70, 2);
    bus.write8(0xD000, 0);
    emulator.load_state(cgb_state);
    check(bus.read8(0xFF4F) == 0xFF && bus.read8(0x8100) == 0x40 &&
              bus.read8(0xD000) == 0x22,
          "save states preserve CGB VRAM, WRAM, palettes, and bank selection");

    gameboy::MemoryBus dmg{gameboy::Cartridge{test_rom()}};
    check(!dmg.cgb_mode() && dmg.read8(0xFF4F) == 0xFF &&
              dmg.read8(0xFF68) == 0xFF && dmg.read8(0xFF70) == 0xFF,
          "CGB-only registers remain unavailable to monochrome cartridges");

    gameboy::Emulator compatibility{
        gameboy::Cartridge{test_rom()}, gameboy::HardwareModel::cgb};
    auto& compatibility_bus = compatibility.bus();
    compatibility_bus.write8(0xFF72, 0x12);
    compatibility_bus.write8(0xFF73, 0x34);
    compatibility_bus.write8(0xFF75, 0xFF);
    check(!compatibility_bus.cgb_mode() &&
              compatibility_bus.read8(0xFF4F) == 0xFE &&
              compatibility_bus.read8(0xFF68) == 0xC8 &&
              compatibility_bus.read8(0xFF69) == 0xFF &&
              compatibility_bus.read8(0xFF72) == 0x12 &&
              compatibility_bus.read8(0xFF73) == 0x34 &&
              compatibility_bus.read8(0xFF75) == 0xFF &&
              compatibility_bus.read8(0xFF76) == 0,
          "DMG software on CGB hardware exposes compatibility-mode registers");
}


} // namespace

int main() {
    try {
        test_dma_instruction_bus_and_stack();
        test_ff74_native_and_compatibility();
        test_hram_dma_return();
        test_cgb_memory_and_rendering();
        test_native_interrupt_fetch_sample();
        test_split_double_speed_clocks();
        test_native_pixel_access_boundaries();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    }

    if (failures == 0) {
        std::cout << "All CGB contract tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
