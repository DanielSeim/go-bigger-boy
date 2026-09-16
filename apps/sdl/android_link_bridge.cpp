#include "android_bridge.hpp"

#ifdef __ANDROID__

#include <SDL3/SDL.h>
#include <jni.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

// Bluetooth remains a deliberately small JNI transport boundary. Java owns
// the socket and worker thread; these functions only exchange already-framed
// packets with the non-blocking emulation loop.
extern "C" bool gbb_android_bluetooth_start_host(const char* uuid) noexcept {
    auto* environment = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (environment == nullptr || activity == nullptr || uuid == nullptr) return false;
    const auto klass = environment->GetObjectClass(activity);
    const auto method = klass == nullptr ? nullptr : environment->GetMethodID(
        klass, "bluetoothStartHost", "(Ljava/lang/String;)Z");
    const auto uuid_string = environment->NewStringUTF(uuid);
    const auto value = method == nullptr || uuid_string == nullptr
                           ? JNI_FALSE
                           : environment->CallBooleanMethod(activity, method,
                                                             uuid_string);
    if (uuid_string != nullptr) environment->DeleteLocalRef(uuid_string);
    if (environment->ExceptionCheck()) environment->ExceptionClear();
    if (klass != nullptr) environment->DeleteLocalRef(klass);
    environment->DeleteLocalRef(activity);
    return value == JNI_TRUE;
}

extern "C" bool gbb_android_bluetooth_start_join(const char* address,
                                                   const char* uuid) noexcept {
    auto* environment = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (environment == nullptr || activity == nullptr || address == nullptr ||
        uuid == nullptr) return false;
    const auto klass = environment->GetObjectClass(activity);
    const auto method = klass == nullptr ? nullptr : environment->GetMethodID(
        klass, "bluetoothStartJoin", "(Ljava/lang/String;Ljava/lang/String;)Z");
    const auto address_string = environment->NewStringUTF(address);
    const auto uuid_string = environment->NewStringUTF(uuid);
    const auto value = method == nullptr ? JNI_FALSE : environment->CallBooleanMethod(
        activity, method, address_string, uuid_string);
    if (address_string != nullptr) environment->DeleteLocalRef(address_string);
    if (uuid_string != nullptr) environment->DeleteLocalRef(uuid_string);
    if (environment->ExceptionCheck()) environment->ExceptionClear();
    if (klass != nullptr) environment->DeleteLocalRef(klass);
    environment->DeleteLocalRef(activity);
    return value == JNI_TRUE;
}

extern "C" int gbb_android_bluetooth_state() noexcept {
    auto* environment = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (environment == nullptr || activity == nullptr) return 0;
    const auto klass = environment->GetObjectClass(activity);
    const auto method = klass == nullptr ? nullptr : environment->GetMethodID(
        klass, "bluetoothState", "()I");
    const auto value = method == nullptr ? 0 : environment->CallIntMethod(activity, method);
    if (environment->ExceptionCheck()) environment->ExceptionClear();
    if (klass != nullptr) environment->DeleteLocalRef(klass);
    environment->DeleteLocalRef(activity);
    return value;
}

extern "C" std::string gbb_android_bluetooth_error() noexcept {
    auto* environment = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (environment == nullptr || activity == nullptr) return {};
    const auto klass = environment->GetObjectClass(activity);
    const auto method = klass == nullptr ? nullptr : environment->GetMethodID(
        klass, "bluetoothError", "()Ljava/lang/String;");
    auto value = method == nullptr ? nullptr : static_cast<jstring>(
        environment->CallObjectMethod(activity, method));
    std::string result;
    if (value != nullptr) {
        const auto* raw = environment->GetStringUTFChars(value, nullptr);
        if (raw != nullptr) {
            result = raw;
            environment->ReleaseStringUTFChars(value, raw);
        }
        environment->DeleteLocalRef(value);
    }
    if (environment->ExceptionCheck()) environment->ExceptionClear();
    if (klass != nullptr) environment->DeleteLocalRef(klass);
    environment->DeleteLocalRef(activity);
    return result;
}

extern "C" void gbb_android_bluetooth_stop() noexcept {
    auto* environment = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (environment == nullptr || activity == nullptr) return;
    const auto klass = environment->GetObjectClass(activity);
    const auto method = klass == nullptr ? nullptr : environment->GetMethodID(
        klass, "bluetoothStop", "()V");
    if (method != nullptr) environment->CallVoidMethod(activity, method);
    if (environment->ExceptionCheck()) environment->ExceptionClear();
    if (klass != nullptr) environment->DeleteLocalRef(klass);
    environment->DeleteLocalRef(activity);
}

extern "C" bool gbb_android_bluetooth_send(const std::uint8_t* bytes,
                                              const std::size_t size) noexcept {
    auto* environment = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (environment == nullptr || activity == nullptr || bytes == nullptr || size == 0 ||
        size > static_cast<std::size_t>(std::numeric_limits<jsize>::max())) return false;
    const auto klass = environment->GetObjectClass(activity);
    const auto method = klass == nullptr ? nullptr : environment->GetMethodID(
        klass, "bluetoothSend", "([B)Z");
    auto array = environment->NewByteArray(static_cast<jsize>(size));
    if (array != nullptr) environment->SetByteArrayRegion(
        array, 0, static_cast<jsize>(size), reinterpret_cast<const jbyte*>(bytes));
    const auto value = method == nullptr || array == nullptr ? JNI_FALSE :
        environment->CallBooleanMethod(activity, method, array);
    if (array != nullptr) environment->DeleteLocalRef(array);
    if (environment->ExceptionCheck()) environment->ExceptionClear();
    if (klass != nullptr) environment->DeleteLocalRef(klass);
    environment->DeleteLocalRef(activity);
    return value == JNI_TRUE;
}

extern "C" std::size_t gbb_android_bluetooth_receive(
    std::uint8_t* bytes, const std::size_t capacity) noexcept {
    auto* environment = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (environment == nullptr || activity == nullptr || bytes == nullptr || capacity == 0) return 0;
    const auto klass = environment->GetObjectClass(activity);
    const auto method = klass == nullptr ? nullptr : environment->GetMethodID(
        klass, "bluetoothReceive", "()[B");
    auto array = method == nullptr ? nullptr : static_cast<jbyteArray>(
        environment->CallObjectMethod(activity, method));
    std::size_t count = 0;
    if (array != nullptr) {
        const auto length = static_cast<std::size_t>(environment->GetArrayLength(array));
        if (length <= capacity) {
            environment->GetByteArrayRegion(array, 0, static_cast<jsize>(length),
                                             reinterpret_cast<jbyte*>(bytes));
            count = length;
        }
        environment->DeleteLocalRef(array);
    }
    if (environment->ExceptionCheck()) environment->ExceptionClear();
    if (klass != nullptr) environment->DeleteLocalRef(klass);
    environment->DeleteLocalRef(activity);
    return count;
}

#endif
