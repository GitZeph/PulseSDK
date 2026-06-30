// loader/scripting/fake_script_engine.hpp — motore di scripting in-memory per i
// test (Layer 6 — Scripting, Requisito 19).
//
// FakeScriptEngine implementa `IScriptEngine` (load/callExported) interamente in
// memoria, senza dipendere da sol2/QuickJS. Consente di iniettare in modo
// deterministico:
//   * errori di SINTASSI al load (Req 19.2), per-Mod oppure globali;
//   * errori di ESECUZIONE su callExported (Req 19.5), per (Mod, funzione);
//   * eccezioni (sia su load sia su callExported), per verificare che lo
//     ScriptingRuntime catturi e isoli senza terminare il gioco.
//
// È header-only e privo di dipendenze esterne: i test girano su qualunque host
// senza linkare un motore reale, coerentemente con il pattern del FakeBackend
// dell'Hooking Engine.
#ifndef PULSE_LOADER_SCRIPTING_FAKE_SCRIPT_ENGINE_HPP
#define PULSE_LOADER_SCRIPTING_FAKE_SCRIPT_ENGINE_HPP

#include "scripting/script_engine.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pulse::scripting {

// Motore di scripting in-memory deterministico per i test dello ScriptingRuntime.
//
// Non è thread-safe: i test lo usano da un singolo thread.
class FakeScriptEngine final : public IScriptEngine {
public:
    FakeScriptEngine() = default;

    // -----------------------------------------------------------------------
    // Iniezione di fallimenti (deterministica).
    // -----------------------------------------------------------------------

    // Fa fallire il load della Mod indicata con un errore di sintassi (Req 19.2).
    void failLoadAt(const ModId& mod,
                    std::string message = "fake: errore di sintassi nello script") {
        loadFailures_[mod] = std::move(message);
    }

    // Fa sollevare un'eccezione std al load della Mod indicata (caso estremo:
    // il runtime deve catturarla e isolarla senza terminare il gioco).
    void throwOnLoadAt(const ModId& mod,
                       std::string message = "fake: eccezione al load") {
        loadThrows_[mod] = std::move(message);
    }

    // Fa fallire l'esecuzione della funzione `fn` della Mod con un errore
    // runtime (Req 19.5).
    void failCallAt(const ModId& mod, const std::string& fn,
                    std::string message = "fake: errore di esecuzione nello script") {
        callFailures_[key(mod, fn)] = std::move(message);
    }

    // Fa sollevare un'eccezione std nell'esecuzione di `fn` della Mod (il
    // runtime deve catturarla e isolarla, Req 19.5).
    void throwOnCallAt(const ModId& mod, const std::string& fn,
                       std::string message = "fake: eccezione a runtime") {
        callThrows_[key(mod, fn)] = std::move(message);
    }

    // -----------------------------------------------------------------------
    // Introspezione per i test.
    // -----------------------------------------------------------------------

    [[nodiscard]] bool isLoaded(const ModId& mod) const {
        return loaded_.find(mod) != loaded_.end();
    }

    [[nodiscard]] std::size_t loadAttempts() const noexcept {
        return loadAttempts_;
    }

    [[nodiscard]] std::size_t callAttempts() const noexcept {
        return callAttempts_;
    }

    // -----------------------------------------------------------------------
    // IScriptEngine.
    // -----------------------------------------------------------------------

    EngineResult load(const ModId& mod, const std::string& script) override {
        ++loadAttempts_;
        (void)script;

        if (const auto it = loadThrows_.find(mod); it != loadThrows_.end()) {
            throw std::runtime_error(it->second);
        }
        if (const auto it = loadFailures_.find(mod); it != loadFailures_.end()) {
            return EngineResult::failure(it->second);
        }

        loaded_.insert(mod);
        return EngineResult::success();
    }

    EngineResult callExported(const ModId& mod,
                              const std::string& exportedFn) override {
        ++callAttempts_;

        const std::string k = key(mod, exportedFn);
        if (const auto it = callThrows_.find(k); it != callThrows_.end()) {
            throw std::runtime_error(it->second);
        }
        if (const auto it = callFailures_.find(k); it != callFailures_.end()) {
            return EngineResult::failure(it->second);
        }
        return EngineResult::success();
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "fake-script-engine";
    }

private:
    static std::string key(const ModId& mod, const std::string& fn) {
        return mod + "::" + fn;
    }

    std::unordered_map<ModId, std::string> loadFailures_{};
    std::unordered_map<ModId, std::string> loadThrows_{};
    std::unordered_map<std::string, std::string> callFailures_{};
    std::unordered_map<std::string, std::string> callThrows_{};
    std::unordered_set<ModId> loaded_{};
    std::size_t loadAttempts_{0};
    std::size_t callAttempts_{0};
};

}  // namespace pulse::scripting

#endif  // PULSE_LOADER_SCRIPTING_FAKE_SCRIPT_ENGINE_HPP
