#include "gbb/scene_json.hpp"

#include <fstream>
#include <string_view>
#include <type_traits>
#include <utility>

namespace gbb {
namespace {

class JsonWriter {
public:
    void begin_object() { append('{'); }
    void end_object() { trim_comma(); append('}'); }
    void begin_array() { append('['); }
    void end_array() { trim_comma(); append(']'); }

    void key(const std::string_view name) {
        append_quoted(name);
        append(':');
    }

    template <typename T>
    void number(const T value) {
        static_assert(std::is_integral_v<T>);
        output_ += std::to_string(value);
        append(',');
    }

    void boolean(const bool value) {
        output_ += value ? "true" : "false";
        append(',');
    }

    void string(const std::string_view value) {
        append_quoted(value);
        append(',');
    }

    void raw(const std::string_view value) { output_ += value; }
    void comma() { append(','); }
    [[nodiscard]] std::string finish() && { return std::move(output_); }

private:
    void append_quoted(const std::string_view value) {
        append('"');
        for (const auto character : value) {
            switch (character) {
            case '"': output_ += "\\\""; break;
            case '\\': output_ += "\\\\"; break;
            case '\n': output_ += "\\n"; break;
            case '\r': output_ += "\\r"; break;
            case '\t': output_ += "\\t"; break;
            default: output_ += character; break;
            }
        }
        append('"');
    }
    void append(const char character) { output_ += character; }
    void trim_comma() {
        if (!output_.empty() && output_.back() == ',') output_.pop_back();
    }

    std::string output_;
};

template <typename T, typename Writer>
void write_array(JsonWriter& json, const T& values, Writer writer) {
    json.begin_array();
    for (const auto& value : values) writer(json, value);
    json.end_array();
    json.comma();
}

void write_tile_layer(JsonWriter& json, const SceneTileLayer& layer) {
    json.begin_object();
    json.key("enabled"); json.boolean(layer.enabled);
    json.key("map_address"); json.number(layer.map_address);
    json.key("tile_data_unsigned"); json.boolean(layer.tile_data_unsigned);
    json.key("width"); json.number(layer.width);
    json.key("height"); json.number(layer.height);
    json.key("tile_ids");
    write_array(json, layer.tile_ids,
                [](JsonWriter& output, const std::uint8_t value) {
                    output.number(value);
                });
    json.key("attributes");
    write_array(json, layer.attributes,
                [](JsonWriter& output, const std::uint8_t value) {
                    output.number(value);
                });
    json.end_object();
    json.comma();
}

void write_sprite(JsonWriter& json, const SceneSprite& sprite) {
    json.begin_object();
    json.key("oam_y"); json.number(sprite.oam_y);
    json.key("oam_x"); json.number(sprite.oam_x);
    json.key("tile"); json.number(sprite.tile);
    json.key("attributes"); json.number(sprite.attributes);
    json.key("screen_x"); json.number(sprite.screen_x);
    json.key("screen_y"); json.number(sprite.screen_y);
    json.key("visible"); json.boolean(sprite.visible);
    json.end_object();
    json.comma();
}

} // namespace

std::string scene_snapshot_to_json(const SceneSnapshot& scene) {
    JsonWriter json;
    json.begin_object();
    json.key("schema"); json.string("gbb.scene.v1");
    json.key("emulation_cycles"); json.number(scene.emulation_cycles);
    json.key("width"); json.number(scene.width);
    json.key("height"); json.number(scene.height);
    json.key("cgb_mode"); json.boolean(scene.cgb_mode);
    json.key("lcdc"); json.number(scene.lcdc);
    json.key("scx"); json.number(scene.scx);
    json.key("scy"); json.number(scene.scy);
    json.key("wx"); json.number(scene.wx);
    json.key("wy"); json.number(scene.wy);
    json.key("bg_palette"); json.number(scene.bg_palette);
    json.key("object_palette_0"); json.number(scene.object_palette_0);
    json.key("object_palette_1"); json.number(scene.object_palette_1);
    json.key("bg_palette_index"); json.number(scene.bg_palette_index);
    json.key("object_palette_index"); json.number(scene.object_palette_index);
    json.key("background"); write_tile_layer(json, scene.background);
    json.key("window"); write_tile_layer(json, scene.window);
    json.key("tile_size_bytes"); json.number(scene.tile_size_bytes);
    json.key("tile_count"); json.number(scene.tile_count);
    json.key("tile_banks"); json.number(scene.tile_banks);
    json.key("tile_bank_stride"); json.number(scene.tile_bank_stride);
    json.key("tile_data");
    write_array(json, scene.tile_data,
                [](JsonWriter& output, const std::uint8_t value) {
                    output.number(value);
                });
    json.key("cgb_bg_palette");
    write_array(json, scene.cgb_bg_palette,
                [](JsonWriter& output, const std::uint8_t value) {
                    output.number(value);
                });
    json.key("cgb_object_palette");
    write_array(json, scene.cgb_object_palette,
                [](JsonWriter& output, const std::uint8_t value) {
                    output.number(value);
                });
    json.key("sprites");
    write_array(json, scene.sprites,
                [](JsonWriter& output, const SceneSprite& value) {
                    write_sprite(output, value);
                });
    json.end_object();
    json.raw("\n");
    return std::move(json).finish();
}

bool write_scene_snapshot_json(const SceneSnapshot& scene,
                               const std::filesystem::path& path) {
    if (path.empty()) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << scene_snapshot_to_json(scene);
    return static_cast<bool>(output);
}

} // namespace gbb
