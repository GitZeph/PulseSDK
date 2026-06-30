// tests/eml_property16_enable_disable_enable_roundtrip_test.cpp
// Feature: external-mod-loading, Property 16 — Round-trip
// enable→disable→enable.
// Validates: Requirements 7.5 (Requisiti 7.5)
//
// Property 16 (design.md §"Property 16"): PER OGNI mod, la sequenza
// enable → disable → enable riporta l'insieme degli hook installati di
// proprietà di quella mod ESATTAMENTE all'insieme dei suoi hook con binding
// risolto, e lascia la mod nello stato Enabled.
//
// Conseguenze verificate (Req 7.2/7.3/7.5):
//   * primo enable → Enabled, con installati e attribuiti ESATTAMENTE gli hook
//     con binding risolto del mod (i non risolti non producono install);
//   * disable → Disabled, ZERO hook del mod (rimozione byte-esatto via
//     RollbackStore/backend), Mod_Module conservato (nessun dlclose);
//   * re-enable → Enabled, RIUSO della finestra di epoca memorizzata SENZA un
//     nuovo dlopen (loadCount invariato), e l'insieme degli hook attribuiti al
//     mod torna ESATTAMENTE a quello del primo enable (gli stessi indirizzi
//     risolti, nessun install sui non risolti).
//
// Modello (host-testable, niente OS loader — coerente con la nota del design
// "FakeBackend che modella i byte delle funzioni bersaglio"):
//   * `FakeModuleLoader` modella il registro di simboli/byte del Mod_Module
//     (entry point sempre risolvibile; conta i load → loadCount());
//   * `FakeBackend` modella i byte delle funzioni bersaglio
//     (readOriginal/install/remove byte-esatti); gli indirizzi RISOLTI sono
//     seminati con byte originali casuali, così install/remove byte-esatto è
//     significativo;
//   * l'invocatore dell'entry point registra nel registro globale
//     `pulse::hooks` TUTTI i `PULSE_HOOK` del mod (risolti e non) UNA sola
//     volta per mod (static-init al dlopen, non ripetuto al re-enable); la
//     risoluzione del binding avviene via `resolve_all` con il `SymbolResolver`,
//     che restituisce `nullptr` per i simboli non risolvibili → nessun install.
//
// Strategia (RapidCheck, ≥100 iterazioni — default RC_GTEST_PROP): si genera
// una mod con 1..5 `PULSE_HOOK` ciascuno casualmente risolvibile o no (mix), con
// indirizzi bersaglio DISTINTI e non nulli per quelli risolti. L'oracolo
// (insieme degli indirizzi RISOLTI del mod) è costruito INDIPENDENTEMENTE dal
// cablaggio. Dopo enable→disable→enable si verifica la proprietà di round-trip.
//
// Header del loader in loader/lifecycle/ (include relativo alla radice loader/);
// la logica del cablaggio è in mod_loader.cpp (compilata in pulse::loader).
// Integrazione RapidCheck+GoogleTest in extras/gtest.

#include "lifecycle/mod_loader.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <filesystem>
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
using pulse::lifecycle::OwnedHook;

// Lunghezza del prologo modellato: coerente con kRollbackPrologueBytes del
// cablaggio (readOriginal/seedOriginal usano questa lunghezza).
constexpr std::size_t kPrologue = pulse::lifecycle::kRollbackPrologueBytes;

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

// Un singolo PULSE_HOOK della mod: simbolo univoco, flag di risolvibilità e
// indirizzo bersaglio (non nullo e distinto solo se risolvibile).
struct HookSpec {
    std::string symbol;
    bool resolvable{false};
    std::uintptr_t address{0};
};

// Insieme ORDINATO dei target attualmente attribuiti al mod (per il confronto
// di round-trip).
std::vector<std::uintptr_t> ownedTargets(const HookOwnershipLedger& ledger,
                                         const ModId& mod) {
    std::vector<std::uintptr_t> out;
    for (const OwnedHook& h : ledger.hooksOf(mod)) out.push_back(h.target);
    std::sort(out.begin(), out.end());
    return out;
}

