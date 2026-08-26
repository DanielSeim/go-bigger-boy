#include "gameboy/emulator.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <string>

namespace gameboy {
namespace {

constexpr std::array<std::uint8_t, 8> state_magic{
    'G', 'B', 'B', 'S', 'T', 'A', 'T', 'E',
};
constexpr std::uint32_t state_version = 16;
constexpr std::uint32_t oldest_supported_state_version = 1;
constexpr std::size_t maximum_state_size = 2 * 1024 * 1024;
constexpr std::size_t maximum_serial_output = 1024 * 1024;

class Writer {
public:
    void u8(const std::uint8_t value) { bytes_.push_back(value); }

    void boolean(const bool value) { u8(value ? 1 : 0); }

    void u16(const std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value));
        u8(static_cast<std::uint8_t>(value >> 8));
    }

    void u32(const std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void u64(const std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void f32(const float value) {
        static_assert(sizeof(float) == sizeof(std::uint32_t),
                      "save states require 32-bit floats");
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }

    void bytes(const std::uint8_t* data, const std::size_t size) {
        bytes_.insert(bytes_.end(), data, data + size);
    }

    void string(const std::string& value) {
        if (value.size() > maximum_serial_output) {
            throw SaveStateError("Serial output is too large for a save state");
        }
        u32(static_cast<std::uint32_t>(value.size()));
        bytes(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
    }

    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept {
        return bytes_;
    }

    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& bytes,
                    const std::size_t begin = 0,
                    const std::size_t size = std::numeric_limits<std::size_t>::max())
        : bytes_(bytes), position_(begin), end_(bytes.size()) {
        if (begin > bytes.size() ||
            (size < std::numeric_limits<std::size_t>::max() &&
             size > bytes.size() - begin)) {
            throw SaveStateError("Save state is truncated");
        }
        if (size < std::numeric_limits<std::size_t>::max()) end_ = begin + size;
    }

    [[nodiscard]] std::uint8_t u8() {
        require(1);
        return bytes_[position_++];
    }

    [[nodiscard]] bool boolean() {
        const auto value = u8();
        if (value > 1) throw SaveStateError("Save state contains an invalid boolean");
        return value != 0;
    }

    [[nodiscard]] std::uint16_t u16() {
        const auto low = u8();
        const auto high = u8();
        return static_cast<std::uint16_t>(
            low | (static_cast<std::uint16_t>(high) << 8));
    }

    [[nodiscard]] std::uint32_t u32() {
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(u8()) << shift;
        }
        return value;
    }

    [[nodiscard]] std::uint64_t u64() {
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            value |= static_cast<std::uint64_t>(u8()) << shift;
        }
        return value;
    }

    [[nodiscard]] float f32() {
        const auto bits = u32();
        float value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    void bytes(std::uint8_t* destination, const std::size_t size) {
        require(size);
        std::copy_n(bytes_.data() + position_, size, destination);
        position_ += size;
    }

    [[nodiscard]] std::string string() {
        const auto size = static_cast<std::size_t>(u32());
        if (size > maximum_serial_output) {
            throw SaveStateError("Save state serial output is too large");
        }
        require(size);
        std::string value(reinterpret_cast<const char*>(bytes_.data() + position_),
                          size);
        position_ += size;
        return value;
    }

    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return end_ - position_; }

    void finish() const {
        if (position_ != end_) {
            throw SaveStateError("Save state contains unexpected trailing data");
        }
    }

private:
    void require(const std::size_t size) const {
        if (size > end_ - position_) throw SaveStateError("Save state is truncated");
    }

    const std::vector<std::uint8_t>& bytes_;
    std::size_t position_{};
    std::size_t end_{};
};

std::uint32_t crc32(const std::uint8_t* data, const std::size_t size) noexcept {
    auto crc = UINT32_C(0xFFFFFFFF);
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) != 0 ? UINT32_C(0xEDB88320) : 0);
        }
    }
    return ~crc;
}

std::int64_t current_unix_seconds() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

class SaveStateCodec {
public:
    [[nodiscard]] static std::vector<std::uint8_t> encode(
        const Emulator& emulator) {
        emulator.bus_.cartridge_.update_rtc();

        Writer payload;
        write_cpu(payload, emulator.cpu_);
        write_bus(payload, emulator.bus_);
        if (payload.data().size() > maximum_state_size) {
            throw SaveStateError("Save state payload is unexpectedly large");
        }

        Writer state;
        state.bytes(state_magic.data(), state_magic.size());
        state.u32(state_version);
        state.u64(emulator.rom_fingerprint());
        state.u32(static_cast<std::uint32_t>(payload.data().size()));
        state.u32(crc32(payload.data().data(), payload.data().size()));
        state.bytes(payload.data().data(), payload.data().size());
        return state.take();
    }

