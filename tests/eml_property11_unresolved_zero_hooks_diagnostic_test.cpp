// tests/eml_property11_unresolved_zero_hooks_diagnostic_test.cpp
// Feature: external-mod-loading, Property 11 — Zero hook su indirizzi non
// risolti, con diagnostica attribuita.
// Validates: Requirements 5.7, 9.1, 9.4 (Requisiti 5.7, 9.1, 9.4)
//
// Property 11 (design.md §"Property 11"): per OGNI mix di `PULSE_HOOK` risolti e
// non risolti registrati dai Mod_Module di una o più mod, il cablaggio del
// ModManager installa ZERO hook sugli indirizzi non risolti e ogni `PULSE_HOOK`
// non risolto produce una diagnostica che identifica il Mod_Id e il simbolo;
// gli hook risolti, viceversa, sono installati e attribuiti al Mod_Id
// proprietario, con i byte delle funzioni bersaglio toccati SOLO per i risolti.
//
// Modello (host-testable, niente OS loader — coerente con la nota del design
// "FakeBackend che modella i byte delle funzioni bersaglio"):
//   * `FakeModuleLoader` modella il registro di simboli/byte del Mod_Module
//     (entry point sempre risolvibile: la proprietà riguarda i `PULSE_HOOK`,
//     non l'entry point);
//   * `FakeBackend` modella i byte delle funzioni bersaglio (readOriginal /
//     install / remove byte-esatti), così l'install effettivo è osservabile;
//   * l'invocatore dell'entry point registra nel registro globale
//     `pulse::hooks` i `PULSE_HOOK` della mod (cadono nella finestra di epoca
//     del mod), sia quelli risolvibili che quelli NON risolvibili;
//   * il `SymbolResolver` iniettato marca un mix casuale di simboli come
//     risolti (indirizzo stabile non nullo) o non risolti (nullptr);
//   * un `CapturingSink` raccoglie le diagnostiche per verificarne
//     l'attribuzione (Mod_Id + simbolo).
//
// Strategia (RapidCheck, ≥100 iterazioni — default RC_GTEST_PROP): si generano
// 1..4 mod con Mod_Id DISTINTI, ciascuna con 0..5 `PULSE_HOOK`, ognuno risolto
// o non risolto con probabilità casuale. L'oracolo (insieme atteso dei risolti
// e dei non risolti, con i loro indirizzi) è costruito INDIPENDENTEMENTE dal
// cablaggio. Dopo `enableAll` si verifica:
//   (a) il numero di hook installati nel backend == numero di hook risolti
//       (ZERO install su indirizzi non risolti — Req 5.7, 9.1, 9.4);
//   (b) ogni indirizzo risolto è installato e attribuito al suo Mod_Id; nessun
//       altro indirizzo è installato (byte invariati per i non risolti);
//   (c) per OGNI `PULSE_HOOK` non risolto esiste una diagnostica che contiene
//       sia il Mod_Id sia il simbolo;
//   (d) `hooksOf(mod)` == esattamente gli hook risolti di quel Mod_Id.
//
// Header del loader in loader/lifecycle/ (include relativo alla radice loader/);
// la logica del cablaggio è in mod_loader.cpp (compilata in pulse::loader).
// Integrazione RapidCheck+GoogleTest in extras/gtest.

#include "lifecycle/mod_loader.hpp"

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
using pulse::lifecycle::ModWiringSpec;
using pulse::lifecycle::OwnedHook;

// Azzera il registro globale dello SDK: è un singleton di processo condiviso fra
// le unità di traduzione, quindi ogni iterazione deve partire da uno stato noto.
void resetRegistry() { pulse::hooks::registry().clear(); }

// Runtime_Context fittizio (macOS arm64, GD 2.2081).
pulse::loader::RuntimeContext makeCtx() {
    pulse::loader::RuntimeContext ctx;
    ctx.gdVersion = pulse::loader::GdVersion{2, 2081};
    ctx.platform = pulse::loader::Platform::MacOS;
    ctx.platformId = "macos-arm64";
    return ctx;
}

