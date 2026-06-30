// loader/core/centralized_loader.cpp — implementazione dell'entry point
// centralizzato del runtime Pulse (task 23.4, Requisiti 1.3, 1.4, 1.6) e della
// policy fail-open centralizzata estesa ai nuovi punti di fallimento del layer
// di integrazione (task 3.12, Requisiti 2.7, 2.8, 5.3, 5.4, 9.6, 10.1, 10.2,
// 10.4).
#include "core/centralized_loader.hpp"

#include <optional>
#include <string>
#include <utility>

#include "hooking/hook_gate.hpp"  // HookGate (riuso): gate disponibilità + resolved

namespace pulse::loader {

ClockFn default_steady_clock() {
    return []() { return std::chrono::steady_clock::now(); };
}

namespace {

// Avvolge un `InitStepFn` booleano in un `RuntimeInitFn`, preservando la
// semantica storica: assente/`true` => mod caricate; `false` => fail-open
// generico `InitFailed` (es. Req 1.7). Mantiene compatibili i chiamanti e i
// test che usano lo step booleano.
RuntimeInitFn wrap_bool_init_step(InitStepFn step) {
    return [step = std::move(step)](const WatchdogToken& token) -> RuntimeInitResult {
        if (!step) {
            return RuntimeInitResult::loaded(0);
        }
        if (step(token)) {
            return RuntimeInitResult::loaded(0);
        }
        return RuntimeInitResult::failOpen(
            StartReason::InitFailed,
            "inizializzazione del runtime fallita");
    };
}

}  // namespace

CentralizedLoader::CentralizedLoader(
    std::shared_ptr<bootstrap::IPlatformBootstrap> bootstrap,
    InitStepFn initStep,
    ClockFn clock,
    DiagnosticSink sink,
    std::chrono::milliseconds budget)
    : bootstrap_(std::move(bootstrap)),
      initStep_(wrap_bool_init_step(std::move(initStep))),
      clock_(clock ? std::move(clock) : default_steady_clock()),
      sink_(sink ? std::move(sink) : default_diagnostic_sink()),
      budget_(budget) {}

CentralizedLoader::CentralizedLoader(
    std::shared_ptr<bootstrap::IPlatformBootstrap> bootstrap,
    RuntimeInitFn runtimeInit,
    ClockFn clock,
    DiagnosticSink sink,
    std::chrono::milliseconds budget)
    : bootstrap_(std::move(bootstrap)),
      initStep_(std::move(runtimeInit)),
      clock_(clock ? std::move(clock) : default_steady_clock()),
      sink_(sink ? std::move(sink) : default_diagnostic_sink()),
      budget_(budget) {}

void CentralizedLoader::log(std::string_view message) const {
    if (sink_) {
        sink_(message);
    }
}

namespace {

// Formatta la coppia rilevata (GD_Version, piattaforma) per le diagnostiche.
std::string describe_pair(const RuntimeContext& ctx) {
    return std::to_string(ctx.gdVersion.major) + "." +
           std::to_string(ctx.gdVersion.minor) + " / " + ctx.platformId;
}

}  // namespace

RuntimeInitFn make_fail_open_init_step(FailOpenRuntime runtime, DiagnosticSink sink) {
    // Sink effettivo: quello fornito o il sink diagnostico di default del loader.
    DiagnosticSink effectiveSink = sink ? std::move(sink) : default_diagnostic_sink();

    return [runtime = std::move(runtime), effectiveSink = std::move(effectiveSink)](
               const WatchdogToken&) -> RuntimeInitResult {
        const auto emit = [&effectiveSink](std::string_view message) {
            if (effectiveSink) {
                effectiveSink(message);
            }
        };

        // 1) Detection della coppia ESATTA dall'immagine reale (Req 5.1). Se non
        //    rilevabile, fail-open: stop del caricamento, 0 hook, GD prosegue
        //    senza mod (Req 5.3).
        std::optional<RuntimeContext> ctx =
            runtime.detect ? runtime.detect() : std::nullopt;
        if (!ctx.has_value()) {
            std::string msg{
                "rilevamento di GD_Version/piattaforma fallito dall'immagine reale "
                "di Geometry Dash"};
            emit(msg);
            return RuntimeInitResult::failOpen(StartReason::VersionDetectionFailed,
                                               std::move(msg));
        }

        // 2) Bindings per la coppia ESATTA, senza fuzzy-match (Req 5.4, 10.2). Un
        //    provider assente o una coppia senza `.pbind` verificato → fail-open.
        if (!runtime.bindingsProvider) {
            std::string msg{
                "provider dei bindings assente: nessun Binding_Set_File (.pbind) "
                "caricabile per la coppia " + describe_pair(*ctx)};
            emit(msg);
            return RuntimeInitResult::failOpen(StartReason::BindingsUnavailable,
                                               std::move(msg));
        }
        const bindings::BindingKey key{
            bindings::GdVersion{static_cast<int>(ctx->gdVersion.major),
                                static_cast<int>(ctx->gdVersion.minor)},
            ctx->platformId};
        std::optional<bindings::BindingSet> set = runtime.bindingsProvider->load(key);
        if (!set.has_value()) {
            std::string msg{
                "nessun Binding_Set_File (.pbind) verificato per la coppia rilevata " +
                describe_pair(*ctx) + " (nessuna corrispondenza esatta)"};
            emit(msg);
            return RuntimeInitResult::failOpen(StartReason::BindingsUnavailable,
                                               std::move(msg));
        }

        // 3) Risoluzione del simbolo bersaglio a corrispondenza esatta (Req 4.4).
        std::optional<bindings::FunctionBinding> binding =
            runtime.bindingsProvider->resolve(runtime.targetSymbol);

        // 4) Gate dell'install via HookGate (riuso): consulta `available()` PRIMA
        //    di ogni install (Req 3.8, 10.3) e verifica `binding.resolved` (Req
        //    4.5, 9.6). Backend assente => trattato come non disponibile.
        if (runtime.backend == nullptr) {
            std::string msg{
                "Hooking_Backend assente/non disponibile a runtime: nessun hook "
                "installato"};
            emit(msg);
            return RuntimeInitResult::failOpen(StartReason::BackendUnavailable,
                                               std::move(msg));
        }

        ::pulse::hooking::HookGate gate{*runtime.backend,
                                        [&emit](std::string_view m) { emit(m); }};
        const ::pulse::hooking::GateResult gated = gate.install(binding, runtime.detour);

        if (gated.backendUnavailable()) {
            // La diagnostica (con il nome del backend) è già stata emessa dal gate.
            return RuntimeInitResult::failOpen(StartReason::BackendUnavailable,
                                               std::string{gated.error.message});
        }
        if (gated.incompatible()) {
            // Binding assente o non risolto: 0 hook su indirizzi non risolti
            // (Req 4.5, 9.6, 5.3).
            std::string msg{"funzione bersaglio '" + runtime.targetSymbol +
                            "' non risolta: nessun hook installato sull'indirizzo "
                            "non risolto"};
            emit(msg);
            return RuntimeInitResult::failOpen(StartReason::SymbolUnresolved,
                                               std::move(msg));
        }
        if (!gated.installed()) {
            // Gate superato (binding risolto) ma il backend ha fallito l'install:
            // fail-open, nessun hook residuo.
            std::string msg{"installazione dell'hook su '" + runtime.targetSymbol +
                            "' fallita: " + std::string{gated.error.message}};
            emit(msg);
            return RuntimeInitResult::failOpen(StartReason::SymbolUnresolved,
                                               std::move(msg));
        }

        // Successo: esattamente un hook installato sulla funzione risolta (Req 9.1).
        std::string msg{"hook installato su '" + runtime.targetSymbol +
                        "' (1 hook); mod caricate per la coppia " + describe_pair(*ctx)};
        emit(msg);
        return RuntimeInitResult::loaded(/*hooks=*/1, std::move(msg));
    };
}

CentralizedStartResult CentralizedLoader::start() {
    // Req 1.4: tutto il caricamento passa per questo unico entry point.
    log("Pulse: avvio del caricamento centralizzato (entry point unico)");

    // 1) Bootstrap di piattaforma (Layer 0). Un bootstrap assente equivale a
    //    iniezione già riuscita (scenari di solo runtime / test host).
    bootstrap::BootstrapResult boot = bootstrap_
                                          ? bootstrap_->inject()
                                          : bootstrap::BootstrapResult::success();
    if (!boot.injected) {
        const std::string cause =
            boot.error.has_value() ? boot.error->message : std::string{"causa sconosciuta"};

        // Req 10.1: piattaforma senza Platform_Bootstrap reale (il bootstrap
        // segnala `UnsupportedHost`). È un caso fail-open distinto: logga la
        // piattaforma non supportata e lascia GD raggiungere la scena iniziale
        // senza mod, senza terminare il processo.
        if (boot.error.has_value() &&
            boot.error->code == bootstrap::BootstrapErrorCode::UnsupportedHost) {
            log(std::string{"Pulse: piattaforma non supportata ("} + cause +
                "): nessun Platform_Bootstrap reale, avvio di Geometry Dash senza "
                "mod (0 hook)");
            return CentralizedStartResult{StartMode::ModLess,
                                          StartReason::UnsupportedPlatform, cause};
        }

        // Req 1.3 / 2.7 / 2.8: iniezione fallita → logga la causa diagnostica e
        // consenti l'avvio del gioco SENZA mod (non si aborta il processo).
        log(std::string{"Pulse: iniezione fallita ("} + cause +
            "): avvio di Geometry Dash senza mod");
        return CentralizedStartResult{StartMode::ModLess, StartReason::InjectionFailed,
                                      cause};
    }

    // 2) Watchdog 10 s sull'inizializzazione del runtime (Req 1.6).
    const std::chrono::steady_clock::time_point startTime = clock_();
    const std::chrono::steady_clock::time_point deadline = startTime + budget_;
    const WatchdogToken token{clock_, deadline};

    const RuntimeInitResult init =
        initStep_ ? initStep_(token) : RuntimeInitResult::loaded(0);

    const std::chrono::steady_clock::time_point endTime = clock_();
    const std::chrono::steady_clock::duration elapsed = endTime - startTime;

    if (elapsed > budget_) {
        // Req 1.6: l'inizializzazione ha superato il budget di 10 s → interrompi
        // il caricamento delle mod, logga il timeout e avvia GD senza mod.
        log("Pulse: inizializzazione del runtime non completata entro 10 s "
            "(timeout di inizializzazione): caricamento delle mod interrotto, "
            "avvio di Geometry Dash in modalita' senza mod");
        return CentralizedStartResult{StartMode::ModLess, StartReason::InitTimeout,
                                       std::string{"timeout di inizializzazione (10 s)"}};
    }

    if (!init.modsLoaded) {
        // Fail-open entro il budget (task 3.12): la causa precisa è già
        // classificata in `init.reason` (versione senza bindings, coppia senza
        // `.pbind`, backend non disponibile, simbolo non risolto, init fallita).
        // 0 hook installati, log della causa, GD parte comunque senza mod
        // (Req 2.8, 5.3, 5.4, 9.6, 10.2, 10.3); l'eseguibile/asset restano
        // byte-for-byte invariati perché il loader non scrive sul binario di GD
        // (Req 10.4).
        log(std::string{"Pulse: caricamento senza mod ("} + init.message +
            "): 0 hook installati, Geometry Dash raggiunge il menu senza mod");
        return CentralizedStartResult{StartMode::ModLess, init.reason, init.message};
    }

    log("Pulse: inizializzazione completata entro il budget; mod caricate "
        "tramite l'entry point centralizzato");
    return CentralizedStartResult{StartMode::ModsLoaded, StartReason::Success,
                                   init.message.empty() ? std::string{"mod caricate"}
                                                        : init.message};
}

}  // namespace pulse::loader
