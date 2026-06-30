// tests/eml_property21_outcome_partition_test.cpp
// Feature: external-mod-loading, Property 21 — Partizione completa degli esiti
// osservabili.
// Validates: Requirements 10.1, 10.4 (Requisiti 10.1, 10.4)
//
// Property 21 (design.md / Req 10.1, 10.4): per ogni insieme di mod individuate
// in discovery, ogni mod finisce in ESATTAMENTE UNO tra {caricata, esclusa,
// isolata}:
//   * l'unione di {loaded, excluded, isolated} coincide con l'insieme delle mod
//     individuate (COMPLETEZZA — partitionComplete()/missingFrom() vuoto);
//   * i tre esiti sono a due a due DISGIUNTI (nessun Mod_Id in due esiti);
//   * ogni Mod_Id caricato compare UNA SOLA volta in loaded() (Req 10.1).
// Inoltre, la rejection del doppio record: un secondo tentativo di registrare un
// QUALUNQUE esito per un Mod_Id già registrato è RIFIUTATO (ritorna false) e la
// mod resta nel suo PRIMO esito (l'invariante di partizione è preservato).
//
// Modello (host-testable, registro PURO): il DiagnosticLedger non scopre mod né
// installa hook — è alimentato dall'orchestratore. Qui si genera un insieme di
// Mod_Id distinti (le "individuate"), si assegna a ciascuno un esito casuale
// (loaded / excluded(cause) / isolated(cause)) e si verificano gli invarianti
// della partizione. La rejection del doppio record è esercitata ritentando un
// esito diverso su un Mod_Id già registrato.
//
// Header del loader in loader/lifecycle/ (include relativo alla radice loader/);
// la logica del DiagnosticLedger è in mod_loader.cpp (compilata in
// pulse::loader). Integrazione RapidCheck+GoogleTest in extras/gtest.

#include "lifecycle/mod_loader.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

