// tests/eml_property18_partial_install_rollback_test.cpp
// Feature: external-mod-loading, Property 18 — Rollback transazionale su
// install parziale fallita.
// Validates: Requirements 9.5 (Requisiti 9.5)
//
// Property 18 (design.md §"Property 18"): per OGNI mod e per OGNI punto di
// fallimento iniettato DOPO che almeno un suo hook è già stato installato, gli
// hook già installati di quella mod sono rimossi con ripristino BYTE-ESATTO, il
// fallimento è confinato al solo Mod_Id (la mod termina Disabled con zero hook)
// e le mod restanti proseguono Enabled con i loro hook.
//
// Modello (host-testable, niente OS loader — coerente con la nota del design
// "FakeBackend che modella i byte delle funzioni bersaglio"):
//   * `FakeModuleLoader` modella il registro di simboli/byte del Mod_Module
//     (entry point sempre risolvibile: la proprietà riguarda l'esito
//     dell'install della finestra, non la risoluzione dell'entry point);
//   * `FakeBackend` modella i byte delle funzioni bersaglio
//     (readOriginal/install/remove byte-esatti) e consente di iniettare un
//     fallimento di install su un indirizzo specifico via `failInstallAt`. Gli
//     indirizzi bersaglio sono seminati con byte originali generati casualmente
//     (`seedOriginal`), così il confronto byte-per-byte pre/post tentativo è
//     significativo (oracolo del rollback byte-esatto);
//   * l'invocatore dell'entry point registra nel registro globale
//     `pulse::hooks` i `PULSE_HOOK` della mod (cadono nella finestra di epoca
//     del mod) la prima volta che la mod è abilitata (static-init al dlopen).
//
// L'install della finestra procede in ordine di registro (start→end): iniettare
// il fallimento all'indice k (con 1 ≤ k < N) garantisce che almeno k hook della
// mod siano stati installati PRIMA del fallimento — esattamente la precondizione
// "dopo ≥1 hook installato" della Property 18.
//
// Strategia (RapidCheck, ≥100 iterazioni — default RC_GTEST_PROP): si generano
// 0..3 mod "buone" (install sempre OK) e 1 mod "bersaglio" con N (2..4) hook,
// con il fallimento di install iniettato a un indice k casuale in [1, N-1]. Ogni
// hook ha un indirizzo bersaglio DISTINTO e risolvibile. L'oracolo (byte
// originali per indirizzo, insieme degli indirizzi per mod) è costruito
// INDIPENDENTEMENTE dal cablaggio. Le mod sono orchestrate in ordine casuale via
// la barriera no-throw `runNoThrow`. Dopo l'orchestrazione si verifica:
//   (a) la mod bersaglio è `Disabled` (fallimento confinato al solo Mod_Id);
//   (b) zero hook attribuiti alla mod bersaglio; nessuno dei suoi indirizzi è
//       installato nel backend (rollback transazionale completo, incluso lo
//       hook k-esimo già installato prima del fallimento);
//   (c) ROLLBACK BYTE-ESATTO: i byte "live" di OGNI indirizzo bersaglio della
//       mod fallita sono identici ai byte originali (stato pre-tentativo);
//   (d) le mod buone sono `Enabled`, i loro hook installati e attribuiti, i
//       loro byte bersaglio patchati (≠ originali) e invariati dal fallimento
//       isolato (le restanti proseguono).
//
// Header del loader in loader/lifecycle/ (include relativo alla radice loader/);
// la logica del cablaggio è in mod_loader.cpp (compilata in pulse::loader).
// Integrazione RapidCheck+GoogleTest in extras/gtest.

#include "lifecycle/mod_loader.hpp"

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

// Un singolo PULSE_HOOK della mod: simbolo univoco + indirizzo bersaglio
// stabile, non nullo e distinto (tutti risolvibili in questo modello).
struct HookSpec {
    std::string symbol;
    std::uintptr_t address{0};
};

// Una mod del modello: Mod_Id + entry point + immagine + i suoi PULSE_HOOK.
struct ModSpec {
    ModId modId;
    std::string entrySymbol;
    Bytes image;
    std::vector<HookSpec> hooks;
};

