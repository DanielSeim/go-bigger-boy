#include "android_bridge.hpp"

#ifdef __ANDROID__

#include "gameboy/rom_library.hpp"
#include "gameboy/video_pipeline.hpp"
#include "settings_persistence.hpp"

#include <jni.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <utility>

namespace gbb::sdl {
namespace {

std::mutex android_rom_request_mutex;
std::optional<AndroidRomRequest> android_rom_request;
std::mutex android_back_request_mutex;
bool android_back_requested{};
gbb::LogContext android_back_context{};
std::mutex android_runtime_context_mutex;
gbb::LogContext android_runtime_context{};

bool context_is_empty(const gbb::LogContext context) noexcept {
    return context.session == 0 && context.frame == 0 &&
           context.cycles == 0 && context.rom == 0;
}

} // namespace

std::optional<AndroidRomRequest> take_android_rom_request() noexcept {
    std::lock_guard<std::mutex> lock(android_rom_request_mutex);
    auto request = std::move(android_rom_request);
    android_rom_request.reset();
    return request;
}

std::optional<gbb::LogContext> take_android_back_request() noexcept {
    std::lock_guard<std::mutex> lock(android_back_request_mutex);
    if (!android_back_requested) return std::nullopt;
    android_back_requested = false;
    return android_back_context;
}

void publish_android_log_context(const gbb::LogContext context) noexcept {
    std::lock_guard<std::mutex> lock(android_runtime_context_mutex);
    android_runtime_context = context;
}

gbb::LogContext latest_android_log_context() noexcept {
    std::lock_guard<std::mutex> lock(android_runtime_context_mutex);
    return android_runtime_context;
}

void request_android_back() noexcept {
    std::lock_guard<std::mutex> lock(android_back_request_mutex);
    if (android_back_requested) return;
    android_back_context = latest_android_log_context();
    android_back_requested = true;
}

void request_android_rom(AndroidRomRequest request) {
    if (context_is_empty(request.log_context)) {
        request.log_context = latest_android_log_context();
    }
    std::lock_guard<std::mutex> lock(android_rom_request_mutex);
    android_rom_request = std::move(request);
}

} // namespace gbb::sdl

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeLibraryEntries(
    JNIEnv* environment, jclass, jstring directory) {
    const auto* raw_directory = environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return nullptr;
    const auto library = gameboy::RomLibrary::load(
        std::filesystem::u8path(raw_directory));
    environment->ReleaseStringUTFChars(directory, raw_directory);

    const auto string_class = environment->FindClass("java/lang/String");
    const auto& entries = library.entries();
    auto result = environment->NewObjectArray(
        static_cast<jsize>(entries.size()), string_class, nullptr);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        std::ostringstream encoded;
        encoded << std::hex << entry.metadata.fingerprint << '\x1f'
                << entry.metadata.crc32 << '\x1f'
                << entry.path.u8string() << '\x1f'
                << entry.metadata.title << '\x1f'
                << gameboy::platform_name(entry.metadata.platform) << '\x1f'
                << entry.metadata.language << '\x1f'
                << gameboy::cover_system_name(entry.metadata.platform) << '\x1f'
                << entry.metadata.cover_name << '\x1f'
                << std::dec << entry.last_played;
        const auto text = encoded.str();
        const auto value = environment->NewStringUTF(text.c_str());
        environment->SetObjectArrayElement(
            result, static_cast<jsize>(index), value);
        environment->DeleteLocalRef(value);
    }
    environment->DeleteLocalRef(string_class);
    return result;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeRemoveLibraryEntry(
    JNIEnv* environment, jclass, jstring directory, jstring fingerprint) {
    const auto* raw_directory = environment->GetStringUTFChars(directory, nullptr);
    const auto* raw_fingerprint =
        environment->GetStringUTFChars(fingerprint, nullptr);
    if (raw_directory == nullptr || raw_fingerprint == nullptr) {
        if (raw_directory != nullptr) {
            environment->ReleaseStringUTFChars(directory, raw_directory);
        }
        if (raw_fingerprint != nullptr) {
            environment->ReleaseStringUTFChars(fingerprint, raw_fingerprint);
        }
        return JNI_FALSE;
    }
    auto library = gameboy::RomLibrary::load(
        std::filesystem::u8path(raw_directory));
    std::uint64_t value{};
    std::istringstream parser(raw_fingerprint);
    parser >> std::hex >> value;
    const auto removed = parser && library.remove(value);
    if (removed) library.save(std::filesystem::u8path(raw_directory));
    environment->ReleaseStringUTFChars(directory, raw_directory);
    environment->ReleaseStringUTFChars(fingerprint, raw_fingerprint);
    return removed ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeTouchControlScale(
    JNIEnv* environment, jclass, jstring directory) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return 1.35F;
    const auto settings = load_touch_control_settings(
        std::filesystem::u8path(raw_directory));
    environment->ReleaseStringUTFChars(directory, raw_directory);
    return settings.scale;
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeTouchControlOpacity(
    JNIEnv* environment, jclass, jstring directory) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return 0.78F;
    const auto settings = load_touch_control_settings(
        std::filesystem::u8path(raw_directory));
    environment->ReleaseStringUTFChars(directory, raw_directory);
    return settings.opacity;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeTouchVoxelOrbitEnabled(
    JNIEnv* environment, jclass, jstring directory) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return JNI_TRUE;
    const auto settings = load_touch_control_settings(
        std::filesystem::u8path(raw_directory));
    environment->ReleaseStringUTFChars(directory, raw_directory);
    return settings.voxel_orbit ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeSetTouchVoxelOrbitEnabled(
    JNIEnv* environment, jclass, jstring directory, const jboolean enabled) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return;
    save_touch_voxel_orbit(std::filesystem::u8path(raw_directory),
                           enabled == JNI_TRUE);
    environment->ReleaseStringUTFChars(directory, raw_directory);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeTouchMenuTopRight(
    JNIEnv* environment, jclass, jstring directory) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return JNI_FALSE;
    const auto settings = load_touch_control_settings(
        std::filesystem::u8path(raw_directory));
    environment->ReleaseStringUTFChars(directory, raw_directory);
    return settings.menu_top_right ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeSetTouchMenuTopRight(
    JNIEnv* environment, jclass, jstring directory, const jboolean top_right) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return;
    save_touch_menu_position(std::filesystem::u8path(raw_directory),
                             top_right == JNI_TRUE);
    environment->ReleaseStringUTFChars(directory, raw_directory);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeLinkRemoteHost(
    JNIEnv* environment, jclass, jstring directory) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return nullptr;
    const auto value = load_app_settings(
        std::filesystem::u8path(raw_directory)).link_remote_host;
    environment->ReleaseStringUTFChars(directory, raw_directory);
    return environment->NewStringUTF(value.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeLinkRemoteBind(
    JNIEnv* environment, jclass, jstring directory) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return nullptr;
    auto value = load_app_settings(
        std::filesystem::u8path(raw_directory)).link_remote_bind;
    if (value == "127.0.0.1") value = "0.0.0.0";
    environment->ReleaseStringUTFChars(directory, raw_directory);
    return environment->NewStringUTF(value.c_str());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeLinkRemotePort(
    JNIEnv* environment, jclass, jstring directory) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return 8765;
    const auto value = load_app_settings(
        std::filesystem::u8path(raw_directory)).link_remote_port;
    environment->ReleaseStringUTFChars(directory, raw_directory);
    return static_cast<jint>(value);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeLinkLanDiscovery(
    JNIEnv* environment, jclass, jstring directory) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return JNI_FALSE;
    const auto value = load_app_settings(
        std::filesystem::u8path(raw_directory)).link_lan_discovery;
    environment->ReleaseStringUTFChars(directory, raw_directory);
    return value ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeSetLinkSettings(
    JNIEnv* environment, jclass, jstring directory, jstring host,
    jstring bind, const jint port, const jboolean discovery) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    const auto* raw_host = environment->GetStringUTFChars(host, nullptr);
    const auto* raw_bind = environment->GetStringUTFChars(bind, nullptr);
    if (raw_directory == nullptr || raw_host == nullptr || raw_bind == nullptr) {
        if (raw_directory != nullptr)
            environment->ReleaseStringUTFChars(directory, raw_directory);
        if (raw_host != nullptr)
            environment->ReleaseStringUTFChars(host, raw_host);
        if (raw_bind != nullptr)
            environment->ReleaseStringUTFChars(bind, raw_bind);
        return;
    }
    auto settings = load_app_settings(std::filesystem::u8path(raw_directory));
    settings.link_remote_host = raw_host;
    settings.link_remote_bind = raw_bind;
    settings.link_remote_port = static_cast<std::uint16_t>(
        std::clamp<jint>(port, 1, 65535));
    settings.link_lan_discovery = discovery == JNI_TRUE;
    write_portable_settings(std::filesystem::u8path(raw_directory), settings);
    environment->ReleaseStringUTFChars(directory, raw_directory);
    environment->ReleaseStringUTFChars(host, raw_host);
    environment->ReleaseStringUTFChars(bind, raw_bind);
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeTouchControlLayout(
    JNIEnv* environment, jclass, jstring directory) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return nullptr;
    const auto positions = load_touch_control_layout(
        std::filesystem::u8path(raw_directory));
    environment->ReleaseStringUTFChars(directory, raw_directory);
    const auto result = environment->NewFloatArray(
        static_cast<jsize>(positions.size()));
    if (result != nullptr) {
        environment->SetFloatArrayRegion(
            result, 0, static_cast<jsize>(positions.size()), positions.data());
    }
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeSetTouchControlSettings(
    JNIEnv* environment, jclass, jstring directory, const jfloat scale,
    const jfloat opacity) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return;
    save_touch_control_settings(std::filesystem::u8path(raw_directory), scale,
                                opacity);
    environment->ReleaseStringUTFChars(directory, raw_directory);
}

extern "C" JNIEXPORT void JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeSetTouchControlLayout(
    JNIEnv* environment, jclass, jstring directory, jfloatArray values) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr || values == nullptr) {
        if (raw_directory != nullptr) {
            environment->ReleaseStringUTFChars(directory, raw_directory);
        }
        return;
    }
    const auto length = environment->GetArrayLength(values);
    if (length >= static_cast<jsize>(touch_layout_count * touch_layout_stride)) {
        std::array<float, touch_layout_count * touch_layout_stride> positions{};
        environment->GetFloatArrayRegion(
            values, 0, static_cast<jsize>(positions.size()), positions.data());
        save_touch_control_layout(std::filesystem::u8path(raw_directory),
                                  positions);
    }
    environment->ReleaseStringUTFChars(directory, raw_directory);
}

