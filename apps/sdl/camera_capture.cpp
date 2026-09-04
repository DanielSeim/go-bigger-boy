#include "camera_capture.hpp"

#include "gameboy/cartridge.hpp"
#include "gbb/frontend_logging.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

#ifdef __ANDROID__
#include <jni.h>
#endif

namespace gbb::sdl {
namespace {

#ifdef __ANDROID__
std::optional<int> android_camera_orientation_correction_degrees() noexcept {
    auto* environment = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (environment == nullptr || activity == nullptr) return std::nullopt;

    static jmethodID orientation_method{};
    if (orientation_method == nullptr) {
        const auto activity_class = environment->GetObjectClass(activity);
        if (activity_class != nullptr) {
            orientation_method = environment->GetMethodID(
                activity_class, "getCameraOrientationCorrectionDegrees", "()I");
            environment->DeleteLocalRef(activity_class);
        }
    }
    std::optional<int> orientation;
    if (orientation_method != nullptr) {
        orientation = static_cast<int>(
            environment->CallIntMethod(activity, orientation_method));
    }
    if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
        orientation.reset();
    }
    environment->DeleteLocalRef(activity);
    return orientation;
}
#endif

} // namespace

CameraCapture::~CameraCapture() {
    close();
}

void CameraCapture::close() noexcept {
    if (camera_ != nullptr) {
        SDL_CloseCamera(camera_);
        camera_ = nullptr;
    }
    if (conversion_surface_ != nullptr) {
        SDL_DestroySurface(conversion_surface_);
        conversion_surface_ = nullptr;
    }
    next_frame_ = {};
    warning_shown_ = false;
    mirror_ = false;
    back_facing_ = false;
}

void CameraCapture::configure(const gameboy::Emulator& emulator) {
    close();
    if (!emulator.has_camera()) return;

    if (!SDL_InitSubSystem(SDL_INIT_CAMERA)) {
        gbb::log_frontend_warning(
            std::string("Camera subsystem is unavailable: ") + SDL_GetError());
        warning_shown_ = true;
        return;
    }
    int count = 0;
    SDL_CameraID* cameras = SDL_GetCameras(&count);
    if (cameras == nullptr || count == 0) {
        SDL_free(cameras);
        gbb::log_frontend_warning(
            "No webcam was found; the Game Boy Camera will use its fallback image.");
        warning_shown_ = true;
        return;
    }

    const auto camera_id = cameras[0];
    const auto camera_position = SDL_GetCameraPosition(camera_id);
    mirror_ = camera_position == SDL_CAMERA_POSITION_FRONT_FACING;
    back_facing_ = camera_position == SDL_CAMERA_POSITION_BACK_FACING;
    constexpr SDL_CameraSpec camera_spec{
        SDL_PIXELFORMAT_RGBA32,
        SDL_COLORSPACE_SRGB,
        static_cast<int>(gameboy::Cartridge::camera_width),
        static_cast<int>(gameboy::Cartridge::camera_height),
        15,
        1,
    };
    camera_ = SDL_OpenCamera(camera_id, &camera_spec);
    SDL_free(cameras);
    if (camera_ == nullptr) {
        gbb::log_frontend_warning(
            std::string("Webcam could not be opened: ") + SDL_GetError());
        warning_shown_ = true;
        return;
    }
    conversion_surface_ = SDL_CreateSurface(
        static_cast<int>(gameboy::Cartridge::camera_width),
        static_cast<int>(gameboy::Cartridge::camera_height),
        SDL_PIXELFORMAT_RGBA32);
    if (conversion_surface_ == nullptr) {
        gbb::log_frontend_warning(
            std::string("Webcam conversion surface could not be created: ") +
            SDL_GetError());
        close();
        warning_shown_ = true;
    }
}

