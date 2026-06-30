// tests/property38_init_failure_isolation_test.cpp
// Feature: pulse-sdk, Property 38 — Isolamento dei fallimenti di
// inizializzazione delle mod.
// Validates: Requirements 4.7, 28.4, 28.5 (Requisiti 4.7, 28.4, 28.5)
//
// Property 38 (design.md / Req 4.7, 28.4, 28.5): per ogni insieme di mod in cui
// un sottoinsieme fallisce durante l'inizializzazione — restituendo un esito di
// errore (`EntryPointOutcome::failure`) O lanciando un'eccezione — il
// `ModManager::enableAll(loadOrder)` deve ISOLARE ciascun fallimento alla sola
// mod imputata: la mod fallita è portata a `Disabled` con una segnalazione
// (mod + causa, con il flag `threw` corretto) e il caricamento delle ALTRE mod
// PROSEGUE (nessuna mod è saltata). Nessuna eccezione propaga fuori da
// `enableAll` (Req 28.4).
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si genera un insieme randomizzato di mod; a ciascuna è assegnato in modo
//     casuale uno fra tre comportamenti dell'entry point: SUCCEED (ok),
//     RETURN_FAILURE (esito di errore) o THROW (eccezione non gestita);
//   * gli id sono univoci (indice) così l'identità è totale;
//   * si registra ogni mod con l'entry point corrispondente e si esegue
//     `enableAll` su una PERMUTAZIONE casuale degli id (ordine di caricamento
//     randomizzato);
//   * si verifica che:
//       - `result.enabled` (come insieme) == mod che riescono;
//       - `result.failed` (come insieme) == mod che falliscono, ciascuna con
//         flag `threw` corretto (true sse ha lanciato);
//       - ogni mod riuscita ha stato `Enabled`; ogni mod fallita ha stato
//         `Disabled`;
//       - `initFailures().size()` == numero di mod fallite;
//       - tutte le mod sono state visitate (enabled ∪ failed == tutte le mod);
//       - nessuna eccezione propaga fuori da `enableAll`.
//
// La logica del ciclo di vita è in mod_manager.cpp (compilata in pulse::loader
// via glob lifecycle/*.cpp); l'header vive in loader/lifecycle/ (include
// relativo alla radice loader/). Integrazione RapidCheck+GoogleTest in
// extras/gtest.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <cstddef>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "lifecycle/mod_manager.hpp"

