#include "gameboy/emulator.hpp"
#include "save_state_container.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
int failures{};
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
std::vector<std::uint8_t> rom(unsigned type, unsigned banks = 128, unsigned ram = 3) {
    std::vector<std::uint8_t> bytes(banks * 0x4000);
    for (unsigned b = 0; b < banks; ++b) {
        bytes[b * 0x4000] = b & 255; bytes[b * 0x4000 + 1] = b >> 8;
    }
    bytes[0x143] = 0x80; bytes[0x147] = type;
    unsigned code = 0; for (auto n = banks; n > 2; n >>= 1) ++code;
    bytes[0x148] = code; bytes[0x149] = ram;
    return bytes;
}
void bank(gameboy::Cartridge& c, unsigned lower, unsigned upper, const char* message) {
    check(c.read(0) == (lower & 255) && c.read(1) == (lower >> 8) &&
          c.read(0x4000) == (upper & 255) && c.read(0x4001) == (upper >> 8), message);
}
std::int64_t clock_now(void* context) noexcept { return *static_cast<std::int64_t*>(context); }
void roundtrip(gameboy::Emulator& e, const char* message) {
    const auto saved = e.save_state();
    e.bus().write8(0x2000, 7); e.bus().write8(0xA000, 0xAA);
    e.load_state(saved);
    check(e.save_state() == saved, message);
}

