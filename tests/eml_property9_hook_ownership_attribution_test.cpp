// tests/eml_property9_hook_ownership_attribution_test.cpp
// Feature: external-mod-loading, Property 9 — Attribuzione degli hook al Mod_Id
// proprietario.
// Validates: Requirements 5.8 (Requisiti 5.8)
//
// Property 9 (design.md §"Property 9"): per OGNI sequenza di caricamenti di mod,
// ogni hook effettivamente installato è attribuito ESATTAMENTE al Mod_Id la cui
// finestra di epoca lo ha registrato, e `hooksOf(modId)` restituisce esattamente
// l'insieme degli hook installati di proprietà di quel Mod_Id (nessuna fuga di
// proprietà tra owner diversi).
//
// Modello (come da §6 del design + pattern di tests/hook_ownership_test.cpp):
// i `PULSE_HOOK` di un Mod_Module eseguono al `dlopen` (static init), quindi
// TUTTE le registrazioni di una mod cadono in un intervallo contiguo del
// registro globale `pulse::hooks::registry()` delimitato da `count()` prima
// (`openEpoch`) e dopo (`closeEpoch`) il caricamento. I caricamenti sono
// serializzati sul thread di early-load, quindi le finestre non si sovrappongono.
//
// Strategia (RapidCheck, ≥100 iterazioni — forzate via RC_GTEST_PROP default 100):
//   * si genera una sequenza di mod con Mod_Id DISTINTI (ogni finestra di epoca
//     è registrata da un solo proprietario), ciascuna con un numero casuale di
//     hook in [0, 5];
//   * per ogni mod si simula: openEpoch() → registrazione dei suoi hook nel
//     registro globale → closeEpoch(owner, start) → attribuzione di un OwnedHook
//     per ciascun indice della finestra;
//   * l'oracolo è una mappa owner → lista di OwnedHook attesi costruita
//     indipendentemente dal ledger;
//   * si verifica: (a) ogni indice del registro è attribuito al proprietario
//     la cui finestra lo ha registrato; (b) hooksOf(owner) == hook attesi di
//     quell'owner; (c) nessuna fuga tra owner; (d) allInstalled() == unione.
//
// Header del loader in loader/lifecycle/ (include relativo alla radice loader/)
// e header pubblico dello SDK <pulse/hooks.hpp>; la logica è in
// hook_ownership.cpp (compilata in pulse::loader via glob lifecycle/*.cpp).

#include "lifecycle/hook_ownership.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <pulse/hooks.hpp>

