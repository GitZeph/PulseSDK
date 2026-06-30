// pulse/version.hpp — versione dello SDK Pulse esposta pubblicamente.
#ifndef PULSE_VERSION_HPP
#define PULSE_VERSION_HPP

#define PULSE_SDK_VERSION_MAJOR 0
#define PULSE_SDK_VERSION_MINOR 1
#define PULSE_SDK_VERSION_PATCH 0

namespace pulse {

struct SdkVersion {
    int major;
    int minor;
    int patch;
};

// Versione dello SDK con cui è stata compilata la mod corrente.
constexpr SdkVersion sdk_version() noexcept {
    return SdkVersion{
        PULSE_SDK_VERSION_MAJOR,
        PULSE_SDK_VERSION_MINOR,
        PULSE_SDK_VERSION_PATCH,
    };
}

}  // namespace pulse

#endif  // PULSE_VERSION_HPP
