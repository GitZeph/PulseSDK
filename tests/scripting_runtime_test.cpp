// tests/scripting_runtime_test.cpp — unit test dello ScriptingRuntime
// (task 36.1, Requisiti 19.1, 19.2, 19.3, 19.4, 19.5).
//
// Verifica le regole osservabili del runtime di scripting (design → "Layer 6 —
// Sandbox & Scripting"), con un orologio iniettato e un FakeScriptEngine
// in-memory (nessuna dipendenza da sol2/QuickJS):
//
//   * Req 19.1 — caricamento riuscito entro il budget di ≤5 s; superamento del
//     budget => fallimento di caricamento isolato alla sola Mod.
//   * Req 19.2 — errore di sintassi al load isolato alla SOLA Mod: l'esito
//     riporta la causa, non solleva eccezioni e le altre Mod restano caricabili
//     (il gioco non viene terminato).
//   * Req 19.3 / 19.4 — le capability di hooking/eventi/UI sono gated dal
//     predicato di permesso iniettato: concesse se nel Manifest, altrimenti
//     bloccate con errore (Mod + capability), senza terminare il gioco.
//   * Req 19.5 — errore di esecuzione (motore con ok=false o eccezione) isolato
//     alla sola Mod, con causa segnalata, senza terminare il gioco.

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <set>
#include <string>

#include "scripting/fake_script_engine.hpp"
#include "scripting/scripting_runtime.hpp"

namespace {

using pulse::scripting::Capability;
using pulse::scripting::FakeScriptEngine;
using pulse::scripting::ScriptErrorKind;
using pulse::scripting::ScriptingRuntime;

// Orologio controllato: avanza in modo esplicito tra le chiamate clock_() per
// simulare la durata del caricamento senza attese reali.
class ManualClock {
public:
    using time_point = std::chrono::steady_clock::time_point;

    // Restituisce l'istante corrente e poi avanza di `step_` per la prossima
    // lettura: così `loadMod` (start poi end) misura esattamente uno `step_`.
    time_point next() {
        const time_point now = now_;
        now_ += step_;
        return now;
    }

    void setStep(std::chrono::nanoseconds step) { step_ = step; }

private:
    time_point now_{};
    std::chrono::nanoseconds step_{0};
};

// Predicato di permesso che concede la capability solo se la coppia (mod, cap)
// è presente nella tabella consentita (rispecchia i permessi del Manifest).
struct PermissionTable {
    // mod -> set di capability concesse (rappresentate come interi).
    std::map<std::string, std::set<int>> allowed;

