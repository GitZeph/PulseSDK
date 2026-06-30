// tests/eml_property17_disallowed_transition_invariance_test.cpp
// Feature: external-mod-loading, Property 17 — Invarianza su transizioni non
// ammesse.
// Validates: Requirements 7.4 (Requisito 7.4)
//
// Property 17 (design.md §"Property 17"): per OGNI mod e per OGNI transizione di
// stato NON ammessa dalla state machine del `ModManager`, la transizione è
// rifiutata lasciando INVARIATI (a) lo stato corrente della mod e (b) l'insieme
// degli hook installati di proprietà della mod, e produce una diagnostica che
// identifica il Mod_Id e la transizione rifiutata (stato corrente -> stato
// richiesto).
//
// La differenza chiave rispetto alla Property 12 (validità delle transizioni in
// isolamento, pulse-sdk) è l'INVARIANZA DEGLI HOOK: il rifiuto di una
// transizione non ammessa non deve aggiungere né rimuovere alcun hook
// attribuito alla mod. L'unico stato in cui una mod possiede hook installati è
// `Enabled`; il test guida quindi deterministicamente la mod nei quattro stati
// raggiungibili — incluso `Enabled` con i suoi hook realmente installati e
// attribuiti — e, per OGNI bersaglio non ammesso dallo stato corrente, verifica
// che lo stato e gli hook restino bit-per-bit invariati e che venga registrata
// una segnalazione.
//
// Modello (host-testable, niente OS loader — coerente con i seam del design):
//   * `FakeModuleLoader` modella il registro di simboli/byte del Mod_Module
//     (entry point sempre risolvibile);
//   * `FakeBackend` modella i byte delle funzioni bersaglio (install/remove
//     byte-esatti); gli indirizzi bersaglio sono seminati con byte originali
//     casuali, così il confronto byte-per-byte è significativo;
//   * l'invocatore dell'entry point registra i `PULSE_HOOK` della mod nel
//     registro globale `pulse::hooks` (cadono nella finestra di epoca del mod)
//     UNA sola volta (static-init al dlopen), così l'enable installa hook reali.
//
// Strategia (RapidCheck, ≥100 iterazioni — default RC_GTEST_PROP): si genera una
// mod con 1..3 `PULSE_HOOK` tutti risolvibili e uno stato corrente bersaglio
// scelto a caso fra {Installed, Enabled, Disabled, Removed}. La mod viene
// guidata in quello stato tramite SOLE transizioni ammesse (via il cablaggio
// reale `ModManagerWiring` per enable/disable, così l'Enabled possiede hook
// installati). Si calcola lo SNAPSHOT (stato + insieme degli hook attribuiti +
// byte live degli indirizzi installati) e poi, per OGNI bersaglio `t` con
// `!isAllowed(corrente, t)`, si richiede `manager.transition(mod, t)` e si
// verifica:
//   (1) la transizione è RIFIUTATA (Req 7.4);
//   (2) lo stato resta invariato (== corrente);
//   (3) gli hook attribuiti alla mod restano invariati: stesso insieme di
//       indirizzi, stesso stato di installazione e stessi byte live;
//   (4) è registrata una segnalazione che identifica mod + (from, requested).
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
using pulse::lifecycle::TransitionResult;

// Lunghezza del prologo modellato: coerente con kRollbackPrologueBytes del
// cablaggio (seedOriginal usa questa lunghezza).
constexpr std::size_t kPrologue = pulse::lifecycle::kRollbackPrologueBytes;

// I quattro stati validi del ciclo di vita (Req 4.4).
constexpr ModState kAllStates[] = {
    ModState::Installed,
    ModState::Enabled,
    ModState::Disabled,
    ModState::Removed,
};

// Azzera il registro globale dello SDK: singleton di processo condiviso fra le
// unità di traduzione, quindi ogni iterazione deve partire da uno stato noto.
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

// Snapshot osservabile degli hook di una mod: per ogni indirizzo bersaglio,
// stato di installazione e byte live. Confrontato pre/post transizione rifiutata.
struct HookSnapshot {
    std::vector<std::uintptr_t> ownedTargets;  // ordinato
    std::unordered_map<std::uintptr_t, bool> installed;
    std::unordered_map<std::uintptr_t, FakeBackend::Bytes> liveBytes;

    friend bool operator==(const HookSnapshot&, const HookSnapshot&) = default;
};

HookSnapshot snapshot(const HookOwnershipLedger& ledger, const FakeBackend& backend,
                      const ModId& mod, const std::vector<HookSpec>& hooks) {
    HookSnapshot s;
    for (const OwnedHook& h : ledger.hooksOf(mod)) s.ownedTargets.push_back(h.target);
    std::sort(s.ownedTargets.begin(), s.ownedTargets.end());
    for (const HookSpec& hook : hooks) {
        s.installed[hook.address] = backend.isInstalled(hook.address);
        const auto live = backend.liveBytes(hook.address);
        if (live.has_value()) s.liveBytes[hook.address] = *live;
    }
    return s;
}