void CameraCapture::update(gameboy::Emulator* emulator) {
    if (emulator == nullptr || !emulator->has_camera() || camera_ == nullptr) {
        return;
    }
    const auto permission = SDL_GetCameraPermissionState(camera_);
    if (permission == SDL_CAMERA_PERMISSION_STATE_DENIED) {
        if (!warning_shown_) {
            gbb::log_frontend_warning(
                "Webcam permission was denied; the Game Boy Camera will use its fallback image.");
            warning_shown_ = true;
        }
        return;
    }
    if (permission != SDL_CAMERA_PERMISSION_STATE_APPROVED) return;

    const auto now = std::chrono::steady_clock::now();
    if (now < next_frame_) return;
    next_frame_ = now + std::chrono::milliseconds(66);

    SDL_Surface* source = SDL_AcquireCameraFrame(camera_, nullptr);
    if (source == nullptr) return;
    auto rotation_degrees = SDL_GetFloatProperty(
        SDL_GetSurfaceProperties(source), SDL_PROP_SURFACE_ROTATION_FLOAT, 0.0F);
#ifdef __ANDROID__
    if (const auto correction = android_camera_orientation_correction_degrees()) {
        rotation_degrees += static_cast<float>(back_facing_ ? -*correction : *correction);
    }
#endif
    auto rotation_quarters = static_cast<int>(
        rotation_degrees / 90.0F + (rotation_degrees < 0.0F ? -0.5F : 0.5F));
    rotation_quarters = (rotation_quarters % 4 + 4) % 4;
    constexpr auto target_width = gameboy::Cartridge::camera_width;
    constexpr auto target_height = gameboy::Cartridge::camera_height;
    SDL_Surface* frame = source;
    if (source->format != SDL_PIXELFORMAT_RGBA32 ||
        source->w != static_cast<int>(target_width) ||
        source->h != static_cast<int>(target_height)) {
        SDL_Rect crop{0, 0, source->w, source->h};
        if (static_cast<std::int64_t>(source->w) * target_height >
            static_cast<std::int64_t>(source->h) * target_width) {
            crop.w = static_cast<int>(static_cast<std::int64_t>(source->h) *
                                      target_width / target_height);
            crop.x = (source->w - crop.w) / 2;
        } else {
            crop.h = static_cast<int>(static_cast<std::int64_t>(source->w) *
                                      target_height / target_width);
            crop.y = (source->h - crop.h) / 2;
        }
        if (!SDL_BlitSurfaceScaled(source, &crop, conversion_surface_, nullptr,
                                   SDL_SCALEMODE_NEAREST)) {
            SDL_ReleaseCameraFrame(camera_, source);
            if (!warning_shown_) {
                gbb::log_frontend_warning(
                    std::string("Webcam frame conversion failed: ") +
                    SDL_GetError());
                warning_shown_ = true;
            }
            return;
        }
        frame = conversion_surface_;
    }

    const auto needs_lock = SDL_MUSTLOCK(frame);
    if (needs_lock && !SDL_LockSurface(frame)) {
        SDL_ReleaseCameraFrame(camera_, source);
        return;
    }
    std::array<std::uint8_t, target_width * target_height> grayscale{};
    const auto* pixels = static_cast<const std::uint8_t*>(frame->pixels);
    const auto rotated_width = rotation_quarters % 2 == 0 ? frame->w : frame->h;
    const auto rotated_height = rotation_quarters % 2 == 0 ? frame->h : frame->w;
    auto crop_x = 0;
    auto crop_y = 0;
    auto crop_width = rotated_width;
    auto crop_height = rotated_height;
    if (static_cast<std::int64_t>(rotated_width) * target_height >
        static_cast<std::int64_t>(rotated_height) * target_width) {
        crop_width = static_cast<int>(static_cast<std::int64_t>(rotated_height) *
                                      target_width / target_height);
        crop_x = (rotated_width - crop_width) / 2;
    } else {
        crop_height = static_cast<int>(static_cast<std::int64_t>(rotated_width) *
                                       target_height / target_width);
        crop_y = (rotated_height - crop_height) / 2;
    }
    for (std::size_t y = 0; y < target_height; ++y) {
        for (std::size_t x = 0; x < target_width; ++x) {
            auto rotated_x = crop_x + static_cast<int>(
                (x * 2 + 1) * static_cast<std::size_t>(crop_width) /
                (target_width * 2));
            const auto rotated_y = crop_y + static_cast<int>(
                (y * 2 + 1) * static_cast<std::size_t>(crop_height) /
                (target_height * 2));
            if (mirror_) rotated_x = rotated_width - 1 - rotated_x;
            auto sample_x = rotated_x;
            auto sample_y = rotated_y;
            switch (rotation_quarters) {
            case 1:
                sample_x = rotated_y;
                sample_y = frame->h - 1 - rotated_x;
                break;
            case 2:
                sample_x = frame->w - 1 - rotated_x;
                sample_y = frame->h - 1 - rotated_y;
                break;
            case 3:
                sample_x = frame->w - 1 - rotated_y;
                sample_y = rotated_x;
                break;
            default: break;
            }
            const auto* pixel = pixels + sample_y * frame->pitch + sample_x * 4;
            grayscale[y * target_width + x] = static_cast<std::uint8_t>(
                (77U * pixel[0] + 150U * pixel[1] + 29U * pixel[2]) >> 8);
        }
    }
    if (needs_lock) SDL_UnlockSurface(frame);
    SDL_ReleaseCameraFrame(camera_, source);
    emulator->set_camera_frame(grayscale.data(), grayscale.size());
}

} // namespace gbb::sdl