void test_mmm01() {
    auto bytes = rom(1,512,0);
    const auto menu = bytes.size() - 0x8000;
    constexpr std::array<std::uint8_t,48> logo{
        0xCE,0xED,0x66,0x66,0xCC,0x0D,0,0x0B,3,0x73,0,0x83,0,0x0C,0,0x0D,
        0,8,0x11,0x1F,0x88,0x89,0,0x0E,0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,
        0xBB,0xBB,0x67,0x63,0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E};
    std::copy(logo.begin(),logo.end(),bytes.begin()+menu+0x104);
    bytes[menu+0x134]='M'; bytes[menu+0x135]=0;
    bytes[0x143]=0; bytes[menu+0x143]=0x80; bytes[menu+0x147]=0x0D;
    bytes[menu+0x148]=8; bytes[menu+0x149]=4;
    // A menu-first dump contains the same chip bytes in cyclic file order.
    // Authored 32-bank fixture: valid menu header, distinct bank markers.
    auto physical = rom(1,32,0);
    const auto tail = physical.size()-0x8000;
    std::copy(logo.begin(),logo.end(),physical.begin()+tail+0x104);
    physical[tail+0x147]=0x0B; physical[tail+0x148]=4;
    physical[tail+0x149]=0;
    unsigned checksum=0;
    for (unsigned i=0x134;i<=0x14C;++i) checksum=checksum-physical[tail+i]-1;
    physical[tail+0x14D]=checksum;
    auto menu_first=physical;
    std::rotate(menu_first.begin(),menu_first.end()-0x8000,menu_first.end());
    const auto unchanged=menu_first;
    gameboy::Cartridge first{menu_first}, last{physical};
    check(first.read(0)==30 && first.read(0x4000)==31,
          "Menu-first MMM01 dump boots the menu, not the file's trailing game banks");
    check(menu_first==unchanged,"Loading does not modify source dump bytes");
    for (unsigned b=0;b<32;++b) {
        first.write(0x2000,b); last.write(0x2000,b);
        first.write(0,0x40); last.write(0,0x40);
        check(first.read(0)==last.read(0) && first.read(0x4000)==last.read(0x4000),
              "Both MMM01 dump layouts expose identical physical banks after mapping");
    }
    std::uint64_t expected_fingerprint=UINT64_C(14695981039346656037);
    for (auto value:unchanged) { expected_fingerprint^=value; expected_fingerprint*=UINT64_C(1099511628211); }
    check(first.rom_fingerprint()==expected_fingerprint,"MMM01 state identity uses original dump bytes");
    auto invalid=unchanged; invalid[0x14D]^=1;
    gameboy::Cartridge unverified{invalid};
    check(unverified.read(0)==28,"Invalid menu-first checksum does not trigger normalization");
    auto ambiguous=physical;
    std::copy(physical.begin()+tail+0x104,physical.begin()+tail+0x150,ambiguous.begin()+0x104);
    gameboy::Cartridge preferred{ambiguous};
    check(preferred.read(0)==30,"Verified trailing MMM01 menu takes precedence over first header");
    for (bool rotated : {false,true}) {
        gameboy::Emulator normalized{gameboy::Cartridge{rotated?unchanged:physical}};
        auto& mapper=normalized.bus().cartridge();
        auto state=normalized.save_state();
        gameboy::Cartridge::Mmm01Diagnostics snapshot;
        check(mapper.debug_mmm01(snapshot) && snapshot.bank0==30 && snapshot.bank1==31 &&
              !snapshot.mapped && snapshot.menu_first==rotated && snapshot.registers==std::array<std::uint8_t,4>{},
              "MMM01 snapshot exposes authoritative reset state and dump provenance");
        check(normalized.save_state()==state,"Readonly mapper diagnostics leave save-state bytes unchanged");
        mapper.write(0x2000,0x48); mapper.write(0x4000,0x30); mapper.write(0x6000,0x30); mapper.write(0,0x40);
        check(mapper.debug_mmm01(snapshot) && snapshot.requested_bank0==456 && snapshot.bank0==8 &&
              snapshot.requested_bank1==457 && snapshot.bank1==9,
              "MMM01 32-bank ROM wraps requested nine-bit banks through real selectors");
        roundtrip(normalized,"Both MMM01 dump layouts preserve mapped state and locks on load");
        state=normalized.save_state();
        gameboy::Emulator again{gameboy::Cartridge{rotated?unchanged:physical}};
        again.load_state(state);
        check(again.save_state()==state,"Normalized MMM01 state loads into a fresh cartridge with same file identity");
        // The old unmapped state remains readable; it uses the corrected ROM layout.
        gameboy::Emulator reset{gameboy::Cartridge{rotated?unchanged:physical}};
        normalized.load_state(reset.save_state());
        check(mapper.debug_mmm01(snapshot) && !snapshot.mapped && snapshot.bank0==30,
              "MMM01 save/load restores unmapped menu selectors");
    }
    gameboy::Emulator e{gameboy::Cartridge{bytes}}; auto& c=e.bus().cartridge();
    check(c.type()==0x0D && c.title()=="M" && c.has_battery() && c.ram_size()==0x20000,
          "MMM01 uses its menu header rather than a contained MBC1 game's header");
    check(e.bus().cgb_mode(),"MMM01 hardware selection uses menu CGB flag");
    bank(c,510,511,"MMM01 initially exposes last two ROM banks");
    c.write(0x2000,0x30); c.write(0x6000,0x20); // low bank 10, ROM lock mask 10
    c.write(0x4000,0x15); // ROM high=1, RAM high=1, RAM low=1
    c.write(0,0x10); c.write(0,0x5A); // RAM low bit 0 locked, map, RAM enabled
    bank(c,176,177,"MMM01 masks bank zero live and combines high/middle/low game bits");
    c.write(0x2000,2); bank(c,176,178,"MMM01 mapped game changes only unlocked low bits");
    c.write(0x4000,0); c.write(0x6000,1); // unlocked RAM low bit1=0, low bit0 remains1
    c.write(0xA000,0x55); c.write(0x4000,2); c.write(0xA000,0x77);
    c.write(0x4000,0); check(c.read(0xA000)==0x55,"MMM01 RAM masks preserve game ownership");
    c.write(0x4000,2); check(c.read(0xA000)==0x77,"MMM01 retains separate unmasked RAM banks");
    c.write(0,0); c.write(0x2000,0xFF); bank(c,176,191,"MMM01 mapping and high game bits cannot be unlocked");
    check(c.read(0xA000)==0xFF,"MMM01 RAM disable returns open bus");
    roundtrip(e,"MMM01 mapping locks and bank/RAM state survive save/load");
    gameboy::Cartridge mux{rom(0x0B,512,0)};
    mux.write(0x2000,0x20); mux.write(0x6000,0x40); mux.write(0x4000,2);
    mux.write(0,0x40); bank(mux,0,65,"MMM01 multiplex swaps ROM middle and RAM low bits");
    mux.write(0x6000,1); bank(mux,64,65,"MMM01 multiplex advanced mode banks the lower ROM window");
    mux.write(0x4000,3); bank(mux,96,97,"MMM01 multiplex retains ROM middle bits in mapped mode");
    check(mux.read(0xA000)==0xFF,"MMM01 ROM-only type has no RAM");
    gameboy::Cartridge locked{rom(0x0C,32,3)};
    locked.write(0x4000,0x40); locked.write(0x6000,1); locked.write(0,0x4A);
    locked.write(0x4000,1); locked.write(0xA000,0x63); locked.write(0x4000,2);
    check(locked.read(0xA000)==0x63,"MMM01 mode write lock retains disabled RAM banking");
}