    static void decode(Emulator& emulator,
                       const std::vector<std::uint8_t>& state) {
        if (state.size() > maximum_state_size) {
            throw SaveStateError("Save state is too large");
        }

        Reader header(state);
        std::array<std::uint8_t, state_magic.size()> magic{};
        header.bytes(magic.data(), magic.size());
        if (magic != state_magic) throw SaveStateError("Not a GBB save state");

        const auto version = header.u32();
        if (version < oldest_supported_state_version || version > state_version) {
            throw SaveStateError("Unsupported save-state version: " +
                                 std::to_string(version));
        }
        if (header.u64() != emulator.rom_fingerprint()) {
            throw SaveStateError("Save state belongs to a different ROM");
        }
        const auto payload_size = static_cast<std::size_t>(header.u32());
        const auto expected_crc = header.u32();
        if (payload_size != header.remaining()) {
            throw SaveStateError("Save-state payload size is invalid");
        }
        const auto payload_position = header.position();
        if (crc32(state.data() + payload_position, payload_size) != expected_crc) {
            throw SaveStateError("Save-state checksum does not match");
        }

        Reader payload(state, payload_position, payload_size);
        read_cpu(payload, emulator.cpu_);
        read_bus(payload, emulator.bus_, version);
        payload.finish();
    }

private:
    template <std::size_t Size>
    static void write_bytes(Writer& writer,
                            const std::array<std::uint8_t, Size>& values) {
        writer.bytes(values.data(), values.size());
    }

    template <std::size_t Size>
    static void read_bytes(Reader& reader,
                           std::array<std::uint8_t, Size>& values) {
        reader.bytes(values.data(), values.size());
    }

    static void write_cpu(Writer& writer, const Cpu& cpu) {
        writer.u8(cpu.registers_.a);
        writer.u8(cpu.registers_.f);
        writer.u8(cpu.registers_.b);
        writer.u8(cpu.registers_.c);
        writer.u8(cpu.registers_.d);
        writer.u8(cpu.registers_.e);
        writer.u8(cpu.registers_.h);
        writer.u8(cpu.registers_.l);
        writer.u16(cpu.registers_.sp);
        writer.u16(cpu.registers_.pc);
        writer.boolean(cpu.ime_);
        writer.boolean(cpu.halted_);
        writer.boolean(cpu.stopped_);
        writer.boolean(cpu.halt_bug_);
        writer.u32(cpu.ime_enable_delay_);
        writer.u32(cpu.step_cycles_);
        writer.u64(cpu.total_cycles_);
    }

    static void read_cpu(Reader& reader, Cpu& cpu) {
        cpu.registers_.a = reader.u8();
        cpu.registers_.f = static_cast<std::uint8_t>(reader.u8() & 0xF0);
        cpu.registers_.b = reader.u8();
        cpu.registers_.c = reader.u8();
        cpu.registers_.d = reader.u8();
        cpu.registers_.e = reader.u8();
        cpu.registers_.h = reader.u8();
        cpu.registers_.l = reader.u8();
        cpu.registers_.sp = reader.u16();
        cpu.registers_.pc = reader.u16();
        cpu.ime_ = reader.boolean();
        cpu.halted_ = reader.boolean();
        cpu.stopped_ = reader.boolean();
        cpu.halt_bug_ = reader.boolean();
        cpu.ime_enable_delay_ = reader.u32();
        cpu.step_cycles_ = reader.u32();
        cpu.total_cycles_ = reader.u64();
    }

