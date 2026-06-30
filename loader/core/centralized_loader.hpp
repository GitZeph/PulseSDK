// loader/core/centralized_loader.hpp — entry point centralizzato del runtime
// Pulse (Layer 0 → Layer 1), task 23.4.
//
// `CentralizedLoader` è l'UNICO punto di ingresso del caricamento (Requisito
// 1.4): un solo `start()` orchestra bootstrap di piattaforma e inizializzazione
// del runtime, anziché richiedere un'iniezione separata per ogni mod. Gestisce
// inoltre i due percorsi di degradazione "senza mod" richiesti dal Requisito 1:
//
//   * Req 1.3 — se l'iniezione di piattaforma fallisce, logga la diagnostica e
//     consente l'avvio di Geometry Dash SENZA mod (non aborta il processo).
//   * Req 1.6 — se l'inizializzazione del runtime non si completa entro 10
//     secondi, un watchdog interrompe il caricamento delle mod, logga un errore
//     diagnostico di timeout e consente l'avvio di GD in modalità senza mod.
//
// Testabilità host (senza GD né attese reali di 10 s): bootstrap, clock e step
// di inizializzazione sono INIETTABILI. Il watchdog misura il tempo trascorso
// tramite un `ClockFn` iniettabile e passa allo step un `WatchdogToken` che può
// segnalare lo scadere del budget; un test può quindi forzare il timeout in
// modo deterministico facendo avanzare il clock fittizio, senza dormire 10 s.
#ifndef PULSE_LOADER_CORE_CENTRALIZED_LOADER_HPP
#define PULSE_LOADER_CORE_CENTRALIZED_LOADER_HPP

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "bindings/bindings.hpp"  // IBindingsProvider, BindingKey, FunctionBinding
#include "bootstrap/platform_bootstrap.hpp"
#include "core/loader_core.hpp"        // DiagnosticSink, default_diagnostic_sink()
#include "core/runtime_context.hpp"    // RuntimeContext (coppia rilevata)
#include "hooking/hook_backend.hpp"    // IHookBackend (disponibilità del backend)

namespace pulse::loader {

// Modalità di avvio risultante dal caricamento centralizzato.
enum class StartMode {
    // Iniezione riuscita e inizializzazione completata entro il budget: le mod
    // sono state caricate attraverso l'entry point centralizzato (Req 1.4).
    ModsLoaded,
    // Avvio di Geometry Dash SENZA mod (Req 1.3 / 1.6): il gioco parte comunque.
    ModLess,
};

// Causa precisa dell'esito, utile a diagnostica e test.
//
// I primi quattro valori sono ereditati dalla spec `pulse-sdk` (degradazione di
// iniezione/timeout/init). I successivi sono i nuovi punti di fallimento cablati
// nella policy fail-open centralizzata dal task 3.12 della feature
// `pulse-gd-integration`: ognuno porta a 0 hook installati, logga la causa e
// lascia Geometry Dash raggiungere il menu senza terminare il processo
// (eseguibile/asset byte-for-byte invariati — Req 10.4).
enum class StartReason {
    Success,          // tutto ok: mod caricate
    InjectionFailed,  // iniezione di piattaforma fallita (Req 1.3 / 2.7, 2.8)
    InitTimeout,      // watchdog 10 s scattato (Req 1.6)
    InitFailed,       // step di init fallito entro il budget (es. Req 1.7)

    // --- Nuovi punti di fallimento fail-open (task 3.12) --------------------
    // Nessun Platform_Bootstrap reale per la piattaforma corrente: il bootstrap
    // ritorna `UnsupportedHost`; zero mod, GD raggiunge la scena iniziale.
    UnsupportedPlatform,      // Req 10.1
    // GD_Version/piattaforma non rilevabile dall'immagine reale: stop del
    // caricamento, zero hook, GD prosegue senza mod.
    VersionDetectionFailed,   // Req 5.3
    // Coppia (GD_Version, piattaforma) rilevata ma priva di Binding_Set_File
    // `.pbind` verificato: nessuna corrispondenza esatta, zero hook.
    BindingsUnavailable,      // Req 5.4, 10.2
    // Hooking_Backend non disponibile a runtime (`available()==false`): nessun
    // install, zero hook, diagnostica che nomina il backend.
    BackendUnavailable,       // Req 3.8, 10.3
    // Funzione bersaglio non risolta (binding assente o `resolved==false`):
    // nessun hook installato sull'indirizzo non risolto.
    SymbolUnresolved,         // Req 4.5, 9.6
};

// Esito del caricamento centralizzato.
struct CentralizedStartResult {
    StartMode mode{StartMode::ModLess};
    StartReason reason{StartReason::InjectionFailed};
    std::string message;

