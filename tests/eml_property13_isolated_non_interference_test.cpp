// tests/eml_property13_isolated_non_interference_test.cpp
// Feature: external-mod-loading, Property 13 — Non-interferenza tra mod isolate.
// Validates: Requirements 6.5 (Requisiti 6.5)
//
// Property 13 (design.md §"Property 13"): l'insieme degli hook installati delle
// mod VALIDE è identico a quello che si otterrebbe caricando le SOLE mod valide;
// il fallimento isolato di una o più mod non aggiunge né rimuove alcun hook
// delle mod altrui caricate con successo (Req 6.5).
//
// Proprietà METAMORFICA. Si esegue lo stesso batch in due scenari:
//   * Scenario MIXED: si abilita via `ModManagerWiring::runNoThrow` un batch che
//     contiene SIA mod valide SIA mod che falliscono (in stadi diversi: modulo
//     non caricabile, entry point in errore, entry point che lancia, entry point
//     che registra alcuni `PULSE_HOOK` e POI lancia — quest'ultimo sposta gli
//     indici nel registro globale, stressando l'attribuzione a finestre).
//   * Scenario VALID-ONLY: si abilita lo stesso insieme di mod valide, nello
//     stesso ordine relativo, MA senza alcuna mod fallita.
// Si cattura per ogni mod valida l'insieme degli hook installati attribuiti dal
// `HookOwnershipLedger` (coppie {simbolo, indirizzo bersaglio}) e si asserisce
// che i due insiemi (mixed vs valid-only) coincidano esattamente, mod per mod e
// globalmente. Il fallimento isolato è dunque trasparente per gli hook altrui.
//
// Modello (host-testable, niente OS loader — coerente con la nota del design):
//   * `FakeModuleLoader` modella il registro di simboli/byte dei Mod_Module
//     (entry point esportato; `failLoad` per il modulo non caricabile);
//   * `FakeBackend` modella i byte delle funzioni bersaglio (readOriginal /
//     install / remove byte-esatti), così l'install effettivo è osservabile;
//   * l'invocatore dell'entry point registra nel registro globale
//     `pulse::hooks` i `PULSE_HOOK` della mod (cadono nella finestra di epoca
//     del mod) e — per le mod fallite — fallisce o lancia secondo il failMode;
//   * il `SymbolResolver` iniettato risolve i simboli ai loro indirizzi stabili.
//
// Strategia (RapidCheck, ≥100 iterazioni — default RC_GTEST_PROP): si generano
// 1..6 mod con Mod_Id DISTINTI, ognuna valida o fallita (failMode casuale).
// Si forza la presenza di ≥1 mod valida e ≥1 mod fallita così il confronto
// metamorfico è significativo a ogni iterazione. Gli indirizzi risolti sono
// globalmente unici (nessuna collisione tra mod). Il registro globale dello SDK
// (singleton di processo) è azzerato all'inizio di OGNI scenario.
//
// Header del loader in loader/lifecycle/ (include relativo alla radice loader/)
// e header pubblico dello SDK <pulse/hooks.hpp>; la logica del cablaggio è in
// mod_loader.cpp/mod_manager.cpp/hook_ownership.cpp (compilate in pulse::loader
// via glob lifecycle/*.cpp). Integrazione RapidCheck+GoogleTest in extras/gtest.

#include "lifecycle/mod_loader.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <pulse/hooks.hpp>

#include "core/runtime_context.hpp"
#include "hooking/fake_backend.hpp"
#include "hooking/rollback_store.hpp"
#include "lifecycle/hook_ownership.hpp"
#include "lifecycle/mod_manager.hpp"
#include "lifecycle/module_loader.hpp"

