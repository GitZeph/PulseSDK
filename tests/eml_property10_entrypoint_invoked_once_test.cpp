// tests/eml_property10_entrypoint_invoked_once_test.cpp
// Feature: external-mod-loading, Property 10 — Entry point invocato esattamente
// una volta all'enable.
// Validates: Requirements 5.4, 7.1 (Requisiti 5.4, 7.1)
//
// Property 10 (design.md §"Property 10"): per OGNI insieme di mod abilitate con
// successo, l'entry point di ciascuna mod è invocato ESATTAMENTE una volta
// tramite la transizione verso lo stato Enabled.
//
// Il comportamento è host-testabile sul cablaggio reale `ModManagerWiring`
// (loader/lifecycle/mod_loader.hpp, task 7.4): la state machine del `ModManager`
// invoca l'`EntryPointFn` registrato dal wiring ESATTAMENTE una volta sulla
// transizione applicata verso `Enabled` (Req 5.4, 7.1). Il wiring espone
// `entryPointInvocations(modId)` come introspezione del numero reale di
// invocazioni dell'entry point per mod (oracolo indipendente: anche
// l'invocatore iniettato conta le proprie chiamate). I seam host-testabili
// (`FakeModuleLoader`, `FakeBackend`, resolver/invocatore iniettati) evitano un
// binario reale di GD; il path `dlopen` reale è coperto in Fase E, non da PBT.
//
// Strategia (RapidCheck, ≥100 iterazioni di default — RC_GTEST_PROP):
//   * si genera un batch di mod con Mod_Id DISTINTI; ogni mod dichiara un entry
//     point e il proprio Mod_Module è programmato nel fake in una categoria:
//       - Enabled    → entry point risolvibile e invocatore che riesce: la mod
//                       raggiunge Enabled e l'entry point è invocato 1 volta;
//       - EntryFail  → entry point risolvibile ma invocatore che fallisce: la
//                       mod è isolata (Disabled), l'entry point è comunque
//                       invocato 1 volta (tramite il tentativo di transizione);
//       - Unresolvable → entry point NON esportato: nessuna invocazione (0),
//                         mod isolata (Disabled);
//   * si registrano tutte le mod nel `ModManager` via il wiring e si esegue
//     `enableAll` sull'intero batch;
//   * l'oracolo, costruito indipendentemente dal wiring, calcola l'insieme atteso
//     di mod Enabled e il conteggio atteso di invocazioni per mod.
//
// Asserzioni chiave:
//   (1) ogni mod abilitata con successo è in stato Enabled, con
//       entryPointInvocations == 1 (Property 10 / Req 5.4, 7.1);
//   (2) ri-richiedere `enable` su una mod già Enabled è rifiutato dalla state
//       machine e NON re-invoca l'entry point (resta 1): "esattamente una volta
//       TRAMITE la transizione verso Enabled" (Req 7.1);
//   (3) coerenza globale: l'insieme delle mod Enabled coincide con l'oracolo.
//
// Header del loader in loader/lifecycle/ (include relativo alla radice loader/);
// la logica è in mod_loader.cpp/mod_manager.cpp/hook_ownership.cpp (compilate in
// pulse::loader via glob lifecycle/*.cpp). Integrazione RapidCheck+GoogleTest in
// extras/gtest.

#include "lifecycle/mod_loader.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
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

// Azzera il registro globale dello SDK: singleton di processo condiviso fra le
// unità di traduzione; ogni iterazione deve partire da uno stato noto.
void resetRegistry() { pulse::hooks::registry().clear(); }

// Runtime_Context fittizio (macOS arm64, GD 2.2081).
pulse::loader::RuntimeContext makeCtx() {
    pulse::loader::RuntimeContext ctx;
    ctx.gdVersion = pulse::loader::GdVersion{2, 2081};
    ctx.platform = pulse::loader::Platform::MacOS;
    ctx.platformId = "macos-arm64";
    return ctx;
}

// Percorso temporaneo unico per il RollbackStore di ogni iterazione (il file di
// rollback è write-through: serve un percorso distinto per iterazione).
std::filesystem::path uniqueRollbackPath() {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
    return std::filesystem::path(::testing::TempDir()) /
           ("pulse_p10_" + std::to_string(n) + ".rbk");
}

