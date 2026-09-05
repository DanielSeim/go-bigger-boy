#include "gbb/plugin_loader.hpp"

#include "gbb/core_contract.hpp"
#include "gbb/log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <malloc.h>
#elif !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
#include <dlfcn.h>
#endif

namespace gbb {
namespace {

constexpr std::uint64_t known_capabilities =
    GBB_PLUGIN_CAP_PERSISTENT_MEMORY | GBB_PLUGIN_CAP_RTC |
    GBB_PLUGIN_CAP_RUMBLE | GBB_PLUGIN_CAP_CAMERA | GBB_PLUGIN_CAP_PRINTER |
    GBB_PLUGIN_CAP_COMPATIBILITY_PALETTE | GBB_PLUGIN_CAP_CHEATS |
    GBB_PLUGIN_CAP_DEBUGGER | GBB_PLUGIN_CAP_SPRITE_EDITOR |
    GBB_PLUGIN_CAP_SCENE_LAYERS | GBB_PLUGIN_CAP_LINK_CABLE;

constexpr std::uint64_t adapter_capabilities =
    GBB_PLUGIN_CAP_PERSISTENT_MEMORY | GBB_PLUGIN_CAP_RTC;
constexpr std::size_t max_plugin_string_bytes = GBB_PLUGIN_MAX_STRING_BYTES;

std::size_t normalize_alignment(const std::uint64_t alignment) noexcept {
    const auto default_alignment = alignof(std::max_align_t);
    const auto valid = [](const std::size_t value) {
        return value >= alignof(void*) && (value & (value - 1)) == 0;
    };
    if (alignment == 0) return valid(default_alignment) ? default_alignment : 0;
    if (alignment > std::numeric_limits<std::size_t>::max() ||
        !valid(static_cast<std::size_t>(alignment))) {
        return 0;
    }
    return static_cast<std::size_t>(alignment);
}

bool supported_header(const gbb_plugin_struct_header& header,
                      const std::uint32_t required_size) noexcept {
    return header.struct_size >= required_size &&
           header.abi_major == GBB_PLUGIN_ABI_MAJOR &&
           header.abi_minor <= GBB_PLUGIN_ABI_MINOR;
}

bool valid_utf8_string(const char* value, const bool allow_empty = false) noexcept {
    if (value == nullptr) return false;
    std::size_t length = 0;
    while (length < max_plugin_string_bytes && value[length] != '\0') ++length;
    if (length == max_plugin_string_bytes || (!allow_empty && length == 0)) {
        return false;
    }
    std::size_t index = 0;
    while (index < length) {
        const auto first = static_cast<std::uint8_t>(value[index]);
        if (first <= 0x7f) {
            ++index;
            continue;
        }
        std::size_t continuation_count = 0;
        std::uint8_t minimum_second = 0x80;
        std::uint8_t maximum_second = 0xbf;
        if (first >= 0xc2 && first <= 0xdf) {
            continuation_count = 1;
        } else if (first == 0xe0) {
            continuation_count = 2;
            minimum_second = 0xa0;
        } else if (first >= 0xe1 && first <= 0xec) {
            continuation_count = 2;
        } else if (first == 0xed) {
            continuation_count = 2;
            maximum_second = 0x9f;
        } else if (first >= 0xee && first <= 0xef) {
            continuation_count = 2;
        } else if (first == 0xf0) {
            continuation_count = 3;
            minimum_second = 0x90;
        } else if (first >= 0xf1 && first <= 0xf3) {
            continuation_count = 3;
        } else if (first == 0xf4) {
            continuation_count = 3;
            maximum_second = 0x8f;
        } else {
            return false;
        }
        if (index + continuation_count >= length) return false;
        const auto second = static_cast<std::uint8_t>(value[index + 1]);
        if (second < minimum_second || second > maximum_second) return false;
        for (std::size_t offset = 2; offset <= continuation_count; ++offset) {
            const auto byte = static_cast<std::uint8_t>(value[index + offset]);
            if (byte < 0x80 || byte > 0xbf) return false;
        }
        index += continuation_count + 1;
    }
    return true;
}

bool valid_system(const std::uint8_t system) noexcept {
    return system == GBB_PLUGIN_SYSTEM_GB ||
           system == GBB_PLUGIN_SYSTEM_GBC ||
           system == GBB_PLUGIN_SYSTEM_GBA;
}

bool valid_input_id(const std::uint16_t id) noexcept {
    return id <= GBB_PLUGIN_INPUT_START;
}

bool validate_descriptor(const gbb_plugin_descriptor_v1& descriptor,
                         std::string& error) {
    if (!supported_header(descriptor.header, sizeof(descriptor))) {
        error = "plugin descriptor has an incompatible header";
        return false;
    }
    if (!valid_utf8_string(descriptor.core_id) ||
        !valid_utf8_string(descriptor.core_name) ||
        (descriptor.software_title != nullptr &&
         !valid_utf8_string(descriptor.software_title, true))) {
        error = "plugin descriptor requires a core id and name";
        return false;
    }
    if (!valid_system(descriptor.system_id)) {
        error = "plugin descriptor has an unknown system id";
        return false;
    }
    if (descriptor.supports_color > 1 || descriptor.requires_color > 1 ||
        descriptor.has_battery > 1 || descriptor.video_width == 0 ||
        descriptor.video_height == 0 || descriptor.refresh_rate_millihz == 0 ||
        descriptor.clock_rate_hz == 0 || descriptor.nominal_cycles_per_frame == 0 ||
        descriptor.audio_sample_rate == 0 || descriptor.audio_channels == 0) {
        error = "plugin descriptor contains invalid dimensions, flags, or timing";
        return false;
    }
    if (descriptor.requires_color != 0 && descriptor.supports_color == 0) {
        error = "plugin descriptor requires color without supporting it";
        return false;
    }
    if ((descriptor.capabilities & ~known_capabilities) != 0) {
        error = "plugin descriptor contains unknown capability bits";
        return false;
    }
    if (descriptor.input_count > 64 ||
        (descriptor.input_count != 0 && descriptor.inputs == nullptr)) {
        error = "plugin descriptor has invalid input metadata";
        return false;
    }
    for (std::uint32_t index = 0; index < descriptor.input_count; ++index) {
        const auto& input = descriptor.inputs[index];
        if (!valid_input_id(input.id) || !valid_utf8_string(input.name)) {
            error = "plugin descriptor has an invalid input";
            return false;
        }
        for (std::uint32_t previous = 0; previous < index; ++previous) {
            const auto& prior = descriptor.inputs[previous];
            if (prior.id == input.id) {
                error = "plugin descriptor has duplicate input ids";
                return false;
            }
            if (std::strcmp(prior.name, input.name) == 0) {
                error = "plugin descriptor has duplicate input names";
                return false;
            }
        }
    }
    if ((descriptor.capabilities & adapter_capabilities) !=
        descriptor.capabilities) {
        error = "plugin advertises a capability not exposed by the core adapter";
        return false;
    }
    return true;
}

bool validate_plugin(const gbb_plugin_v1& plugin, std::string& error) {
    if (!supported_header(plugin.header, GBB_PLUGIN_V1_REQUIRED_SIZE)) {
        error = "plugin table has an incompatible header";
        return false;
    }
    if (!validate_descriptor(plugin.descriptor, error) ||
        !supported_header(plugin.core.header, GBB_PLUGIN_CORE_API_V1_REQUIRED_SIZE)) {
        if (error.empty()) error = "plugin core table has an incompatible header";
        return false;
    }
    const auto& core = plugin.core;
    if (core.create == nullptr || core.destroy == nullptr || core.reset == nullptr ||
        core.step_instruction == nullptr || core.frame_ready == nullptr ||
        core.consume_frame == nullptr || core.video_frame == nullptr ||
        core.audio_read == nullptr || core.set_input == nullptr ||
        core.save_state == nullptr || core.load_state == nullptr ||
        core.rom_fingerprint == nullptr || core.has_persistent_data == nullptr ||
        core.read_persistent_data == nullptr ||
        core.write_persistent_data == nullptr || core.release_blob == nullptr ||
        core.flush_persistent_data == nullptr || core.query_extension == nullptr) {
        error = "plugin core table is missing a required function";
        return false;
    }
    return true;
}

void* GBB_PLUGIN_CALL allocate(void*, const std::uint64_t size,
                               const std::uint64_t alignment) {
    if (size == 0 || size > std::numeric_limits<std::size_t>::max()) {
        return nullptr;
    }
    const auto normalized_alignment = normalize_alignment(alignment);
    if (normalized_alignment == 0) return nullptr;
#if defined(_WIN32)
    return _aligned_malloc(static_cast<std::size_t>(size),
                           normalized_alignment);
#else
    void* pointer = nullptr;
    if (posix_memalign(&pointer, normalized_alignment,
                       static_cast<std::size_t>(size)) != 0) {
        return nullptr;
    }
    return pointer;
#endif
}

void GBB_PLUGIN_CALL deallocate(void*, void* pointer, std::uint64_t,
                                std::uint64_t) {
#if defined(_WIN32)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

void GBB_PLUGIN_CALL log_message(void*, const std::uint32_t level,
                                 const char* message) {
    if (message == nullptr) return;
    LogLevel mapped = LogLevel::debug;
    switch (level) {
    case GBB_PLUGIN_LOG_ERROR: mapped = LogLevel::error; break;
    case GBB_PLUGIN_LOG_WARNING: mapped = LogLevel::warning; break;
    case GBB_PLUGIN_LOG_INFO: mapped = LogLevel::info; break;
    case GBB_PLUGIN_LOG_DEBUG: mapped = LogLevel::debug; break;
    case GBB_PLUGIN_LOG_TRACE: mapped = LogLevel::trace; break;
    default: break;
    }
    Logger::instance().write(mapped, LogCategory::core, message);
}

gbb_plugin_host_v1 host_callbacks() noexcept {
    return {{sizeof(gbb_plugin_host_v1), GBB_PLUGIN_ABI_MAJOR,
             GBB_PLUGIN_ABI_MINOR},
            nullptr, allocate, deallocate, log_message};
}

[[noreturn]] void throw_plugin_error(const char* operation,
                                     const gbb_plugin_result result) {
    throw std::runtime_error(std::string("plugin ") + operation +
                             " failed with result " +
                             std::to_string(static_cast<int>(result)));
}

void log_plugin_failure(const char* operation, const gbb_plugin_result result) noexcept {
    try {
        Logger::instance().write(
            LogLevel::error, LogCategory::core,
            std::string("plugin ") + operation + " failed with result " +
                std::to_string(static_cast<int>(result)));
    } catch (...) {
    }
}

InputId input_id(const std::uint16_t id) {
    switch (id) {
    case GBB_PLUGIN_INPUT_RIGHT: return InputId::right;
    case GBB_PLUGIN_INPUT_LEFT: return InputId::left;
    case GBB_PLUGIN_INPUT_UP: return InputId::up;
    case GBB_PLUGIN_INPUT_DOWN: return InputId::down;
    case GBB_PLUGIN_INPUT_A: return InputId::a;
    case GBB_PLUGIN_INPUT_B: return InputId::b;
    case GBB_PLUGIN_INPUT_X: return InputId::x;
    case GBB_PLUGIN_INPUT_Y: return InputId::y;
    case GBB_PLUGIN_INPUT_L: return InputId::l;
    case GBB_PLUGIN_INPUT_R: return InputId::r;
    case GBB_PLUGIN_INPUT_SELECT: return InputId::select;
    case GBB_PLUGIN_INPUT_START: return InputId::start;
    default: throw std::runtime_error("plugin input id is unknown");
    }
}

class PluginCore final : public EmulatorCore {
public:
    PluginCore(std::shared_ptr<const PluginLoader> loader,
               const gbb_plugin_v1& plugin, std::vector<std::uint8_t> rom,
               const CoreLoadOptions& options)
        : loader_(std::move(loader)), plugin_(plugin), host_(host_callbacks()) {
        const auto& source = plugin_.descriptor;
        descriptor_.core_id = source.core_id;
        descriptor_.core_name = source.core_name;
        descriptor_.system = source.system_id == GBB_PLUGIN_SYSTEM_GB
                                 ? SystemId::game_boy
                             : source.system_id == GBB_PLUGIN_SYSTEM_GBC
                                 ? SystemId::game_boy_color
                                 : SystemId::game_boy_advance;
        descriptor_.video_width = source.video_width;
        descriptor_.video_height = source.video_height;
        descriptor_.refresh_rate =
            static_cast<double>(source.refresh_rate_millihz) / 1000.0;
        descriptor_.clock_rate = static_cast<double>(source.clock_rate_hz);
        descriptor_.nominal_cycles_per_frame = source.nominal_cycles_per_frame;
        descriptor_.audio_sample_rate = source.audio_sample_rate;
        descriptor_.audio_channels = source.audio_channels;
        descriptor_.capabilities =
            static_cast<CoreCapability>(source.capabilities);
        descriptor_.software_title = source.software_title == nullptr
                                         ? std::string_view{}
                                         : source.software_title;
        descriptor_.rom_size = source.rom_size;
        descriptor_.save_ram_size = source.save_ram_size;
        descriptor_.has_battery = source.has_battery != 0;
        descriptor_.supports_color = source.supports_color != 0;
        descriptor_.requires_color = source.requires_color != 0;

        inputs_.reserve(source.input_count);
        for (std::uint32_t index = 0; index < source.input_count; ++index) {
            inputs_.push_back({input_id(source.inputs[index].id),
                               source.inputs[index].name});
        }
        descriptor_.inputs = inputs_.data();
        descriptor_.input_count = inputs_.size();

        if (descriptor_.video_width >
            std::numeric_limits<std::size_t>::max() / descriptor_.video_height) {
            throw std::runtime_error("plugin video dimensions overflow host size");
        }
        const auto pixel_count = descriptor_.video_width * descriptor_.video_height;
        if (pixel_count >
            std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t)) {
            throw std::runtime_error("plugin video buffer size overflows host size");
        }
        frame_.resize(pixel_count);

        gbb_plugin_create_options_v1 create_options{
            {sizeof(gbb_plugin_create_options_v1), GBB_PLUGIN_ABI_MAJOR,
             GBB_PLUGIN_ABI_MINOR},
            nullptr, nullptr};
        const auto source_path = options.source_path.u8string();
        const auto persistence_path = options.persistence_path.u8string();
        create_options.source_path =
            source_path.empty() ? nullptr : source_path.c_str();
        create_options.persistence_path =
            persistence_path.empty() ? nullptr : persistence_path.c_str();
        const auto result = plugin_.core.create(
            rom.data(), rom.size(), &create_options, &core_);
        if (result != GBB_PLUGIN_OK || core_ == nullptr) {
            if (core_ != nullptr) plugin_.core.destroy(core_);
            throw_plugin_error("create", result);
        }
    }

    ~PluginCore() override {
        if (core_ != nullptr) plugin_.core.destroy(core_);
    }

    const CoreDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    void reset() noexcept override {
        const auto result = plugin_.core.reset(core_);
        if (result != GBB_PLUGIN_OK) log_plugin_failure("reset", result);
    }

    unsigned step_instruction() override {
        std::uint32_t cycles = 0;
        const auto result = plugin_.core.step_instruction(core_, &cycles);
        if (result != GBB_PLUGIN_OK) throw_plugin_error("step", result);
        return cycles;
    }

    bool frame_ready() const noexcept override {
        std::uint8_t ready = 0;
        const auto result = plugin_.core.frame_ready(core_, &ready);
        if (result != GBB_PLUGIN_OK) {
            log_plugin_failure("frame_ready", result);
            return false;
        }
        return ready != 0;
    }

    void consume_frame() noexcept override {
        const auto result = plugin_.core.consume_frame(core_);
        if (result != GBB_PLUGIN_OK) log_plugin_failure("consume_frame", result);
    }

    VideoFrameView video_frame() const noexcept override {
        gbb_plugin_video_frame_v1 frame{};
        frame.header = {sizeof(gbb_plugin_video_frame_v1), GBB_PLUGIN_ABI_MAJOR,
                        GBB_PLUGIN_ABI_MINOR};
        const auto result = plugin_.core.video_frame(
            core_, reinterpret_cast<std::uint8_t*>(frame_.data()),
            frame_.size() * sizeof(std::uint32_t), &frame);
        if (result != GBB_PLUGIN_OK ||
            frame.pixel_format != GBB_PLUGIN_PIXEL_FORMAT_XRGB8888 ||
            frame.width != descriptor_.video_width ||
            frame.height != descriptor_.video_height ||
            frame.pitch < frame.width * sizeof(std::uint32_t) ||
            frame.height > std::numeric_limits<std::uint64_t>::max() /
                               frame.pitch ||
            frame.required_bytes <
                static_cast<std::uint64_t>(frame.pitch) * frame.height ||
            frame.required_bytes > frame_.size() * sizeof(std::uint32_t)) {
            if (result != GBB_PLUGIN_OK) log_plugin_failure("video_frame", result);
            else Logger::instance().write(LogLevel::error, LogCategory::core,
                                          "plugin video frame metadata is invalid");
            return {};
        }
        return {frame_.data(), frame_.size(), frame.width, frame.height,
                frame.pitch};
    }

    std::vector<std::int16_t> take_audio_samples() override {
        std::uint64_t required = 0;
        auto result = plugin_.core.audio_read(core_, nullptr, 0, &required);
        if (result == GBB_PLUGIN_OK && required == 0) return {};
        if (result != GBB_PLUGIN_BUFFER_TOO_SMALL ||
            required > std::numeric_limits<std::size_t>::max() ||
            required % descriptor_.audio_channels != 0) {
            throw_plugin_error("audio size query", result);
        }
        std::vector<std::int16_t> samples(static_cast<std::size_t>(required));
        std::uint64_t written = 0;
        result = plugin_.core.audio_read(core_, samples.data(), samples.size(),
                                         &written);
        if (result != GBB_PLUGIN_OK || written > samples.size() ||
            written % descriptor_.audio_channels != 0) {
            throw_plugin_error("audio read", result);
        }
        samples.resize(static_cast<std::size_t>(written));
        return samples;
    }

    void set_input(const InputId input, const bool pressed) noexcept override {
        const auto raw = static_cast<std::uint16_t>(input);
        const auto result = plugin_.core.set_input(core_, raw, pressed ? 1 : 0);
        if (result != GBB_PLUGIN_OK) log_plugin_failure("set_input", result);
    }

    std::vector<std::uint8_t> save_state() const override {
        gbb_plugin_blob_v1 blob{};
        const auto result = plugin_.core.save_state(core_, &host_, &blob);
        if (result != GBB_PLUGIN_OK) {
            if (blob.data != nullptr) {
                static_cast<void>(plugin_.core.release_blob(core_, &host_, &blob));
            }
            throw_plugin_error("save_state", result);
        }
        if ((blob.data == nullptr) != (blob.size == 0)) {
            if (blob.data != nullptr) {
                plugin_.core.release_blob(core_, &host_, &blob);
            }
            throw std::runtime_error("plugin save_state returned an invalid blob");
        }
        if (blob.size > std::numeric_limits<std::size_t>::max()) {
            if (blob.data != nullptr) {
                static_cast<void>(plugin_.core.release_blob(core_, &host_, &blob));
            }
            throw std::runtime_error("plugin save_state exceeds host size");
        }
        std::vector<std::uint8_t> state;
        try {
            if (blob.size != 0) state.assign(blob.data, blob.data + blob.size);
        } catch (...) {
            static_cast<void>(plugin_.core.release_blob(core_, &host_, &blob));
            throw;
        }
        if (blob.data != nullptr) {
            const auto release = plugin_.core.release_blob(core_, &host_, &blob);
            if (release != GBB_PLUGIN_OK) throw_plugin_error("release_blob", release);
        }
        return state;
    }

    void load_state(const std::vector<std::uint8_t>& state) override {
        const auto result = plugin_.core.load_state(core_, state.data(), state.size());
        if (result != GBB_PLUGIN_OK) throw_plugin_error("load_state", result);
    }

    std::uint64_t rom_fingerprint() const noexcept override {
        std::uint64_t fingerprint = 0;
        const auto result = plugin_.core.rom_fingerprint(core_, &fingerprint);
        if (result != GBB_PLUGIN_OK) {
            log_plugin_failure("rom_fingerprint", result);
            return 0;
        }
        return fingerprint;
    }

    void flush_persistent_data() override {
        const auto result = plugin_.core.flush_persistent_data(core_);
        if (result != GBB_PLUGIN_OK) {
            throw_plugin_error("flush_persistent_data", result);
        }
    }

    bool has_persistent_data(const PersistentDataKind kind) const noexcept override {
        std::uint8_t present = 0;
        const auto result = plugin_.core.has_persistent_data(
            core_, static_cast<std::uint16_t>(kind), &present);
        if (result != GBB_PLUGIN_OK) {
            log_plugin_failure("has_persistent_data", result);
            return false;
        }
        return present != 0;
    }

    std::vector<std::uint8_t> export_persistent_data(
        const PersistentDataKind kind) const override {
        gbb_plugin_blob_v1 blob{};
        const auto result = plugin_.core.read_persistent_data(
            core_, static_cast<std::uint16_t>(kind), &host_,
            &blob);
        if (result == GBB_PLUGIN_UNSUPPORTED) return {};
        if (result != GBB_PLUGIN_OK) {
            throw_plugin_error("read_persistent_data", result);
        }
        if ((blob.data == nullptr) != (blob.size == 0)) {
            if (blob.data != nullptr) {
                static_cast<void>(plugin_.core.release_blob(core_, &host_, &blob));
            }
            throw std::runtime_error(
                "plugin read_persistent_data returned an invalid blob");
        }
        if (blob.size > std::numeric_limits<std::size_t>::max()) {
            if (blob.data != nullptr) {
                static_cast<void>(plugin_.core.release_blob(core_, &host_, &blob));
            }
            throw std::runtime_error(
                "plugin persistent data exceeds host size");
        }
        std::vector<std::uint8_t> data;
        try {
            if (blob.size != 0) data.assign(blob.data, blob.data + blob.size);
        } catch (...) {
            static_cast<void>(plugin_.core.release_blob(core_, &host_, &blob));
            throw;
        }
        if (blob.data != nullptr) {
            const auto release = plugin_.core.release_blob(core_, &host_, &blob);
            if (release != GBB_PLUGIN_OK) throw_plugin_error("release_blob", release);
        }
        return data;
    }

    void import_persistent_data(
        const PersistentDataKind kind,
        const std::vector<std::uint8_t>& data) override {
        const auto result = plugin_.core.write_persistent_data(
            core_, static_cast<std::uint16_t>(kind), data.data(),
            data.size());
        if (result != GBB_PLUGIN_OK) {
            throw_plugin_error("write_persistent_data", result);
        }
    }

private:
    std::shared_ptr<const PluginLoader> loader_;
    gbb_plugin_v1 plugin_{};
    gbb_plugin_host_v1 host_{};
    gbb_plugin_core_handle core_{};
    std::vector<InputDescriptor> inputs_;
    mutable std::vector<std::uint32_t> frame_;
    CoreDescriptor descriptor_{};
};

} // namespace

PluginLoader::PluginLoader(void* library, const gbb_plugin_v1 plugin,
                           std::filesystem::path path) noexcept
    : library_(library), plugin_(plugin), path_(std::move(path)) {}

PluginLoader::~PluginLoader() {
#if defined(_WIN32)
    if (library_ != nullptr) FreeLibrary(static_cast<HMODULE>(library_));
#elif !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
    if (library_ != nullptr) dlclose(library_);
#endif
    library_ = nullptr;
}

std::shared_ptr<PluginLoader> PluginLoader::load(
    const std::filesystem::path& path, std::string& error) {
    error.clear();
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    static_cast<void>(path);
    error = "dynamic plugins are not supported on this platform";
    return nullptr;
#else
    if (path.empty()) {
        error = "plugin path is empty";
        return nullptr;
    }
#if defined(_WIN32)
    const auto library = static_cast<void*>(LoadLibraryW(path.wstring().c_str()));
    if (library == nullptr) {
        error = "could not load plugin library: " + path.string();
        return nullptr;
    }
    const auto query = reinterpret_cast<gbb_plugin_query_fn>(
        GetProcAddress(static_cast<HMODULE>(library), "gbb_plugin_query"));
#else
    const auto library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        const auto* detail = dlerror();
        error = std::string("could not load plugin library: ") +
                (detail == nullptr ? "unknown loader error" : detail);
        return nullptr;
    }
    const auto query = reinterpret_cast<gbb_plugin_query_fn>(
        dlsym(library, "gbb_plugin_query"));
#endif
    if (query == nullptr) {
        error = "plugin does not export gbb_plugin_query";
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(library));
#else
        dlclose(library);
#endif
        return nullptr;
    }
    auto host = host_callbacks();
    gbb_plugin_v1 plugin{};
    plugin.header = {GBB_PLUGIN_V1_REQUIRED_SIZE, GBB_PLUGIN_ABI_MAJOR,
                     GBB_PLUGIN_ABI_MINOR};
    gbb_plugin_result result = GBB_PLUGIN_FATAL;
    try {
        result = query(&host, &plugin);
    } catch (const std::exception& exception) {
        error = std::string("plugin query threw an exception: ") +
                exception.what();
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(library));
#else
        dlclose(library);