    static void write_bus(Writer& writer, const MemoryBus& bus) {
        write_cartridge(writer, bus.cartridge_);
        write_bytes(writer, bus.wram_);
        write_bytes(writer, bus.io_);
        write_bytes(writer, bus.hram_);
        writer.u8(bus.interrupt_enable_);
        write_joypad(writer, bus.joypad_);
        write_apu(writer, bus.apu_);
        write_ppu(writer, bus.ppu_);
        write_timer(writer, bus.timer_);
        writer.string(bus.serial_output_);
        writer.u32(bus.serial_cycles_remaining_);
        writer.boolean(bus.oam_dma_active_);
        writer.u8(static_cast<std::uint8_t>(bus.oam_dma_source_ >> 8));
        writer.u16(bus.oam_dma_index_);
        writer.u8(static_cast<std::uint8_t>(bus.oam_dma_cycle_));
        writer.u8(static_cast<std::uint8_t>(bus.oam_dma_pending_source_ >> 8));
        writer.u8(static_cast<std::uint8_t>(bus.oam_dma_start_delay_));
        writer.u32(bus.ppu_.mode3_end_dot_);
        writer.u8(bus.ppu_.window_line_);
        writer.boolean(bus.ppu_.window_y_triggered_);
        writer.boolean(bus.ppu_.coincidence_);
        writer.u8(bus.ppu_.stat_mode_);
        writer.boolean(bus.ppu_.lcd_startup_);
        writer.boolean(bus.timer_.reload_happened_);
        writer.u16(bus.serial_clock_);
        write_bytes(writer, *bus.cgb_wram_);
        write_bytes(writer, *bus.ppu_.cgb_vram_);
        write_bytes(writer, bus.ppu_.cgb_bg_palette_);
        write_bytes(writer, bus.ppu_.cgb_object_palette_);
        writer.u8(bus.wram_bank_);
        writer.u8(bus.ppu_.vram_bank_);
        writer.u8(bus.ppu_.bg_palette_index_);
        writer.u8(bus.ppu_.object_palette_index_);
        writer.u16(bus.hdma_source_);
        writer.u16(bus.hdma_destination_);
        writer.u8(bus.hdma_blocks_remaining_);
        writer.boolean(bus.hdma_active_);
        writer.boolean(bus.double_speed_);
        writer.boolean(bus.speed_switch_requested_);
        writer.boolean(bus.cartridge_.has_camera());
        if (bus.cartridge_.has_camera()) {
            writer.boolean(bus.cartridge_.camera_registers_mapped_);
            write_bytes(writer, bus.cartridge_.camera_registers_);
            writer.bytes(bus.cartridge_.camera_frame_.data(),
                         bus.cartridge_.camera_frame_.size());
            writer.bytes(bus.cartridge_.camera_image_.data(),
                         bus.cartridge_.camera_image_.size());
        }
        writer.boolean(bus.ppu_.window_rendered_this_line_);
        writer.u8(bus.ppu_.background_fifo_size_);
        for (const auto& pixel : bus.ppu_.background_fifo_) {
            writer.u8(pixel.color);
            writer.u8(pixel.palette);
            writer.boolean(pixel.priority);
        }
        for (const auto& pixel : bus.ppu_.object_pixels_) {
            writer.u8(pixel.color);
            writer.u8(pixel.attributes);
            writer.u8(pixel.oam_index);
            writer.boolean(pixel.valid);
        }
        write_bytes(writer, bus.ppu_.line_sprites_);
        writer.u8(bus.ppu_.fetcher_phase_);
        writer.u8(bus.ppu_.fetcher_phase_ticks_);
        writer.u8(bus.ppu_.fetcher_tile_index_);
        writer.u8(bus.ppu_.fetched_tile_);
        writer.u8(bus.ppu_.fetched_attributes_);
        writer.u8(bus.ppu_.fetched_low_);
        writer.u8(bus.ppu_.fetched_high_);
        writer.u8(bus.ppu_.fetched_row_);
        writer.u8(bus.ppu_.line_sprite_count_);
        writer.u8(bus.ppu_.next_line_sprite_);
        writer.u8(bus.ppu_.output_x_);
        writer.u8(bus.ppu_.startup_delay_);
        writer.u8(bus.ppu_.scroll_discard_);
        writer.u8(bus.ppu_.window_delay_);
        writer.u8(bus.ppu_.sprite_delay_);
        writer.u16(static_cast<std::uint16_t>(bus.ppu_.fetched_source_x_));
        writer.u16(static_cast<std::uint16_t>(bus.ppu_.previous_sprite_tile_));
        writer.boolean(bus.ppu_.fetched_window_);
        writer.boolean(bus.ppu_.using_window_);
        writer.boolean(bus.ppu_.previous_sprite_was_window_);
        writer.boolean(bus.ppu_.have_previous_sprite_tile_);
        writer.u8(bus.ppu_.window_glitch_x_);
        writer.boolean(bus.ppu_.window_glitch_pending_);
        writer.u16(bus.ppu_.window_source_x_);
        writer.u8(bus.ppu_.window_glitch_applied_x_);
        writer.u32(bus.ppu_.window_glitch_restore_color_);
        writer.boolean(bus.ppu_.window_glitch_applied_);
        writer.u8(bus.ppu_.window_activation_count_);
        writer.u8(bus.ppu_.window_fetch_line_);
        writer.boolean(bus.ppu_.window_retrigger_armed_);
        writer.u8(bus.ppu_.window_fetch_start_x_);
        writer.boolean(bus.ppu_.window_disable_pending_);
        writer.u8(bus.ppu_.window_trigger_x_);
        writer.boolean(bus.ppu_.window_trigger_pending_);
        writer.u16(bus.ppu_.window_disable_source_x_);
        writer.boolean(bus.ppu_.discard_first_fetch_);
        writer.u8(bus.ppu_.fetched_source_y_);
        writer.u8(bus.ppu_.line_sprite_height_);
        writer.u64(bus.ppu_.pending_sprite_mask_);
        writer.u64(bus.ppu_.rendered_sprite_mask_);
        write_bytes(writer, bus.ppu_.pending_sprite_deadlines_);
        write_bytes(writer, bus.ppu_.render_sprite_deadlines_);
        writer.f32(bus.apu_.sample_integrator_left_);
        writer.f32(bus.apu_.sample_integrator_right_);
    }