namespace {

using pulse::lifecycle::HookOwnershipLedger;
using pulse::lifecycle::ModId;
using pulse::lifecycle::OwnedHook;

// Azzera il registro globale dello SDK: è un singleton di processo condiviso fra
// le unità di traduzione, quindi ogni iterazione deve partire da uno stato noto.
void resetRegistry() { pulse::hooks::registry().clear(); }

// Descrizione di una mod nella sequenza generata.
struct ModSpec {
    std::string modId;
    int hookCount{0};  // numero di hook registrati nella sua finestra di epoca
};

// Genera una sequenza di mod con Mod_Id DISTINTI (1..6) e hookCount in [0, 5].
std::vector<ModSpec> genMods() {
    const int n = *rc::gen::inRange(0, 7);
    std::vector<ModSpec> mods;
    std::set<std::string> used;
    for (int i = 0; i < n; ++i) {
        // Mod_Id distinto: deriva da un indice univoco per garantire unicità
        // (ogni finestra di epoca ha esattamente un proprietario).
        std::string id = "mod." + std::to_string(i);
        used.insert(id);
        mods.push_back(ModSpec{std::move(id), *rc::gen::inRange(0, 6)});
    }
    return mods;
}

// --- Property 9 — attribuzione degli hook al Mod_Id proprietario -----------
// Feature: external-mod-loading, Property 9.
// Validates: Requirements 5.8.
RC_GTEST_PROP(EmlProperty9HookOwnershipAttribution,
              EachInstalledHookAttributedToOwningEpochWindow,
              ()) {
    resetRegistry();
    HookOwnershipLedger ledger;

    const std::vector<ModSpec> mods = genMods();

    // Oracolo costruito indipendentemente dal ledger:
    //   * expectedHooksByOwner[modId] = OwnedHook attesi (in ordine).
    //   * expectedOwnerByIndex[index] = proprietario della registrazione.
    std::map<std::string, std::vector<OwnedHook>> expectedHooksByOwner;
    std::map<std::size_t, std::string> expectedOwnerByIndex;
    // Ordine globale di attribuzione atteso (per allInstalled()).
    std::vector<OwnedHook> expectedAll;

    for (const ModSpec& mod : mods) {
        // (1) Apertura finestra: snapshot del registro PRIMA del "dlopen".
        const std::size_t start = ledger.openEpoch();
        RC_ASSERT(start == pulse::hooks::count());

        // (2) I PULSE_HOOK della mod si registrano (qui simulati): la finestra
        //     è [start, start + hookCount).
        for (int h = 0; h < mod.hookCount; ++h) {
            pulse::hooks::register_hook(mod.modId + "::sym" + std::to_string(h),
                                        /*detour=*/nullptr,
                                        /*trampoline=*/nullptr);
        }

        // (3) Chiusura finestra: gli indici [start, count()) appartengono alla mod.
        const std::vector<std::size_t> window = ledger.closeEpoch(mod.modId, start);

        // L'oracolo: la finestra è esattamente [start, start + hookCount).
        std::vector<std::size_t> expectedWindow;
        for (int h = 0; h < mod.hookCount; ++h)
            expectedWindow.push_back(start + static_cast<std::size_t>(h));
        RC_ASSERT(window == expectedWindow);

        // (4) Attribuzione: per ogni indice della finestra installa un OwnedHook.
        for (std::size_t idx : window) {
            OwnedHook hook{mod.modId,
                           mod.modId + "::sym" + std::to_string(idx - start),
                           static_cast<std::uintptr_t>(0x1000 + idx * 0x10),
                           idx};
            ledger.attribute(hook);
            expectedHooksByOwner[mod.modId].push_back(hook);
            expectedOwnerByIndex[idx] = mod.modId;
            expectedAll.push_back(hook);
        }
    }

    // --- (a) Ogni indice del registro è attribuito al proprietario la cui
    //         finestra di epoca lo ha registrato (Req 5.8). -----------------
    const std::size_t total = pulse::hooks::count();
    for (std::size_t idx = 0; idx < total; ++idx) {
        const std::string expectedOwner = expectedOwnerByIndex.count(idx)
                                              ? expectedOwnerByIndex[idx]
                                              : std::string{};
        RC_ASSERT(ledger.ownerOfIndex(idx) == ModId(expectedOwner));
    }

    // --- (b) hooksOf(modId) restituisce ESATTAMENTE gli hook installati di
    //         quel Mod_Id, in ordine di attribuzione (Req 5.8). -------------
    for (const auto& [owner, expectedHooks] : expectedHooksByOwner) {
        const std::vector<OwnedHook> got = ledger.hooksOf(ModId(owner));
        RC_ASSERT(got == expectedHooks);
        // Ogni hook restituito appartiene davvero a quell'owner (no fuga).
        for (const OwnedHook& h : got) RC_ASSERT(h.owner == ModId(owner));
    }

    // --- (c) Nessuna fuga di proprietà tra owner: la somma degli hook per
    //         owner == numero totale di hook installati, e owner distinti non
    //         condividono alcun hook. ----------------------------------------
    std::size_t sumPerOwner = 0;
    std::set<std::size_t> seenIndices;
    for (const auto& [owner, expectedHooks] : expectedHooksByOwner) {
        const std::vector<OwnedHook> got = ledger.hooksOf(ModId(owner));
        sumPerOwner += got.size();
        for (const OwnedHook& h : got)
            RC_ASSERT(seenIndices.insert(h.registryIndex).second);  // indice unico
    }
    RC_ASSERT(sumPerOwner == ledger.installedCount());

    // --- (d) allInstalled() è esattamente l'unione (ordine di attribuzione). -
    RC_ASSERT(ledger.allInstalled() == expectedAll);

    // Un Mod_Id mai caricato non possiede alcun hook.
    RC_ASSERT(ledger.hooksOf(ModId("mod.absent")).empty());
}

}  // namespace
