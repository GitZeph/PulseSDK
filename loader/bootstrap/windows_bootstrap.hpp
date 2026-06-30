// loader/bootstrap/windows_bootstrap.hpp — bootstrap Windows x64 (Req 2.3, 2.6).
//
// Implementazione di `IPlatformBootstrap` per Windows basata sulla tecnica
// del DLL proxy: il loader viene distribuito come libreria con lo stesso nome
// di una DLL che Geometry Dash carica al lancio (es. `XInput9_1_0.dll` o
// `fmod.dll`). Il loader del SO mappa questa DLL PRIMA dell'esecuzione del
// `main`/della scena iniziale del gioco; il punto di ingresso della DLL
// (`DllMain`/costruttore di early-load) avvia il runtime centralizzato
// `pulse_loader_runtime_entry` e poi forwarda gli export alla DLL reale, così
// il gioco continua a funzionare normalmente (Req 2.3). Vettore di fallback:
// injection esterna via `CreateRemoteThread` (vedi il .cpp).
//
// Dietro lo stesso seam `IPlatformBootstrap` del bootstrap macOS, condivide la
// medesima policy: un idempotency guard di processo garantisce che l'entry
// centralizzato sia invocato UNA SOLA VOLTA anche con più vettori di early-load
// presenti (Req 2.6), e una policy safe-fail logga la causa diagnostica senza
// terminare il processo, lasciando partire GD con zero mod (Req 2.7, 2.8).
//
// La dichiarazione della classe è platform-agnostic, così l'intero progetto
// configura e compila anche su host non-Windows (macOS/Linux). Il codice che
// usa le API Win32 è isolato nel .cpp dietro `#if defined(_WIN32)`.
#ifndef PULSE_LOADER_BOOTSTRAP_WINDOWS_BOOTSTRAP_HPP
#define PULSE_LOADER_BOOTSTRAP_WINDOWS_BOOTSTRAP_HPP

#include <functional>
#include <string_view>

#include "bootstrap/platform_bootstrap.hpp"

namespace pulse::bootstrap {

// Entry point centralizzato del runtime invocato dal bootstrap una volta
// ottenuto il controllo (Req 2.6). Restituisce `true` se il runtime è stato
// avviato correttamente. È iniettabile per consentire i test e per disaccoppiare
// il bootstrap dal Loader Core (riuso del `CentralizedLoader`).
using RuntimeEntryPoint = std::function<bool()>;

// DLL "ospite" verso cui il proxy inoltra gli export. Configurabile per
// supportare diverse DLL caricate da Geometry Dash al lancio a seconda della
// build/installazione. `XInput9_1_0.dll` è il default; `fmod.dll` è una
// alternativa comune (GD linka FMOD per l'audio) (Req 2.3).
inline constexpr std::string_view kDefaultProxyTargetDll = "XInput9_1_0.dll";
inline constexpr std::string_view kAlternateProxyTargetDll = "fmod.dll";

class WindowsBootstrap final : public IPlatformBootstrap {
public:
    WindowsBootstrap() = default;

    // Costruisce il bootstrap con un entry point del runtime esplicito.
    // Se non fornito, `inject()` usa un entry point di default no-op che si
    // limita a segnalare il successo dell'aggancio (utile finché il Loader
    // Core non è disponibile).
    explicit WindowsBootstrap(RuntimeEntryPoint runtime_entry)
        : runtime_entry_(std::move(runtime_entry)) {}

    // Esegue il bootstrap via DLL proxy. Su Windows: verifica di non essere
    // già iniettato, prepara la catena di forwarding verso la DLL di sistema
    // reale e invoca l'entry point del runtime prima della scena iniziale.
    // Su host non-Windows ritorna un fallimento diagnostico
    // (`UnsupportedHost`) senza side effect, così la build cross-platform resta
    // compilabile e testabile.
    BootstrapResult inject() override;

    // Questa implementazione serve esclusivamente Windows x64 (Req 1.1).
    Platform platform() const override { return Platform::WindowsX64; }

private:
    RuntimeEntryPoint runtime_entry_{};
    bool              injected_{false};
};

}  // namespace pulse::bootstrap

#endif  // PULSE_LOADER_BOOTSTRAP_WINDOWS_BOOTSTRAP_HPP
