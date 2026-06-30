// loader/lifecycle/compatibility.cpp — implementazione del predicato di
// compatibilità tra Mod_Manifest e Runtime_Context (task 1.4).
#include "lifecycle/compatibility.hpp"

namespace pulse::lifecycle {

namespace {

// Rappresentazione testuale "major.minor.patch" di una SemVer, usata nei
// messaggi diagnostici (Req 3.4).
std::string semver_to_string(const pulse::manifest::SemVer& v) {
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." +
           std::to_string(v.patch);
}

}  // namespace

std::optional<pulse::manifest::TargetPlatform> runtime_target_platform(
    pulse::loader::Platform platform) noexcept {
    using pulse::loader::Platform;
    using pulse::manifest::TargetPlatform;

    switch (platform) {
        case Platform::WindowsX64:
            return TargetPlatform::Windows;
        case Platform::MacOS:
            // L'insieme finito modella esclusivamente "macOS arm64" (Apple
            // Silicon, Prioritized_Target). Solo una build arm64 mappa su
            // MacOSArm64; macOS x86-64 non è rappresentabile e resta `nullopt`.
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
            return TargetPlatform::MacOSArm64;
#else
            return std::nullopt;
#endif
        case Platform::AndroidArm64:
        case Platform::AndroidArmV7:
            return TargetPlatform::Android;
        case Platform::IOSArm64:
            return TargetPlatform::IOS;
        case Platform::Linux:
        case Platform::Unknown:
        default:
            return std::nullopt;
    }
}

CompatResult check_compatibility(const pulse::manifest::Manifest& m,
                                 const pulse::loader::RuntimeContext& ctx) {
    using pulse::manifest::SemVer;

    const pulse::manifest::Compatibility& compat = m.compat;

    // (1) Compat assente: piattaforma e/o intervallo non dichiarati (Req 3.6).
    if (!compat.platform.has_value() || !compat.gdVersion.has_value()) {
        std::string missing;
        if (!compat.platform.has_value() && !compat.gdVersion.has_value()) {
            missing = "platform e gd_version";
        } else if (!compat.platform.has_value()) {
            missing = "platform";
        } else {
            missing = "gd_version";
        }
        return CompatResult{
            false, CompatCause::MissingCompat,
            "Mod '" + m.id + "': sezione [compat] incompleta, campo mancante: " +
                missing + " (Req 3.6)"};
    }

    // (2) Piattaforma bersaglio vs piattaforma del Runtime_Context (Req 3.3).
    const pulse::manifest::TargetPlatform declared = *compat.platform;
    const std::optional<pulse::manifest::TargetPlatform> runtime =
        runtime_target_platform(ctx.platform);
    if (!runtime.has_value() || *runtime != declared) {
        const std::string runtimeName =
            ctx.platformId.empty() ? std::string{pulse::loader::platform_id(ctx.platform)}
                                   : ctx.platformId;
        return CompatResult{
            false, CompatCause::PlatformMismatch,
            "Mod '" + m.id + "': piattaforma bersaglio dichiarata '" +
                pulse::manifest::to_string(declared) +
                "' diversa dalla piattaforma del Runtime_Context '" + runtimeName +
                "' (Req 3.3)"};
    }

    // (3) Versione di GD del Runtime_Context promossa a SemVer{major, minor, 0}
    //     e confrontata con [min, max], ESTREMI INCLUSI (Req 3.4).
    const SemVer runtimeVersion{ctx.gdVersion.major, ctx.gdVersion.minor, 0};
    const pulse::manifest::GdVersionRange& range = *compat.gdVersion;
    if (!range.contains(runtimeVersion)) {
        return CompatResult{
            false, CompatCause::VersionOutOfRange,
            "Mod '" + m.id + "': versione GD del Runtime_Context " +
                semver_to_string(runtimeVersion) + " fuori dall'intervallo bersaglio [" +
                semver_to_string(range.min) + ", " + semver_to_string(range.max) +
                "] (estremi inclusi) (Req 3.4)"};
    }

    // Compatibile: piattaforma coincidente e versione nell'intervallo (Req 3.2).
    return CompatResult{true, CompatCause::Ok, "Mod '" + m.id + "': compatibile"};
}

}  // namespace pulse::lifecycle
