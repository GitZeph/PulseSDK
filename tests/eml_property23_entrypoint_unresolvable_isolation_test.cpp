// tests/eml_property23_entrypoint_unresolvable_isolation_test.cpp
// Feature: external-mod-loading, Property 23 — Entry point non risolvibile
// isolato senza invocazione.
// Validates: Requirements 5.3 (Requisiti 5.3)
//
// Property 23 (design.md): per ogni mod il cui entry point dichiarato non è
// esportato o non è risolvibile dal Mod_Module, nessun entry point è invocato
// per quella mod, si registra una diagnostica con il Mod_Id e il nome
// dell'entry point, e le mod restanti (con entry point risolvibile) proseguono.
//
// Il comportamento è host-testabile sul seam `IModuleLoader` tramite il
// `FakeModuleLoader` (header-only, in `loader/lifecycle/module_loader.hpp`), che
// modella un registro di simboli e i byte delle funzioni esportate da un
// Mod_Module senza alcun `dlopen` reale: `resolveEntryPoint` restituisce
// `SymbolNotFound` per un simbolo non esportato (Req 5.3). Il path `dlopen`/
// `dlsym` reale è coperto in Fase E e non da PBT.
//
// L'invocazione dell'entry point è modellata via un contatore di chiamate
// per-mod: il "gate di invocazione" del Mod_Loader (Req 5.3) risolve l'entry
// point dichiarato e lo invoca SOLO se la risoluzione riesce; un simbolo non
// risolvibile non viene mai invocato e produce diagnostica attribuita. Il
// driver qui sotto incapsula esattamente questo gate, esercitando la primitiva
// reale `FakeModuleLoader::resolveEntryPoint` come oracolo di risolvibilità.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si genera un batch di mod con Mod_Id DISTINTI; ogni mod dichiara un
//     entry point (simbolo) e il proprio Mod_Module è programmato nel fake in
//     una delle due categorie:
//       - Resolvable   → il modulo esporta il simbolo dichiarato (deve essere
//                         invocato esattamente una volta e proseguire);
//       - Unresolvable → il modulo NON esporta il simbolo dichiarato (esporta
//                         altri simboli o nessuno): l'entry point non è
//                         risolvibile → nessuna invocazione + diagnostica con
//                         Mod_Id + nome dell'entry point;
//   * l'oracolo verifica, indipendentemente dal gate, l'insieme atteso di mod
//     invocate (le sole Resolvable) e di mod non risolvibili (con diagnostica).

#include "lifecycle/module_loader.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/loader_core.hpp"