    // Vero sse le mod sono state caricate (avvio normale).
    [[nodiscard]] bool modsLoaded() const noexcept {
        return mode == StartMode::ModsLoaded;
    }
    // Vero sse GD deve partire senza mod (degradazione Req 1.3 / 1.6).
    [[nodiscard]] bool modLess() const noexcept { return mode == StartMode::ModLess; }
};

// Sorgente di tempo iniettabile. Default: orologio monotono di sistema.
using ClockFn = std::function<std::chrono::steady_clock::time_point()>;

// Restituisce il clock di default (std::chrono::steady_clock::now()).
ClockFn default_steady_clock();

// Token passato allo step di inizializzazione: consente al lavoro cooperativo
// di interrompersi non appena il budget del watchdog è scaduto (Req 1.6).
class WatchdogToken {
public:
    WatchdogToken(ClockFn clock, std::chrono::steady_clock::time_point deadline)
        : clock_(std::move(clock)), deadline_(deadline) {}

    // Vero quando l'istante corrente ha superato la scadenza del watchdog.
    [[nodiscard]] bool expired() const { return clock_() > deadline_; }

    // Istante limite oltre il quale l'inizializzazione è considerata in timeout.
    [[nodiscard]] std::chrono::steady_clock::time_point deadline() const noexcept {
        return deadline_;
    }

private:
    ClockFn clock_;
    std::chrono::steady_clock::time_point deadline_;
};

// Step di inizializzazione del runtime, eseguito sotto il watchdog. Riceve il
// `WatchdogToken` per poter abortire cooperativamente al superamento del
// budget. Restituisce `true` se l'inizializzazione (e il caricamento delle mod)
// è andata a buon fine, `false` in caso di fallimento (es. GD_Version non
// rilevata, Req 1.7).
using InitStepFn = std::function<bool(const WatchdogToken&)>;

// Esito ricco dello step di inizializzazione del runtime (task 3.12).
//
// A differenza di `InitStepFn` (che riporta solo successo/fallimento), questo
// risultato consente allo step di inizializzazione di comunicare al
// `CentralizedLoader` la *causa precisa* di un fallimento fail-open (versione
// senza bindings, coppia senza `.pbind`, backend non disponibile, simbolo non
// risolto), così la diagnostica centralizzata può loggare la causa esatta e
// classificarla in `StartReason`. Riporta inoltre il numero di hook installati
// per rendere verificabile l'invariante "0 hook su fallimento".
struct RuntimeInitResult {
    bool        modsLoaded{false};                  // true sse il caricamento è riuscito
    StartReason reason{StartReason::Success};       // causa precisa dell'esito
    std::string message;                            // diagnostica leggibile
    std::size_t installedHooks{0};                  // hook installati (0 su fallimento)

    // Costruisce un esito di successo con `hooks` hook installati.
    static RuntimeInitResult loaded(std::size_t hooks,
                                    std::string msg = "mod caricate") {
        return RuntimeInitResult{true, StartReason::Success, std::move(msg), hooks};
    }

    // Costruisce un esito di fail-open: 0 hook installati, causa classificata e
    // messaggio diagnostico (Req 2.8, 5.3, 5.4, 9.6, 10.1, 10.2, 10.4).
    static RuntimeInitResult failOpen(StartReason reason, std::string msg) {
        return RuntimeInitResult{false, reason, std::move(msg), 0};
    }
};

// Variante "ricca" dello step di inizializzazione (task 3.12): riporta un
// `RuntimeInitResult` anziché un semplice booleano, così la causa fail-open è
// propagata al `CentralizedLoader`. Riceve il `WatchdogToken` per abortire
// cooperativamente al superamento del budget, come `InitStepFn`.
using RuntimeInitFn = std::function<RuntimeInitResult(const WatchdogToken&)>;

// Orchestratore del caricamento centralizzato (Req 1.3, 1.4, 1.6).
class CentralizedLoader {
public:
    // Budget del watchdog sull'inizializzazione del runtime (Req 1.6).
    static constexpr std::chrono::milliseconds kWatchdogBudget{
        std::chrono::seconds(10)};

    // Costruisce con i collaboratori iniettabili:
    //  - `bootstrap`: iniezione di piattaforma (Layer 0). Se nullo, il bootstrap
    //    è considerato già riuscito (utile in scenari di solo runtime/test).
    //  - `initStep` : inizializzazione del runtime + caricamento mod sotto
    //    watchdog. Se nullo, l'inizializzazione è considerata riuscita.
    //  - `clock`    : sorgente di tempo (default: steady_clock di sistema).
    //  - `sink`     : sink diagnostico (default: stderr).
    //  - `budget`   : budget del watchdog (default: 10 s, Req 1.6).
    explicit CentralizedLoader(
        std::shared_ptr<bootstrap::IPlatformBootstrap> bootstrap,
        InitStepFn initStep,
        ClockFn clock = nullptr,
        DiagnosticSink sink = nullptr,
        std::chrono::milliseconds budget = kWatchdogBudget);

