// loader/core/runtime_context.cpp — rilevamento piattaforma e identificatori.
#include "core/runtime_context.hpp"

#include "pulse_loader/loader.hpp"

namespace pulse::loader {

std::string_view platform_id(Platform platform) noexcept {
    switch (platform) {
        case Platform::WindowsX64:
            return "windows-x64";
        case Platform::MacOS:
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
            return "macos-arm64";
#else
            return "macos-x64";
#endif
        case Platform::Linux:
#if defined(__aarch64__) || defined(__arm64__)
            return "linux-arm64";
#else
            return "linux-x64";
#endif
        case Platform::AndroidArm64:
            return "android-arm64";
        case Platform::AndroidArmV7:
            return "android-armv7";
        case Platform::IOSArm64:
            return "ios-arm64";
        case Platform::Unknown:
        default:
            return "unknown";
    }
}

Platform current_platform() noexcept {
    // La piattaforma host è determinata a compile-time. Android e iOS non
    // sono target host del MVP e vengono rilevati dai rispettivi bootstrap.
#if defined(__ANDROID__)
#if defined(__aarch64__)
    return Platform::AndroidArm64;
#else
    return Platform::AndroidArmV7;
#endif
#else
    switch (host_os()) {
        case HostOs::Windows:
            return Platform::WindowsX64;
        case HostOs::MacOS:
            return Platform::MacOS;
        case HostOs::Linux:
            return Platform::Linux;
    }
    return Platform::Unknown;
#endif
}

}  // namespace pulse::loader