namespace {

using pulse::lifecycle::CauseCategory;
using pulse::lifecycle::DiagnosticEntry;
using pulse::lifecycle::DiagnosticLedger;
using pulse::lifecycle::ModId;
using pulse::lifecycle::ModOutcome;

// Insieme CHIUSO delle cause (Req 10.2): usato per assegnare una causa agli
// esiti Excluded/Isolated. Loaded non ha causa.
constexpr CauseCategory kCauses[] = {
    CauseCategory::InvalidPackage,
    CauseCategory::VersionOrPlatformIncompatible,
    CauseCategory::DependencyUnsatisfied,
    CauseCategory::DependencyCycle,
    CauseCategory::ModuleNotLoadable,
    CauseCategory::EntryPointFailed,
    CauseCategory::SymbolUnresolved,
};
constexpr int kNumCauses = static_cast<int>(sizeof(kCauses) / sizeof(kCauses[0]));

// Esito assegnato a una mod individuata, con la causa per Excluded/Isolated.
struct Assignment {
    ModId modId;
    ModOutcome outcome{ModOutcome::Loaded};
    CauseCategory cause{CauseCategory::InvalidPackage};  // ignorata per Loaded
};

// --- Property 21 — partizione completa {loaded|excluded|isolated} ----------
// Feature: external-mod-loading, Property 21.
// Validates: Requirements 10.1, 10.4.
RC_GTEST_PROP(EmlProperty21OutcomePartition,
              EveryDiscoveredInExactlyOneOutcomeUnionCompleteDisjointLoadedOnce,
              ()) {
    // --- genera un insieme di Mod_Id DISTINTI (le mod individuate) ----------
    const int numMods = *rc::gen::inRange(0, 12);  // 0..11 mod
    std::vector<Assignment> plan;
    plan.reserve(static_cast<std::size_t>(numMods));

    for (int i = 0; i < numMods; ++i) {
        Assignment a;
        a.modId = "mod." + std::to_string(i);  // distinti per costruzione
        const int kind = *rc::gen::inRange(0, 3);  // 0=loaded 1=excluded 2=isolated
        if (kind == 0) {
            a.outcome = ModOutcome::Loaded;
        } else if (kind == 1) {
            a.outcome = ModOutcome::Excluded;
            a.cause = kCauses[*rc::gen::inRange(0, kNumCauses)];
        } else {
            a.outcome = ModOutcome::Isolated;
            a.cause = kCauses[*rc::gen::inRange(0, kNumCauses)];
        }
        plan.push_back(std::move(a));
    }

    // Insieme delle mod individuate (oracolo della completezza).
    std::vector<ModId> discovered;
    discovered.reserve(plan.size());
    for (const Assignment& a : plan) discovered.push_back(a.modId);

    // --- registra ogni esito sul ledger -------------------------------------
    DiagnosticLedger ledger;
    for (const Assignment& a : plan) {
        bool recorded = false;
        switch (a.outcome) {
            case ModOutcome::Loaded:
                recorded = ledger.recordLoaded(a.modId);
                break;
            case ModOutcome::Excluded:
                recorded = ledger.recordExcluded(a.modId, a.cause);
                break;
            case ModOutcome::Isolated:
                recorded = ledger.recordIsolated(a.modId, a.cause);
                break;
        }
        // Prima registrazione di un Mod_Id distinto → sempre accettata.
        RC_ASSERT(recorded);
    }

    // --- (1) COMPLETEZZA: unione == individuate -----------------------------
    RC_ASSERT(ledger.partitionComplete(discovered));
    RC_ASSERT(ledger.missingFrom(discovered).empty());

    const std::vector<ModId> loaded = ledger.loaded();
    const std::vector<DiagnosticEntry> excluded = ledger.excluded();
    const std::vector<DiagnosticEntry> isolated = ledger.isolated();

    // L'unione dei tre esiti ha cardinalità == numero di individuate.
    RC_ASSERT(loaded.size() + excluded.size() + isolated.size() ==
              static_cast<std::size_t>(numMods));

    // --- (2) DISGIUNZIONE a due a due + ogni mod in ESATTAMENTE un esito ----
    std::unordered_set<ModId> seen;
    auto markUnique = [&](const ModId& id) {
        // Ogni Mod_Id compare in al più un esito (a due a due disgiunti).
        RC_ASSERT(seen.insert(id).second);
    };
    for (const ModId& id : loaded) markUnique(id);
    for (const DiagnosticEntry& e : excluded) {
        markUnique(e.modId);
        RC_ASSERT(e.outcome == ModOutcome::Excluded);
        RC_ASSERT(e.cause.has_value());  // esclusa → esattamente una causa
    }
    for (const DiagnosticEntry& e : isolated) {
        markUnique(e.modId);
        RC_ASSERT(e.outcome == ModOutcome::Isolated);
        RC_ASSERT(e.cause.has_value());  // isolata → esattamente una causa
    }
    // L'unione copre esattamente l'insieme delle individuate.
    RC_ASSERT(seen.size() == static_cast<std::size_t>(numMods));
    for (const ModId& id : discovered) RC_ASSERT(seen.count(id) == 1u);

    // outcomeOf coincide con l'esito pianificato per ogni mod (esattamente uno).
    for (const Assignment& a : plan) {
        const auto outcome = ledger.outcomeOf(a.modId);
        RC_ASSERT(outcome.has_value());
        RC_ASSERT(*outcome == a.outcome);
        RC_ASSERT(ledger.contains(a.modId));
    }

    // --- (3) ogni Mod_Id caricato compare UNA SOLA volta (Req 10.1) ---------
    {
        std::unordered_set<ModId> loadedSet(loaded.begin(), loaded.end());
        RC_ASSERT(loadedSet.size() == loaded.size());  // nessun duplicato
        // loaded() coincide esattamente con le mod pianificate come Loaded.
        std::size_t plannedLoaded = 0;
        for (const Assignment& a : plan)
            if (a.outcome == ModOutcome::Loaded) ++plannedLoaded;
        RC_ASSERT(loaded.size() == plannedLoaded);
    }

    // --- (4) REJECTION del doppio record: la mod resta nel PRIMO esito ------
    // Per ogni mod già registrata, ritentare un esito DIVERSO è rifiutato e
    // l'esito originale è preservato (partizione disgiunta mantenuta).
    for (const Assignment& a : plan) {
        // Sceglie un esito alternativo deterministico (≠ esito corrente).
        const CauseCategory altCause = CauseCategory::ModuleNotLoadable;
        bool retry = true;
        if (a.outcome != ModOutcome::Loaded) {
            retry = ledger.recordLoaded(a.modId);  // tenta Loaded
        } else {
            retry = ledger.recordExcluded(a.modId, altCause);  // tenta Excluded
        }
        RC_ASSERT(!retry);  // doppio record rifiutato

        // L'esito (e il conteggio) restano invariati: ancora il primo esito.
        const auto outcome = ledger.outcomeOf(a.modId);
        RC_ASSERT(outcome.has_value());
        RC_ASSERT(*outcome == a.outcome);
    }

    // Dopo i tentativi rifiutati la partizione resta completa e disgiunta, e il
    // conteggio totale è invariato (nessuna voce aggiunta dai doppi record).
    RC_ASSERT(ledger.partitionComplete(discovered));
    RC_ASSERT(ledger.loaded().size() + ledger.excluded().size() +
                  ledger.isolated().size() ==
              static_cast<std::size_t>(numMods));
    RC_ASSERT(ledger.entries().size() == static_cast<std::size_t>(numMods));
}

}  // namespace