namespace {

using pulse::lifecycle::Bytes;
using pulse::lifecycle::FakeModuleLoader;
using pulse::lifecycle::ModId;
using pulse::lifecycle::ModuleHandle;

// Sink diagnostico che cattura i messaggi (come negli altri property test EML).
struct CapturingSink {
    std::vector<std::string> messages;
    pulse::loader::DiagnosticSink sink() {
        return [this](std::string_view m) { messages.emplace_back(m); };
    }
    bool anyContains(std::string_view a, std::string_view b) const {
        for (const std::string& m : messages) {
            if (m.find(a) != std::string::npos && m.find(b) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

// Specifica di una mod del batch: Mod_Id, simbolo di entry point dichiarato nel
// Mod_Manifest, byte (distinti) del Mod_Module e se l'entry point è risolvibile.
struct ModUnderTest {
    ModId modId;
    std::string entrySymbol;
    Bytes moduleImage;
    bool resolvable{false};
};

// Esito del gate di invocazione per una mod.
struct GateOutcome {
    int invocations{0};       // quante volte l'entry point è stato invocato
    bool reportedNotLoadable{false};  // diagnostica "non caricabile" emessa
};

// ---------------------------------------------------------------------------
// run_entrypoint_gate — modella il gate di invocazione dell'entry point del
// Mod_Loader (Req 5.3): per ogni mod del batch carica il Mod_Module, risolve
// l'entry point dichiarato via il seam `IModuleLoader` e lo invoca (incrementa
// il contatore) SOLO se la risoluzione riesce; in caso di `SymbolNotFound` non
// invoca nulla, emette una diagnostica con Mod_Id + nome dell'entry point e
// segnala la mod come non caricabile, proseguendo con le restanti.
// ---------------------------------------------------------------------------
std::map<ModId, GateOutcome> run_entrypoint_gate(
    FakeModuleLoader& loader, const std::vector<ModUnderTest>& mods,
    CapturingSink& sink) {
    std::map<ModId, GateOutcome> outcomes;
    auto emit = sink.sink();

    for (const ModUnderTest& mod : mods) {
        GateOutcome& outcome = outcomes[mod.modId];

        auto loaded = loader.load(mod.modId, mod.moduleImage);
        if (!loaded.has_value()) {
            // Caricamento fallito: fuori scope di Property 23; nessuna
            // invocazione e diagnostica generica. Non accade in questo test
            // (nessuna immagine è programmata con failLoad).
            outcome.reportedNotLoadable = true;
            emit("mod '" + mod.modId + "' non caricabile: modulo non caricato");
            continue;
        }
        ModuleHandle handle = loaded.value();

        auto entry = loader.resolveEntryPoint(handle, mod.entrySymbol);
        if (!entry.has_value()) {
            // Entry point non risolvibile (Req 5.3): NESSUNA invocazione,
            // diagnostica con Mod_Id + nome dell'entry point, mod non caricabile.
            outcome.reportedNotLoadable = true;
            emit("mod '" + mod.modId + "': entry point '" + mod.entrySymbol +
                 "' non risolvibile (non esportato dal Mod_Module)");
            (void)loader.unload(handle);
            continue;
        }

        // Entry point risolto: il seam restituisce un indirizzo non nullo
        // (modello dell'invocazione via il puntatore risolto / contatore di
        // chiamate). Lo invochiamo esattamente una volta.
        const bool resolvedAddressNonNull = entry.value() != nullptr;
        RC_ASSERT(resolvedAddressNonNull);
        outcome.invocations += 1;
        (void)loader.unload(handle);
    }

    return outcomes;
}

// --- Property 23 — entry point non risolvibile isolato senza invocazione ----
// Feature: external-mod-loading, Property 23.
// Validates: Requirements 5.3.
RC_GTEST_PROP(EmlProperty23EntryPointUnresolvableIsolation,
              UnresolvableEntryPointNeverInvokedAndOthersProceed,
              ()) {
    const int count = *rc::gen::inRange(0, 13);

    FakeModuleLoader loader;
    std::vector<ModUnderTest> mods;
    std::set<ModId> expectedInvoked;       // mod con entry point risolvibile
    std::set<ModId> expectedUnresolvable;  // mod con entry point non risolvibile

    for (int i = 0; i < count; ++i) {
        const std::string idx = std::to_string(i);
        const bool resolvable = *rc::gen::arbitrary<bool>();

        ModUnderTest mod;
        mod.modId = "mod." + idx;
        mod.entrySymbol = "entry_" + idx;
        // Byte del Mod_Module distinti per mod (match esatto nel fake).
        mod.moduleImage = Bytes{static_cast<std::uint8_t>(0xA0 + (i & 0x0F)),
                                static_cast<std::uint8_t>(i & 0xFF)};
        mod.resolvable = resolvable;

        FakeModuleLoader::ModuleSpec spec;
        if (resolvable) {
            // Il modulo esporta il simbolo dichiarato → entry point risolvibile.
            spec.exports.push_back({mod.entrySymbol, {0x11, 0x22, 0x33}});
            expectedInvoked.insert(mod.modId);
        } else {
            // Il modulo NON esporta il simbolo dichiarato (esporta un simbolo
            // diverso o nessuno) → entry point NON risolvibile (Req 5.3).
            const bool exportsOther = *rc::gen::arbitrary<bool>();
            if (exportsOther) {
                spec.exports.push_back({"other_" + idx, {0x44}});
            }
            expectedUnresolvable.insert(mod.modId);
        }
        loader.program(mod.moduleImage, spec);
        mods.push_back(std::move(mod));
    }

    CapturingSink sink;
    const std::map<ModId, GateOutcome> outcomes =
        run_entrypoint_gate(loader, mods, sink);

    // (1) Ogni mod con entry point NON risolvibile: ZERO invocazioni, segnalata
    //     non caricabile, e diagnostica con Mod_Id + nome dell'entry point.
    for (const ModUnderTest& mod : mods) {
        if (mod.resolvable) continue;
        const GateOutcome& o = outcomes.at(mod.modId);
        RC_ASSERT(o.invocations == 0);
        RC_ASSERT(o.reportedNotLoadable);
        RC_ASSERT(sink.anyContains(mod.modId, mod.entrySymbol));
    }

    // (2) Ogni mod con entry point risolvibile prosegue: entry point invocato
    //     ESATTAMENTE una volta, senza essere segnalata non caricabile. Questo
    //     vale indipendentemente da quante mod del batch falliscono (le
    //     restanti proseguono, Req 5.3).
    for (const ModUnderTest& mod : mods) {
        if (!mod.resolvable) continue;
        const GateOutcome& o = outcomes.at(mod.modId);
        RC_ASSERT(o.invocations == 1);
        RC_ASSERT(!o.reportedNotLoadable);
    }

    // (3) Coerenza globale: esattamente le mod risolvibili sono state invocate.
    std::set<ModId> actuallyInvoked;
    for (const auto& [id, o] : outcomes) {
        if (o.invocations > 0) actuallyInvoked.insert(id);
    }
    RC_ASSERT(actuallyInvoked == expectedInvoked);
}

}  // namespace