// Categoria di una mod del batch.
enum class Category {
    Enabled,       // entry risolvibile + invocatore ok → Enabled, 1 invocazione
    EntryFail,     // entry risolvibile + invocatore che fallisce → Disabled, 1 invoc.
    Unresolvable,  // entry NON esportato → Disabled, 0 invocazioni
};

struct ModUnderTest {
    ModId modId;
    std::string entrySymbol;
    Bytes moduleImage;
    Category category{Category::Enabled};
};

// Banco di prova: instrada l'invocazione dell'entry point e modella i PULSE_HOOK
// registrati all'enable (cadono nella finestra di epoca del mod).
struct Harness {
    FakeModuleLoader moduleLoader;
    FakeBackend backend;
    HookOwnershipLedger ledger;
    RollbackStore rollback;

    // simbolo → indirizzo risolto (per i PULSE_HOOK della mod).
    std::unordered_map<std::string, std::uintptr_t> resolved;
    // modId → simboli registrati all'enable.
    std::unordered_map<ModId, std::vector<std::string>> hooksByMod;
    // modId → true se l'invocatore dell'entry point deve fallire.
    std::unordered_map<ModId, bool> failMod;
    // oracolo indipendente: invocazioni dell'entry point per mod.
    std::unordered_map<ModId, int> invocations;
    // storage stabile per detour/trampolini registrati.
    std::deque<int> detourStorage;
    std::deque<void*> trampSlots;

    Harness() : rollback(uniqueRollbackPath()) {}

    // Programma una mod nel FakeModuleLoader e registra i suoi hook risolvibili.
    void programMod(const ModUnderTest& mod) {
        FakeModuleLoader::ModuleSpec spec;
        if (mod.category != Category::Unresolvable) {
            spec.exports.push_back({mod.entrySymbol, {0x90, 0x90}});
        } else {
            // Esporta un simbolo diverso (l'entry dichiarato resta irrisolvibile).
            spec.exports.push_back({"other_" + mod.entrySymbol, {0x00}});
        }
        moduleLoader.program(mod.moduleImage, spec);

        failMod[mod.modId] = (mod.category == Category::EntryFail);

        // Ogni mod registra un paio di PULSE_HOOK risolvibili all'enable.
        for (int h = 0; h < 2; ++h) {
            const std::string sym = mod.modId + "::f" + std::to_string(h);
            hooksByMod[mod.modId].push_back(sym);
            resolved[sym] =
                0x4000 + static_cast<std::uintptr_t>(
                             std::hash<std::string>{}(sym) & 0xFFFF);
        }
    }

    ModManagerWiring::SymbolResolver makeResolver() {
        return [this](std::string_view symbol) -> void* {
            auto it = resolved.find(std::string(symbol));
            if (it == resolved.end() || it->second == 0) return nullptr;
            return reinterpret_cast<void*>(it->second);
        };
    }

    // Invocatore dell'entry point: conta l'invocazione, fallisce se richiesto,
    // altrimenti registra i PULSE_HOOK del modulo nella finestra di epoca.
    ModManagerWiring::EntryPointInvoker makeInvoker() {
        return [this](const ModId& modId, void* entry) -> EntryPointOutcome {
            (void)entry;
            invocations[modId] += 1;
            if (failMod[modId]) {
                return EntryPointOutcome::failure("entry point fallito (test)");
            }
            for (const std::string& sym : hooksByMod[modId]) {
                detourStorage.push_back(0);
                trampSlots.push_back(nullptr);
                void* detour = static_cast<void*>(&detourStorage.back());
                void** tramp = &trampSlots.back();
                pulse::hooks::register_hook(sym, detour, tramp);
            }
            return EntryPointOutcome::success();
        };
    }
};