    static void read_bus(Reader& reader, MemoryBus& bus,
                         const std::uint32_t version) {
        read_cartridge(reader, bus.cartridge_);
        read_bytes(reader, bus.wram_);
        read_bytes(reader, bus.io_);
        read_bytes(reader, bus.hram_);
        bus.interrupt_enable_ = reader.u8();
        read_joypad(reader, bus.joypad_);
        read_apu(reader, bus.apu_, version);
        read_ppu(reader, bus.ppu_);
        read_timer(reader, bus.timer_);
        bus.serial_output_ = reader.string();
        bus.serial_cycles_remaining_ = reader.u32();
        if (version >= 2) {
            bus.oam_dma_active_ = reader.boolean();
            bus.oam_dma_source_ = static_cast<std::uint16_t>(reader.u8() << 8);
            bus.oam_dma_index_ = reader.u16();
            bus.oam_dma_cycle_ = reader.u8();
            bus.oam_dma_pending_source_ =
                static_cast<std::uint16_t>(reader.u8() << 8);
            bus.oam_dma_start_delay_ = reader.u8();
            if (bus.oam_dma_index_ > 0xA0 || bus.oam_dma_cycle_ >= 4 ||
                bus.oam_dma_start_delay_ > 8 ||
                (bus.oam_dma_active_ && bus.oam_dma_index_ == 0xA0)) {
                throw SaveStateError("Save state contains invalid OAM DMA state");
            }
        } else {
            bus.oam_dma_active_ = false;
            bus.oam_dma_source_ = 0;
            bus.oam_dma_index_ = 0;
            bus.oam_dma_cycle_ = 0;
            bus.oam_dma_pending_source_ = 0;
            bus.oam_dma_start_delay_ = 0;
        }
        if (version >= 3) {
            bus.ppu_.mode3_end_dot_ = reader.u32();
            bus.ppu_.window_line_ = reader.u8();
            bus.ppu_.window_y_triggered_ = reader.boolean();
            if (bus.ppu_.mode3_end_dot_ < 252 ||
                bus.ppu_.mode3_end_dot_ > 369 ||
                bus.ppu_.window_line_ > Ppu::screen_height) {
                throw SaveStateError("Save state contains invalid PPU timing state");
            }
        } else {
            bus.ppu_.mode3_end_dot_ = 252;
            bus.ppu_.window_y_triggered_ = bus.ppu_.ly_ >= bus.ppu_.window_y_;
            bus.ppu_.window_line_ = bus.ppu_.window_y_triggered_
                                        ? static_cast<std::uint8_t>(
                                              bus.ppu_.ly_ - bus.ppu_.window_y_)
                                        : 0;
        }
        if (version >= 5) {
            bus.ppu_.coincidence_ = reader.boolean();
        } else {
            bus.ppu_.coincidence_ = bus.ppu_.ly_ == bus.ppu_.lyc_;
        }
        if (version >= 6) {
            bus.ppu_.stat_mode_ = reader.u8();
            if (bus.ppu_.stat_mode_ > 3) {
                throw SaveStateError("invalid PPU STAT mode");
            }
            bus.ppu_.lcd_startup_ = reader.boolean();
            bus.timer_.reload_happened_ = reader.boolean();
            bus.serial_clock_ = reader.u16();
        } else {
            bus.ppu_.stat_mode_ = bus.ppu_.mode_;
            bus.ppu_.lcd_startup_ = false;
            bus.timer_.reload_happened_ = false;
            bus.serial_clock_ = 0;
        }
        if (version >= 4) {
            read_bytes(reader, *bus.cgb_wram_);
            read_bytes(reader, *bus.ppu_.cgb_vram_);
            read_bytes(reader, bus.ppu_.cgb_bg_palette_);
            read_bytes(reader, bus.ppu_.cgb_object_palette_);
            bus.wram_bank_ = reader.u8();
            bus.ppu_.vram_bank_ = reader.u8();
            bus.ppu_.bg_palette_index_ = reader.u8();
            bus.ppu_.object_palette_index_ = reader.u8();
            bus.hdma_source_ = reader.u16();
            bus.hdma_destination_ = reader.u16();
            bus.hdma_blocks_remaining_ = reader.u8();
            bus.hdma_active_ = reader.boolean();
            bus.double_speed_ = reader.boolean();
            bus.speed_switch_requested_ = reader.boolean();
            if (bus.wram_bank_ < 1 || bus.wram_bank_ > 7 ||
                bus.ppu_.vram_bank_ > 1 ||
                (bus.ppu_.bg_palette_index_ & 0x40) != 0 ||
                (bus.ppu_.object_palette_index_ & 0x40) != 0 ||
                bus.hdma_destination_ < 0x8000 ||
                bus.hdma_destination_ > 0x9FFF ||
                (bus.hdma_destination_ & 0x000F) != 0 ||
                (bus.hdma_source_ & 0x000F) != 0 ||
                (bus.hdma_active_ && bus.hdma_blocks_remaining_ == 0) ||
                (!bus.cgb_mode_ &&
                 (bus.double_speed_ || bus.speed_switch_requested_))) {
                throw SaveStateError("Save state contains invalid CGB bank state");
            }
            bus.timer_.set_double_speed(bus.double_speed_);
        } else {
            bus.cgb_wram_->fill(0);
            bus.ppu_.cgb_vram_->fill(0);
            bus.ppu_.cgb_bg_palette_.fill(0xFF);
            bus.ppu_.cgb_object_palette_.fill(0xFF);
            bus.wram_bank_ = 1;
            bus.ppu_.vram_bank_ = 0;
            bus.ppu_.bg_palette_index_ = 0;
            bus.ppu_.object_palette_index_ = 0;
            bus.hdma_source_ = 0;
            bus.hdma_destination_ = 0x8000;
            bus.hdma_blocks_remaining_ = 0;
            bus.hdma_active_ = false;
            bus.double_speed_ = false;
            bus.speed_switch_requested_ = false;
            bus.timer_.set_double_speed(false);
        }
        if (version >= 7) {
            const auto contains_camera = reader.boolean();
            if (contains_camera != bus.cartridge_.has_camera()) {
                throw SaveStateError(
                    "Save-state camera hardware does not match cartridge");
            }
            if (contains_camera) {
                bus.cartridge_.camera_registers_mapped_ = reader.boolean();
                read_bytes(reader, bus.cartridge_.camera_registers_);
                reader.bytes(bus.cartridge_.camera_frame_.data(),
                             bus.cartridge_.camera_frame_.size());
                reader.bytes(bus.cartridge_.camera_image_.data(),
                             bus.cartridge_.camera_image_.size());
                bus.cartridge_.camera_image_dirty_ = true;
            }
        } else if (bus.cartridge_.has_camera()) {
            bus.cartridge_.camera_registers_mapped_ = false;
            bus.cartridge_.camera_registers_.fill(0);
            bus.cartridge_.capture_camera_image();
        }
        if (version >= 8) {
            bus.ppu_.window_rendered_this_line_ = reader.boolean();
        } else {
            bus.ppu_.window_rendered_this_line_ = false;
        }
        if (version >= 9) {
            bus.ppu_.background_fifo_size_ = reader.u8();
            for (auto& pixel : bus.ppu_.background_fifo_) {
                pixel.color = reader.u8();
                pixel.palette = reader.u8();
                pixel.priority = reader.boolean();
            }
            for (auto& pixel : bus.ppu_.object_pixels_) {
                pixel.color = reader.u8();
                pixel.attributes = reader.u8();
                pixel.oam_index = reader.u8();
                pixel.valid = reader.boolean();
            }
            read_bytes(reader, bus.ppu_.line_sprites_);
            bus.ppu_.fetcher_phase_ = reader.u8();
            bus.ppu_.fetcher_phase_ticks_ = reader.u8();
            bus.ppu_.fetcher_tile_index_ = reader.u8();
            bus.ppu_.fetched_tile_ = reader.u8();
            bus.ppu_.fetched_attributes_ = reader.u8();
            bus.ppu_.fetched_low_ = reader.u8();
            bus.ppu_.fetched_high_ = reader.u8();
            bus.ppu_.fetched_row_ = reader.u8();
            bus.ppu_.line_sprite_count_ = reader.u8();
            bus.ppu_.next_line_sprite_ = reader.u8();
            bus.ppu_.output_x_ = reader.u8();
            bus.ppu_.startup_delay_ = reader.u8();
            bus.ppu_.scroll_discard_ = reader.u8();
            bus.ppu_.window_delay_ = reader.u8();
            bus.ppu_.sprite_delay_ = reader.u8();
            bus.ppu_.fetched_source_x_ =
                static_cast<std::int16_t>(reader.u16());
            bus.ppu_.previous_sprite_tile_ =
                static_cast<std::int16_t>(reader.u16());
            bus.ppu_.fetched_window_ = reader.boolean();
            bus.ppu_.using_window_ = reader.boolean();
            bus.ppu_.previous_sprite_was_window_ = reader.boolean();
            bus.ppu_.have_previous_sprite_tile_ = reader.boolean();
            bus.ppu_.window_glitch_x_ = reader.u8();
            bus.ppu_.window_glitch_pending_ = reader.boolean();
            bus.ppu_.window_source_x_ = reader.u16();
            bus.ppu_.window_glitch_applied_x_ = reader.u8();
            bus.ppu_.window_glitch_restore_color_ = reader.u32();
            bus.ppu_.window_glitch_applied_ = reader.boolean();
            bus.ppu_.window_activation_count_ = reader.u8();
            bus.ppu_.window_fetch_line_ = reader.u8();
            bus.ppu_.window_retrigger_armed_ = reader.boolean();
            bus.ppu_.window_fetch_start_x_ = reader.u8();
            bus.ppu_.window_disable_pending_ = reader.boolean();
            if (version >= 10) {
                bus.ppu_.window_trigger_x_ = reader.u8();
                bus.ppu_.window_trigger_pending_ = reader.boolean();
                bus.ppu_.window_disable_source_x_ = reader.u16();
            } else {
                bus.ppu_.window_trigger_x_ = 0;
                bus.ppu_.window_trigger_pending_ = false;
                bus.ppu_.window_disable_source_x_ =
                    bus.ppu_.window_disable_pending_
                        ? static_cast<std::uint16_t>(
                              (bus.ppu_.window_source_x_ + 7U) & ~7U)
                        : std::uint16_t{0};
            }
            bus.ppu_.discard_first_fetch_ =
                version >= 11 ? reader.boolean() : false;
            bus.ppu_.fetched_source_y_ =
                version >= 11 ? reader.u8() : bus.ppu_.fetched_row_;
            bus.ppu_.line_sprite_height_ =
                version >= 12 ? reader.u8()
                              : ((bus.ppu_.lcdc_ & 0x04) != 0 ? 16 : 8);
            bus.ppu_.pending_sprite_mask_ =
                version >= 13 ? reader.u64() : 0;
            bus.ppu_.rendered_sprite_mask_ =
                version >= 15 ? reader.u64() : 0;
            if (version >= 14) {
                read_bytes(reader, bus.ppu_.pending_sprite_deadlines_);
            } else {
                bus.ppu_.pending_sprite_deadlines_.fill(0);
                for (unsigned index = 0; index < 40; ++index) {
                    if ((bus.ppu_.pending_sprite_mask_ &
                         (std::uint64_t{1} << index)) != 0) {
                        // Version 13 represented the queue as one aggregate
                        // completion point. Preserve that behavior when
                        // loading it into the per-sprite representation.
                        bus.ppu_.pending_sprite_deadlines_[index] =
                            bus.ppu_.sprite_delay_;
                    }
                }
            }
            if (version >= 15) {
                read_bytes(reader, bus.ppu_.render_sprite_deadlines_);
            } else {
                bus.ppu_.render_sprite_deadlines_.fill(0);
                for (unsigned index = 0; index < 40; ++index) {
                    if ((bus.ppu_.pending_sprite_mask_ &
                         (std::uint64_t{1} << index)) != 0) {
                        bus.ppu_.render_sprite_deadlines_[index] =
                            bus.ppu_.pending_sprite_deadlines_[index];
                    }
                }
            }
            if (bus.ppu_.background_fifo_size_ >
                    bus.ppu_.background_fifo_.size() ||
                bus.ppu_.fetcher_phase_ > 4 ||
                bus.ppu_.fetcher_phase_ticks_ > 2 ||
                bus.ppu_.line_sprite_count_ > bus.ppu_.line_sprites_.size() ||
                bus.ppu_.next_line_sprite_ > bus.ppu_.line_sprite_count_ ||
                bus.ppu_.output_x_ > Ppu::screen_width ||
                bus.ppu_.scroll_discard_ > 7 ||
                bus.ppu_.window_glitch_x_ >= Ppu::screen_width ||
                bus.ppu_.window_glitch_applied_x_ >= Ppu::screen_width ||
                bus.ppu_.window_trigger_x_ >= Ppu::screen_width ||
                bus.ppu_.window_disable_source_x_ > 256) {
                throw SaveStateError("Save state contains invalid PPU fetcher state");
            }
            if (bus.ppu_.line_sprite_height_ != 8 &&
                bus.ppu_.line_sprite_height_ != 16) {
                throw SaveStateError("Save state contains invalid sprite height");
            }
            if ((bus.ppu_.pending_sprite_mask_ >> 40) != 0) {
                throw SaveStateError("Save state contains invalid sprite fetch mask");
            }
            if ((bus.ppu_.rendered_sprite_mask_ >> 40) != 0) {
                throw SaveStateError("Save state contains invalid rendered sprite mask");
            }
            for (unsigned index = 0; index < bus.ppu_.pending_sprite_deadlines_.size();
                 ++index) {
                const auto deadline = bus.ppu_.pending_sprite_deadlines_[index];
                const auto pending =
                    (bus.ppu_.pending_sprite_mask_ & (std::uint64_t{1} << index)) != 0;
                const auto render_deadline = bus.ppu_.render_sprite_deadlines_[index];
                const auto rendered =
                    (bus.ppu_.rendered_sprite_mask_ &
                     (std::uint64_t{1} << index)) != 0;
                if ((pending && deadline == 0) || (!pending && deadline != 0) ||
                    (rendered && render_deadline != 0) ||
                    (!rendered && render_deadline == 0 && pending)) {
                    throw SaveStateError("Save state contains invalid sprite fetch deadline");
                }
            }
        } else {
            bus.ppu_.background_fifo_.fill(Ppu::BackgroundPixel{});
            bus.ppu_.object_pixels_.fill(Ppu::ObjectPixel{});
            bus.ppu_.line_sprites_.fill(0);
            bus.ppu_.background_fifo_size_ = 0;
            bus.ppu_.fetcher_phase_ = 0;
            bus.ppu_.fetcher_phase_ticks_ = 2;
            bus.ppu_.fetcher_tile_index_ = 0;
            bus.ppu_.fetched_tile_ = 0;
            bus.ppu_.fetched_attributes_ = 0;
            bus.ppu_.fetched_low_ = 0;
            bus.ppu_.fetched_high_ = 0;
            bus.ppu_.fetched_row_ = 0;
            bus.ppu_.fetched_source_y_ = 0;
            bus.ppu_.line_sprite_height_ = 8;
            bus.ppu_.pending_sprite_mask_ = 0;
            bus.ppu_.rendered_sprite_mask_ = 0;
            bus.ppu_.pending_sprite_deadlines_.fill(0);
            bus.ppu_.render_sprite_deadlines_.fill(0);
            bus.ppu_.line_sprite_count_ = 0;
            bus.ppu_.next_line_sprite_ = 0;
            bus.ppu_.output_x_ = 0;
            bus.ppu_.startup_delay_ = 0;
            bus.ppu_.scroll_discard_ = 0;
            bus.ppu_.window_delay_ = 0;
            bus.ppu_.sprite_delay_ = 0;
            bus.ppu_.fetched_source_x_ = 0;
            bus.ppu_.previous_sprite_tile_ = 0;
            bus.ppu_.fetched_window_ = false;
            bus.ppu_.using_window_ = false;
            bus.ppu_.previous_sprite_was_window_ = false;
            bus.ppu_.have_previous_sprite_tile_ = false;
            bus.ppu_.window_glitch_x_ = 0;
            bus.ppu_.window_glitch_pending_ = false;
            bus.ppu_.window_source_x_ = 0;
            bus.ppu_.window_glitch_applied_x_ = 0;
            bus.ppu_.window_glitch_restore_color_ = 0;
            bus.ppu_.window_glitch_applied_ = false;
            bus.ppu_.window_activation_count_ = 0;
            bus.ppu_.window_fetch_line_ = bus.ppu_.window_line_;
            bus.ppu_.window_retrigger_armed_ = false;
            bus.ppu_.window_fetch_start_x_ = 0;
            bus.ppu_.window_disable_pending_ = false;
            bus.ppu_.window_trigger_x_ = 0;
            bus.ppu_.window_trigger_pending_ = false;
            bus.ppu_.window_disable_source_x_ = 0;
            bus.ppu_.discard_first_fetch_ = false;
        }
        if (version >= 16) {
            bus.apu_.sample_integrator_left_ = reader.f32();
            bus.apu_.sample_integrator_right_ = reader.f32();
        }
        // The printer represents an external device and is deliberately not
        // embedded in emulator save states. Loading a state starts a fresh
        // printer session while preserving whether it is connected.
        if (bus.printer_connected_) bus.printer_.reset();
    }

