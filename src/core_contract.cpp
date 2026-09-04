#include "gbb/core_contract.hpp"

#include <cstdint>
#include <limits>

namespace gbb {
namespace {

bool fail(std::string& error, const char* message) {
    error = message;
    return false;
}

bool validate_scene_contract(const EmulatorCore& core,
                            const CoreDescriptor& descriptor,
                            std::string& error) {
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
    return true;
}

} // namespace

bool validate_core_contract(const EmulatorCore& core, std::string& error) {
    error.clear();
    const auto& descriptor = core.descriptor();
    if (descriptor.core_id.empty()) return fail(error, "core id is empty");
    if (descriptor.core_name.empty()) return fail(error, "core name is empty");
    if (descriptor.video_width == 0 || descriptor.video_height == 0) {
        return fail(error, "video dimensions must be non-zero");
    }
    if (descriptor.video_width >
        std::numeric_limits<std::size_t>::max() / descriptor.video_height) {
        return fail(error, "video dimensions overflow the pixel count");
    }
    if (!(descriptor.refresh_rate > 0.0) ||
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
        if (descriptor.inputs[index].name.empty()) {
            return fail(error, "input descriptor has an empty name");
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (descriptor.inputs[previous].id == descriptor.inputs[index].id) {
                return fail(error, "input descriptors contain a duplicate id");
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