// --- Property 10 — entry point invocato esattamente una volta all'enable ----
// Feature: external-mod-loading, Property 10.
// Validates: Requirements 5.4, 7.1.
RC_GTEST_PROP(EmlProperty10EntryPointInvokedOnce,
              EntryPointInvokedExactlyOnceViaTransitionToEnabled,
              ()) {
    resetRegistry();

    const int count = *rc::gen::inRange(0, 11);

    Harness h;
    std::vector<ModUnderTest> mods;
    std::vector<ModId> loadOrder;

    // Oracolo indipendente dal wiring.
    std::set<ModId> expectedEnabled;       // mod che raggiungono Enabled
    std::unordered_map<ModId, int> expectedInvocations;

    for (int i = 0; i < count; ++i) {
        const std::string idx = std::to_string(i);
        const int catPick = *rc::gen::inRange(0, 3);
        const Category category = static_cast<Category>(catPick);

        ModUnderTest mod;
        mod.modId = "mod." + idx;
        mod.entrySymbol = "entry_" + idx;
        // Byte del Mod_Module distinti per mod (match esatto nel fake).
        mod.moduleImage = Bytes{static_cast<std::uint8_t>(0xC0 + (i & 0x0F)),
                                static_cast<std::uint8_t>((i >> 4) & 0xFF),
                                static_cast<std::uint8_t>(i & 0xFF)};
        mod.category = category;

        switch (category) {
            case Category::Enabled:
                expectedEnabled.insert(mod.modId);
                expectedInvocations[mod.modId] = 1;  // invocato 1 volta, ok
                break;
            case Category::EntryFail:
                expectedInvocations[mod.modId] = 1;  // invocato 1 volta, poi Disabled
                break;
            case Category::Unresolvable:
                expectedInvocations[mod.modId] = 0;  // mai invocato
                break;
        }

        h.programMod(mod);
        loadOrder.push_back(mod.modId);
        mods.push_back(std::move(mod));
    }

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), h.makeInvoker(),
                            /*sink=*/{});
    ModManager manager;
    for (const ModUnderTest& mod : mods) {
        wiring.registerMod(manager,
                           ModWiringSpec{mod.modId, mod.moduleImage, mod.entrySymbol});
    }

    // Abilita in blocco l'intero batch nell'ordine di caricamento.
    const auto result = manager.enableAll(loadOrder);

    // --- (1) Ogni mod abilitata con successo: stato Enabled e l'entry point
    //         invocato ESATTAMENTE una volta (Property 10 / Req 5.4, 7.1). ----
    std::set<ModId> actuallyEnabled(result.enabled.begin(), result.enabled.end());
    for (const ModId& modId : result.enabled) {
        RC_ASSERT(manager.stateOf(modId) == ModState::Enabled);
        // Conteggio reale del wiring e oracolo indipendente concordi su 1.
        RC_ASSERT(wiring.entryPointInvocations(modId) == 1);
        RC_ASSERT(h.invocations[modId] == 1);
    }

    // --- (2) "Esattamente una volta TRAMITE la transizione verso Enabled":
    //         ri-richiedere enable su una mod già Enabled è rifiutato dalla
    //         state machine e NON re-invoca l'entry point (resta 1) (Req 7.1). -
    for (const ModId& modId : result.enabled) {
        const auto again = manager.enable(modId);
        RC_ASSERT(again.rejected());
        RC_ASSERT(manager.stateOf(modId) == ModState::Enabled);
        RC_ASSERT(wiring.entryPointInvocations(modId) == 1);
        RC_ASSERT(h.invocations[modId] == 1);
    }

    // --- (3) Coerenza globale: l'insieme delle mod Enabled coincide con
    //         l'oracolo, e per ogni mod del batch il numero di invocazioni
    //         dell'entry point è quello atteso (1 per Enabled/EntryFail, 0 per
    //         entry non risolvibile). --------------------------------------
    RC_ASSERT(actuallyEnabled == expectedEnabled);
    for (const ModUnderTest& mod : mods) {
        RC_ASSERT(wiring.entryPointInvocations(mod.modId) ==
                  expectedInvocations[mod.modId]);
        RC_ASSERT(h.invocations[mod.modId] == expectedInvocations[mod.modId]);
        if (mod.category != Category::Enabled) {
            // Le mod non abilitate con successo sono isolate a Disabled.
            RC_ASSERT(manager.stateOf(mod.modId) == ModState::Disabled);
        }
    }
}

}  // namespace