    static void write_cartridge(Writer& writer, const Cartridge& cartridge) {
        writer.u32(static_cast<std::uint32_t>(cartridge.ram_.size()));
        if (!cartridge.ram_.empty()) {
            writer.bytes(cartridge.ram_.data(), cartridge.ram_.size());
        }
        writer.boolean(cartridge.rumble_active_);
        writer.boolean(cartridge.ram_enabled_);
        writer.u8(cartridge.rom_bank_low_);
        writer.u8(cartridge.bank_upper_);
        writer.u8(cartridge.banking_mode_);
        writer.u16(cartridge.selected_rom_bank_);
        writer.u8(cartridge.ram_rtc_select_);
        writer.u8(cartridge.rtc_latch_value_);
        write_bytes(writer, cartridge.rtc_);
        write_bytes(writer, cartridge.latched_rtc_);
    }

    static void read_cartridge(Reader& reader, Cartridge& cartridge) {
        const auto ram_size = static_cast<std::size_t>(reader.u32());
        if (ram_size != cartridge.ram_.size()) {
            throw SaveStateError("Save-state cartridge RAM size does not match");
        }
        if (!cartridge.ram_.empty()) {
            reader.bytes(cartridge.ram_.data(), cartridge.ram_.size());
        }
        cartridge.rumble_active_ = reader.boolean() && cartridge.rumble_present_;
        cartridge.ram_enabled_ = reader.boolean();
        cartridge.rom_bank_low_ = reader.u8();
        cartridge.bank_upper_ = reader.u8();
        cartridge.banking_mode_ = reader.u8();
        cartridge.selected_rom_bank_ = reader.u16();
        cartridge.ram_rtc_select_ = reader.u8();
        cartridge.rtc_latch_value_ = reader.u8();
        read_bytes(reader, cartridge.rtc_);
        read_bytes(reader, cartridge.latched_rtc_);
        cartridge.rtc_last_update_ = current_unix_seconds();
        cartridge.ram_dirty_ = cartridge.battery_ && !cartridge.ram_.empty();
        cartridge.rtc_dirty_ = cartridge.rtc_present_;
    }

