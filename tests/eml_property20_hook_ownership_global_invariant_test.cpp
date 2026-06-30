// tests/eml_property20_hook_ownership_global_invariant_test.cpp
// Feature: external-mod-loading, Property 20 — Invariante globale di proprietà
// degli hook.
// Validates: Requirements 7.3, 9.6 (Requisiti 7.3, 9.6)
//
// Property 20 (design.md): per ogni sequenza di operazioni di
// enable/disable/teardown, IN OGNI ISTANTE l'insieme degli hook installati è
// esattamente l'UNIONE degli hook di proprietà delle mod nello stato Enabled,
// senza alcun hook installato privo di una mod proprietaria Enabled.
//
// Modello (host-testable, niente OS loader): il `HookOwnershipLedger` è la sola
// fonte di verità sugli hook INSTALLATI. Si modella un insieme finito di mod,
// ciascuna con un insieme fisso di hook (simbolo/target/indice); l'abilitazione
// di una mod ATTRIBUISCE i suoi hook al ledger (`attribute`), la disabilitazione
// li RILASCIA (`release`). Un oracolo indipendente tiene traccia dell'insieme
// delle mod Enabled e quindi dell'unione attesa dei loro hook. Dopo OGNI
// operazione si verifica l'invariante globale:
//   (a) allInstalled() == unione degli hook delle sole mod Enabled (come set);
//   (b) nessun hook installato ha un owner NON Enabled;
//   (c) hooksOf(m) == hook di m sse m è Enabled, altrimenti vuoto.
//
// Strategia (RapidCheck, ≥100 iterazioni — qui forzate a ≥100): si genera un
// piccolo universo di mod e una sequenza randomizzata di operazioni
// (enable/disable/teardown) che esercita anche le no-op (enable di una mod già
// attiva, disable di una inattiva) e ripetuti round-trip.
//
// Header del loader in loader/lifecycle/ (include relativo alla radice loader/)
// e header pubblico dello SDK <pulse/hooks.hpp>; la logica è in
// hook_ownership.cpp (compilata in pulse::loader via glob lifecycle/*.cpp).

#include "lifecycle/hook_ownership.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <pulse/hooks.hpp>