void test_control_write_observer() {
    struct Event { std::uint64_t cycle{}; std::uint16_t address{}; std::uint8_t value{}; bool before{}; };
    struct Capture { std::array<Event,16> events{}; unsigned count{}; };
    for (bool fast : {false,true}) {
        auto bytes=rom(0x0B,32,0);
        const std::array<std::uint8_t,17> program{
            0xF3,0x10,0, // DI; STOP (speed switch only when requested)
            0x31,0x34,0x12,0x08,0,0x20, // SP=1234; LD [2000],SP (two writes)
            0x3E,0,0xE0,0x40, // LCD off
            0x18,0xFE,0,0};
        const auto tail=bytes.size()-0x8000;
        std::copy(program.begin(),program.end(),bytes.begin()+tail+0x100);
        // Remain monochrome in the normal-speed case; CGB permits STOP speed switching.
        bytes[0x143]=fast?0x80:0;
        if (!fast) { bytes[tail+0x101]=0; bytes[tail+0x102]=0; }
        gameboy::Emulator plain{gameboy::Cartridge{bytes}}, observed{gameboy::Cartridge{bytes}};
        if (fast) { plain.bus().write8(0xFF4D,1); observed.bus().write8(0xFF4D,1); }
        Capture capture;
        observed.bus().debug_set_control_write_observer(
            [](void* context,const gameboy::MemoryBus& bus,std::uint16_t address,std::uint8_t value,bool before) noexcept {
                auto& result=*static_cast<Capture*>(context);
                if (result.count<result.events.size()) result.events[result.count++]={bus.debug_bus_cycles(),address,value,before};
                gameboy::Cartridge::Mmm01Diagnostics snapshot;
                (void)bus.cartridge().debug_mmm01(snapshot);
                (void)bus.debug_cpu_read8(0x100);
            },&capture);
        for (unsigned i=0;i<50;++i) { (void)plain.step(); (void)observed.step(); }
        check(plain.save_state()==observed.save_state(),"Control write observer preserves exact state at either CPU speed");
        check(capture.count==6 && capture.events[0].address==0x2000 && capture.events[2].address==0x2001 &&
              capture.events[4].address==0xFF40,"Observer reports both writes of LD [a16],SP and LCDC");
        check(capture.events[0].before && !capture.events[1].before &&
              capture.events[0].cycle==capture.events[1].cycle &&
              capture.events[2].cycle-capture.events[0].cycle==4,
              "Observer brackets actual CPU writes without advancing the clock");
    }
}

void test_huc1() {
    gameboy::Emulator e{gameboy::Cartridge{rom(0xFF,64)}}; auto& c=e.bus().cartridge();
    check(c.type()==0xFF && c.has_battery() && c.has_infrared() && !c.has_rtc(),"HuC1 capabilities");
    bank(c,0,1,"HuC1 initial ROM mapping"); c.write(0x2000,0); bank(c,0,0,"HuC1 permits upper bank zero");
    c.write(0x2000,0xFF); bank(c,0,63,"HuC1 uses six ROM bank bits");
    c.write(0xA000,0x12); c.write(0,0); check(c.read(0xA000)==0x12,"HuC1 RAM remains enabled after zero select");
    c.write(0x4000,3); c.write(0xA000,0x34); c.write(0x6000,1); c.write(0x4000,0);
    check(c.read(0xA000)==0x12,"HuC1 RAM bank is independent of MBC1 mode writes");
    c.write(0,0x0E); check(c.read(0xA000)==0xC0,"HuC1 disconnected IR reads no light");
    c.set_infrared_input(true); c.write(0xBFFF,1);
    check(c.read(0xA123)==0xC1 && c.infrared_output(),"HuC1 IR register mirrors and separates receive/transmit");
    roundtrip(e,"HuC1 restores IR mode and pins");
    c.write(0,0x0A); c.write(0x4000,3); check(c.read(0xA000)==0x34,"HuC1 IR never corrupts SRAM");
}