    static void write_joypad(Writer& writer, const Joypad& joypad) {
        writer.u8(joypad.select_);
        writer.u8(joypad.directions_);
        writer.u8(joypad.actions_);
    }

    static void read_joypad(Reader& reader, Joypad& joypad) {
        joypad.select_ = static_cast<std::uint8_t>(reader.u8() & 0x30);
        joypad.directions_ = static_cast<std::uint8_t>(reader.u8() & 0x0F);
        joypad.actions_ = static_cast<std::uint8_t>(reader.u8() & 0x0F);
    }

    static void write_timer(Writer& writer, const Timer& timer) {
        writer.u16(timer.divider_counter_);
        writer.u8(timer.counter_);
        writer.u8(timer.modulo_);
        writer.u8(timer.control_);
        writer.u32(timer.reload_delay_);
        writer.u32(timer.apu_ticks_);
    }

    static void read_timer(Reader& reader, Timer& timer) {
        timer.divider_counter_ = reader.u16();
        timer.counter_ = reader.u8();
        timer.modulo_ = reader.u8();
        timer.control_ = static_cast<std::uint8_t>(reader.u8() & 0x07);
        timer.reload_delay_ = reader.u32();
        timer.apu_ticks_ = reader.u32();
    }

    static void write_ppu(Writer& writer, const Ppu& ppu) {
        write_bytes(writer, ppu.vram_);
        write_bytes(writer, ppu.oam_);
        for (const auto pixel : *ppu.framebuffer_) writer.u32(pixel);
        writer.u8(ppu.lcdc_);
        writer.u8(ppu.stat_select_);
        writer.u8(ppu.scy_);
        writer.u8(ppu.scx_);
        writer.u8(ppu.ly_);
        writer.u8(ppu.lyc_);
        writer.u8(ppu.bg_palette_);
        writer.u8(ppu.object_palette_0_);
        writer.u8(ppu.object_palette_1_);
        writer.u8(ppu.window_y_);
        writer.u8(ppu.window_x_);
        writer.u32(ppu.dot_);
        writer.u8(ppu.mode_);
        writer.boolean(ppu.stat_line_);
        writer.boolean(ppu.frame_ready_);
    }