namespace {

using pulse::lifecycle::EntryPointOutcome;
using pulse::lifecycle::InitFailure;
using pulse::lifecycle::ModManager;
using pulse::lifecycle::ModManagerModId;
using pulse::lifecycle::ModState;

// Comportamento randomizzato dell'entry point di una mod (Req 4.7):
//   Succeed       -> l'entry point restituisce ok;
//   ReturnFailure -> l'entry point restituisce un esito di errore;
//   Throw         -> l'entry point lancia un'eccezione non gestita (Req 28.4).
enum class Behavior { Succeed, ReturnFailure, Throw };

// Costruisce un id univoco e deterministico per la mod i-esima.
ModManagerModId modIdAt(std::size_t i) {
    return "mod_" + std::to_string(i);
}

// --- Property 38 — isolamento dei fallimenti di inizializzazione ----------
// Feature: pulse-sdk, Property 38. Validates: Requirements 4.7, 28.4, 28.5.
RC_GTEST_PROP(Property38InitFailureIsolation,
              FailuresAreIsolatedAndOthersStillLoad,
              ()) {
    // Insieme randomizzato di comportamenti: ogni elemento è una mod (fino a
    // 64 mod). Almeno una mod per garantire input non degenere non è richiesto:
    // l'insieme vuoto è un caso valido (nessuna mod -> nessun fallimento).
    const auto behaviors = *rc::gen::container<std::vector<Behavior>>(
        rc::gen::element(Behavior::Succeed, Behavior::ReturnFailure, Behavior::Throw))
                               .as("comportamenti delle mod");

    const std::size_t n = behaviors.size();

    ModManager manager;

    // Insiemi attesi (per id) delle mod che riescono e di quelle che falliscono.
    std::set<ModManagerModId> expectedSucceeders;
    std::set<ModManagerModId> expectedFailers;
    std::set<ModManagerModId> expectedThrowers;  // sottoinsieme dei failers

    for (std::size_t i = 0; i < n; ++i) {
        const ModManagerModId id = modIdAt(i);
        const Behavior b = behaviors[i];

        switch (b) {
            case Behavior::Succeed:
                expectedSucceeders.insert(id);
                manager.registerMod(id, [] { return EntryPointOutcome::success(); });
                break;
            case Behavior::ReturnFailure:
                expectedFailers.insert(id);
                manager.registerMod(id, [] {
                    return EntryPointOutcome::failure("init error simulato");
                });
                break;
            case Behavior::Throw:
                expectedFailers.insert(id);
                expectedThrowers.insert(id);
                manager.registerMod(id, []() -> EntryPointOutcome {
                    throw std::runtime_error("eccezione di init simulata");
                });
                break;
        }
    }

    // Ordine di caricamento randomizzato: una permutazione degli id registrati.
    std::vector<ModManagerModId> loadOrder;
    loadOrder.reserve(n);
    for (std::size_t i = 0; i < n; ++i) loadOrder.push_back(modIdAt(i));

    if (n > 1) {
        // Permutazione indotta da chiavi casuali (copre molti ordini, incluso
        // il rovesciato), così l'ordine di abilitazione è randomizzato.
        const auto permKeys =
            *rc::gen::container<std::vector<int>>(n, rc::gen::arbitrary<int>())
                 .as("chiavi di permutazione del load order");
        std::vector<std::size_t> idx(n);
        for (std::size_t i = 0; i < n; ++i) idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(), [&](std::size_t l, std::size_t r) {
            return permKeys[l] < permKeys[r];
        });
        std::vector<ModManagerModId> permuted;
        permuted.reserve(n);
        for (std::size_t i : idx) permuted.push_back(modIdAt(i));
        loadOrder = std::move(permuted);
    }

    // enableAll NON deve propagare alcuna eccezione (Req 28.4): se ne sfugge
    // una, RC_FAIL segnala la violazione invece di far crashare il test.
    pulse::lifecycle::EnableAllResult result;
    try {
        result = manager.enableAll(loadOrder);
    } catch (...) {
        RC_FAIL("enableAll ha propagato un'eccezione: isolamento violato (Req 28.4)");
    }

    // --- enabled set == mod che riescono ---------------------------------
    const std::set<ModManagerModId> enabledSet(result.enabled.begin(),
                                                result.enabled.end());
    // Nessun duplicato: ogni mod è visitata una sola volta.
    RC_ASSERT(enabledSet.size() == result.enabled.size());
    RC_ASSERT(enabledSet == expectedSucceeders);

    // --- failed set == mod che falliscono, con flag `threw` corretto -----
    std::set<ModManagerModId> failedSet;
    for (const InitFailure& f : result.failed) {
        failedSet.insert(f.mod);
        const bool isThrower = expectedThrowers.count(f.mod) > 0;
        // Il flag `threw` distingue eccezione (Req 28.4) da esito di errore.
        RC_ASSERT(f.threw == isThrower);
        // La causa è presente (mod + causa, Req 28.5).
        RC_ASSERT(!f.cause.empty());
    }
    RC_ASSERT(failedSet.size() == result.failed.size());
    RC_ASSERT(failedSet == expectedFailers);

    // --- stati finali: riuscite Enabled, fallite Disabled ----------------
    for (const ModManagerModId& id : expectedSucceeders) {
        RC_ASSERT(manager.stateOf(id) == ModState::Enabled);
    }
    for (const ModManagerModId& id : expectedFailers) {
        RC_ASSERT(manager.stateOf(id) == ModState::Disabled);
    }

    // --- conteggio dei fallimenti accumulati == numero di mod fallite ----
    RC_ASSERT(manager.initFailures().size() == expectedFailers.size());

    // --- tutte le mod visitate: enabled ∪ failed == tutte le mod ---------
    RC_ASSERT(result.enabled.size() + result.failed.size() == n);
}

}  // namespace