    // Variante con lo step di inizializzazione "ricco" (task 3.12): lo step
    // riporta un `RuntimeInitResult` che porta la *causa precisa* di un
    // fail-open (versione senza bindings, coppia senza `.pbind`, backend non
    // disponibile, simbolo non risolto). Permette al `start()` di loggare e
    // classificare ogni nuovo punto di fallimento mantenendo 0 hook e lasciando
    // partire GD senza mod. Gli altri collaboratori sono identici.
    CentralizedLoader(
        std::shared_ptr<bootstrap::IPlatformBootstrap> bootstrap,
        RuntimeInitFn runtimeInit,
        ClockFn clock = nullptr,
        DiagnosticSink sink = nullptr,
        std::chrono::milliseconds budget = kWatchdogBudget);

    // UNICO entry point centralizzato del runtime (Req 1.4):
    //   1. esegue il bootstrap di piattaforma; su iniezione fallita logga e
    //      restituisce un esito "senza mod" (Req 1.3) senza abortire il gioco;
    //   2. esegue lo step di inizializzazione sotto watchdog 10 s; se il budget
    //      è superato, interrompe il caricamento, logga il timeout e restituisce
    //      un esito "senza mod" (Req 1.6);
    //   3. altrimenti restituisce un esito con mod caricate.
    CentralizedStartResult start();

private:
    void log(std::string_view message) const;

    std::shared_ptr<bootstrap::IPlatformBootstrap> bootstrap_;
    RuntimeInitFn initStep_;
    ClockFn clock_;
    DiagnosticSink sink_;
    std::chrono::milliseconds budget_;
};

// ---------------------------------------------------------------------------
// Cablaggio della policy fail-open centralizzata (task 3.12).
//
// `FailOpenRuntime` raccoglie i collaboratori RIUSATI dal layer di
// integrazione (detection reale, provider dei bindings, backend di hooking)
// che lo step di inizializzazione attraversa in sequenza. `make_fail_open_init_step`
// li compone in un `RuntimeInitFn` che applica la policy fail-open a ciascun
// nuovo punto di fallimento:
//
//   1. detection nullopt           -> StartReason::VersionDetectionFailed (Req 5.3)
//   2. provider assente / load nullopt -> StartReason::BindingsUnavailable (Req 5.4, 10.2)
//   3. backend assente/non disponibile -> StartReason::BackendUnavailable (Req 3.8, 10.3)
//   4. simbolo non risolto         -> StartReason::SymbolUnresolved (Req 4.5, 9.6)
//   5. tutto risolto + backend ok  -> esattamente 1 hook, mod caricate (Req 9.1)
//
// L'invariante "0 hook su indirizzi non risolti" è garantita riusando `HookGate`
// (che consulta `available()` e `binding.resolved` prima di ogni install). La
// causa "bootstrap assente / piattaforma senza Platform_Bootstrap reale"
// (Req 10.1) è gestita a monte da `CentralizedLoader::start()` quando il
// bootstrap ritorna `UnsupportedHost`. Nessun passo scrive sul binario di GD:
// l'eseguibile e gli asset restano byte-for-byte invariati (Req 10.4).
struct FailOpenRuntime {
    // Detection della coppia ESATTA (GD_Version, piattaforma) dall'immagine
    // reale (Req 5.1). `nullopt` => rilevamento fallito (Req 5.3).
    std::function<std::optional<RuntimeContext>()> detect;
    // Provider dei bindings: `load` a corrispondenza esatta della coppia, senza
    // fuzzy-match (Req 5.4, 10.2); `resolve` del simbolo bersaglio (Req 4.4).
    std::shared_ptr<bindings::IBindingsProvider> bindingsProvider;
    // Backend di hooking; `available()` è consultata via `HookGate` (Req 3.8,
    // 10.3). `nullptr` è trattato come backend non disponibile (fail-open).
    ::pulse::hooking::IHookBackend* backend{nullptr};
    // Simbolo della funzione bersaglio della demo mod (Req 9.1, 9.6).
    std::string targetSymbol{"MenuLayer::init"};
    // Detour da installare sull'indirizzo risolto del bersaglio.
    void* detour{nullptr};
};

// Costruisce lo step di inizializzazione "ricco" che applica la policy
// fail-open centralizzata ai nuovi punti di fallimento (task 3.12). Ogni causa
// è loggata via `sink` (se fornito; in mancanza, via il sink di default del
// loader) e classificata in `StartReason`. Riusa `HookGate` per garantire 0
// hook su backend non disponibile o simbolo non risolto.
RuntimeInitFn make_fail_open_init_step(FailOpenRuntime runtime,
                                       DiagnosticSink sink = nullptr);

}  // namespace pulse::loader

#endif  // PULSE_LOADER_CORE_CENTRALIZED_LOADER_HPP