// --- Property 16 — round-trip enable→disable→enable -------------------------
// Feature: external-mod-loading, Property 16.
// Validates: Requirements 7.5.
RC_GTEST_PROP(EmlProperty16EnableDisableEnableRoundTrip,
              RoundTripRestoresResolvedHookSetAndLeavesEnabled, ()) {
    resetRegistry();

    // --- generazione della mod con mix di hook risolti/non risolti ----------
    const ModId modId = "mod.rt";
    const int numHooks = *rc::gen::inRange(1, 6);  // 1..5 PULSE_HOOK
    std::vector<HookSpec> hooks;
    hooks.reserve(static_cast<std::size_t>(numHooks));
    std::uintptr_t nextAddress = 0x4000;  // indirizzi bersaglio distinti
    for (int h = 0; h < numHooks; ++h) {
        HookSpec hook;
        hook.symbol = modId + "::sym" + std::to_string(h);
        hook.resolvable = *rc::gen::arbitrary<bool>();
        if (hook.resolvable) {
            hook.address = nextAddress;
            nextAddress += 0x100;  // separazione → nessuna collisione di regione
        }
        hooks.push_back(std::move(hook));
    }

    // Oracolo INDIPENDENTE: l'insieme atteso degli indirizzi RISOLTI del mod.
    std::vector<std::uintptr_t> expectedResolved;
    for (const HookSpec& hook : hooks)
        if (hook.resolvable) expectedResolved.push_back(hook.address);
    std::sort(expectedResolved.begin(), expectedResolved.end());

    // --- banco di prova host-testabile --------------------------------------
    FakeModuleLoader moduleLoader;
    FakeBackend backend;
    HookOwnershipLedger ledger;
    RollbackStore rollback{std::filesystem::path(::testing::TempDir()) /
                           ("pulse_eml_p16_" +
                            std::to_string(reinterpret_cast<std::uintptr_t>(&backend)) +
                            ".rbk")};

    const std::string entrySymbol = modId + "_init";
    const Bytes image{0x42};

    // Entry point sempre risolvibile: la proprietà riguarda i binding degli
    // hook, non la risoluzione dell'entry point.
    FakeModuleLoader::ModuleSpec spec;
    spec.exports.push_back({entrySymbol, {0x90, 0x90}});
    moduleLoader.program(image, spec);

    // simbolo (PULSE_HOOK) → indirizzo risolto (0 se non risolvibile).
    std::unordered_map<std::string, std::uintptr_t> resolvedAddr;
    for (const HookSpec& hook : hooks) {
        resolvedAddr[hook.symbol] = hook.resolvable ? hook.address : 0;
        if (hook.resolvable) {
            // Semina byte originali casuali per gli indirizzi risolti (oracolo
            // del rollback byte-esatto al disable).
            FakeBackend::Bytes original(kPrologue);
            for (std::size_t i = 0; i < kPrologue; ++i)
                original[i] = static_cast<FakeBackend::Byte>(
                    *rc::gen::inRange<int>(0, 256));
            backend.seedOriginal(hook.address, original);
        }
    }

    // Resolver dei simboli: indirizzo risolto o nullptr (non risolvibile).
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
    bool registeredOnce = false;

    // Invocatore: conta SEMPRE l'invocazione, ma registra i PULSE_HOOK del
    // modulo (TUTTI, risolti e non) SOLO alla prima invocazione (static-init al
    // dlopen, non ripetuto al re-enable). La risoluzione del binding la fa
    // resolve_all via il resolver.
    ModManagerWiring::EntryPointInvoker invoker =
        [&](const ModId& id, void* entry) -> EntryPointOutcome {
        (void)id;
        (void)entry;
        if (!registeredOnce) {
            registeredOnce = true;
            for (const HookSpec& hook : hooks) {
                detourStorage.push_back(0);
                trampSlots.push_back(nullptr);
                void* detour = static_cast<void*>(&detourStorage.back());
                void** tramp = &trampSlots.back();
                pulse::hooks::register_hook(hook.symbol, detour, tramp);
            }
        }
        return EntryPointOutcome::success();
    };

    ModManagerWiring wiring(moduleLoader, backend, rollback, ledger, makeCtx(),
                            resolver, invoker, /*sink=*/{});
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{modId, image, entrySymbol});

    // --- primo enable: Enabled, install ESATTAMENTE degli hook risolti -------
    RC_ASSERT(wiring.enable(manager, modId).applied());
    RC_ASSERT(manager.stateOf(modId) == ModState::Enabled);
    const std::vector<std::uintptr_t> before = ownedTargets(ledger, modId);
    RC_ASSERT(before == expectedResolved);
    RC_ASSERT(backend.installedCount() == expectedResolved.size());
    for (std::uintptr_t addr : expectedResolved) RC_ASSERT(backend.isInstalled(addr));
    const auto window0 = wiring.epochWindow(modId);
    RC_ASSERT(moduleLoader.loadCount() == 1u);

    // --- disable: Disabled, ZERO hook del mod, modulo conservato ------------
    RC_ASSERT(wiring.disable(manager, modId).applied());
    RC_ASSERT(manager.stateOf(modId) == ModState::Disabled);
    RC_ASSERT(ledger.hooksOf(modId).empty());        // zero hook (Req 7.3)
    for (std::uintptr_t addr : expectedResolved) RC_ASSERT(!backend.isInstalled(addr));
    RC_ASSERT(wiring.moduleLoaded(modId));           // Mod_Module conservato
    RC_ASSERT(moduleLoader.unloadCount() == 0u);     // nessun dlclose al disable

    // --- re-enable: Enabled, stessa finestra, NESSUN nuovo dlopen -----------
    RC_ASSERT(wiring.enable(manager, modId).applied());
    RC_ASSERT(manager.stateOf(modId) == ModState::Enabled);
    RC_ASSERT(moduleLoader.loadCount() == 1u);       // nessun nuovo dlopen (Req 7.5)
    const auto window1 = wiring.epochWindow(modId);
    RC_ASSERT(window1 == window0);                   // finestra di epoca riusata

    // --- ROUND-TRIP: insieme degli hook ESATTAMENTE quello dei risolti ------
    const std::vector<std::uintptr_t> after = ownedTargets(ledger, modId);
    RC_ASSERT(after == before);                      // stesso insieme di hook
    RC_ASSERT(after == expectedResolved);            // = hook con binding risolto
    RC_ASSERT(backend.installedCount() == expectedResolved.size());
    for (std::uintptr_t addr : expectedResolved) RC_ASSERT(backend.isInstalled(addr));
}

}  // namespace
