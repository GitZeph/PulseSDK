// tests/eml_property14_init_reinit_failure_rollback_test.cpp
// Feature: external-mod-loading, Property 14 — Fallimento di
// inizializzazione/re-inizializzazione → Disabled con rollback byte-esatto.
// Validates: Requirements 6.2, 7.6 (Requisiti 6.2, 7.6)
//
// Property 14 (design.md §"Property 14"): per OGNI mod il cui entry point
// fallisce — restituendo un esito di errore OPPURE lanciando un'eccezione — sia
// alla prima abilitazione (enable) sia alla ri-abilitazione (re-enable, dopo un
// enable riuscito e un disable), il Mod_Manager porta quella mod allo stato
// `Disabled`, gli eventuali hook installati durante il tentativo sono rimossi
// con ripristino BYTE-ESATTO via Rollback_Store/Hook_Engine (i byte delle
// funzioni bersaglio della mod tornano identici allo stato precedente il
// tentativo) e le ALTRE mod valide proseguono Enabled con i loro hook
// installati e invariati.
//
// Modello (host-testable, niente OS loader — coerente con la nota del design
// "FakeBackend che modella i byte delle funzioni bersaglio"):
//   * `FakeModuleLoader` modella il registro di simboli/byte del Mod_Module
//     (entry point sempre risolvibile: la proprietà riguarda l'esito dell'entry
//     point, non la sua risoluzione);
//   * `FakeBackend` modella i byte delle funzioni bersaglio
//     (readOriginal/install/remove byte-esatti). Tutti gli indirizzi bersaglio
//     sono seminati con byte originali generati casualmente, così il confronto
//     byte-per-byte pre/post tentativo è significativo;
//   * l'invocatore dell'entry point registra nel registro globale
//     `pulse::hooks` i `PULSE_HOOK` della mod (cadono nella finestra di epoca
//     del mod) SOLO quando l'invocazione ha successo; quando deve fallire
//     restituisce errore o lancia, senza registrare nulla (modella un entry
//     point che fallisce);
//   * un contatore per-mod distingue la prima invocazione (enable) dalla
//     seconda (re-enable) per iniettare il fallimento nella fase scelta.
//
// Strategia (RapidCheck, ≥100 iterazioni — default RC_GTEST_PROP): si generano
// 0..3 mod "buone" (entry point sempre OK) e 1 mod "bersaglio" che fallisce.
// Si scelgono casualmente: la FASE del fallimento (enable della prima volta vs
// re-enable dopo enable+disable) e il MODO del fallimento (errore vs eccezione).
// Ogni mod ha 1..3 `PULSE_HOOK` con indirizzi bersaglio DISTINTI e tutti
// risolvibili. L'oracolo (byte originali per indirizzo, insieme degli indirizzi
// per mod) è costruito INDIPENDENTEMENTE dal cablaggio. Dopo l'orchestrazione si
// verifica:
//   (a) la mod bersaglio è `Disabled` (Req 6.2, 7.6);
//   (b) zero hook attribuiti alla mod bersaglio; nessuno dei suoi indirizzi è
//       installato nel backend;
//   (c) ROLLBACK BYTE-ESATTO: i byte "live" di OGNI indirizzo bersaglio della
//       mod fallita sono identici ai byte originali (stato pre-tentativo);
//   (d) le mod buone sono `Enabled`, i loro hook installati e attribuiti, i
//       loro byte bersaglio patchati (≠ originali) e invariati dal fallimento
//       isolato (Req 6.2 "proseguire con le mod restanti").
//
// Header del loader in loader/lifecycle/ (include relativo alla radice loader/);
// la logica del cablaggio è in mod_loader.cpp (compilata in pulse::loader).
// Integrazione RapidCheck+GoogleTest in extras/gtest.

#include "lifecycle/mod_loader.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <stdexcept>
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