// --- Property 17 — invarianza su transizioni non ammesse --------------------
// Feature: external-mod-loading, Property 17.
// Validates: Requirements 7.4.
RC_GTEST_PROP(EmlProperty17DisallowedTransitionInvariance,
              DisallowedTransitionRejectedLeavesStateAndHooksUnchanged,
              ()) {
    resetRegistry();

    // --- generazione della mod e dei suoi PULSE_HOOK (tutti risolvibili) ----
    const ModId modId = "mod.under.test";
    const std::string entrySymbol = "mod_init";
    const Bytes image{0x42};

    const int numHooks = *rc::gen::inRange(1, 4);  // 1..3 PULSE_HOOK
    std::vector<HookSpec> hooks;
    std::uintptr_t nextAddress = 0x4000;
    for (int h = 0; h < numHooks; ++h) {
        HookSpec hook;
        hook.symbol = "Sym::f" + std::to_string(h);
        hook.address = nextAddress;
        nextAddress += 0x100;  // separazione → nessuna collisione di regione
        hooks.push_back(std::move(hook));
    }

    // Stato corrente bersaglio in cui guidare la mod prima del rifiuto.
    const ModState currentState = *rc::gen::element(
        ModState::Installed, ModState::Enabled, ModState::Disabled,
        ModState::Removed);

    // --- banco di prova host-testabile --------------------------------------
    FakeModuleLoader moduleLoader;
    FakeBackend backend;
    HookOwnershipLedger ledger;
    RollbackStore rollback{std::filesystem::path(::testing::TempDir()) /
                           ("pulse_eml_p17_" +
                            std::to_string(reinterpret_cast<std::uintptr_t>(&backend)) +
                            ".rbk")};

    // Entry point sempre risolvibile.
    FakeModuleLoader::ModuleSpec moduleSpec;
    moduleSpec.exports.push_back({entrySymbol, {0x90, 0x90}});
    moduleLoader.program(image, moduleSpec);

    // simbolo (PULSE_HOOK) → indirizzo risolto; semina byte originali casuali.
    std::unordered_map<std::string, std::uintptr_t> resolvedAddr;
    for (const HookSpec& hook : hooks) {
        resolvedAddr[hook.symbol] = hook.address;
        FakeBackend::Bytes original(kPrologue);
        for (std::size_t i = 0; i < kPrologue; ++i)
            original[i] = static_cast<FakeBackend::Byte>(*rc::gen::inRange<int>(0, 256));
        backend.seedOriginal(hook.address, original);
    }

    ModManagerWiring::SymbolResolver resolver =
        [&resolvedAddr](std::string_view symbol) -> void* {
        const auto it = resolvedAddr.find(std::string(symbol));
        if (it == resolvedAddr.end() || it->second == 0) return nullptr;
        return reinterpret_cast<void*>(it->second);
    };

    // Slot stabili per detour/trampolini dei PULSE_HOOK registrati.
    std::deque<int> detourStorage;
    std::deque<void*> trampSlots;
    bool registeredOnce = false;

    // Invocatore: registra i PULSE_HOOK del modulo UNA sola volta (static-init
    // al dlopen) e ha SEMPRE successo (l'enable, quando ammesso, è applicato).
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

    // --- guida la mod nello stato corrente bersaglio via SOLE transizioni ammesse
    switch (currentState) {
        case ModState::Installed:
            // stato iniziale: nessuna azione.
            break;
        case ModState::Enabled:
            RC_ASSERT(wiring.enable(manager, modId).applied());
            break;
        case ModState::Disabled:
            RC_ASSERT(wiring.enable(manager, modId).applied());
            RC_ASSERT(wiring.disable(manager, modId).applied());
            break;
        case ModState::Removed:
            // Installed -> Removed è ammessa; la mod non possiede hook.
            RC_ASSERT(manager.transition(modId, ModState::Removed).applied());
            break;
    }
    RC_ASSERT(manager.stateOf(modId) == currentState);

    // Coerenza del banco: solo Enabled possiede hook installati e attribuiti.
    if (currentState == ModState::Enabled) {
        RC_ASSERT(ledger.hooksOf(modId).size() == hooks.size());
    } else {
        RC_ASSERT(ledger.hooksOf(modId).empty());
    }

    // --- SNAPSHOT pre-transizione (stato già verificato sopra) --------------
    const HookSnapshot before = snapshot(ledger, backend, modId, hooks);
    const std::size_t rejectionsBefore = manager.rejections().size();

    // --- per OGNI bersaglio NON ammesso dallo stato corrente ----------------
    std::size_t expectedRejections = rejectionsBefore;
    for (const ModState target : kAllStates) {
        if (ModManager::isAllowed(currentState, target)) continue;  // ammessa: skip

        const TransitionResult r = manager.transition(modId, target);

        // (1) transizione RIFIUTATA (Req 7.4).
        RC_ASSERT(r.rejected());

        // (2) stato corrente INVARIATO (Req 7.4).
        RC_ASSERT(r.state == currentState);
        RC_ASSERT(manager.stateOf(modId) == currentState);

        // (3) hook della mod INVARIATI: stesso insieme attribuito, stesso stato
        //     di installazione e stessi byte live (Req 7.4).
        const HookSnapshot after = snapshot(ledger, backend, modId, hooks);
        RC_ASSERT(after == before);

        // (4) segnalazione registrata che identifica mod + (from, requested).
        RC_ASSERT(r.rejection.has_value());
        RC_ASSERT(r.rejection->mod == modId);
        RC_ASSERT(r.rejection->from == currentState);
        RC_ASSERT(r.rejection->requested == target);

        ++expectedRejections;
        RC_ASSERT(manager.rejections().size() == expectedRejections);
        RC_ASSERT(manager.rejections().back().mod == modId);
        RC_ASSERT(manager.rejections().back().from == currentState);
        RC_ASSERT(manager.rejections().back().requested == target);
    }

    // Almeno una transizione non ammessa esiste per ogni stato (sanity).
    RC_ASSERT(expectedRejections > rejectionsBefore);
}

}  // namespace