    static void read_ppu(Reader& reader, Ppu& ppu) {
        read_bytes(reader, ppu.vram_);
        read_bytes(reader, ppu.oam_);
        for (auto& pixel : *ppu.framebuffer_) pixel = reader.u32();
        ppu.lcdc_ = reader.u8();
        ppu.stat_select_ = static_cast<std::uint8_t>(reader.u8() & 0x78);
        ppu.scy_ = reader.u8();
        ppu.scx_ = reader.u8();
        ppu.ly_ = reader.u8();
        ppu.lyc_ = reader.u8();
        ppu.bg_palette_ = reader.u8();
        ppu.object_palette_0_ = reader.u8();
        ppu.object_palette_1_ = reader.u8();
        ppu.window_y_ = reader.u8();
        ppu.window_x_ = reader.u8();
        ppu.dot_ = reader.u32();
        ppu.mode_ = reader.u8();
        ppu.stat_line_ = reader.boolean();
        ppu.frame_ready_ = reader.boolean();
    }

    static void write_envelope(Writer& writer,
                               const Apu::EnvelopeState& envelope) {
        writer.boolean(envelope.running);
        writer.u8(envelope.volume);
        writer.u8(envelope.timer);
    }

    static void read_envelope(Reader& reader, Apu::EnvelopeState& envelope) {
        envelope.running = reader.boolean();
        envelope.volume = reader.u8();
        envelope.timer = reader.u8();
    }

