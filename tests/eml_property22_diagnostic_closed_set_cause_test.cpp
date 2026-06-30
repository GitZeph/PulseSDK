// tests/eml_property22_diagnostic_closed_set_cause_test.cpp
// Feature: external-mod-loading, Property 22 — Diagnostica con causa
// appartenente all'insieme chiuso.
// Validates: Requirements 10.2, 10.3 (Requisiti 10.2, 10.3)
//
// Property 22 (design.md / tasks.md 7.21): ogni mod ESCLUSA o ISOLATA registra
// il proprio Mod_Id, l'esito che distingue esclusa (Excluded) da isolata
// (Isolated) ed ESATTAMENTE UNA `CauseCategory` appartenente all'insieme CHIUSO
// {InvalidPackage, VersionOrPlatformIncompatible, DependencyUnsatisfied,
//  DependencyCycle, ModuleNotLoadable, EntryPointFailed, SymbolUnresolved}
// (Req 10.2); inoltre ogni evento hook identifica Mod_Id, simbolo bersaglio e
// tipo di operazione (Install | Remove) (Req 10.3).
//
// Modello (host-testable, niente OS loader): si alimenta un `DiagnosticLedger`
// con un insieme randomizzato di mod, ciascuna con esito generato tra
// {Loaded, Excluded, Isolated} e, per gli esiti non-Loaded, una causa estratta
// dall'insieme chiuso; in parallelo si emettono eventi hook install/remove con
// Mod_Id/simbolo casuali. Un oracolo indipendente tiene traccia di cosa è stato
// registrato e si verifica che:
//   (a) ogni voce Excluded ha outcome==Excluded e UNA causa dell'insieme chiuso;
//   (b) ogni voce Isolated ha outcome==Isolated e UNA causa dell'insieme chiuso;
//   (c) il Mod_Id di ogni voce coincide con quello registrato;
//   (d) le voci Loaded non portano causa (coerenza dell'insieme chiuso);
//   (e) ogni HookEvent identifica Mod_Id, simbolo e op in {Install, Remove},
//       coerente con la sequenza emessa, in ordine di emissione.
//
// Strategia (RapidCheck, ≥100 iterazioni — RC_GTEST_PROP esegue 100 di default):
// Mod_Id univoci, esiti/cause/eventi randomizzati per coprire l'intero insieme
// chiuso e i due esiti non-Loaded.
//
// Header del loader in loader/lifecycle/ (include relativo alla radice loader/);
// la logica è in mod_loader.cpp (compilata in pulse::loader via glob
// lifecycle/*.cpp). Integrazione RapidCheck+GoogleTest in extras/gtest.

#include "lifecycle/mod_loader.hpp"

#include <array>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