// Cattura le diagnostiche emesse dal cablaggio.
struct CapturingSink {
    std::vector<std::string> messages;
    pulse::loader::DiagnosticSink sink() {
        return [this](std::string_view m) { messages.emplace_back(m); };
    }
    bool anyContains(std::string_view a, std::string_view b) const {
        for (const std::string& m : messages)
            if (m.find(a) != std::string::npos && m.find(b) != std::string::npos)
                return true;
        return false;
    }
};

// Un singolo PULSE_HOOK della mod: simbolo univoco + se è risolvibile + (se
// risolto) il suo indirizzo bersaglio stabile e non nullo.
struct HookSpec {
    std::string symbol;
    bool resolved{false};
    std::uintptr_t address{0};  // valido sse resolved
};

// Una mod del modello: Mod_Id + i suoi PULSE_HOOK + il simbolo dell'entry point.
struct ModSpec {
    ModId modId;
    std::string entrySymbol;
    Bytes image;
    std::vector<HookSpec> hooks;
};

// --- Property 11 — zero hook su indirizzi non risolti, diagnostica attribuita -
// Feature: external-mod-loading, Property 11.
// Validates: Requirements 5.7, 9.1, 9.4.
RC_GTEST_PROP(EmlProperty11UnresolvedZeroHooksDiagnostic,
              UnresolvedHooksNeverInstalledAndDiagnosedWithModIdAndSymbol,
              ()) {
    resetRegistry();

    // --- generazione del mix di mod e hook risolti/non risolti --------------
    const int numMods = *rc::gen::inRange(1, 5);  // 1..4 mod
    std::vector<ModSpec> mods;
    std::uintptr_t nextAddress = 0x4000;  // indirizzi risolti globalmente unici
    for (int m = 0; m < numMods; ++m) {
        ModSpec mod;
        mod.modId = "mod." + std::to_string(m);
        mod.entrySymbol = mod.modId + "_init";
        // Immagine del Mod_Module distinta per mod (chiave del FakeModuleLoader).
        mod.image = Bytes{static_cast<std::uint8_t>(0x10 + m)};
        const int numHooks = *rc::gen::inRange(0, 6);  // 0..5 PULSE_HOOK
        for (int h = 0; h < numHooks; ++h) {
            HookSpec hook;
            hook.symbol = mod.modId + "::sym" + std::to_string(h);
            hook.resolved = *rc::gen::arbitrary<bool>();
            if (hook.resolved) {
                hook.address = nextAddress;
                nextAddress += 0x100;  // separazione → nessuna collisione
            }
            mod.hooks.push_back(std::move(hook));
        }
        mods.push_back(std::move(mod));
    }

    // --- banco di prova host-testabile --------------------------------------
    FakeModuleLoader moduleLoader;
    FakeBackend backend;
    HookOwnershipLedger ledger;
    CapturingSink sink;
    static int rollbackSeq = 0;
    const std::filesystem::path rbkPath =
        std::filesystem::path(::testing::TempDir()) /
        ("pulse_eml_p11_" + std::to_string(rollbackSeq++) + ".rbk");
    RollbackStore rollback{rbkPath};

    // simbolo (PULSE_HOOK) → indirizzo risolto (0 = non risolvibile).
    std::unordered_map<std::string, std::uintptr_t> resolvedAddr;
    // modId → simboli registrati dal Mod_Module all'enable (PULSE_HOOK).
    std::unordered_map<ModId, std::vector<std::string>> hooksByMod;

    for (const ModSpec& mod : mods) {
        // Entry point SEMPRE risolvibile: la proprietà riguarda i PULSE_HOOK.
        FakeModuleLoader::ModuleSpec spec;
        spec.exports.push_back({mod.entrySymbol, {0x90, 0x90}});
        moduleLoader.program(mod.image, spec);

        for (const HookSpec& hook : mod.hooks) {
            hooksByMod[mod.modId].push_back(hook.symbol);
            resolvedAddr[hook.symbol] = hook.resolved ? hook.address : 0;
        }
    }

    // Resolver dei simboli: indirizzo risolto non nullo o nullptr (non risolto).
    ModManagerWiring::SymbolResolver resolver =
        [&resolvedAddr](std::string_view symbol) -> void* {
        const auto it = resolvedAddr.find(std::string(symbol));
        if (it == resolvedAddr.end() || it->second == 0) return nullptr;
        return reinterpret_cast<void*>(it->second);
    };

    // Slot stabili per detour/trampolini dei PULSE_HOOK registrati (il deque
    // non invalida i riferimenti agli elementi già inseriti).
    std::deque<int> detourStorage;
    std::deque<void*> trampSlots;

    // Invocatore dell'entry point: registra nel registro globale i PULSE_HOOK
    // del modulo (cadono nella finestra di epoca del mod), risolti e non.
    ModManagerWiring::EntryPointInvoker invoker =
        [&](const ModId& modId, void* entry) -> EntryPointOutcome {
        (void)entry;
        for (const std::string& sym : hooksByMod[modId]) {
            detourStorage.push_back(0);
            trampSlots.push_back(nullptr);
            void* detour = static_cast<void*>(&detourStorage.back());
            void** tramp = &trampSlots.back();
            pulse::hooks::register_hook(sym, detour, tramp);
        }
        return EntryPointOutcome::success();
    };

    ModManagerWiring wiring(moduleLoader, backend, rollback, ledger, makeCtx(),
                            resolver, invoker, sink.sink());
    ModManager manager;
    std::vector<std::string> order;
    for (const ModSpec& mod : mods) {
        wiring.registerMod(manager, ModWiringSpec{mod.modId, mod.image, mod.entrySymbol});
        order.push_back(mod.modId);
    }

    manager.enableAll(order);

    // --- oracolo indipendente: risolti e non risolti attesi -----------------
    std::set<std::uintptr_t> expectedResolvedAddrs;
    std::size_t expectedResolvedCount = 0;
    for (const ModSpec& mod : mods) {
        std::vector<std::string> expectedResolvedSymbols;
        for (const HookSpec& hook : mod.hooks) {
            if (hook.resolved) {
                ++expectedResolvedCount;
                expectedResolvedAddrs.insert(hook.address);
                expectedResolvedSymbols.push_back(hook.symbol);

                // (b) ogni indirizzo risolto è installato e attribuito al mod.
                RC_ASSERT(backend.isInstalled(hook.address));
            } else {
                // (c) ogni PULSE_HOOK NON risolto → diagnostica con Mod_Id +
                //     simbolo (Req 5.7, 9.4).
                RC_ASSERT(sink.anyContains(mod.modId, hook.symbol));
            }
        }

        // (d) hooksOf(mod) == esattamente gli hook RISOLTI del mod, attribuiti.
        const std::vector<OwnedHook> owned = ledger.hooksOf(mod.modId);
        RC_ASSERT(owned.size() == expectedResolvedSymbols.size());
        std::set<std::string> ownedSymbols;
        for (const OwnedHook& hook : owned) {
            RC_ASSERT(hook.owner == mod.modId);
            // L'indirizzo attribuito è uno degli indirizzi risolti del mod.
            RC_ASSERT(resolvedAddr[hook.symbol] == hook.target);
            RC_ASSERT(hook.target != 0);
            ownedSymbols.insert(hook.symbol);
        }
        for (const std::string& sym : expectedResolvedSymbols)
            RC_ASSERT(ownedSymbols.count(sym) == 1u);
    }

    // (a) ZERO hook su indirizzi non risolti: il numero TOTALE di hook
    //     installati nel backend è esattamente il numero di hook risolti, e
    //     l'insieme degli hook attribuiti coincide (Req 5.7, 9.1, 9.4).
    RC_ASSERT(backend.installedCount() == expectedResolvedCount);
    RC_ASSERT(ledger.allInstalled().size() == expectedResolvedCount);

    // Solo gli indirizzi risolti risultano installati (byte invariati per i
    // non risolti: non esiste alcuna regione installata fuori dal set risolto).
    for (const OwnedHook& hook : ledger.allInstalled()) {
        RC_ASSERT(expectedResolvedAddrs.count(hook.target) == 1u);
        RC_ASSERT(backend.isInstalled(hook.target));
    }

    // Un record di rollback (byte originali, owner=Mod_Id) per ogni hook
    // RISOLTO e installato; nessun record per gli indirizzi non risolti.
    RC_ASSERT(rollback.records().size() == expectedResolvedCount);
}

}  // namespace