void enable(gameboy::Cartridge& c) { c.write(0,0x0A); c.write(0x4000,0x40); }
void serial_bit(gameboy::Cartridge& c, bool bit) {
    c.write(0xA080,0x80 | (bit ? 2 : 0)); c.write(0xA080,0xC0 | (bit ? 2 : 0));
}
void command(gameboy::Cartridge& c, unsigned value) {
    c.write(0xA080,0); serial_bit(c,1);
    for (int i=9;i>=0;--i) serial_bit(c,(value>>i)&1);
}
void data_word(gameboy::Cartridge& c, unsigned value, unsigned begin=16, unsigned end=0) {
    for (int i=static_cast<int>(begin)-1;i>=static_cast<int>(end);--i) serial_bit(c,(value>>i)&1);
}
void finish_write(gameboy::Cartridge& c) { c.write(0xA080,0); c.tick(20972); }
unsigned read_word(gameboy::Cartridge& c, unsigned address) {
    command(c,0x200|address); check(!(c.read(0xA080)&1),"EEPROM READ has a dummy zero");
    unsigned value{}; for(unsigned i=0;i<16;++i) { serial_bit(c,0); value=(value<<1)|(c.read(0xA080)&1); }
    return value;
}
void test_mbc7() {
    gameboy::Emulator e{gameboy::Cartridge{rom(0x22,128,0)}}; auto& c=e.bus().cartridge();
    check(c.ram_size()==256 && c.has_battery() && c.has_accelerometer(),"MBC7 owns EEPROM, not header SRAM");
    bank(c,0,1,"MBC7 initial ROM mapping");
    check(c.read(0xA020)==0xFF,"MBC7 registers initially disabled"); c.write(0,10);
    check(c.read(0xA020)==0xFF,"MBC7 needs both enable latches"); enable(c);
    check(c.read(0xA030)==0x80,"MBC7 sensor starts erased");
    c.write(0xA010,0xAA); check(c.read(0xA030)==0x80,"MBC7 cannot latch without erase");
    c.write(0xA100,0x55); c.write(0xA11F,0xAA);
    check(c.read(0xA02F)==0xD0 && c.read(0xA030)==0x81,"MBC7 neutral calibrated sensor is deterministic");
    c.set_accelerometer(0x8234,0x7FED); c.write(0xA010,0xAA);
    check(c.read(0xA020)==0xD0,"MBC7 latched sample does not change without re-arm");
    c.write(0xA000,0x55); c.write(0xA010,0xAA);
    check(c.read(0xA020)==0x34 && c.read(0xA030)==0x82 && c.read(0xA040)==0xED && c.read(0xA050)==0x7F,
          "MBC7 latches both sensor axes");
    check(c.read(0xB080)==0xFF && c.read(0xA060)==0 && c.read(0xA070)==0xFF,"MBC7 unused register decoding");
    c.write(0x2000,0xFF); bank(c,0,127,"MBC7 ROM bank mask"); c.write(0x2000,0); bank(c,0,0,"MBC7 bank zero model");
    command(c,0x105); data_word(c,0x1234); finish_write(c);
    check(read_word(c,5)==0xFFFF,"EEPROM starts erased and write-disabled");
    command(c,0x0C0); // EWEN
    command(c,0x105); data_word(c,0x1234); c.write(0xA080,0); c.write(0xA080,0x80);
    check(!(c.read(0xA080)&1) && c.export_battery_ram()[10]==0xFF,"EEPROM remains busy with uncommitted data");
    c.tick(20971); check(!(c.read(0xA080)&1),"EEPROM busy interval is bounded base-clock time");
    const auto busy=e.save_state(); c.tick(1); const auto complete=e.save_state();
    e.load_state(busy); c.tick(1); check(e.save_state()==complete,"EEPROM busy save/load commits exactly once");
    check(read_word(c,5)==0x1234,"EEPROM serial WRITE and READ are MSB-first");
    command(c,0x106); data_word(c,0xBEEF,16,8); const auto partial=e.save_state();
    data_word(c,0xBEEF,8); finish_write(c); check(read_word(c,6)==0xBEEF,"EEPROM partial word fixture");
    e.load_state(partial); data_word(c,0xBEEF,8); finish_write(c);
    check(read_word(c,6)==0xBEEF,"EEPROM mid-command save/load retains serial edges and shift register");
    command(c,0x107); data_word(c,0x1111,16,8); c.write(0xA080,0);
    check(read_word(c,7)==0xFFFF,"EEPROM aborted WRITE cannot program data");
    command(c,0x000); command(c,0x305); finish_write(c);
    check(read_word(c,5)==0x1234,"EEPROM EWDS protects ERASE");
    command(c,0x0C0); command(c,0x305); finish_write(c);
    check(read_word(c,5)==0xFFFF,"EEPROM ERASE sets one word to FFFF");
    command(c,0x040); data_word(c,0xCAFE); finish_write(c);
    check(read_word(c,0)==0xCAFE && read_word(c,127)==0xCAFE,"EEPROM WRAL writes all 128 words");
    serial_bit(c,0); unsigned sequential=c.read(0xA080)&1;
    for(unsigned i=1;i<16;++i){serial_bit(c,0);sequential=(sequential<<1)|(c.read(0xA080)&1);}
    check(sequential==0xCAFE,"EEPROM sequential READ wraps word 127 to zero");
    command(c,0x080); finish_write(c);
    const auto erased=c.export_battery_ram(); check(std::all_of(erased.begin(),erased.end(),[](auto v){return v==0xFF;}),"EEPROM ERAL clears all words");
    roundtrip(e,"MBC7 sensor and EEPROM protocol roundtrip");
    c.write(0x4000,0); check(c.read(0xA020)==0xFF,"MBC7 second latch can disable registers");
}