    bool operator()(const std::string& mod, Capability cap) const {
        const auto it = allowed.find(mod);
        if (it == allowed.end()) return false;
        return it->second.count(static_cast<int>(cap)) > 0;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Req 19.1 — caricamento riuscito entro il budget di 5 s.
// ---------------------------------------------------------------------------
TEST(ScriptingRuntimeTest, LoadsWithinFiveSecondBudget) {
    FakeScriptEngine engine;
    ManualClock clock;
    clock.setStep(std::chrono::seconds(2));  // 2 s < 5 s

    ScriptingRuntime runtime(
        engine, [](const std::string&, Capability) { return true; },
        [&clock]() { return clock.next(); });

    const auto out = runtime.loadMod("mod.alpha", "print('ciao')");

    EXPECT_TRUE(out.loaded);
    EXPECT_EQ(out.error.kind, ScriptErrorKind::None);
    EXPECT_LE(out.elapsed, runtime.loadBudget());
    EXPECT_TRUE(runtime.isLoaded("mod.alpha"));
}

// ---------------------------------------------------------------------------
// Req 19.1 — caricamento oltre il budget di 5 s => fallimento isolato.
// ---------------------------------------------------------------------------
TEST(ScriptingRuntimeTest, LoadExceedingBudgetFailsIsolated) {
    FakeScriptEngine engine;
    ManualClock clock;
    clock.setStep(std::chrono::seconds(6));  // 6 s > 5 s

    ScriptingRuntime runtime(
        engine, [](const std::string&, Capability) { return true; },
        [&clock]() { return clock.next(); });

    const auto out = runtime.loadMod("mod.slow", "while true do end");

    EXPECT_FALSE(out.loaded);
    EXPECT_EQ(out.error.kind, ScriptErrorKind::LoadTimeout);
    EXPECT_EQ(out.error.mod, "mod.slow");
    EXPECT_FALSE(runtime.isLoaded("mod.slow"));
}

// ---------------------------------------------------------------------------
// Req 19.2 — errore di sintassi al load isolato alla sola Mod: le altre Mod
// restano caricabili e nessuna eccezione viene propagata (gioco non terminato).
// ---------------------------------------------------------------------------
TEST(ScriptingRuntimeTest, SyntaxLoadErrorIsolatedToSingleMod) {
    FakeScriptEngine engine;
    engine.failLoadAt("mod.broken", "':' inatteso alla riga 3");

    ManualClock clock;
    clock.setStep(std::chrono::milliseconds(10));

    ScriptingRuntime runtime(
        engine, [](const std::string&, Capability) { return true; },
        [&clock]() { return clock.next(); });

    // La Mod con errore di sintassi fallisce in isolamento (nessun throw).
    const auto bad = runtime.loadMod("mod.broken", "func( = {");
    EXPECT_FALSE(bad.loaded);
    EXPECT_EQ(bad.error.kind, ScriptErrorKind::LoadSyntax);
    EXPECT_EQ(bad.error.mod, "mod.broken");
    EXPECT_FALSE(bad.error.message.empty());
    EXPECT_FALSE(runtime.isLoaded("mod.broken"));

    // Un'altra Mod continua a caricarsi normalmente (le altre restano attive).
    const auto good = runtime.loadMod("mod.ok", "print('ok')");
    EXPECT_TRUE(good.loaded);
    EXPECT_TRUE(runtime.isLoaded("mod.ok"));
    EXPECT_EQ(runtime.loadedCount(), 1u);
}

// ---------------------------------------------------------------------------
// Req 19.2 — un'eccezione del motore al load è catturata e isolata (il gioco
// non viene terminato), riportata come errore di caricamento della sola Mod.
// ---------------------------------------------------------------------------
TEST(ScriptingRuntimeTest, EngineExceptionOnLoadIsolated) {
    FakeScriptEngine engine;
    engine.throwOnLoadAt("mod.throws", "boom al load");

    ScriptingRuntime runtime(
        engine, [](const std::string&, Capability) { return true; });

    EXPECT_NO_THROW({
        const auto out = runtime.loadMod("mod.throws", "...");
        EXPECT_FALSE(out.loaded);
        EXPECT_EQ(out.error.kind, ScriptErrorKind::LoadSyntax);
        EXPECT_EQ(out.error.mod, "mod.throws");
    });
}

// ---------------------------------------------------------------------------
// Req 19.3 / 19.4 — gating delle capability: concessa se dichiarata, negata e
// segnalata altrimenti (senza terminare il gioco).
// ---------------------------------------------------------------------------
TEST(ScriptingRuntimeTest, CapabilityGatedByPermission) {
    FakeScriptEngine engine;
    PermissionTable perms;
    // mod.gameplay ha solo Hooking ed Events; NON ha UI.
    perms.allowed["mod.gameplay"] = {static_cast<int>(Capability::Hooking),
                                     static_cast<int>(Capability::Events)};

    ScriptingRuntime runtime(engine, perms);

    // Concesse: hooking ed eventi.
    const auto hooking = runtime.useCapability("mod.gameplay", Capability::Hooking);
    EXPECT_TRUE(hooking.allowed);
    EXPECT_EQ(hooking.error.kind, ScriptErrorKind::None);

    const auto events = runtime.useCapability("mod.gameplay", Capability::Events);
    EXPECT_TRUE(events.allowed);

    // Negata: UI non dichiarata => bloccata con errore (Mod + capability).
    const auto ui = runtime.useCapability("mod.gameplay", Capability::UI);
    EXPECT_FALSE(ui.allowed);
    EXPECT_EQ(ui.error.kind, ScriptErrorKind::PermissionDenied);
    EXPECT_EQ(ui.error.mod, "mod.gameplay");
    ASSERT_TRUE(ui.error.capability.has_value());
    EXPECT_EQ(ui.error.capability.value(), Capability::UI);
    EXPECT_FALSE(ui.error.message.empty());

    // Una Mod senza alcun permesso: ogni capability è negata.
    const auto deniedAll =
        runtime.useCapability("mod.unknown", Capability::Hooking);
    EXPECT_FALSE(deniedAll.allowed);
    EXPECT_EQ(deniedAll.error.kind, ScriptErrorKind::PermissionDenied);
}

// ---------------------------------------------------------------------------
// Req 19.5 — errore di esecuzione isolato alla sola Mod (motore con ok=false).
// ---------------------------------------------------------------------------
TEST(ScriptingRuntimeTest, RuntimeErrorIsolatedToSingleMod) {
    FakeScriptEngine engine;
    engine.failCallAt("mod.buggy", "onUpdate", "nil index in 'player'");

    ScriptingRuntime runtime(
        engine, [](const std::string&, Capability) { return true; });

    ASSERT_TRUE(runtime.loadMod("mod.buggy", "...").loaded);
    ASSERT_TRUE(runtime.loadMod("mod.healthy", "...").loaded);

    // La funzione che genera l'errore runtime fallisce in isolamento.
    const auto bad = runtime.run("mod.buggy", "onUpdate");
    EXPECT_FALSE(bad.ok);
    EXPECT_EQ(bad.error.kind, ScriptErrorKind::Runtime);
    EXPECT_EQ(bad.error.mod, "mod.buggy");
    EXPECT_FALSE(bad.error.message.empty());

    // La Mod sana resta eseguibile (stato delle altre preservato).
    const auto good = runtime.run("mod.healthy", "onUpdate");
    EXPECT_TRUE(good.ok);
    EXPECT_EQ(good.error.kind, ScriptErrorKind::None);
}

// ---------------------------------------------------------------------------
// Req 19.5 — un'eccezione del motore a runtime è catturata e isolata (il gioco
// non viene terminato).
// ---------------------------------------------------------------------------
TEST(ScriptingRuntimeTest, EngineExceptionAtRuntimeIsolated) {
    FakeScriptEngine engine;
    engine.throwOnCallAt("mod.crash", "onEvent", "segfault simulato");

    ScriptingRuntime runtime(
        engine, [](const std::string&, Capability) { return true; });

    ASSERT_TRUE(runtime.loadMod("mod.crash", "...").loaded);

    EXPECT_NO_THROW({
        const auto out = runtime.run("mod.crash", "onEvent");
        EXPECT_FALSE(out.ok);
        EXPECT_EQ(out.error.kind, ScriptErrorKind::Runtime);
        EXPECT_EQ(out.error.mod, "mod.crash");
    });
}

// ---------------------------------------------------------------------------
// run su una Mod non caricata correttamente è rifiutato (NotLoaded), coerente
// con l'isolamento: un load fallito non lascia la Mod eseguibile (Req 19.2).
// ---------------------------------------------------------------------------
TEST(ScriptingRuntimeTest, RunOnUnloadedModRejected) {
    FakeScriptEngine engine;
    engine.failLoadAt("mod.broken", "errore di sintassi");

    ScriptingRuntime runtime(
        engine, [](const std::string&, Capability) { return true; });

    EXPECT_FALSE(runtime.loadMod("mod.broken", "...").loaded);

    const auto out = runtime.run("mod.broken", "onUpdate");
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(out.error.kind, ScriptErrorKind::NotLoaded);
}
