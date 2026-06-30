// loader/core/version_detector.cpp — implementazione di default del detector.
#include "core/version_detector.hpp"

#include <string>

// Detection reale dall'immagine Mach-O in memoria (Requisito 5.1): attiva solo
// sul Prioritized_Target macOS quando il loader è compilato come Loader_Artifact
// iniettato nel processo di Geometry Dash. Sull'host di test la macro
// `PULSE_LOADER_ARTIFACT` non è definita e la lettura ricade sullo span
// vuoto/iniettabile, preservando i test host.
#if defined(__APPLE__) && defined(PULSE_LOADER_ARTIFACT)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/machine.h>

#include <cstring>
#include <string_view>
#endif

namespace pulse::loader {

#if defined(__APPLE__) && defined(PULSE_LOADER_ARTIFACT)
namespace {

// Header Mach-O dell'immagine principale del processo (l'eseguibile di GD,
// indice 0 nella lista delle immagini di dyld).
const mach_header_64* main_macho_header() noexcept {
    return reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(0));
}

// Confronta in modo sicuro un campo a lunghezza fissa (segname/sectname,
// `char[16]`, non necessariamente terminato da NUL) con un nome atteso.
bool fixed_name_equals(const char* field, std::size_t field_size,
                       std::string_view expected) noexcept {
    const std::size_t len = ::strnlen(field, field_size);
    return std::string_view(field, len) == expected;
}

}  // namespace
#endif

std::uint64_t section_hash(std::span<const std::byte> bytes) noexcept {
    // FNV-1a 64-bit: deterministico e privo di dipendenze esterne.
    constexpr std::uint64_t kOffsetBasis = 1469598103934665603ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;

    std::uint64_t hash = kOffsetBasis;
    for (const std::byte b : bytes) {
        hash ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(b));
        hash *= kPrime;
    }
    return hash;
}

DefaultVersionDetector::DefaultVersionDetector() = default;

DefaultVersionDetector::DefaultVersionDetector(std::vector<VersionSignature> signatures)
    : signatures_(std::move(signatures)) {}

#if defined(__APPLE__) && defined(PULSE_LOADER_ARTIFACT)

std::span<const std::byte> DefaultVersionDetector::known_section_bytes() const {
    // Legge la sezione nota (`__TEXT,__cstring`) direttamente dall'immagine
    // Mach-O caricata in memoria (Req 5.1). Lo span punta nei byte già mappati
    // dell'immagine, viva per tutta la durata del processo: nessuna copia.
    const mach_header_64* header = main_macho_header();
    if (header == nullptr || header->magic != MH_MAGIC_64) {
        return {};
    }
    const std::intptr_t slide = _dyld_get_image_vmaddr_slide(0);

    const auto* cmd = reinterpret_cast<const load_command*>(
        reinterpret_cast<const std::uint8_t*>(header) + sizeof(mach_header_64));
    for (std::uint32_t i = 0; i < header->ncmds; ++i) {
        if (cmd->cmd == LC_SEGMENT_64) {
            const auto* seg = reinterpret_cast<const segment_command_64*>(cmd);
            if (fixed_name_equals(seg->segname, sizeof(seg->segname), "__TEXT")) {
                const auto* sect = reinterpret_cast<const section_64*>(
                    reinterpret_cast<const std::uint8_t*>(seg) +
                    sizeof(segment_command_64));
                for (std::uint32_t s = 0; s < seg->nsects; ++s) {
                    if (fixed_name_equals(sect[s].sectname,
                                          sizeof(sect[s].sectname), "__cstring")) {
                        const auto base = static_cast<std::uintptr_t>(sect[s].addr) +
                                          static_cast<std::uintptr_t>(slide);
                        return std::span<const std::byte>(
                            reinterpret_cast<const std::byte*>(base),
                            static_cast<std::size_t>(sect[s].size));
                    }
                }
            }
        }
        cmd = reinterpret_cast<const load_command*>(
            reinterpret_cast<const std::uint8_t*>(cmd) + cmd->cmdsize);
    }
    return {};
}

Platform DefaultVersionDetector::detected_platform() const {
    // La piattaforma è derivata dal `cputype` dell'header Mach-O caricato
    // (Req 5.1): nessuna derivazione da config/default/stub.
    const mach_header_64* header = main_macho_header();
    if (header == nullptr) {
        return Platform::Unknown;
    }
    switch (header->cputype) {
        case CPU_TYPE_ARM64:
        case CPU_TYPE_X86_64:
            return Platform::MacOS;
        default:
            return Platform::Unknown;
    }
}

std::string DefaultVersionDetector::detected_platform_id() const {
    // L'identificatore "<os>-<arch>" è ricavato da `cputype/cpusubtype` del
    // Mach-O caricato, distinguendo arm64 da x86_64 indipendentemente
    // dall'architettura di compilazione.
    const mach_header_64* header = main_macho_header();
    if (header != nullptr) {
        switch (header->cputype) {
            case CPU_TYPE_ARM64:
                return "macos-arm64";
            case CPU_TYPE_X86_64:
                return "macos-x64";
            default:
                break;
        }
    }
    return std::string{platform_id(Platform::Unknown)};
}

#else  // host di test: span vuoto/iniettabile, piattaforma da compile-time.

std::span<const std::byte> DefaultVersionDetector::known_section_bytes() const {
    // Nessun binario di GD mappato nel contesto host: nessuna sezione da
    // ispezionare. Le piattaforme reali sovrascrivono questo metodo (o il
    // bootstrap fornisce un detector con accesso alle sezioni del processo).
    return {};
}

Platform DefaultVersionDetector::detected_platform() const {
    // Sull'host di test la piattaforma è quella rilevata a compile-time.
    return current_platform();
}

std::string DefaultVersionDetector::detected_platform_id() const {
    return std::string{platform_id(detected_platform())};
}

#endif

std::optional<GdVersion> DefaultVersionDetector::detect() {
    const std::span<const std::byte> section = known_section_bytes();
    if (section.empty()) {
        // Sezione non disponibile/illeggibile: impossibile identificare la
        // versione (Requisito 1.7).
        return std::nullopt;
    }

    const std::uint64_t hash = section_hash(section);
    for (const VersionSignature& sig : signatures_) {
        if (sig.sectionHash == hash) {
            return sig.version;
        }
    }
    return std::nullopt;
}

std::optional<RuntimeContext> DefaultVersionDetector::detect_context() {
    // Rileva la GD_Version dal binario realmente caricato. Se non identificabile
    // non si espone alcuna coppia (Req 5.3): il chiamante fallisce in sicurezza.
    const std::optional<GdVersion> version = detect();
    if (!version.has_value()) {
        return std::nullopt;
    }
    // Coppia *esatta* (GD_Version, piattaforma) da inoltrare a IBindingsProvider
    // (Req 5.2), esposta come RuntimeContext in sola lettura (Req 5.5).
    return RuntimeContext{*version, detected_platform(), detected_platform_id()};
}

}  // namespace pulse::loader
