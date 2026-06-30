// loader/scripting/scripting_runtime.hpp — runtime opzionale per le Mod di
// scripting (Layer 6 — Scripting, Requisito 19).
//
// `ScriptingRuntime` orchestra un `IScriptEngine` (Lua via sol2, JS via QuickJS;
// vedi script_engine.hpp) e realizza le regole OSSERVABILI del Requisito 19,
// in modo indipendente dal motore concreto e testabile su host con un fake:
//
//   * Req 19.1 — carica ed esegue lo script entro un budget di ≤5 s. Il budget
//     è verificato con un orologio INIETTABILE (Clock), così i test possono
//     simulare caricamenti lenti senza attese reali. Il superamento del budget
//     è trattato come un fallimento di caricamento isolato alla sola Mod.
//
//   * Req 19.2 — un errore di SINTASSI al load interrompe l'attivazione della
//     SOLA Mod interessata, mantiene attive le altre e riporta la causa. Il
//     runtime non solleva eccezioni: ritorna un esito con l'errore. Eventuali
//     eccezioni del motore concreto sono catturate e convertite in errore.
//
//   * Req 19.3 / 19.4 — il runtime espone alle Mod le capability di hooking,
//     eventi e UI delle Mod native, ma OGNI chiamata a una capability è GATED
//     da un predicato di permesso INIETTATO (std::function<bool(ModId,
//     Capability)>), che rispecchia il gating del Sandbox nativo SENZA
//     dipendere dal tipo Sandbox (task concorrente). Capability non concessa =>
//     operazione bloccata e errore con Mod + capability negata, gioco non
//     terminato.
//
//   * Req 19.5 — un errore a RUNTIME durante l'esecuzione dello script è
//     isolato alla sola Mod, con la causa segnalata, senza terminare il gioco.
//
// Stack: C++20/23 (Requisito 26.1). Header-only, nessuna dipendenza esterna
// oltre all'interfaccia `IScriptEngine`.
#ifndef PULSE_LOADER_SCRIPTING_SCRIPTING_RUNTIME_HPP
#define PULSE_LOADER_SCRIPTING_SCRIPTING_RUNTIME_HPP

#include "scripting/script_engine.hpp"

#include <chrono>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace pulse::scripting {

// ---------------------------------------------------------------------------
// Capability esposte alle Mod di scripting (le stesse delle Mod native, Req
// 19.3): hooking, gestione eventi e UI. Ogni chiamata è gated dai permessi.
// ---------------------------------------------------------------------------
enum class Capability { Hooking, Events, UI };

