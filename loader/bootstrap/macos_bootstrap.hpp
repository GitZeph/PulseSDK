// loader/bootstrap/macos_bootstrap.hpp — bootstrap macOS (Req 1.1, 1.2).
//
// Implementazione di `IPlatformBootstrap` per macOS (x86-64 / arm64) basata su
// una `dylib` con un costruttore `__attribute__((constructor))`. La libreria
// viene mappata nel processo di Geometry Dash PRIMA dell'esecuzione del
// `main`/della scena iniziale del gioco con uno di questi due meccanismi:
//
//   1. `DYLD_INSERT_LIBRARIES` — la dylib del loader viene precaricata dal
//      dynamic linker (dyld) prima delle dipendenze del gioco; il costruttore
//      della dylib è eseguito durante il caricamento delle immagini, quindi
//      prima del `main`.
//   2. Patch del Mach-O load command — si aggiunge un comando di load
//      (`LC_LOAD_DYLIB`) all'eseguibile di GD in modo che dyld carichi la
//      dylib del loader come dipendenza; anche in questo caso il costruttore
//      è invocato prima del `main`.
//
// In entrambi i casi il costruttore early-load invoca l'entry point
// centralizzato del runtime (Req 1.4), ottenendo il controllo prima della
// scena iniziale (Req 1.2).
//
// La dichiarazione della classe è platform-agnostic, così l'intero progetto
// configura e compila anche su host non-Apple (Windows/Linux). Il codice che
// usa le API specifiche di macOS è isolato nel .cpp dietro `#if defined(__APPLE__)`.
#ifndef PULSE_LOADER_BOOTSTRAP_MACOS_BOOTSTRAP_HPP
#define PULSE_LOADER_BOOTSTRAP_MACOS_BOOTSTRAP_HPP

#include <functional>
#include <string_view>

#include "bootstrap/platform_bootstrap.hpp"

namespace pulse::bootstrap {

// Entry point centralizzato del runtime invocato dal bootstrap una volta
// ottenuto il controllo (Req 1.4). Restituisce `true` se il runtime è stato
// avviato correttamente. È iniettabile per consentire i test e per
// disaccoppiare il bootstrap dal Loader Core.
using MacOSRuntimeEntryPoint = std::function<bool()>;

// Variabile d'ambiente usata da dyld per precaricare la dylib del loader prima
// delle dipendenze del gioco. Documentata qui come riferimento del meccanismo
// di early-load primario.
inline constexpr std::string_view kDyldInsertLibrariesEnv = "DYLD_INSERT_LIBRARIES";

class MacOSBootstrap final : public IPlatformBootstrap {
public:
    MacOSBootstrap() = default;

    // Costruisce il bootstrap con un entry point del runtime esplicito.
    // Se non fornito, `inject()` considera l'aggancio comunque riuscito (utile
    // finché il Loader Core non è disponibile in questo target).
    explicit MacOSBootstrap(MacOSRuntimeEntryPoint runtime_entry)
        : runtime_entry_(std::move(runtime_entry)) {}

    // Esegue il bootstrap via dylib early-load. Su macOS: verifica di non
    // essere già iniettato, conferma di trovarsi nel contesto di caricamento
    // anticipato (costruttore della dylib, prima della scena iniziale) e invoca
    // l'entry point del runtime (Req 1.2, 1.4). Su host non-Apple ritorna un
    // fallimento diagnostico (`UnsupportedHost`) senza side effect, così la
    // build cross-platform resta compilabile e testabile (Req 1.3).
    BootstrapResult inject() override;

    // Questa implementazione serve macOS (x86-64 / arm64) (Req 1.1).
    Platform platform() const override { return Platform::MacOS; }

private:
    MacOSRuntimeEntryPoint runtime_entry_{};
    bool                   injected_{false};
};

}  // namespace pulse::bootstrap

#endif  // PULSE_LOADER_BOOTSTRAP_MACOS_BOOTSTRAP_HPP