namespace {

using pulse::hooking::FakeBackend;
using pulse::hooking::RollbackStore;
using pulse::lifecycle::Bytes;
using pulse::lifecycle::EntryPointOutcome;
using pulse::lifecycle::FakeModuleLoader;
using pulse::lifecycle::HookOwnershipLedger;
using pulse::lifecycle::ModId;
using pulse::lifecycle::ModManager;
using pulse::lifecycle::ModManagerWiring;
using pulse::lifecycle::ModState;
using pulse::lifecycle::ModWiringSpec;
using pulse::lifecycle::OwnedHook;

// Azzera il registro globale dello SDK: singleton di processo condiviso tra le
// unità di traduzione, quindi ogni scenario deve partire da uno stato noto.
void resetRegistry() { pulse::hooks::registry().clear(); }

// Runtime_Context fittizio (macOS arm64, GD 2.2081).
pulse::loader::RuntimeContext makeCtx() {
    pulse::loader::RuntimeContext ctx;
    ctx.gdVersion = pulse::loader::GdVersion{2, 2081};
    ctx.platform = pulse::loader::Platform::MacOS;
    ctx.platformId = "macos-arm64";
    return ctx;
}

// Un singolo PULSE_HOOK della mod: simbolo univoco + indirizzo bersaglio stabile
// (globalmente unico, sempre risolvibile in questo modello).
struct HookSpec {
    std::string symbol;
    std::uintptr_t address{0};
};

// Modalità di fallimento iniettata per le mod NON valide.
enum class FailMode {
    ModuleNotLoadable,  // FakeModuleLoader::failLoad → entry point mai invocato
    EntryError,         // entry point restituisce errore (nessun PULSE_HOOK)
    EntryThrow,         // entry point lancia un'eccezione (nessun PULSE_HOOK)
    RegisterThenThrow,  // registra alcuni PULSE_HOOK e POI lancia (sposta indici)
};

// Una mod del modello.
struct ModSpec {
    ModId modId;
    std::string entrySymbol;
    Bytes image;
    bool valid{true};
    FailMode failMode{FailMode::EntryError};  // valido solo se !valid
    std::vector<HookSpec> hooks;              // PULSE_HOOK registrati all'enable
};

// Insieme degli hook installati di una mod, indipendente dall'indice di registro
// (artefatto di posizione nel registro globale): coppie {simbolo, indirizzo}.
using HookKeySet = std::set<std::pair<std::string, std::uintptr_t>>;

// Esito di uno scenario: per ogni Mod_Id incluso, lo stato finale e — per le mod
// valide — l'insieme degli hook installati attribuiti dal ledger.
struct ScenarioResult {
    std::unordered_map<ModId, std::optional<ModState>> state;
    std::unordered_map<ModId, HookKeySet> installedByMod;  // solo mod valide
    HookKeySet installedAll;  // unione globale (tutte le mod incluse)
};

// Esegue UNO scenario abilitando, via il cablaggio reale, esattamente le mod
// puntate da `included`, nell'ordine dato. Ritorna lo stato e gli insiemi di
// hook installati. Azzera il registro globale prima di iniziare.
ScenarioResult runScenario(const std::vector<const ModSpec*>& included,
                           int scenarioTag) {
    resetRegistry();

    FakeModuleLoader moduleLoader;
    FakeBackend backend;
    HookOwnershipLedger ledger;
    static int rollbackSeq = 0;
    const std::filesystem::path rbkPath =
        std::filesystem::path(::testing::TempDir()) /
        ("pulse_eml_p13_" + std::to_string(scenarioTag) + "_" +
         std::to_string(rollbackSeq++) + ".rbk");
    RollbackStore rollback{rbkPath};
    pulse::loader::DiagnosticSink sink = [](std::string_view) {};

    // simbolo (PULSE_HOOK) → indirizzo risolto stabile (tutti risolvibili qui).
    std::unordered_map<std::string, std::uintptr_t> resolvedAddr;
    // modId → simboli da registrare all'enable (PULSE_HOOK del Mod_Module).
    std::unordered_map<ModId, std::vector<std::string>> hooksByMod;
    // modId → modalità di fallimento (solo per le mod non valide).
    std::unordered_map<ModId, bool> isValid;
    std::unordered_map<ModId, FailMode> failByMod;

    for (const ModSpec* mod : included) {
        FakeModuleLoader::ModuleSpec spec;
        if (!mod->valid && mod->failMode == FailMode::ModuleNotLoadable) {
            spec.failLoad = true;  // load simulata fallita → entry mai invocato
        } else {
            // Entry point esportato (risolvibile): il fallimento, se previsto,
            // avviene nell'invocazione dell'entry point, non nella risoluzione.
            spec.exports.push_back({mod->entrySymbol, {0x90, 0x90}});
        }
        moduleLoader.program(mod->image, spec);

        for (const HookSpec& hook : mod->hooks) {
            hooksByMod[mod->modId].push_back(hook.symbol);
            resolvedAddr[hook.symbol] = hook.address;
        }
        isValid[mod->modId] = mod->valid;
        failByMod[mod->modId] = mod->failMode;
    }

    ModManagerWiring::SymbolResolver resolver =
        [&resolvedAddr](std::string_view symbol) -> void* {
        const auto it = resolvedAddr.find(std::string(symbol));
        if (it == resolvedAddr.end() || it->second == 0) return nullptr;
        return reinterpret_cast<void*>(it->second);
    };

    // Slot stabili per detour/trampolini (il deque non invalida i riferimenti
    // agli elementi già inseriti).
    std::deque<int> detourStorage;
    std::deque<void*> trampSlots;

    // Registra nel registro globale i PULSE_HOOK del modulo (finestra di epoca).
    auto registerHooks = [&](const ModId& modId) {
        for (const std::string& sym : hooksByMod[modId]) {
            detourStorage.push_back(0);
            trampSlots.push_back(nullptr);
            void* detour = static_cast<void*>(&detourStorage.back());
            void** tramp = &trampSlots.back();
            pulse::hooks::register_hook(sym, detour, tramp);
        }
    };

    // Invocatore dell'entry point: dispatch sul failMode della mod.
    ModManagerWiring::EntryPointInvoker invoker =
        [&](const ModId& modId, void* entry) -> EntryPointOutcome {
        (void)entry;
        const bool valid = isValid[modId];
        if (valid) {
            registerHooks(modId);
            return EntryPointOutcome::success();
        }
        switch (failByMod[modId]) {
            case FailMode::EntryError:
                return EntryPointOutcome::failure("entry point fallito (test)");
            case FailMode::EntryThrow:
                throw std::runtime_error("entry point: boom (test)");
            case FailMode::RegisterThenThrow:
                // Registra alcuni PULSE_HOOK (sposta gli indici del registro
                // globale) e POI lancia: stressa l'attribuzione a finestre.
                registerHooks(modId);
                throw std::runtime_error("entry point: registra-poi-boom (test)");
            case FailMode::ModuleNotLoadable:
            default:
                // Non dovrebbe essere invocato (load già fallita); difensivo.
                return EntryPointOutcome::failure("modulo non caricabile (test)");
        }
    };

    ModManagerWiring wiring(moduleLoader, backend, rollback, ledger, makeCtx(),
                            resolver, invoker, sink);
    ModManager manager;
    std::vector<ModId> order;
    order.reserve(included.size());
    for (const ModSpec* mod : included) {
        wiring.registerMod(manager,
                           ModWiringSpec{mod->modId, mod->image, mod->entrySymbol});
        order.push_back(mod->modId);
    }

    wiring.runNoThrow(manager, order);

    // --- raccolta dell'esito --------------------------------------------------
    ScenarioResult result;
    for (const ModSpec* mod : included) {
        result.state[mod->modId] = manager.stateOf(mod->modId);
        if (mod->valid) {
            HookKeySet keys;
            for (const OwnedHook& h : ledger.hooksOf(mod->modId))
                keys.insert({h.symbol, h.target});
            result.installedByMod[mod->modId] = std::move(keys);
        }
    }
    for (const OwnedHook& h : ledger.allInstalled())
        result.installedAll.insert({h.symbol, h.target});

    return result;
}

// --- Property 13 — non-interferenza tra mod isolate -------------------------
// Feature: external-mod-loading, Property 13.
// Validates: Requirements 6.5.
RC_GTEST_PROP(EmlProperty13IsolatedNonInterference,
              IsolatedFailureNeverAddsOrRemovesOtherModsHooks,
              ()) {
    // --- generazione del batch (mix di mod valide e fallite) ----------------
    const int numMods = *rc::gen::inRange(2, 7);  // 2..6 mod
    std::vector<ModSpec> mods;
    std::uintptr_t nextAddress = 0x4000;  // indirizzi risolti globalmente unici
    int validCount = 0;
    int failCount = 0;

    for (int m = 0; m < numMods; ++m) {
        ModSpec mod;
        mod.modId = "mod." + std::to_string(m);
        mod.entrySymbol = mod.modId + "_init";
        mod.image = Bytes{static_cast<std::uint8_t>(0x10 + m)};
        mod.valid = *rc::gen::arbitrary<bool>();

        int numHooks = 0;
        if (mod.valid) {
            numHooks = *rc::gen::inRange(0, 5);  // 0..4 PULSE_HOOK risolvibili
            ++validCount;
        } else {
            mod.failMode = *rc::gen::element(
                FailMode::ModuleNotLoadable, FailMode::EntryError,
                FailMode::EntryThrow, FailMode::RegisterThenThrow);
            if (mod.failMode == FailMode::RegisterThenThrow)
                numHooks = *rc::gen::inRange(1, 4);  // 1..3 prima del boom
            ++failCount;
        }

        for (int h = 0; h < numHooks; ++h) {
            HookSpec hook;
            hook.symbol = mod.modId + "::sym" + std::to_string(h);
            hook.address = nextAddress;
            nextAddress += 0x100;  // separazione → nessuna collisione di indirizzi
            mod.hooks.push_back(std::move(hook));
        }
        mods.push_back(std::move(mod));
    }

    // Forza la presenza di ≥1 valida e ≥1 fallita così il confronto metamorfico
    // è significativo (altrimenti i due scenari sarebbero identici per banalità).
    if (validCount == 0) {
        mods.front().valid = true;
        mods.front().failMode = FailMode::EntryError;  // ignorato perché valido
        // Dai alla mod resa valida un paio di hook risolvibili.
        mods.front().hooks.clear();
        for (int h = 0; h < 2; ++h) {
            HookSpec hook;
            hook.symbol = mods.front().modId + "::sym" + std::to_string(h);
            hook.address = nextAddress;
            nextAddress += 0x100;
            mods.front().hooks.push_back(std::move(hook));
        }
    }
    if (failCount == 0) {
        mods.back().valid = false;
        mods.back().failMode = FailMode::EntryThrow;
        mods.back().hooks.clear();  // EntryThrow non registra hook
    }

    // --- scenario MIXED: tutte le mod (valide + fallite) --------------------
    std::vector<const ModSpec*> all;
    all.reserve(mods.size());
    for (const ModSpec& mod : mods) all.push_back(&mod);
    const ScenarioResult mixed = runScenario(all, /*scenarioTag*/ 0);

    // --- scenario VALID-ONLY: solo le mod valide, stesso ordine relativo ----
    std::vector<const ModSpec*> validOnly;
    for (const ModSpec& mod : mods)
        if (mod.valid) validOnly.push_back(&mod);
    const ScenarioResult valids = runScenario(validOnly, /*scenarioTag*/ 1);

    // --- asserzioni metamorfiche (Req 6.5) ----------------------------------
    // (1) Ogni mod valida è Enabled in ENTRAMBI gli scenari: il fallimento
    //     isolato non impedisce alle valide di abilitarsi (Req 6.5/6.6).
    HookKeySet expectedValidUnion;
    for (const ModSpec& mod : mods) {
        if (!mod.valid) continue;

        const auto itMixedState = mixed.state.find(mod.modId);
        const auto itValidState = valids.state.find(mod.modId);
        RC_ASSERT(itMixedState != mixed.state.end());
        RC_ASSERT(itValidState != valids.state.end());
        RC_ASSERT(itMixedState->second == ModState::Enabled);
        RC_ASSERT(itValidState->second == ModState::Enabled);

        // (2) L'insieme degli hook installati della mod valida è IDENTICO nei
        //     due scenari: il fallimento isolato non aggiunge né rimuove hook
        //     della mod valida (Req 6.5).
        const auto itMixed = mixed.installedByMod.find(mod.modId);
        const auto itValid = valids.installedByMod.find(mod.modId);
        RC_ASSERT(itMixed != mixed.installedByMod.end());
        RC_ASSERT(itValid != valids.installedByMod.end());
        RC_ASSERT(itMixed->second == itValid->second);

        // L'oracolo indipendente dell'insieme atteso: esattamente i suoi hook.
        HookKeySet expectedForMod;
        for (const HookSpec& hook : mod.hooks)
            expectedForMod.insert({hook.symbol, hook.address});
        RC_ASSERT(itValid->second == expectedForMod);

        expectedValidUnion.insert(expectedForMod.begin(), expectedForMod.end());
    }

    // (3) L'insieme GLOBALE degli hook installati nello scenario mixed coincide
    //     esattamente con quello dello scenario valid-only, ed entrambi con
    //     l'unione attesa degli hook delle sole mod valide: nessun hook
    //     installato è imputabile alle mod fallite (Req 6.5, 9.6).
    RC_ASSERT(mixed.installedAll == valids.installedAll);
    RC_ASSERT(mixed.installedAll == expectedValidUnion);
}

}  // namespace