#endif
        return nullptr;
    } catch (...) {
        error = "plugin query threw an unknown exception";
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(library));
#else
        dlclose(library);
#endif
        return nullptr;
    }
    if (result != GBB_PLUGIN_OK || !validate_plugin(plugin, error)) {
        if (error.empty()) {
            error = "plugin query failed with result " +
                    std::to_string(static_cast<int>(result));
        }
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(library));
#else
        dlclose(library);
#endif
        return nullptr;
    }
    try {
        return std::shared_ptr<PluginLoader>(
            new PluginLoader(static_cast<void*>(library), plugin, path));
    } catch (const std::exception& exception) {
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(library));
#else
        dlclose(library);
#endif
        error = std::string("could not allocate plugin loader: ") +
                exception.what();
        return nullptr;
    } catch (...) {
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(library));
#else
        dlclose(library);
#endif
        error = "could not allocate plugin loader";
        return nullptr;
    }
#endif
}

const gbb_plugin_descriptor_v1& PluginLoader::descriptor() const noexcept {
    return plugin_.descriptor;
}

std::unique_ptr<EmulatorCore> PluginLoader::create(
    std::vector<std::uint8_t> rom, const CoreLoadOptions& options) const {
    auto core = std::make_unique<PluginCore>(shared_from_this(), plugin_,
                                              std::move(rom), options);
    std::string error;
    if (!validate_core_contract(*core, error)) {
        throw std::runtime_error("plugin core contract violation: " + error);
    }
    return core;
}

} // namespace gbb