unsigned huc_command(gameboy::Cartridge& c,unsigned command) {
    c.write(0,0xB); c.write(0xA000,command); c.write(0,0xD); c.write(0xA000,0xFE);
    check(c.read(0xA000)&1,"HuC3 MCU reports ready after synchronous command");
    c.write(0,0xC); return c.read(0xA000)&15;
}
void huc_address(gameboy::Cartridge& c,unsigned address) {
    huc_command(c,0x40|(address&15)); huc_command(c,0x50|(address>>4));
}
unsigned huc_value(gameboy::Cartridge& c,unsigned address) {
    huc_address(c,address); unsigned result{};
    for(unsigned i=0;i<3;++i) result|=huc_command(c,0x10)<<(i*4);
    return result;
}
void test_huc3() {
    std::int64_t now=1000;
    gameboy::Emulator e{gameboy::Cartridge{rom(0xFE,128,4)}}; auto& c=e.bus().cartridge(); c.set_rtc_clock(clock_now,&now);
    check(c.has_rtc() && c.has_infrared() && c.has_battery(),"HuC3 capabilities");
    bank(c,0,1,"HuC3 initial mapping"); c.write(0x2000,0); bank(c,0,0,"HuC3 permits bank zero");
    c.write(0x2000,0xFF); bank(c,0,127,"HuC3 ROM bank mask");
    c.write(0,0xA); c.write(0xA000,0x72); c.write(0,0); c.write(0xA000,0x43);
    check(c.read(0xA000)==0x72,"HuC3 zero mode is read-only SRAM");
    c.write(0,0xA); c.write(0x4000,15); c.write(0xA000,0x21); c.write(0x4000,0);
    check(c.read(0xA000)==0x72,"HuC3 supports independent RAM banks");
    c.write(0,7); check(c.read(0xA000)==0xFF,"HuC3 invalid modes return open bus");
    huc_address(c,0xFF); huc_command(c,0x35); huc_command(c,0x36); huc_address(c,0xFF);
    check(huc_command(c,0x10)==5 && huc_command(c,0x10)==6,"HuC3 nibble access increments and wraps at FF");
    huc_address(c,0x80); c.write(0,0xB); c.write(0xA000,0x39); c.write(0,0xC);
    check((c.read(0xA000)&0x70)==0x30,"HuC3 mailbox preserves command bits before execution");
    huc_address(c,0x80); check(huc_command(c,0x10)==0,"HuC3 mailbox write alone does not execute");
    now+=86399; huc_command(c,0x60);
    check(huc_value(c,0)==1439 && huc_value(c,3)==0,"HuC3 atomic time copy captures minute/day counters");
    now+=1; huc_command(c,0x60);
    check(huc_value(c,0)==0 && huc_value(c,3)==1,"HuC3 keeps fractional minutes across requests");
    huc_address(c,0); huc_command(c,0x35); huc_command(c,0x30); huc_command(c,0x30);
    huc_command(c,0x61); huc_command(c,0x60);
    check(huc_value(c,0)==5 && huc_value(c,0x58)==5,
          "HuC3 set-time adjusts event time by the same minute delta");
    check(huc_command(c,0x62)==1,"HuC3 documented extended status returns one");
    c.write(0,0xE); c.set_infrared_input(true); c.write(0xA001,1);
    check(c.read(0xAFFF)==0xC1 && c.infrared_output(),"HuC3 IR register");
    roundtrip(e,"HuC3 mailbox, time memory and IR pins roundtrip");
    auto rtc=c.export_rtc_data(); gameboy::Cartridge other{rom(0xFE,128,4)}; other.set_rtc_clock(clock_now,&now);
    other.import_rtc_data(rtc); check(other.export_rtc_data()==rtc,"HuC3 RTC battery format roundtrip");
    const auto before=c.export_rtc_data(); rtc[274]=16;
    try { c.import_rtc_data(rtc); check(false,"HuC3 invalid RTC nibble must be rejected"); } catch(const std::exception&) {}
    check(c.export_rtc_data()==before,"Invalid RTC import leaves clock unchanged");
}