// --- Property 14 — fallimento di init/re-init → Disabled + rollback byte-esatto
// Feature: external-mod-loading, Property 14.
// Validates: Requirements 6.2, 7.6.
RC_GTEST_PROP(EmlProperty14InitReinitFailureRollback,
              FailedEnableOrReenableDisablesWithByteExactRollbackOthersProceed,
              ()) {
    resetRegistry();

    // --- scelta della fase e del modo di fallimento -------------------------
    // Fase: false = fallimento alla prima abilitazione (enable);
    //        true  = fallimento alla ri-abilitazione (re-enable dopo enable+disable, Req 7.6).
    const bool failOnReenable = *rc::gen::arbitrary<bool>();
    // Modo: false = entry point restituisce errore; true = entry point lancia.
    const bool failByThrow = *rc::gen::arbitrary<bool>();

    // --- generazione delle mod (buone + 1 bersaglio) ------------------------
    const int numGood = *rc::gen::inRange(0, 4);  // 0..3 mod buone
    std::vector<ModSpec> mods;
    std::uintptr_t nextAddress = 0x4000;  // indirizzi bersaglio globalmente unici

    auto makeMod = [&](const std::string& id) {
        ModSpec mod;
        mod.modId = id;
        mod.entrySymbol = id + "_init";
        mod.image = Bytes{static_cast<std::uint8_t>(0x10 + mods.size())};
        const int numHooks = *rc::gen::inRange(1, 4);  // 1..3 PULSE_HOOK
        for (int h = 0; h < numHooks; ++h) {
            HookSpec hook;
            hook.symbol = id + "::sym" + std::to_string(h);
            hook.address = nextAddress;
            nextAddress += 0x100;  // separazione → nessuna collisione di regione
            mod.hooks.push_back(std::move(hook));
        }
        return mod;
    };

    for (int g = 0; g < numGood; ++g) mods.push_back(makeMod("mod.good" + std::to_string(g)));
    const ModId targetId = "mod.target";
    mods.push_back(makeMod(targetId));

    // --- banco di prova host-testabile --------------------------------------
    FakeModuleLoader moduleLoader;
    FakeBackend backend;
    HookOwnershipLedger ledger;
    RollbackStore rollback{std::filesystem::path(::testing::TempDir()) /
                           ("pulse_eml_p14_" +
                            std::to_string(reinterpret_cast<std::uintptr_t>(&backend)) +
                            ".rbk")};

    // simbolo (PULSE_HOOK) → indirizzo risolto.
    std::unordered_map<std::string, std::uintptr_t> resolvedAddr;
    // modId → simboli registrati dal Mod_Module all'enable (PULSE_HOOK).
    std::unordered_map<ModId, std::vector<std::string>> hooksByMod;
    // byte originali per indirizzo (oracolo del rollback byte-esatto).
    std::unordered_map<std::uintptr_t, FakeBackend::Bytes> originalBytes;

    for (const ModSpec& mod : mods) {
        // Entry point SEMPRE risolvibile: la proprietà riguarda l'ESITO
        // dell'entry point, non la sua risoluzione.
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

    // Contatore di invocazioni dell'entry point per mod (distingue
    // enable=1ª invocazione da re-enable=2ª invocazione).
    std::unordered_map<ModId, int> invokeCount;

    // Invocatore dell'entry point: registra i PULSE_HOOK della mod SOLO quando
    // l'invocazione ha successo. Per la mod bersaglio inietta il fallimento
    // (errore o eccezione) nella fase scelta.
    ModManagerWiring::EntryPointInvoker invoker =
        [&](const ModId& modId, void* entry) -> EntryPointOutcome {
        (void)entry;
        const int n = ++invokeCount[modId];

        const bool isTarget = (modId == targetId);
        const int failAt = failOnReenable ? 2 : 1;  // 2ª invocazione = re-enable
        if (isTarget && n == failAt) {
            // Entry point che fallisce: NON registra alcun PULSE_HOOK.
            if (failByThrow) {
                throw std::runtime_error("entry point: eccezione iniettata per '" +
                                         modId + "'");
            }
            return EntryPointOutcome::failure("entry point fallito (test)");
        }

        // Invocazione riuscita: registra i PULSE_HOOK del modulo nel registro
        // globale (cadono nella finestra di epoca del mod).
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
                            resolver, invoker, /*sink=*/{});
    ModManager manager;
    std::vector<ModId> order;
    for (const ModSpec& mod : mods) {
        wiring.registerMod(manager, ModWiringSpec{mod.modId, mod.image, mod.entrySymbol});
        order.push_back(mod.modId);
    }

    // --- orchestrazione ------------------------------------------------------
    // runNoThrow garantisce la barriera no-throw per-mod (Req 6.2/6.3/6.4): per
    // il modo "eccezione" è essenziale che nessuna eccezione si propaghi.
    wiring.runNoThrow(manager, order);

    if (failOnReenable) {
        // Scenario re-enable (Req 7.6): la mod bersaglio si è abilitata con
        // successo (1ª invocazione), ora la disabilitiamo (rollback byte-esatto
        // dei suoi hook) e la ri-abilitiamo: la 2ª invocazione fallisce.
        RC_ASSERT(manager.stateOf(targetId) == ModState::Enabled);
        manager.disable(targetId);
        RC_ASSERT(manager.stateOf(targetId) == ModState::Disabled);
        // Re-enable: l'entry point (2ª invocazione) fallisce → torna Disabled.
        manager.enable(targetId);
    }

    // --- (a) la mod bersaglio è Disabled (Req 6.2, 7.6) ---------------------
    RC_ASSERT(manager.stateOf(targetId) == ModState::Disabled);

    // --- (b) zero hook della mod bersaglio; nessun suo indirizzo installato --
    RC_ASSERT(ledger.hooksOf(targetId).empty());
    const ModSpec& targetMod = mods.back();
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
