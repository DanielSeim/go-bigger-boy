#include "gbb/core_contract.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

namespace gbb {
namespace {

bool fail(std::string& error, const char* message) {
    error = message;
    return false;
}

bool valid_input_id(const InputId id) noexcept {
    switch (id) {
    case InputId::right:
    case InputId::left:
    case InputId::up:
    case InputId::down:
    case InputId::a:
    case InputId::b:
    case InputId::x:
    case InputId::y:
    case InputId::l:
    case InputId::r:
    case InputId::select:
    case InputId::start: return true;
    }
    return false;
}

bool valid_system_id(const SystemId id) noexcept {
    switch (id) {
    case SystemId::game_boy:
    case SystemId::game_boy_color:
    case SystemId::game_boy_advance: return true;
    case SystemId::unknown: return false;
    }
    return false;
}

constexpr std::uint64_t known_capability_bits =
    static_cast<std::uint64_t>(CoreCapability::persistent_memory) |
    static_cast<std::uint64_t>(CoreCapability::rtc) |
    static_cast<std::uint64_t>(CoreCapability::rumble) |
    static_cast<std::uint64_t>(CoreCapability::camera) |
    static_cast<std::uint64_t>(CoreCapability::printer) |
    static_cast<std::uint64_t>(CoreCapability::compatibility_palette) |
    static_cast<std::uint64_t>(CoreCapability::cheats) |
    static_cast<std::uint64_t>(CoreCapability::debugger) |
    static_cast<std::uint64_t>(CoreCapability::sprite_editor) |
    static_cast<std::uint64_t>(CoreCapability::scene_layers) |
    static_cast<std::uint64_t>(CoreCapability::link_cable);

bool validate_tile_layer(const SceneTileLayer& layer, std::string& error) {
    if ((layer.width == 0) != (layer.height == 0)) {
        return fail(error, "scene tile-layer dimensions must be both zero or non-zero");
    }
    if (layer.width == 0) {
        if (!layer.tile_ids.empty() || !layer.attributes.empty()) {
            return fail(error, "empty scene tile-layer has tile data");
        }
        return true;
    }
    if (layer.width > std::numeric_limits<std::size_t>::max() / layer.height) {
        return fail(error, "scene tile-layer dimensions overflow the tile count");
    }
    const auto tile_count = layer.width * layer.height;
    if (layer.tile_ids.size() != tile_count ||
        layer.attributes.size() != tile_count) {
        return fail(error, "scene tile-layer data does not match its dimensions");
    }
    return true;
}

bool validate_tile_buffer(const SceneSnapshot& scene, std::string& error) {
    const auto has_metadata = scene.tile_size_bytes != 0 ||
                              scene.tile_count != 0 ||
                              scene.tile_banks != 0 ||
                              scene.tile_bank_stride != 0 ||
                              !scene.tile_data.empty();
    if (!has_metadata) return true;
    if (scene.tile_size_bytes == 0 || scene.tile_count == 0 ||
        scene.tile_banks == 0) {
        return fail(error, "scene tile metadata is incomplete");
    }
    if (scene.tile_count >
        std::numeric_limits<std::size_t>::max() / scene.tile_size_bytes) {
        return fail(error, "scene tile metadata overflows the bank stride");
    }
    const auto expected_stride = scene.tile_count * scene.tile_size_bytes;
    if (scene.tile_bank_stride != expected_stride ||
        scene.tile_banks >
            std::numeric_limits<std::size_t>::max() / expected_stride) {
        return fail(error, "scene tile metadata has an invalid bank stride");
    }
    if (scene.tile_data.size() != scene.tile_banks * expected_stride) {
        return fail(error, "scene tile data does not match its metadata");
    }
    return true;
}

bool validate_scene_contract(const EmulatorCore& core,
                            const CoreDescriptor& descriptor,
                            std::string& error) {
    if (descriptor.scene_layer_format_count != 0 &&
        descriptor.scene_layer_formats == nullptr) {
        return fail(error, "scene layer format descriptors are missing");
    }
    if (descriptor.scene_layer_format_count != 0 &&
        !has_capability(descriptor.capabilities, CoreCapability::scene_layers)) {
        return fail(error, "scene layer formats require the scene_layers capability");
    }
    for (std::size_t index = 0; index < descriptor.scene_layer_format_count;
         ++index) {
        if (descriptor.scene_layer_formats[index].empty()) {
            return fail(error, "scene layer format descriptor is empty");
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (descriptor.scene_layer_formats[previous] ==
                descriptor.scene_layer_formats[index]) {
                return fail(error, "scene layer format descriptors contain a duplicate");
            }
        }
    }
    if (!has_capability(descriptor.capabilities, CoreCapability::scene_layers)) {
        return true;
    }
    const auto& scene = core.scene_snapshot();
    if (scene.schema_version == 0) {
        return fail(error, "scene schema version must be non-zero");
    }
    if (scene.width != descriptor.video_width ||
        scene.height != descriptor.video_height) {
        return fail(error, "scene dimensions do not match the descriptor");
    }
    if (!validate_tile_layer(scene.background, error) ||
        !validate_tile_layer(scene.window, error) ||
        !validate_tile_buffer(scene, error)) {
        return false;
    }
    for (const auto& layer : scene.layers) {
        if (layer.id.empty() || layer.format.empty()) {
            return fail(error, "scene layer requires an id and format");
        }
        if ((layer.width == 0) != (layer.height == 0)) {
            return fail(error, "scene layer dimensions must be both zero or non-zero");
        }
        if (layer.width != 0 &&
            layer.height > std::numeric_limits<std::size_t>::max() /
                               layer.width) {
            return fail(error, "scene layer dimensions overflow the pixel count");
        }
        if (layer.width != 0 && layer.payload.empty()) {
            return fail(error, "scene layer payload is empty");
        }
    }
    for (std::size_t index = 0; index < scene.layers.size(); ++index) {
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (scene.layers[previous].id == scene.layers[index].id) {
                return fail(error, "scene layers contain a duplicate id");
            }
        }
    }
    if (!scene.layers.empty() && descriptor.scene_layer_format_count == 0) {
        return fail(error, "scene layer formats must be advertised");
    }
    for (const auto& layer : scene.layers) {
        if (!scene_layer_format_advertised(descriptor, layer.format)) {
            return fail(error, "scene layer format is not advertised");
        }
    }
    return true;
}

} // namespace

