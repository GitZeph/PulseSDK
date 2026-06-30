// tests/property13_reverse_order_termination_test.cpp
// Feature: pulse-sdk, Property 13 — Ordine inverso di terminazione.
// Validates: Requirements 4.8 (Requisito 4.8)
//
// Property 13 (design.md / Req 4.8): *Per ogni* ordine di caricamento delle
// mod abilitate, la sequenza di invocazione dei punti di terminazione alla
// chiusura del gioco è esattamente l'INVERSO dell'ordine di caricamento.
//
// Componente sotto test: `pulse::lifecycle::ModManager`
//   (loader/lifecycle/mod_manager.hpp). API pubblica STABILE usata:
//     * registerMod(id, entry, terminator)
//     * enable(id)
//     * shutdown(loadOrder) -> ordine di invocazione dei terminator
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si genera un insieme randomizzato di mod con id UNIVOCI;
//   * si genera un ordine di caricamento randomizzato (permutazione delle mod);
//   * un sottoinsieme casuale di mod viene abilitato;
//   * gli entry point hanno SEMPRE successo (così enable porta deterministicamente
//     allo stato Enabled — indipendente dal task 12.2 che instrada gli entry
//     point falliti verso Disabled);
//   * i terminator registrano il proprio id in un trace condiviso;
//   * si invoca shutdown(loadOrder) e si verifica che:
//       (a) la sequenza di invocazione registrata == loadOrder filtrato alle
//           mod abilitate, poi rovesciato;
//       (b) i terminator delle mod NON abilitate non sono mai invocati;
//       (c) l'ordine restituito da shutdown coincide con il trace registrato.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "lifecycle/mod_manager.hpp"

namespace {

using pulse::lifecycle::EntryPointOutcome;
using pulse::lifecycle::ModManager;
using pulse::lifecycle::ModManagerModId;

// Ordine atteso: loadOrder filtrato alle mod abilitate, poi rovesciato.
std::vector<ModManagerModId> expectedReverseOrder(
    const std::vector<ModManagerModId>& loadOrder,
    const std::unordered_set<ModManagerModId>& enabled) {
    std::vector<ModManagerModId> filtered;
    filtered.reserve(loadOrder.size());
    for (const auto& id : loadOrder) {
        if (enabled.count(id) != 0) filtered.push_back(id);
    }
    std::reverse(filtered.begin(), filtered.end());
    return filtered;
}

// --- Property 13 — ordine inverso di terminazione --------------------------
// Feature: pulse-sdk, Property 13. Validates: Requirements 4.8.
RC_GTEST_PROP(Property13ReverseOrderTermination,
              TerminatorsInvokedInReverseLoadOrderForEnabledMods,
              ()) {
    // Numero di mod: fino a 32 per esercitare catene non banali.
    const std::size_t n =
        *rc::gen::inRange<std::size_t>(0, 33).as("numero di mod");

    // Id univoci e deterministici.
    std::vector<ModManagerModId> ids;
    ids.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        ids.push_back("mod" + std::to_string(i));
    }

    // Sottoinsieme abilitato: per ogni mod un flag casuale.
    std::unordered_set<ModManagerModId> enabledSet;
    std::vector<bool> enableFlags;
    enableFlags.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const bool en = *rc::gen::arbitrary<bool>();
        enableFlags.push_back(en);
        if (en) enabledSet.insert(ids[i]);
    }

    // Ordine di caricamento randomizzato: permutazione delle mod indotta da
    // chiavi casuali (copre molti ordini, incluso identità e rovesciato).
    std::vector<std::size_t> perm(n);
    for (std::size_t i = 0; i < n; ++i) perm[i] = i;
    if (n > 0) {
        const auto keys = *rc::gen::container<std::vector<int>>(
                               n, rc::gen::arbitrary<int>())
                              .as("chiavi di permutazione");
        std::stable_sort(perm.begin(), perm.end(),
                         [&](std::size_t l, std::size_t r) {
                             return keys[l] < keys[r];
                         });
    }
    std::vector<ModManagerModId> loadOrder;
    loadOrder.reserve(n);
    for (std::size_t idx : perm) loadOrder.push_back(ids[idx]);

    // Trace condiviso: ogni terminator vi registra il proprio id.
    auto trace = std::make_shared<std::vector<ModManagerModId>>();

    ModManager manager;
    for (std::size_t i = 0; i < n; ++i) {
        const ModManagerModId id = ids[i];
        // Entry point che ha SEMPRE successo: enable -> Enabled deterministico.
        auto entry = []() { return EntryPointOutcome::success(); };
        auto terminator = [trace, id]() { trace->push_back(id); };
        manager.registerMod(id, entry, terminator);
    }

    // Abilita il sottoinsieme scelto.
    for (std::size_t i = 0; i < n; ++i) {
        if (enableFlags[i]) {
            const auto result = manager.enable(ids[i]);
            // Entry point sempre OK => transizione applicata verso Enabled.
            RC_ASSERT(result.applied());
        }
    }

    // Chiusura del gioco: invoca i terminator in ordine inverso al caricamento.
    const std::vector<ModManagerModId> invoked = manager.shutdown(loadOrder);

    const std::vector<ModManagerModId> expected =
        expectedReverseOrder(loadOrder, enabledSet);

    // (a) L'ordine restituito è loadOrder filtrato alle abilitate, rovesciato.
    RC_ASSERT(invoked == expected);

    // (c) Il trace registrato dai terminator coincide con l'ordine restituito.
    RC_ASSERT(*trace == expected);

    // (b) Nessun terminator di mod NON abilitata è stato invocato.
    for (const auto& id : *trace) {
        RC_ASSERT(enabledSet.count(id) != 0);
    }

    // Ogni mod abilitata presente nel loadOrder è stata terminata esattamente
    // una volta (il trace è una permutazione dell'atteso, già verificata da ==).
    RC_ASSERT(invoked.size() == expected.size());
}

}  // namespace
