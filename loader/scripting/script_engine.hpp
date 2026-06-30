// loader/scripting/script_engine.hpp — astrazione del motore di scripting di
// Pulse (Layer 6 — Scripting, Requisito 19).
//
// Questo header definisce l'interfaccia platform-agnostic `IScriptEngine` e i
// tipi di supporto usati dallo `ScriptingRuntime`. Il motore concreto (Lua via
// sol2, JS via QuickJS) verrà selezionato a compile-time dietro opzioni CMake
// che restano OFF di default (stesso pattern di Dobby/ShadowHook nel loader):
// in questo modo la build host compila SENZA scaricare sol2/QuickJS, usando lo
// stub/fake iniettabile per validare il comportamento osservabile.
//
// L'interfaccia espone SOLO due primitive — load (compila/carica lo script) e
// callExported (esegue una funzione esportata) — così la politica di budget di
// caricamento (≤5 s, Req 19.1), il gating delle capability sui permessi
// (Req 19.3, 19.4) e l'isolamento degli errori di load/runtime (Req 19.2,
// 19.5) restano nel codice Pulse `ScriptingRuntime`, indipendenti dal motore.
//
// Stack: C++20/23 (Requisito 26.1). Header-only, nessuna dipendenza dal motore
// concreto: i file che includono questo header compilano su ogni host.
#ifndef PULSE_LOADER_SCRIPTING_SCRIPT_ENGINE_HPP
#define PULSE_LOADER_SCRIPTING_SCRIPT_ENGINE_HPP

#include <string>
#include <string_view>
#include <utility>

namespace pulse::scripting {

// Identità della Mod di scripting, coerente con gli altri moduli (ModId è una
// stringa: pulse::hooking::ModId, pulse::lifecycle::ModId, pulse::events::ModId).
using ModId = std::string;

// ---------------------------------------------------------------------------
// EngineResult — esito di una primitiva del motore: ok oppure errore con causa.
//
// Le primitive del motore NON sollevano eccezioni nel percorso normale:
// segnalano gli errori (sintassi al load, errori a runtime) tramite `ok=false`
// e un messaggio leggibile. Lo `ScriptingRuntime` cattura comunque eventuali
// eccezioni del motore concreto per garantire l'isolamento (game non terminato).
// ---------------------------------------------------------------------------
struct EngineResult {
    bool ok{true};        // true sse l'operazione è riuscita
    std::string error{};  // causa leggibile quando ok == false

    static EngineResult success() { return EngineResult{true, {}}; }
    static EngineResult failure(std::string message) {
        return EngineResult{false, std::move(message)};
    }
};

// ---------------------------------------------------------------------------
// IScriptEngine — astrazione del runtime di scripting (Lua/JS).
// ---------------------------------------------------------------------------
class IScriptEngine {
public:
    virtual ~IScriptEngine() = default;

    // Compila/carica il codice di script della Mod. Un errore di SINTASSI al
    // caricamento è segnalato con `ok=false` e un messaggio che ne indica la
    // natura (Req 19.2). Non deve sollevare eccezioni nel percorso normale.
    virtual EngineResult load(const ModId& mod, const std::string& script) = 0;

    // Invoca una funzione esportata dallo script della Mod. Un errore di
    // ESECUZIONE è segnalato con `ok=false` e un messaggio (Req 19.5).
    virtual EngineResult callExported(const ModId& mod,
                                      const std::string& exportedFn) = 0;

    // Nome del motore (per logging/diagnostica, es. "lua-sol2", "js-quickjs").
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

}  // namespace pulse::scripting

#endif  // PULSE_LOADER_SCRIPTING_SCRIPT_ENGINE_HPP
