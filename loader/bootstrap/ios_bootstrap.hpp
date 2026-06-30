// loader/bootstrap/ios_bootstrap.hpp — bootstrap iOS arm64 (Req 1.1, 1.2).
//
// Implementazione di `IPlatformBootstrap` per iOS. Il loader è distribuito come
// `dylib` che ottiene il controllo del processo di Geometry Dash PRIMA della
// scena iniziale (Req 1.2) tramite un costruttore con early-load
// (`__attribute__((constructor))`). Due scenari di distribuzione sono previsti
// (entrambi documentati nel .cpp):
//   1. Jailbreak — la dylib è iniettata da un framework di tweak injection
//      (Cydia Substrate / ElleKit) che la carica dentro ogni processo bersaglio
//      prima del `main`.
//   2. Repacking / sideload — la dylib è inserita nei load command del Mach-O
//      dell'app ripacchettizzata (es. via `optool`/`insert_dylib`) e firmata
//      nuovamente, così il dynamic linker la carica all'avvio del processo.
//
// La dichiarazione della classe è platform-agnostic, così l'intero progetto
// configura e compila anche sull'host di sviluppo macOS/Linux. Il codice
// specifico iOS è isolato nel .cpp dietro la guardia TargetConditionals
// (`TARGET_OS_IPHONE`) per NON attivarsi su macOS desktop (il path macOS è di
// competenza di `MacOSBootstrap`, attività 23.1).
#ifndef PULSE_LOADER_BOOTSTRAP_IOS_BOOTSTRAP_HPP
#define PULSE_LOADER_BOOTSTRAP_IOS_BOOTSTRAP_HPP

#include <functional>

#include "bootstrap/platform_bootstrap.hpp"

namespace pulse::bootstrap {

// Entry point centralizzato del runtime invocato dal bootstrap una volta
// ottenuto il controllo (Req 1.4). Restituisce `true` se il runtime è stato
// avviato correttamente. È iniettabile per consentire i test e per disaccoppiare
// il bootstrap dal Loader Core.
using RuntimeEntryPoint = std::function<bool()>;

class IOSBootstrap final : public IPlatformBootstrap {
public:
    IOSBootstrap() = default;

    // Costruisce il bootstrap con un entry point del runtime esplicito.
    // Se non fornito, `inject()` su iOS si limita a segnalare il successo
    // dell'early-load (utile finché il Loader Core non è disponibile).
    explicit IOSBootstrap(RuntimeEntryPoint runtime_entry)
        : runtime_entry_(std::move(runtime_entry)) {}

    // Esegue il bootstrap via constructor early-load. Su iOS
    // (TARGET_OS_IPHONE): verifica di non essere già iniettato e invoca l'entry
    // point del runtime prima della scena iniziale (Req 1.2, 1.4). Su host non
    // iOS (incluso macOS desktop) ritorna un fallimento diagnostico
    // (`UnsupportedHost`) senza side effect, così la build cross-platform resta
    // compilabile e testabile (Req 1.3).
    BootstrapResult inject() override;

    // Questa implementazione serve esclusivamente iOS arm64 (Req 1.1).
    Platform platform() const override { return Platform::IOSArm64; }

private:
    RuntimeEntryPoint runtime_entry_{};
    bool              injected_{false};
};

}  // namespace pulse::bootstrap

#endif  // PULSE_LOADER_BOOTSTRAP_IOS_BOOTSTRAP_HPP