// --- Property 18 — install parziale fallita → rollback byte-esatto + isolamento
// Feature: external-mod-loading, Property 18.
// Validates: Requirements 9.5.
RC_GTEST_PROP(EmlProperty18PartialInstallRollback,
              PartialInstallFailureRollsBackByteExactConfinedOthersProceed,
              ()) {
    resetRegistry();

    // --- generazione delle mod (buone + 1 bersaglio) ------------------------
    const int numGood = *rc::gen::inRange(0, 4);  // 0..3 mod buone
    std::vector<ModSpec> mods;
    std::uintptr_t nextAddress = 0x4000;  // indirizzi bersaglio globalmente unici

    auto makeMod = [&](const std::string& id, int numHooks) {
        ModSpec mod;
        mod.modId = id;
        mod.entrySymbol = id + "_init";
        mod.image = Bytes{static_cast<std::uint8_t>(0x10 + mods.size())};
        for (int h = 0; h < numHooks; ++h) {
            HookSpec hook;
            hook.symbol = id + "::sym" + std::to_string(h);
            hook.address = nextAddress;
            nextAddress += 0x100;  // separazione → nessuna collisione di regione
            mod.hooks.push_back(std::move(hook));
        }
        return mod;
    };

    for (int g = 0; g < numGood; ++g) {
        const int numHooks = *rc::gen::inRange(1, 4);  // 1..3 PULSE_HOOK
        mods.push_back(makeMod("mod.good" + std::to_string(g), numHooks));
    }

    // Mod bersaglio: N hook (2..4) così esiste un indice k ≥ 1 valido.
    const ModId targetId = "mod.target";
    const int targetHooks = *rc::gen::inRange(2, 5);  // 2..4 hook
    mods.push_back(makeMod(targetId, targetHooks));
    const ModSpec& targetMod = mods.back();

    // Punto di fallimento iniettato: indice k in [1, N-1] → almeno 1 hook della
    // mod è installato PRIMA del fallimento (precondizione della Property 18).
    const int failIndex = *rc::gen::inRange(1, targetHooks);
    const std::uintptr_t failAddress = targetMod.hooks[static_cast<std::size_t>(failIndex)].address;

    // --- banco di prova host-testabile --------------------------------------
    FakeModuleLoader moduleLoader;
    FakeBackend backend;
    HookOwnershipLedger ledger;
    RollbackStore rollback{std::filesystem::path(::testing::TempDir()) /
                           ("pulse_eml_p18_" +
                            std::to_string(reinterpret_cast<std::uintptr_t>(&backend)) +
                            ".rbk")};

    // simbolo (PULSE_HOOK) → indirizzo risolto.
    std::unordered_map<std::string, std::uintptr_t> resolvedAddr;
    // modId → simboli registrati dal Mod_Module all'enable (PULSE_HOOK).
    std::unordered_map<ModId, std::vector<std::string>> hooksByMod;
    // byte originali per indirizzo (oracolo del rollback byte-esatto).
    std::unordered_map<std::uintptr_t, FakeBackend::Bytes> originalBytes;

    for (const ModSpec& mod : mods) {
        // Entry point SEMPRE risolvibile: la proprietà riguarda l'install della
        // finestra, non la risoluzione dell'entry point.
        FakeModuleLoader::ModuleSpec spec;
        spec.exports.push_back({mod.entrySymbol, {0x90, 0x90}});
        moduleLoader.program(mod.image, spec);

        for (const HookSpec& hook : mod.hooks) {
            hooksByMod[mod.modId].push_back(hook.symbol);
            resolvedAddr[hook.symbol] = hook.address;

            // Semina byte originali casuali (oracolo del rollback byte-esatto).
            FakeBackend::Bytes original(kPrologue);
            for (std::size_t i = 0; i < kPrologue; ++i)
                original[i] = static_cast<FakeBackend::Byte>(
                    *rc::gen::inRange<int>(0, 256));
            backend.seedOriginal(hook.address, original);
            originalBytes[hook.address] = std::move(original);
        }
    }

    // Inietta il fallimento di install sul k-esimo hook della mod bersaglio.
    backend.failInstallAt(failAddress, pulse::hooking::HookErrorCode::BackendFailure,
                          "fake: install parziale forzato a fallire (P18)");

    // Resolver dei simboli: tutti gli indirizzi sono risolvibili.
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
    std::unordered_map<ModId, bool> registeredOnce;

    // Invocatore dell'entry point: registra i PULSE_HOOK della mod nel registro
    // globale una sola volta (static-init al dlopen). L'esito è sempre success:
    // il fallimento riguarda l'INSTALL della finestra, non l'entry point.
    ModManagerWiring::EntryPointInvoker invoker =
        [&](const ModId& modId, void* entry) -> EntryPointOutcome {
        (void)entry;
        if (!registeredOnce[modId]) {
            registeredOnce[modId] = true;
            for (const std::string& sym : hooksByMod[modId]) {
                detourStorage.push_back(0);
                trampSlots.push_back(nullptr);
                void* detour = static_cast<void*>(&detourStorage.back());
                void** tramp = &trampSlots.back();
                pulse::hooks::register_hook(sym, detour, tramp);
            }
        }
        return EntryPointOutcome::success();
    };

    ModManagerWiring wiring(moduleLoader, backend, rollback, ledger, makeCtx(),
                            resolver, invoker, /*sink=*/{});
    ModManager manager;
    for (const ModSpec& mod : mods) {
        wiring.registerMod(manager, ModWiringSpec{mod.modId, mod.image, mod.entrySymbol});
    }

    // --- orchestrazione (posizione del bersaglio casuale) -------------------
    // Costruisce l'ordine di abilitazione inserendo la mod bersaglio a una
    // posizione casuale tra le mod buone, così il fallimento è esercitato in
    // qualunque posizione relativa. runNoThrow garantisce la barriera no-throw
    // per-mod: nessuna eccezione si propaga.
    const int targetPos = *rc::gen::inRange(0, numGood + 1);  // [0, numGood]
    std::vector<ModId> order;
    int placed = 0;
    for (int g = 0; g < numGood; ++g) {
        if (placed == targetPos) order.push_back(targetId);
        order.push_back(mods[static_cast<std::size_t>(g)].modId);
        ++placed;
    }
    if (placed == targetPos) order.push_back(targetId);
    wiring.runNoThrow(manager, order);

    // --- (a) la mod bersaglio è Disabled (fallimento confinato al Mod_Id) ---
    RC_ASSERT(manager.stateOf(targetId) == ModState::Disabled);

    // --- (b) zero hook della mod bersaglio; nessun suo indirizzo installato --
    RC_ASSERT(ledger.hooksOf(targetId).empty());
    for (const HookSpec& hook : targetMod.hooks) {
        RC_ASSERT(!backend.isInstalled(hook.address));

        // --- (c) ROLLBACK BYTE-ESATTO: i byte live tornano agli originali ----
        const auto live = backend.liveBytes(hook.address);
        RC_ASSERT(live.has_value());
        RC_ASSERT(*live == originalBytes[hook.address]);
    }

    // --- (d) le mod buone proseguono Enabled, hook installati e invariati ----
    for (std::size_t i = 0; i + 1 < mods.size(); ++i) {  // tutte tranne il bersaglio
        const ModSpec& good = mods[i];
        RC_ASSERT(manager.stateOf(good.modId) == ModState::Enabled);

        const std::vector<OwnedHook> owned = ledger.hooksOf(good.modId);
        RC_ASSERT(owned.size() == good.hooks.size());
        for (const HookSpec& hook : good.hooks) {
            // hook installato e attribuito alla mod buona.
            RC_ASSERT(backend.isInstalled(hook.address));
            // byte bersaglio patchati (≠ originali): l'install ha effetto reale.
            const auto live = backend.liveBytes(hook.address);
            RC_ASSERT(live.has_value());
            RC_ASSERT(*live != originalBytes[hook.address]);
        }
    }
}

}  // namespace