bool validate_core_contract(const EmulatorCore& core, std::string& error) {
    error.clear();
    const auto& descriptor = core.descriptor();
    if (descriptor.api_version != core_api_version) {
        return fail(error, "unsupported core API version");
    }
    if (descriptor.core_id.empty()) return fail(error, "core id is empty");
    if (descriptor.core_name.empty()) return fail(error, "core name is empty");
    if (!valid_system_id(descriptor.system)) {
        return fail(error, "core system id is unknown");
    }
    if ((static_cast<std::uint64_t>(descriptor.capabilities) &
         ~known_capability_bits) != 0) {
        return fail(error, "core descriptor contains unknown capability bits");
    }
    if (descriptor.requires_color && !descriptor.supports_color) {
        return fail(error, "color-required core does not support color");
    }
    if (descriptor.video_width == 0 || descriptor.video_height == 0) {
        return fail(error, "video dimensions must be non-zero");
    }
    if (descriptor.video_width >
        std::numeric_limits<std::size_t>::max() / descriptor.video_height) {
        return fail(error, "video dimensions overflow the pixel count");
    }
    if (!std::isfinite(descriptor.refresh_rate) ||
        !std::isfinite(descriptor.clock_rate) ||
        !(descriptor.refresh_rate > 0.0) ||
        !(descriptor.clock_rate > 0.0) ||
        descriptor.nominal_cycles_per_frame == 0) {
        return fail(error, "timing values must be positive");
    }
    if (descriptor.audio_sample_rate == 0 || descriptor.audio_channels == 0) {
        return fail(error, "audio format must be non-zero");
    }
    if (descriptor.input_count != 0 && descriptor.inputs == nullptr) {
        return fail(error, "input descriptors are missing");
    }
    for (std::size_t index = 0; index < descriptor.input_count; ++index) {
        if (!valid_input_id(descriptor.inputs[index].id)) {
            return fail(error, "input descriptor has an unknown id");
        }
        if (descriptor.inputs[index].name.empty()) {
            return fail(error, "input descriptor has an empty name");
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (descriptor.inputs[previous].id == descriptor.inputs[index].id) {
                return fail(error, "input descriptors contain a duplicate id");
            }
            if (descriptor.inputs[previous].name == descriptor.inputs[index].name) {
                return fail(error, "input descriptors contain a duplicate name");
            }
        }
    }

    const auto frame = core.video_frame();
    if (frame.width != descriptor.video_width ||
        frame.height != descriptor.video_height) {
        return fail(error, "video frame dimensions do not match the descriptor");
    }
    if (frame.width > std::numeric_limits<std::size_t>::max() / frame.height) {
        return fail(error, "video frame dimensions overflow the pixel count");
    }
    const auto expected_pixels = frame.width * frame.height;
    if (frame.pixels == nullptr || frame.pixel_count < expected_pixels) {
        return fail(error, "video frame does not provide enough pixels");
    }
    if (frame.width > std::numeric_limits<std::size_t>::max() /
                            sizeof(std::uint32_t) ||
        frame.pitch < frame.width * sizeof(std::uint32_t)) {
        return fail(error, "video frame pitch is too small");
    }
    return validate_scene_contract(core, descriptor, error);
}

} // namespace gbb
