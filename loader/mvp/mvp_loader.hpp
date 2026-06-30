// loader/mvp/mvp_loader.hpp — cablaggio centralizzato dell'MVP (task 4.4).
//
// `MvpLoader` collega i mattoni dell'MVP in un unico flusso di caricamento
// centralizzato (Req 1.4), realizzando il criterio di completamento dell'MVP
// (design → "Definizione dell'MVP"):
//
//   bootstrap → core → bindings → hook backend → PULSE_HOOK
//
// Sequenza (happy path):
//   1. Bootstrap ottiene il controllo del processo e invoca l'entry point del
//      runtime PRIMA della scena iniziale (Req 1.2, 1.4) — qui `run()`.
//   2. Loader Core rileva piattaforma + GD_Version ed espone il RuntimeContext
//      (Req 1.5); su mancato rilevamento aborta (Req 1.7).
//   3. Bindings System carica il set esatto (GD_Version, piattaforma) e risolve
//      l'indirizzo di `MenuLayer::init` (Req 20.1, 20.2).
//   4. Hook backend installa il detour dichiarato con PULSE_HOOK all'indirizzo
//      risolto e restituisce il trampolino verso l'originale (Req 2.2).
//   5. Il trampolino viene cablato nello slot del detour: invocando il detour,
//      questo esegue e poi chiama l'originale preservando parametri/ritorno
//      (Req 2.2, 5.3).
//
// Testabilità (host macOS/Linux senza GD reale né MinHook): detector e backend
// sono INIETTABILI. I test usano un detector che restituisce GD 2.2074 e il
// `FakeBackend` in-memory; un override opzionale del platformId consente di
// puntare al set (2.2074, windows-x64) anche quando l'host non è Windows.
//
// Riconciliazione `GdVersion`: il Loader Core usa `pulse::loader::GdVersion`
// (campi `std::uint32_t`), il Bindings System usa `pulse::loader::bindings::
// GdVersion` (campi `int`). La conversione avviene QUI, al cablaggio, senza
// modificare l'interfaccia pubblica dei due moduli.
#ifndef PULSE_LOADER_MVP_MVP_LOADER_HPP
#define PULSE_LOADER_MVP_MVP_LOADER_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "bindings/bindings.hpp"
#include "bootstrap/platform_bootstrap.hpp"
#include "core/loader_core.hpp"
#include "core/runtime_context.hpp"
#include "core/version_detector.hpp"
#include "hooking/hook_backend.hpp"
#include "mvp/menulayer_init_hook.hpp"

namespace pulse::loader::mvp {

// Esito del flusso di caricamento centralizzato dell'MVP.
enum class MvpStatus {
    Success,                  // detour installato e trampolino cablato
    VersionDetectionFailed,   // GD_Version non rilevata (Req 1.7)
    BindingsNotFound,         // nessun set esatto per (versione, piattaforma) (Req 20.3)
    SymbolUnresolved,         // simbolo non risolto o non risolvibile (Req 20.4)
    HookNotRegistered,        // nessun detour PULSE_HOOK per il simbolo
    InstallFailed,            // il backend ha fallito l'installazione (Req 2.5)
    TrampolineBindFailed,     // impossibile cablare il trampolino nello slot
};

// Bersaglio dell'hook: simbolo del binding del gioco + simbolo di registrazione
// del detour PULSE_HOOK. Per l'MVP punta a `MenuLayer::init` / `MenuLayer_init`.
struct HookTarget {
    std::string_view bindingSymbol{kMenuLayerInitBindingSymbol};
    std::string_view registrationSymbol{kMenuLayerInitRegistrationSymbol};
};

// Configurazione del flusso MVP.
struct MvpConfig {
    HookTarget target{};
    // Override opzionale del platformId per la chiave dei bindings. Vuoto =>
    // usa il platformId del RuntimeContext rilevato. Utile per puntare al set
    // (2.2074, windows-x64) dagli host di test non-Windows.
    std::string platformIdOverride{};
};

// Risultato dettagliato del flusso, con diagnostica e indirizzo agganciato.
struct MvpResult {
    MvpStatus status{MvpStatus::VersionDetectionFailed};
    std::string message{};
    std::uintptr_t hookedAddress{0};
    bool injected{false};  // true sse lo status è Success

    [[nodiscard]] bool ok() const noexcept { return status == MvpStatus::Success; }
};

// Converte la GdVersion del Loader Core nella GdVersion del Bindings System.
// Riconciliazione al cablaggio (campi uint32_t -> int).
[[nodiscard]] bindings::GdVersion to_bindings_version(const GdVersion& version) noexcept;

// Costruisce la chiave dei bindings dal contesto runtime, applicando l'override
// opzionale del platformId.
[[nodiscard]] bindings::BindingKey make_binding_key(const RuntimeContext& context,
                                                    std::string_view platformIdOverride = {});

// Orchestratore del flusso di caricamento centralizzato dell'MVP.
class MvpLoader {
public:
    // Costruisce con i collaboratori iniettabili:
    //  - `detector`: rilevamento di GD_Version (test: detector fittizio);
    //  - `backend` : backend di hooking (test: FakeBackend in-memory);
    //  - `provider`: provider di bindings (default: EmbeddedBindingsProvider);
    //  - `log`     : sink diagnostico del flusso (default: stderr).
    // Nessun argomento può essere nullo eccetto `log`.
    MvpLoader(std::shared_ptr<IVersionDetector> detector,
              std::unique_ptr<hooking::IHookBackend> backend,
              std::shared_ptr<bindings::IBindingsProvider> provider,
              MvpConfig config = {},
              DiagnosticSink log = nullptr);

    // Entry point centralizzato del runtime (Req 1.4): esegue l'intero flusso
    // core → bindings → backend → cablaggio del trampolino e installa il detour
    // dimostrativo su `MenuLayer::init`.
    MvpResult run();

    // Esegue il bootstrap di piattaforma con `run()` come entry point del
    // runtime (Req 1.2). Su host non-Windows il `WindowsBootstrap` riporta
    // `UnsupportedHost` senza eseguire l'entry point, lasciando partire GD
    // senza mod (Req 1.3); su Windows l'entry point avvia il flusso completo.
    bootstrap::BootstrapResult bootstrap_and_run();

    // Contesto runtime rilevato (valido dopo un `run()` riuscito) (Req 1.5).
    [[nodiscard]] const RuntimeContext& context() const noexcept;

    // Accesso al backend di hooking iniettato (per introspezione nei test).
    [[nodiscard]] hooking::IHookBackend& backend() noexcept { return *backend_; }

    // Indirizzo agganciato dall'ultimo `run()` riuscito (0 se nessuno).
    [[nodiscard]] std::uintptr_t hooked_address() const noexcept { return hooked_address_; }

private:
    void log(std::string_view message) const;
    MvpResult fail(MvpStatus status, std::string message) const;

    LoaderCore core_;
    std::unique_ptr<hooking::IHookBackend> backend_;
    std::shared_ptr<bindings::IBindingsProvider> provider_;
    MvpConfig config_;
    DiagnosticSink log_;
    std::uintptr_t hooked_address_{0};
};

}  // namespace pulse::loader::mvp

#endif  // PULSE_LOADER_MVP_MVP_LOADER_HPP