void tama_write(gameboy::Cartridge& c,unsigned reg,unsigned value) { c.write(0xA001,reg); c.write(0xA000,value); }
void tama_command(gameboy::Cartridge& c,unsigned command,unsigned value=0) {
    tama_write(c,4,value&15); tama_write(c,5,value>>4); tama_write(c,6,command>>4); tama_write(c,7,command&15);
}
unsigned tama_read(gameboy::Cartridge& c) {
    c.write(0xA001,0xC); const auto lo=c.read(0xA000)&15;
    c.write(0xA001,0xD); return lo|((c.read(0xA000)&15)<<4);
}
void tama_clock_write(gameboy::Cartridge& c,unsigned page,unsigned reg,unsigned value) {
    tama_command(c,0x80|(page<<1), reg|(value<<4));
}
unsigned tama_clock_read(gameboy::Cartridge& c,unsigned page,unsigned reg) {
    tama_command(c,0x81|(page<<1),reg); c.write(0xA001,0xC); return c.read(0xA000)&15;
}
void test_tama5() {
    std::int64_t now=1000;
    gameboy::Emulator e{gameboy::Cartridge{rom(0xFD,32,0)}}; auto& c=e.bus().cartridge(); c.set_rtc_clock(clock_now,&now);
    check(c.ram_size()==32 && c.has_rtc() && c.has_battery(),"TAMA5 owns 32-byte memory and RTC");
    bank(c,0,1,"TAMA5 initial ROM mapping (deterministic unmeasured power-on default)");
    check(c.read(0xA000)==0xF0 && c.read(0xA001)==0xFF,"TAMA5 power-on interface is locked");
    c.write(0xA001,10); check(c.read(0xA000)==0xF1,"TAMA5 unlock and ready status");
    tama_write(c,0,0xF); tama_write(c,1,0xF); bank(c,0,31,"TAMA5 uses five ROM bank bits");
    tama_write(c,0,0); tama_write(c,1,0); bank(c,0,0,"TAMA5 allows bank zero");
    for(unsigned address : {0U,15U,16U,31U}) {
        tama_command(c,address,0xA0+address); tama_command(c,0x20|address);
        check(tama_read(c)==0xA0+address,"TAMA5 nibble protocol accesses all 32 persistent bytes");
    }
    tama_clock_write(c,1,10,1); // 24-hour mode
    tama_clock_write(c,0,7,8); tama_clock_write(c,0,8,2); // Feb 28
    tama_clock_write(c,0,9,2); tama_clock_write(c,0,10,0);
    tama_clock_write(c,0,11,4); tama_clock_write(c,0,12,2); // year 24
    tama_clock_write(c,1,11,0); // independent leap cycle
    tama_command(c,0x45,0x23); tama_command(c,0x44,0x59); tama_command(c,0x41);
    now+=60;
    check(tama_clock_read(c,0,7)==9 && tama_clock_read(c,0,8)==2 && tama_clock_read(c,0,4)==0,
          "TAMA5 clock advances across midnight into leap day");
    now+=86400; check(tama_clock_read(c,0,9)==3 && tama_clock_read(c,0,7)==1,"TAMA5 leap day rolls to March");
    tama_command(c,0x40); now+=3600; check(tama_clock_read(c,0,4)==0,"TAMA5 stopped timer does not advance");
    for(unsigned page : {2U,3U}) { tama_clock_write(c,page,12,0xB+page); check(tama_clock_read(c,page,12)==0xB+page,"TAMA5 free RTC pages remain distinct"); }
    tama_clock_write(c,1,0,15); check(tama_clock_read(c,1,0)==0,"TAMA5 unwired alarm bits read zero");
    tama_clock_write(c,0,13,15); check((tama_clock_read(c,0,13)&8)==0,"TAMA5 direct writes cannot bypass timer command");
    tama_command(c,0x51); check(tama_clock_read(c,0,13)&4,"TAMA5 retains alarm-enable flag without fabricating sound");
    roundtrip(e,"TAMA5 command, RAM and RTC pages roundtrip");
    const auto rtc=c.export_rtc_data(); gameboy::Cartridge copy{rom(0xFD,32,0)}; copy.set_rtc_clock(clock_now,&now);
    copy.import_rtc_data(rtc); check(copy.export_rtc_data()==rtc,"TAMA5 RTC battery format roundtrip");
    tama_clock_write(c,1,10,0); // 12-hour clock, 11 PM to midnight.
    tama_clock_write(c,0,7,1); tama_clock_write(c,0,8,3);
    tama_clock_write(c,0,9,2); tama_clock_write(c,0,10,1);
    tama_clock_write(c,0,11,9); tama_clock_write(c,0,12,9);
    tama_clock_write(c,1,11,3);
    tama_command(c,0x45,0x31); tama_command(c,0x44,0x59); tama_command(c,0x41);
    now+=60;
    check(tama_clock_read(c,0,4)==0 && tama_clock_read(c,0,5)==0 &&
          tama_clock_read(c,0,7)==1 && tama_clock_read(c,0,9)==1 &&
          tama_clock_read(c,0,11)==0 && tama_clock_read(c,0,12)==0 && tama_clock_read(c,1,11)==0,
          "TAMA5 12-hour PM clock wraps century and independent leap cycle at midnight");
    tama_clock_write(c,0,0,0xF); check(tama_clock_read(c,0,0)==0xF,"TAMA5 retains invalid BCD without unsafe normalization");
}