extern "C" JNIEXPORT void JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeResetTouchControlLayout(
    JNIEnv* environment, jclass, jstring directory) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return;
    const TouchControlSettings defaults;
    save_touch_control_layout(std::filesystem::u8path(raw_directory),
                              defaults.positions);
    environment->ReleaseStringUTFChars(directory, raw_directory);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeVideoMode(
    JNIEnv* environment, jclass, jstring directory) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return nullptr;
    const auto mode = load_video_mode(std::filesystem::u8path(raw_directory));
    environment->ReleaseStringUTFChars(directory, raw_directory);
    const auto id = gameboy::video_mode_info(mode).id;
    return environment->NewStringUTF(std::string{id}.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeSetVideoMode(
    JNIEnv* environment, jclass, jstring directory, jstring mode) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    const auto* raw_mode = mode == nullptr
                               ? nullptr
                               : environment->GetStringUTFChars(mode, nullptr);
    if (raw_directory != nullptr && raw_mode != nullptr) {
        save_video_mode(std::filesystem::u8path(raw_directory),
                        gameboy::video_mode_from_id(raw_mode));
    }
    if (raw_mode != nullptr) environment->ReleaseStringUTFChars(mode, raw_mode);
    if (raw_directory != nullptr) {
        environment->ReleaseStringUTFChars(directory, raw_directory);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_danielseim_gbb_GbbActivity_nativeAndroidBackPressed(
    JNIEnv*, jclass) {
    gbb::sdl::request_android_back();
}

extern "C" JNIEXPORT void JNICALL
Java_com_danielseim_gbb_GbbActivity_nativeOpenRom(
    JNIEnv* environment, jclass, jstring rom, jstring display_name) {
    const auto* raw_rom = environment->GetStringUTFChars(rom, nullptr);
    if (raw_rom == nullptr) return;
    const auto* raw_name = display_name == nullptr
                               ? nullptr
                               : environment->GetStringUTFChars(display_name,
                                                                 nullptr);
    gbb::sdl::request_android_rom(gbb::sdl::AndroidRomRequest{
        raw_rom, raw_name == nullptr ? std::string{} : std::string{raw_name},
        gbb::current_log_context()});
    if (raw_name != nullptr) {
        environment->ReleaseStringUTFChars(display_name, raw_name);
    }
    environment->ReleaseStringUTFChars(rom, raw_rom);
}

#endif