namespace {

using pulse::lifecycle::CauseCategory;
using pulse::lifecycle::DiagnosticEntry;
using pulse::lifecycle::DiagnosticLedger;
using pulse::lifecycle::HookEvent;
using pulse::lifecycle::HookOp;
using pulse::lifecycle::ModId;
using pulse::lifecycle::ModOutcome;

// L'insieme CHIUSO delle cause (Req 10.2): nessun valore aggiuntivo ammesso.
constexpr std::array<CauseCategory, 7> kClosedCauseSet{
    CauseCategory::InvalidPackage,
    CauseCategory::VersionOrPlatformIncompatible,
    CauseCategory::DependencyUnsatisfied,
    CauseCategory::DependencyCycle,
    CauseCategory::ModuleNotLoadable,
    CauseCategory::EntryPointFailed,
    CauseCategory::SymbolUnresolved,
};

bool inClosedSet(CauseCategory cause) {
    for (CauseCategory c : kClosedCauseSet)
        if (c == cause) return true;
    return false;
}

// Genera una causa dell'insieme chiuso.
rc::Gen<CauseCategory> genCause() {
    return rc::gen::elementOf(kClosedCauseSet);
}

// --- Property 22 — diagnostica con causa dell'insieme chiuso ---------------
// Feature: external-mod-loading, Property 22.
// Validates: Requirements 10.2, 10.3.
RC_GTEST_PROP(EmlProperty22DiagnosticClosedSetCause,
              ExcludedIsolatedRecordModOutcomeAndExactlyOneClosedCause,
              ()) {
    DiagnosticLedger ledger;

    const int numMods = *rc::gen::inRange(1, 12);

    // Oracolo: per ogni Mod_Id registrato, l'esito e (se non Loaded) la causa.
    struct Expected {
        ModId modId;
        ModOutcome outcome;
        CauseCategory cause{};  // significativo solo se outcome != Loaded
        bool hasCause{false};
    };
    std::vector<Expected> expected;
    std::set<ModId> seen;

    for (int i = 0; i < numMods; ++i) {
        const ModId modId = ModId("mod." + std::to_string(i));
        // Mod_Id univoci per costruzione; salta duplicati (difensivo).
        if (!seen.insert(modId).second) continue;

        // Esito: 0=Loaded, 1=Excluded, 2=Isolated. Pesi tali da coprire
        // ampiamente i due esiti non-Loaded (oggetto della proprietà).
        const int kind = *rc::gen::weightedElement<int>(
            {{1, 0}, {3, 1}, {3, 2}});

        Expected e;
        e.modId = modId;
        if (kind == 0) {
            e.outcome = ModOutcome::Loaded;
            e.hasCause = false;
            RC_ASSERT(ledger.recordLoaded(modId, "loaded ok"));
        } else if (kind == 1) {
            e.outcome = ModOutcome::Excluded;
            e.cause = *genCause();
            e.hasCause = true;
            RC_ASSERT(ledger.recordExcluded(modId, e.cause, "excluded"));
        } else {
            e.outcome = ModOutcome::Isolated;
            e.cause = *genCause();
            e.hasCause = true;
            RC_ASSERT(ledger.recordIsolated(modId, e.cause, "isolated"));
        }
        expected.push_back(e);
    }

    // --- Eventi hook install/remove (Req 10.3) -----------------------------
    // Sequenza randomizzata di eventi su simboli/mod arbitrari.
    struct ExpectedHook {
        ModId modId;
        std::string symbol;
        HookOp op;
    };
    std::vector<ExpectedHook> expectedHooks;

    const int numHookEvents = *rc::gen::inRange(0, 30);
    for (int h = 0; h < numHookEvents; ++h) {
        const int modIdx = *rc::gen::inRange(0, numMods);
        const ModId owner = ModId("mod." + std::to_string(modIdx));
        const std::string symbol =
            owner + "::sym" + std::to_string(*rc::gen::inRange(0, 5));
        const bool install = *rc::gen::arbitrary<bool>();
        if (install) {
            ledger.recordHookInstalled(owner, symbol);
            expectedHooks.push_back({owner, symbol, HookOp::Install});
        } else {
            ledger.recordHookRemoved(owner, symbol);
            expectedHooks.push_back({owner, symbol, HookOp::Remove});
        }
    }

    // === Verifiche degli esiti (Req 10.2) ==================================

    // (a) Ogni voce Excluded: outcome==Excluded, ESATTAMENTE una causa
    //     dell'insieme chiuso, Mod_Id corretto.
    const std::vector<DiagnosticEntry> excluded = ledger.excluded();
    for (const DiagnosticEntry& entry : excluded) {
        RC_ASSERT(entry.outcome == ModOutcome::Excluded);
        RC_ASSERT(entry.cause.has_value());
        RC_ASSERT(inClosedSet(*entry.cause));
    }

    // (b) Ogni voce Isolated: outcome==Isolated, una causa dell'insieme chiuso.
    const std::vector<DiagnosticEntry> isolated = ledger.isolated();
    for (const DiagnosticEntry& entry : isolated) {
        RC_ASSERT(entry.outcome == ModOutcome::Isolated);
        RC_ASSERT(entry.cause.has_value());
        RC_ASSERT(inClosedSet(*entry.cause));
    }

    // (c)+(d) Coerenza voce-per-voce con l'oracolo: Mod_Id, esito e causa
    //         (assente per Loaded, presente e nel set chiuso altrimenti).
    for (const Expected& e : expected) {
        const auto outcome = ledger.outcomeOf(e.modId);
        RC_ASSERT(outcome.has_value());
        RC_ASSERT(*outcome == e.outcome);

        // Recupera la voce registrata per questo Mod_Id.
        const DiagnosticEntry* found = nullptr;
        for (const DiagnosticEntry& entry : ledger.entries())
            if (entry.modId == e.modId) found = &entry;
        RC_ASSERT(found != nullptr);
        RC_ASSERT(found->modId == e.modId);

        if (e.outcome == ModOutcome::Loaded) {
            RC_ASSERT(!found->cause.has_value());  // Loaded → nessuna causa
        } else {
            RC_ASSERT(found->cause.has_value());
            RC_ASSERT(inClosedSet(*found->cause));
            RC_ASSERT(*found->cause == e.cause);  // esattamente la causa registrata
        }
    }

    // L'unione di {Loaded, Excluded, Isolated} non deve introdurre cause fuori
    // dall'insieme chiuso per alcuna voce non-Loaded.
    for (const DiagnosticEntry& entry : ledger.entries()) {
        if (entry.outcome == ModOutcome::Loaded) {
            RC_ASSERT(!entry.cause.has_value());
        } else {
            RC_ASSERT(entry.cause.has_value());
            RC_ASSERT(inClosedSet(*entry.cause));
        }
    }

    // === Verifiche degli eventi hook (Req 10.3) ============================
    // Ogni evento identifica Mod_Id, simbolo e op in {Install, Remove}, in
    // ordine di emissione e coerente con la sequenza emessa.
    const std::vector<HookEvent>& events = ledger.hookEvents();
    RC_ASSERT(events.size() == expectedHooks.size());
    for (std::size_t i = 0; i < events.size(); ++i) {
        const HookEvent& ev = events[i];
        RC_ASSERT(ev.op == HookOp::Install || ev.op == HookOp::Remove);
        RC_ASSERT(!ev.modId.empty());
        RC_ASSERT(!ev.symbol.empty());
        RC_ASSERT(ev.modId == expectedHooks[i].modId);
        RC_ASSERT(ev.symbol == expectedHooks[i].symbol);
        RC_ASSERT(ev.op == expectedHooks[i].op);
    }
}

}  // namespace