void test_eeprom_bus_clock() {
    for (const bool fast : {false,true}) {
        auto bytes=rom(0x22,32,0); bytes[0x100]=0x10; bytes[0x101]=0;
        gameboy::Emulator e{gameboy::Cartridge{std::move(bytes)}};
        auto& bus=e.bus(); auto& c=bus.cartridge();
        if (fast) { bus.write8(0xFF4D,1); static_cast<void>(e.step()); }
        check(bus.double_speed()==fast,"EEPROM timing fixture uses requested CPU speed");
        enable(c); command(c,0xC0); command(c,0x100); data_word(c,0x2468);
        c.write(0xA080,0); c.write(0xA080,0x80);
        const unsigned stride=fast ? 8 : 4;
        for (unsigned i=0;i<5242;++i) bus.tick(stride);
        check(!(c.read(0xA080)&1) && c.export_battery_ram()[0]==0xFF,
              "EEPROM stays busy until full base-clock programming interval at either CPU speed");
        bus.tick(stride);
        check((c.read(0xA080)&1) && read_word(c,0)==0x2468,
              "Memory bus commits EEPROM after same elapsed base-clock time in both speeds");
    }
}

void test_hardware_initialization() {
    for (const unsigned type : {0x0BU,0x22U,0xFDU,0xFEU,0xFFU}) {
        for (const unsigned color : {0U,0x80U,0xC0U}) {
            auto bytes = rom(type,32,type==0x0B ? 0 : 3);
            bytes[0x143] = color;
            gameboy::Emulator e{gameboy::Cartridge{std::move(bytes)}};
            check(e.bus().cgb_mode() == (color != 0), "Rare controller respects automatic DMG/CGB initialization");
            check(e.cpu().registers().pc == 0x100, "Rare controller retains normal CPU post-boot entry");
        }
    }
}

