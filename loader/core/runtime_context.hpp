// loader/core/runtime_context.hpp — contesto runtime esposto alle mod.
//
// Definisce l'identità della piattaforma corrente e della versione di
// Geometry Dash rilevata. Il `RuntimeContext` è il valore che il LoaderCore
// rende disponibile a tutte le mod caricate dopo un'inizializzazione riuscita
// (Requisito 1.5).
//
// Tipi mantenuti nel namespace `pulse::loader` per restare self-contained nel
// target del loader ed evitare collisioni con il `pulse::Platform` del layer
// di bootstrap.
#ifndef PULSE_LOADER_CORE_RUNTIME_CONTEXT_HPP
#define PULSE_LOADER_CORE_RUNTIME_CONTEXT_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace pulse::loader {

// Versione del binario di Geometry Dash (GD_Version), es. {2, 2074}.
struct GdVersion {
    std::uint32_t major{0};
    std::uint32_t minor{0};

    friend constexpr bool operator==(const GdVersion&, const GdVersion&) = default;
};

// Piattaforma runtime sulla quale il loader sta operando.
//
// Include le piattaforme target di Pulse più i valori `Linux`/`Unknown`
// necessari per l'esecuzione host (build e test su Linux/macOS dev machine).
enum class Platform {
    Unknown,
    WindowsX64,
    MacOS,
    Linux,
    AndroidArm64,
    AndroidArmV7,
    IOSArm64,
};

// Identificatore testuale stabile della piattaforma, nel formato
// "<os>-<arch>" (es. "windows-x64", "macos-arm64", "android-arm64").
std::string_view platform_id(Platform platform) noexcept;

// Rileva a compile-time la piattaforma host su cui il loader è stato compilato.
Platform current_platform() noexcept;

// Contesto runtime esposto a tutte le mod caricate (Requisito 1.5):
// versione di GD rilevata, piattaforma e relativo identificatore testuale.
struct RuntimeContext {
    GdVersion gdVersion{};
    Platform platform{Platform::Unknown};
    std::string platformId;
};

}  // namespace pulse::loader

#endif  // PULSE_LOADER_CORE_RUNTIME_CONTEXT_HPP
