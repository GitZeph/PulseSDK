// tests/eml_property15_install_remove_byte_exact_roundtrip_test.cpp
// Feature: external-mod-loading, Property 15 — Round-trip install→remove
// byte-esatto per mod.
// Validates: Requirements 7.2, 8.3, 8.4 (Requisiti 7.2, 8.3, 8.4)
//
// Property 15 (design.md §"Property 15"): PER OGNI mod caricata,
// l'installazione seguita dalla rimozione dei suoi hook riporta i byte di
// TUTTE le funzioni bersaglio identici allo stato precedente
// all'installazione.
//
// Conseguenze verificate:
//   * Req 7.2/8.3: la rimozione degli hook del mod (via disable, o via il
//     terminator durante il teardown) ripristina i byte delle funzioni
//     bersaglio BYTE-ESATTO usando i byte originali persistiti nel
//     RollbackStore PRIMA dell'install;
//   * Req 8.4: a hook rimossi, lo stato dei byte bersaglio è identico
//     byte-per-byte al pre-installazione (nessun residuo del detour).
//
// Modello (host-testable, niente OS loader — coerente con la nota del design
// "FakeBackend che modella i byte delle funzioni bersaglio"):
//   * `FakeModuleLoader` modella il registro di simboli/byte del Mod_Module
//     (entry point sempre risolvibile);
//   * `FakeBackend` modella i byte delle funzioni bersaglio
//     (seedOriginal/install/remove/liveBytes byte-esatti); gli indirizzi
//     RISOLTI sono seminati con byte originali casuali, così install/remove
//     byte-esatto è significativo (l'install patcha i byte "live" in uno stub
//     di detour garantito diverso dagli originali);
//   * l'invocatore dell'entry point registra nel registro globale
//     `pulse::hooks` TUTTI i `PULSE_HOOK` del mod UNA volta (static-init al
//     dlopen); la risoluzione del binding avviene via `resolve_all` con il
//     `SymbolResolver`, che restituisce `nullptr` per i simboli non risolvibili
//     → nessun install (e quindi nessun byte mutato per quegli indirizzi).
//
// Strategia (RapidCheck, ≥100 iterazioni — default RC_GTEST_PROP): si genera
// una mod con 1..5 `PULSE_HOOK` ciascuno casualmente risolvibile o no (mix),
// con indirizzi bersaglio DISTINTI e non nulli per quelli risolti, e byte
// originali casuali. Si CATTURA lo snapshot dei byte di ogni bersaglio risolto
// PRIMA dell'install, si esegue enable (install) → disable (remove) e si
// verifica che i byte "live" di OGNI bersaglio risolto tornino identici allo
// snapshot pre-install.
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

// --- Property 15 — round-trip install→remove byte-esatto per mod ------------
// Feature: external-mod-loading, Property 15.
// Validates: Requirements 7.2, 8.3, 8.4.
RC_GTEST_PROP(EmlProperty15InstallRemoveByteExactRoundTrip,
              RemoveRestoresTargetFunctionBytesByteExact, ()) {
    resetRegistry();

    // --- generazione della mod con mix di hook risolti/non risolti ----------
    const ModId modId = "mod.byteexact";
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

    // --- banco di prova host-testabile --------------------------------------
    FakeModuleLoader moduleLoader;
    FakeBackend backend;
    HookOwnershipLedger ledger;
    RollbackStore rollback{std::filesystem::path(::testing::TempDir()) /
                           ("pulse_eml_p15_" +
                            std::to_string(reinterpret_cast<std::uintptr_t>(&backend)) +
                            ".rbk")};

    const std::string entrySymbol = modId + "_init";
    const Bytes image{0x42};

    // Entry point sempre risolvibile: la proprietà riguarda i byte delle
    // funzioni bersaglio, non la risoluzione dell'entry point.
    FakeModuleLoader::ModuleSpec spec;
    spec.exports.push_back({entrySymbol, {0x90, 0x90}});
    moduleLoader.program(image, spec);

    // simbolo (PULSE_HOOK) → indirizzo risolto (0 se non risolvibile).
    std::unordered_map<std::string, std::uintptr_t> resolvedAddr;
    // Insieme degli indirizzi RISOLTI del mod (le funzioni bersaglio reali).
    std::vector<std::uintptr_t> resolvedTargets;
    for (const HookSpec& hook : hooks) {
        resolvedAddr[hook.symbol] = hook.resolvable ? hook.address : 0;
        if (hook.resolvable) {
            // Semina byte originali casuali per gli indirizzi risolti: è
            // l'oracolo del round-trip byte-esatto.
            FakeBackend::Bytes original(kPrologue);
            for (std::size_t i = 0; i < kPrologue; ++i)
                original[i] = static_cast<FakeBackend::Byte>(
                    *rc::gen::inRange<int>(0, 256));
            backend.seedOriginal(hook.address, original);
            resolvedTargets.push_back(hook.address);
        }
    }

    // --- SNAPSHOT pre-installazione dei byte di OGNI funzione bersaglio ------
    // Oracolo INDIPENDENTE dal cablaggio: i byte "live" di ciascun bersaglio
    // risolto PRIMA di qualunque install.
    std::unordered_map<std::uintptr_t, FakeBackend::Bytes> preInstallBytes;
    for (std::uintptr_t addr : resolvedTargets) {
        const auto live = backend.liveBytes(addr);
        RC_ASSERT(live.has_value());
        preInstallBytes[addr] = *live;
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

    // Invocatore: registra i PULSE_HOOK del modulo (TUTTI, risolti e non) SOLO
    // alla prima invocazione (static-init al dlopen). La risoluzione del binding
    // la fa resolve_all via il resolver.
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

    // --- enable: install ESATTAMENTE degli hook risolti ---------------------
    RC_ASSERT(wiring.enable(manager, modId).applied());
    RC_ASSERT(manager.stateOf(modId) == ModState::Enabled);
    RC_ASSERT(backend.installedCount() == resolvedTargets.size());
    for (std::uintptr_t addr : resolvedTargets) {
        RC_ASSERT(backend.isInstalled(addr));
        // Sanity: l'install ha effettivamente patchato i byte "live" (diversi
        // dal pre-install), così il round-trip è significativo.
        const auto live = backend.liveBytes(addr);
        RC_ASSERT(live.has_value());
        RC_ASSERT(*live != preInstallBytes[addr]);
    }

    // --- remove (disable): ripristino BYTE-ESATTO delle funzioni bersaglio ---
    RC_ASSERT(wiring.disable(manager, modId).applied());
    RC_ASSERT(manager.stateOf(modId) == ModState::Disabled);
    RC_ASSERT(backend.installedCount() == 0u);

    // PROPRIETÀ: i byte di OGNI funzione bersaglio sono identici al
    // pre-installazione (Req 7.2/8.3/8.4).
    for (std::uintptr_t addr : resolvedTargets) {
        RC_ASSERT(!backend.isInstalled(addr));
        const auto live = backend.liveBytes(addr);
        RC_ASSERT(live.has_value());
        RC_ASSERT(*live == preInstallBytes[addr]);
    }
}

}  // namespace