void test_persistence_and_legacy_states() {
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory=std::filesystem::temp_directory_path()/("gbb-mapper-"+std::to_string(stamp));
    std::filesystem::create_directories(directory);
    for(unsigned type : {0x0DU,0x22U,0xFDU,0xFEU,0xFFU}) {
        const auto banks=type==0xFD ? 32U : 64U;
        const auto path=directory/(std::to_string(type)+".gb");
        std::vector<std::uint8_t> expected, expected_rtc;
        std::int64_t now=1000;
        {
            gameboy::Cartridge c{rom(type,banks)}; c.set_rtc_clock(clock_now,&now); c.set_persistence_path(path);
            auto data=c.export_battery_ram(); data.front()=0x73; data.back()=0x52;
            c.import_battery_save(data); expected=c.export_battery_save(); expected_rtc=c.export_rtc_data(); c.flush_battery();
            if(c.has_rtc()) check(std::filesystem::exists(directory/(std::to_string(type)+".rtc")),"Rare RTC uses persistent sidecar");
        }
        gameboy::Cartridge restored{rom(type,banks)}; restored.set_rtc_clock(clock_now,&now); restored.set_persistence_path(path);
        check(restored.export_rtc_data()==expected_rtc,"Each rare RTC sidecar reloads its exact deterministic clock data");
        check(restored.export_battery_save()==expected,"Each rare controller reloads exact persistent bytes");
    }
    std::filesystem::remove_all(directory);
    for(unsigned type : {1U,5U,0x10U,0x1BU}) {
        gameboy::Emulator e{gameboy::Cartridge{rom(type,8,type==5 ? 0 : 3)}};
        const auto saved=e.save_state(); auto decoded=gameboy::save_state_container::decode(saved,e.rom_fingerprint());
        check(decoded.payload.back()==0,"Legacy mapper has only a false extension marker"); decoded.payload.pop_back();
        auto legacy=gameboy::save_state_container::encode(e.rom_fingerprint(),decoded.payload); legacy[8]=42;
        e.load_state(legacy); check(e.save_state()==saved,"Version 42 states still load for MBC1/2/3/5");
    }
    gameboy::Emulator e{gameboy::Cartridge{rom(0xFF,64)}};
    const auto saved=e.save_state(); auto decoded=gameboy::save_state_container::decode(saved,e.rom_fingerprint());
    auto old=gameboy::save_state_container::encode(e.rom_fingerprint(),decoded.payload); old[8]=42;
    try {e.load_state(old);check(false,"Old states must reject formerly unsupported controllers");}catch(const gameboy::SaveStateError&){}
    check(e.save_state()==saved,"Rejected legacy rare-mapper state rolls back atomically");
    decoded.payload.back()=2; // Last extension field is a boolean.
    try {e.load_state(gameboy::save_state_container::encode(e.rom_fingerprint(),decoded.payload));check(false,"Invalid mapper state must reject");}catch(const gameboy::SaveStateError&){}
    check(e.save_state()==saved,"Invalid extension rolls back atomically");
    try {gameboy::Cartridge unsupported{rom(0x20,64)};check(false,"Unknown mapper must not silently fall back");}catch(const std::invalid_argument&){}
}
} // namespace
int main() {
    try { test_mmm01(); test_control_write_observer(); test_huc1(); test_mbc7(); test_huc3(); test_tama5(); test_eeprom_bus_clock(); test_hardware_initialization(); test_persistence_and_legacy_states(); }
    catch(const std::exception& e) {std::cerr<<"Unexpected exception: "<<e.what()<<'\n';return 1;}
    if(!failures) std::cout<<"All rare cartridge controller contracts passed\n";
    return failures ? 1 : 0;
}
