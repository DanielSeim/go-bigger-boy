#include "save_state_ppu.hpp"

#include "gameboy/ppu.hpp"

namespace gameboy {
namespace {

template <std::size_t Size>
void write_bytes(save_state_format::Writer& writer,
                 const std::array<std::uint8_t, Size>& values) {
    writer.bytes(values.data(), values.size());
}

template <std::size_t Size>
void read_bytes(save_state_format::Reader& reader,
                std::array<std::uint8_t, Size>& values) {
    reader.bytes(values.data(), values.size());
}

} // namespace

void SaveStatePpuCodec::write(save_state_format::Writer& writer,
                              const Ppu& ppu) {
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

void SaveStatePpuCodec::read(save_state_format::Reader& reader, Ppu& ppu) {
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

} // namespace gameboy