namespace {

using pulse::lifecycle::HookOwnershipLedger;
using pulse::lifecycle::ModId;
using pulse::lifecycle::OwnedHook;

// Ordinamento totale su OwnedHook per confronto set-based (allInstalled() e
// hooksOf() restituiscono in ordine di attribuzione, non rilevante qui).
bool ownedLess(const OwnedHook& a, const OwnedHook& b) {
    return std::tie(a.owner, a.symbol, a.target, a.registryIndex) <
           std::tie(b.owner, b.symbol, b.target, b.registryIndex);
}

std::vector<OwnedHook> sorted(std::vector<OwnedHook> v) {
    std::sort(v.begin(), v.end(), ownedLess);
    return v;
}

// Una mod del modello: Mod_Id + il suo insieme FISSO di hook attribuibili.
struct ModelMod {
    ModId id;
    std::vector<OwnedHook> hooks;  // insieme fisso, attribuito all'enable
};

// Costruisce `numMods` mod con Mod_Id distinti, ciascuna con 0..3 hook dai
// simboli/target/indici globalmente univoci (così l'owner identifica l'hook).
std::vector<ModelMod> makeUniverse(int numMods) {
    std::vector<ModelMod> universe;
    std::size_t globalIndex = 0;
    for (int m = 0; m < numMods; ++m) {
        ModelMod mod;
        mod.id = ModId("mod." + std::to_string(m));
        const int numHooks = *rc::gen::inRange(0, 4);  // 0..3
        for (int h = 0; h < numHooks; ++h) {
            OwnedHook hook;
            hook.owner = mod.id;
            hook.symbol = mod.id + "::sym" + std::to_string(h);
            hook.target = 0x1000 + globalIndex * 0x10;
            hook.registryIndex = globalIndex;
            ++globalIndex;
            mod.hooks.push_back(std::move(hook));
        }
        universe.push_back(std::move(mod));
    }
    return universe;
}

// Verifica l'invariante globale (a)(b)(c) contro l'oracolo `enabled`.
void checkInvariant(const HookOwnershipLedger& ledger,
                    const std::vector<ModelMod>& universe,
                    const std::set<int>& enabled) {
    // (a) Unione attesa degli hook delle SOLE mod Enabled.
    std::vector<OwnedHook> expectedUnion;
    for (int idx : enabled) {
        const std::vector<OwnedHook>& hs = universe[static_cast<std::size_t>(idx)].hooks;
        expectedUnion.insert(expectedUnion.end(), hs.begin(), hs.end());
    }
    RC_ASSERT(sorted(ledger.allInstalled()) == sorted(expectedUnion));

    // (b) Nessun hook installato privo di una mod proprietaria Enabled.
    std::set<ModId> enabledIds;
    for (int idx : enabled)
        enabledIds.insert(universe[static_cast<std::size_t>(idx)].id);
    for (const OwnedHook& hook : ledger.allInstalled())
        RC_ASSERT(enabledIds.count(hook.owner) == 1u);

    // (c) hooksOf(m) coerente con lo stato di m.
    for (std::size_t i = 0; i < universe.size(); ++i) {
        const ModelMod& mod = universe[i];
        const std::vector<OwnedHook> got = ledger.hooksOf(mod.id);
        if (enabled.count(static_cast<int>(i))) {
            RC_ASSERT(sorted(got) == sorted(mod.hooks));
        } else {
            RC_ASSERT(got.empty());
        }
    }
}

// --- Property 20 — invariante globale di proprietà degli hook --------------
// Feature: external-mod-loading, Property 20.
// Validates: Requirements 7.3, 9.6.
RC_GTEST_PROP(EmlProperty20HookOwnershipGlobalInvariant,
              InstalledHooksAreExactlyTheUnionOfEnabledMods,
              ()) {
    // Registro globale pulito: il ledger non lo usa in questo modello (operiamo
    // solo su attribute/release), ma lo azzeriamo per igiene tra le run.
    pulse::hooks::registry().clear();

    const int numMods = *rc::gen::inRange(1, 6);  // 1..5 mod
    const std::vector<ModelMod> universe = makeUniverse(numMods);

    HookOwnershipLedger ledger;
    std::set<int> enabled;  // oracolo: indici delle mod attualmente Enabled

    // Stato iniziale: nessuna mod abilitata → nessun hook installato.
    checkInvariant(ledger, universe, enabled);

    // Sequenza randomizzata di operazioni. 0=enable, 1=disable, 2=teardown.
    const int numOps = *rc::gen::inRange(1, 40);
    for (int op = 0; op < numOps; ++op) {
        const int action = *rc::gen::weightedElement<int>(
            {{6, 0}, {6, 1}, {1, 2}});

        if (action == 2) {
            // Teardown: rilascia tutte le mod Enabled (in un ordine qualsiasi);
            // l'insieme installato deve azzerarsi.
            for (int idx : enabled)
                ledger.release(universe[static_cast<std::size_t>(idx)].id);
            enabled.clear();
        } else {
            const int idx = *rc::gen::inRange(0, numMods);
            const ModelMod& mod = universe[static_cast<std::size_t>(idx)];
            if (action == 0) {
                // Enable: se già attiva è una no-op coerente (re-enable: rilascia
                // e riattribuisce gli stessi hook, l'insieme resta identico).
                if (enabled.count(idx)) {
                    ledger.release(mod.id);
                }
                for (const OwnedHook& hook : mod.hooks)
                    ledger.attribute(hook);
                enabled.insert(idx);
            } else {
                // Disable: rilascia gli hook della mod (no-op se non attiva).
                ledger.release(mod.id);
                enabled.erase(idx);
            }
        }

        // Invariante globale verificato dopo OGNI operazione (in ogni istante).
        checkInvariant(ledger, universe, enabled);
    }

    // Teardown finale esplicito: dopo il rilascio di tutte le mod nessun hook
    // resta installato (nessun hook privo di mod proprietaria Enabled).
    for (int idx : enabled)
        ledger.release(universe[static_cast<std::size_t>(idx)].id);
    enabled.clear();
    checkInvariant(ledger, universe, enabled);
    RC_ASSERT(ledger.allInstalled().empty());
}

}  // namespace