    static void write_pulse(Writer& writer, const Apu::PulseState& pulse) {
        writer.boolean(pulse.enabled);
        writer.boolean(pulse.dac_enabled);
        writer.boolean(pulse.length_enabled);
        writer.u8(pulse.length);
        writer.u8(pulse.duty_step);
        writer.u32(pulse.timer);
        write_envelope(writer, pulse.envelope);
    }

    static void read_pulse(Reader& reader, Apu::PulseState& pulse) {
        pulse.enabled = reader.boolean();
        pulse.dac_enabled = reader.boolean();
        pulse.length_enabled = reader.boolean();
        pulse.length = reader.u8();
        pulse.duty_step = reader.u8();
        pulse.timer = reader.u32();
        read_envelope(reader, pulse.envelope);
    }

    static void write_apu(Writer& writer, const Apu& apu) {
        write_bytes(writer, apu.registers_);
        write_bytes(writer, apu.wave_ram_);
        writer.boolean(apu.powered_);
        write_pulse(writer, apu.pulse1_);
        write_pulse(writer, apu.pulse2_);
        writer.boolean(apu.wave_.enabled);
        writer.boolean(apu.wave_.dac_enabled);
        writer.boolean(apu.wave_.length_enabled);
        writer.u16(apu.wave_.length);
        writer.u8(apu.wave_.position);
        writer.u8(apu.wave_.sample);
        writer.u32(apu.wave_.timer);
        writer.boolean(apu.wave_.clock_phase);
        writer.boolean(apu.wave_.wave_ram_accessible);
        writer.boolean(apu.noise_.enabled);
        writer.boolean(apu.noise_.dac_enabled);
        writer.boolean(apu.noise_.length_enabled);
        writer.u8(apu.noise_.length);
        writer.u16(apu.noise_.lfsr);
        writer.u32(apu.noise_.timer);
        write_envelope(writer, apu.noise_.envelope);
        writer.u16(apu.sweep_shadow_frequency_);
        writer.u8(apu.sweep_timer_);
        writer.boolean(apu.sweep_enabled_);
        writer.boolean(apu.sweep_negated_);
        writer.u8(apu.frame_sequencer_step_);
        writer.u32(apu.sample_accumulator_);
        writer.f32(apu.left_capacitor_);
        writer.f32(apu.right_capacitor_);
    }

    static void read_apu(Reader& reader, Apu& apu,
                         const std::uint32_t /*version*/) {
        read_bytes(reader, apu.registers_);
        read_bytes(reader, apu.wave_ram_);
        apu.samples_.clear();
        apu.powered_ = reader.boolean();
        read_pulse(reader, apu.pulse1_);
        read_pulse(reader, apu.pulse2_);
        apu.wave_.enabled = reader.boolean();
        apu.wave_.dac_enabled = reader.boolean();
        apu.wave_.length_enabled = reader.boolean();
        apu.wave_.length = reader.u16();
        apu.wave_.position = reader.u8();
        apu.wave_.sample = reader.u8();
        apu.wave_.timer = reader.u32();
        apu.wave_.clock_phase = reader.boolean();
        apu.wave_.wave_ram_accessible = reader.boolean();
        apu.noise_.enabled = reader.boolean();
        apu.noise_.dac_enabled = reader.boolean();
        apu.noise_.length_enabled = reader.boolean();
        apu.noise_.length = reader.u8();
        apu.noise_.lfsr = reader.u16();
        apu.noise_.timer = reader.u32();
        read_envelope(reader, apu.noise_.envelope);
        apu.sweep_shadow_frequency_ = reader.u16();
        apu.sweep_timer_ = reader.u8();
        apu.sweep_enabled_ = reader.boolean();
        apu.sweep_negated_ = reader.boolean();
        apu.frame_sequencer_step_ = reader.u8();
        apu.sample_accumulator_ = reader.u32();
        apu.left_capacitor_ = reader.f32();
        apu.right_capacitor_ = reader.f32();
        apu.sample_integrator_left_ = 0.0F;
        apu.sample_integrator_right_ = 0.0F;
    }
};

std::uint64_t Emulator::rom_fingerprint() const noexcept {
    return bus_.cartridge().rom_fingerprint();
}

std::vector<std::uint8_t> Emulator::save_state() const {
    return SaveStateCodec::encode(*this);
}

void Emulator::load_state(const std::vector<std::uint8_t>& state) {
    const auto backup = save_state();
    try {
        SaveStateCodec::decode(*this, state);
    } catch (...) {
        try {
            SaveStateCodec::decode(*this, backup);
        } catch (...) {
            // The in-memory backup was produced by this exact codec and ROM.
        }
        throw;
    }
}

} // namespace gameboy