// Etichetta leggibile della capability (per i messaggi di errore).
[[nodiscard]] inline std::string_view to_string(Capability cap) noexcept {
    switch (cap) {
        case Capability::Hooking: return "hooking";
        case Capability::Events:  return "events";
        case Capability::UI:      return "ui";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Categoria dell'errore isolato dal runtime di scripting.
// ---------------------------------------------------------------------------
enum class ScriptErrorKind {
    None,              // nessun errore
    LoadSyntax,        // errore di sintassi/caricamento dello script (Req 19.2)
    LoadTimeout,       // caricamento oltre il budget di ≤5 s (Req 19.1)
    PermissionDenied,  // capability non dichiarata nel Manifest (Req 19.4)
    Runtime,           // errore di esecuzione dello script (Req 19.5)
    NotLoaded,         // operazione su una Mod non caricata correttamente
};

// Descrizione strutturata dell'errore isolato: categoria + Mod responsabile +
// messaggio leggibile + (per PermissionDenied) la capability negata.
struct ScriptError {
    ScriptErrorKind kind{ScriptErrorKind::None};
    ModId mod{};
    std::string message{};
    std::optional<Capability> capability{};
};

// ---------------------------------------------------------------------------
// Esiti delle operazioni del runtime. Nessuna operazione solleva eccezioni
// verso il chiamante: l'errore è sempre incapsulato nell'esito (isolamento).
// ---------------------------------------------------------------------------

// Esito del caricamento di una Mod di scripting (Req 19.1, 19.2).
struct LoadOutcome {
    bool loaded{false};                  // true sse caricata entro il budget
    ScriptError error{};                 // valorizzato quando loaded == false
    std::chrono::nanoseconds elapsed{0}; // durata misurata del caricamento

    [[nodiscard]] bool ok() const noexcept { return loaded; }
};

// Esito di una richiesta di capability (Req 19.3, 19.4).
struct CapabilityOutcome {
    bool allowed{false};  // true sse il permesso è concesso
    ScriptError error{};  // PermissionDenied quando allowed == false

    [[nodiscard]] bool ok() const noexcept { return allowed; }
};

// Esito dell'esecuzione di una funzione esportata (Req 19.5).
struct RunOutcome {
    bool ok{false};       // true sse l'esecuzione è terminata senza errori
    ScriptError error{};  // Runtime/NotLoaded quando ok == false
};

// ---------------------------------------------------------------------------
// ScriptingRuntime — orchestratore del motore di scripting con budget di load,
// gating delle capability e isolamento degli errori per singola Mod.
// ---------------------------------------------------------------------------
class ScriptingRuntime {
public:
    // Orologio iniettabile: restituisce l'istante monotòno corrente. Di default
    // usa std::chrono::steady_clock; i test iniettano un orologio controllato.
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    // Predicato di permesso iniettato: true sse la Mod ha la capability nel
    // proprio Manifest (rispecchia il gating del Sandbox nativo, Req 19.3/19.4).
    using PermissionPredicate = std::function<bool(const ModId&, Capability)>;

    // Budget di caricamento di default: 5 secondi (Req 19.1).
    static constexpr std::chrono::seconds kDefaultLoadBudget{5};

    // Orologio di default basato su steady_clock.
    static std::chrono::steady_clock::time_point defaultClock() {
        return std::chrono::steady_clock::now();
    }

    // Costruisce il runtime su un motore concreto/fake, un predicato di permesso
    // e (opzionali) un orologio iniettabile e un budget di caricamento.
    explicit ScriptingRuntime(
        IScriptEngine& engine, PermissionPredicate permission,
        Clock clock = &ScriptingRuntime::defaultClock,
        std::chrono::nanoseconds loadBudget = kDefaultLoadBudget)
        : engine_(engine),
          permission_(std::move(permission)),
          clock_(std::move(clock)),
          loadBudget_(loadBudget) {}

    // -----------------------------------------------------------------------
    // Caricamento di una Mod di scripting (Req 19.1, 19.2).
    //
    // Misura la durata del load con l'orologio iniettato. Se il motore segnala
    // un errore di sintassi (o solleva un'eccezione), l'attivazione della SOLA
    // Mod fallisce con causa riportata e le altre Mod restano attive (nessuna
    // eccezione propagata: il gioco non viene terminato). Se il caricamento
    // supera il budget, l'esito è un fallimento di tipo LoadTimeout.
    // -----------------------------------------------------------------------
    LoadOutcome loadMod(const ModId& mod, const std::string& script) {
        const auto start = clock_();

        EngineResult result;
        try {
            result = engine_.load(mod, script);
        } catch (const std::exception& ex) {
            // Isolamento: un'eccezione del motore è convertita in errore di
            // caricamento della sola Mod (il gioco non viene terminato).
            const auto end = clock_();
            LoadOutcome out;
            out.loaded = false;
            out.elapsed = end - start;
            out.error = ScriptError{
                ScriptErrorKind::LoadSyntax, mod,
                std::string{"errore di caricamento dello script: "} + ex.what(),
                std::nullopt};
            return out;
        } catch (...) {
            const auto end = clock_();
            LoadOutcome out;
            out.loaded = false;
            out.elapsed = end - start;
            out.error = ScriptError{
                ScriptErrorKind::LoadSyntax, mod,
                "errore di caricamento dello script: eccezione non std", std::nullopt};
            return out;
        }

        const auto end = clock_();
        LoadOutcome out;
        out.elapsed = end - start;

        if (!result.ok) {
            // Req 19.2: errore di sintassi/caricamento isolato alla sola Mod.
            out.loaded = false;
            out.error = ScriptError{ScriptErrorKind::LoadSyntax, mod,
                                    result.error, std::nullopt};
            return out;
        }

        if (out.elapsed > loadBudget_) {
            // Req 19.1: caricamento oltre il budget di ≤5 s => fallimento
            // isolato alla sola Mod (le altre Mod restano attive).
            out.loaded = false;
            out.error = ScriptError{
                ScriptErrorKind::LoadTimeout, mod,
                "caricamento dello script oltre il budget di 5 s", std::nullopt};
            return out;
        }

        // Caricamento riuscito entro il budget.
        out.loaded = true;
        loaded_.insert(mod);
        return out;
    }

    // -----------------------------------------------------------------------
    // Richiesta di una capability da parte di una Mod di scripting (Req 19.3,
    // 19.4). L'operazione è bloccata e segnalata se la capability non è
    // dichiarata nel Manifest (predicato false), senza terminare il gioco.
    // -----------------------------------------------------------------------
    CapabilityOutcome useCapability(const ModId& mod, Capability cap) {
        CapabilityOutcome out;
        if (permission_ && permission_(mod, cap)) {
            out.allowed = true;
            return out;
        }
        // Req 19.4: capability negata => blocco + errore con Mod + capability.
        out.allowed = false;
        out.error = ScriptError{
            ScriptErrorKind::PermissionDenied, mod,
            std::string{"capability '"} + std::string{to_string(cap)} +
                "' negata: permesso non dichiarato nel Manifest della Mod",
            cap};
        return out;
    }

    // -----------------------------------------------------------------------
    // Esecuzione di una funzione esportata dallo script (Req 19.5).
    //
    // Un errore di esecuzione (motore con ok=false o eccezione) è isolato alla
    // sola Mod, con la causa segnalata, senza terminare il gioco né alterare
    // lo stato delle altre Mod. Eseguibile solo su Mod caricate correttamente.
    // -----------------------------------------------------------------------
    RunOutcome run(const ModId& mod, const std::string& exportedFn) {
        RunOutcome out;
        if (!isLoaded(mod)) {
            out.ok = false;
            out.error = ScriptError{ScriptErrorKind::NotLoaded, mod,
                                    "la Mod non è stata caricata correttamente",
                                    std::nullopt};
            return out;
        }

        EngineResult result;
        try {
            result = engine_.callExported(mod, exportedFn);
        } catch (const std::exception& ex) {
            // Req 19.5: eccezione runtime isolata alla sola Mod.
            out.ok = false;
            out.error = ScriptError{
                ScriptErrorKind::Runtime, mod,
                std::string{"errore di esecuzione dello script: "} + ex.what(),
                std::nullopt};
            return out;
        } catch (...) {
            out.ok = false;
            out.error = ScriptError{
                ScriptErrorKind::Runtime, mod,
                "errore di esecuzione dello script: eccezione non std", std::nullopt};
            return out;
        }

        if (!result.ok) {
            // Req 19.5: errore runtime segnalato dal motore, isolato alla Mod.
            out.ok = false;
            out.error = ScriptError{ScriptErrorKind::Runtime, mod, result.error,
                                    std::nullopt};
            return out;
        }

        out.ok = true;
        return out;
    }

    // -----------------------------------------------------------------------
    // Introspezione.
    // -----------------------------------------------------------------------

    // True sse la Mod è stata caricata correttamente entro il budget.
    [[nodiscard]] bool isLoaded(const ModId& mod) const {
        return loaded_.find(mod) != loaded_.end();
    }

    // Numero di Mod di scripting attualmente caricate.
    [[nodiscard]] std::size_t loadedCount() const noexcept {
        return loaded_.size();
    }

    // Budget di caricamento corrente.
    [[nodiscard]] std::chrono::nanoseconds loadBudget() const noexcept {
        return loadBudget_;
    }

private:
    IScriptEngine& engine_;
    PermissionPredicate permission_;
    Clock clock_;
    std::chrono::nanoseconds loadBudget_;
    std::unordered_set<ModId> loaded_{};
};

}  // namespace pulse::scripting

#endif  // PULSE_LOADER_SCRIPTING_SCRIPTING_RUNTIME_HPP
