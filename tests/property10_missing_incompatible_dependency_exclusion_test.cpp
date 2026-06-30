// tests/property10_missing_incompatible_dependency_exclusion_test.cpp
// Feature: pulse-sdk, Property 10 — Esclusione di dipendenze mancanti o
// incompatibili.
// Validates: Requirements 4.2 (Requisito 4.2)
//
// Property 10 (design.md / Req 4.2): per ogni grafo in cui alcune mod
// dichiarano una dipendenza NON installata oppure installata con una versione
// INCOMPATIBILE, `DependencyResolver::resolve()` deve:
//   * escludere ESATTAMENTE quelle mod (e, transitivamente, i loro dipendenti)
//     con il motivo corretto e identificando la dipendenza problematica;
//   * lasciare nel piano (`order`) tutte le mod sane non coinvolte;
//   * non far MAI comparire una mod esclusa nell'ordine di caricamento.
//
// Asserzioni mirate (dal task):
//   (a) una mod con dipendenza mancante è esclusa con MissingDependency che
//       nomina la dipendenza assente;
//   (b) una mod che richiede una versione non soddisfatta dalla versione
//       installata è esclusa con IncompatibleDependency (con la versione
//       trovata) e nomina la dipendenza offendente;
//   (c) le mod sane e indipendenti restano in `order`;
//   (d) le mod escluse non compaiono mai in `order`.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si generano `baseCount` mod SANE e INDIPENDENTI (nessuna dipendenza),
//     con id e versioni randomizzati — devono restare tutte in `order`;
//   * `missingCount` mod che dipendono ciascuna da un id GARANTITO assente
//     ("ghost_*") → MissingDependency;
//   * `incompatCount` mod che dipendono da una mod base installata ma con un
//     VincoloVersione costruito per essere INSODDISFABILE dalla versione
//     installata → IncompatibleDependency.
//   Le mod "miss"/"incompat" sono foglie (nessuno dipende da loro): la loro
//   esclusione non si propaga alle mod sane, così l'invariante "le sane
//   restano" è esercitato in modo netto.

#include "lifecycle/dependency_resolver.hpp"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace {

using pulse::lifecycle::Dependency;
using pulse::lifecycle::DependencyResolver;
using pulse::lifecycle::Exclusion;
using pulse::lifecycle::ExclusionReason;
using pulse::lifecycle::LoadPlan;
using pulse::lifecycle::ModId;
using pulse::lifecycle::ResolverManifest;
using pulse::lifecycle::SemVer;
using pulse::lifecycle::VersionConstraint;

const Exclusion* findExclusion(const LoadPlan& plan, const ModId& id) {
    for (const auto& e : plan.excluded) {
        if (e.mod == id) return &e;
    }
    return nullptr;
}

bool inOrder(const std::vector<ModId>& order, const ModId& id) {
    return std::find(order.begin(), order.end(), id) != order.end();
}

// Genera una SemVer con componenti in un range che lascia "spazio" per
// costruire vincoli insoddisfabili senza overflow (major >= 1).
SemVer genBaseVersion() {
    const auto major = static_cast<std::uint32_t>(*rc::gen::inRange(1, 100));
    const auto minor = static_cast<std::uint32_t>(*rc::gen::inRange(0, 100));
    const auto patch = static_cast<std::uint32_t>(*rc::gen::inRange(0, 100));
    return SemVer{major, minor, patch};
}

// --- Property 10 — esclusione di dipendenze mancanti/incompatibili --------
// Feature: pulse-sdk, Property 10. Validates: Requirements 4.2.
RC_GTEST_PROP(Property10MissingIncompatibleDependencyExclusion,
              ExcludesMissingAndIncompatibleDependentsKeepsHealthy,
              ()) {
    // Almeno una mod sana così che le mod "incompatibili" abbiano un bersaglio
    // installato su cui agganciarsi.
    const int baseCount = *rc::gen::inRange(1, 7).as("# mod sane indipendenti");
    const int missingCount = *rc::gen::inRange(0, 5).as("# mod con dip mancante");
    const int incompatCount =
        *rc::gen::inRange(0, 5).as("# mod con dip incompatibile");

    std::vector<ResolverManifest> mods;
    std::vector<ModId> baseIds;
    std::vector<SemVer> baseVersions;

    // --- Mod sane e indipendenti (nessuna dipendenza) ---------------------
    for (int i = 0; i < baseCount; ++i) {
        ModId id = "base_" + std::to_string(i);
        SemVer v = genBaseVersion();
        baseIds.push_back(id);
        baseVersions.push_back(v);
        mods.push_back(ResolverManifest{id, v, {}});
    }

    // --- Mod con dipendenza MANCANTE --------------------------------------
    // Ognuna dipende da un id "ghost_*" garantito non installato.
    std::vector<ModId> missingIds;
    std::vector<ModId> missingGhosts;
    for (int i = 0; i < missingCount; ++i) {
        ModId id = "miss_" + std::to_string(i);
        ModId ghost = "ghost_" + std::to_string(i);  // mai installato
        missingIds.push_back(id);
        missingGhosts.push_back(ghost);
        mods.push_back(ResolverManifest{
            id, SemVer{1, 0, 0}, {Dependency{ghost, VersionConstraint::any()}}});
    }

    // --- Mod con dipendenza di versione INCOMPATIBILE ---------------------
    // Ognuna dipende da una mod base installata ma con un vincolo costruito
    // per NON essere soddisfatto dalla versione installata.
    std::vector<ModId> incompatIds;
    std::vector<ModId> incompatTargets;
    std::vector<SemVer> incompatFound;
    for (int i = 0; i < incompatCount; ++i) {
        ModId id = "incompat_" + std::to_string(i);
        const int targetIdx = *rc::gen::inRange(0, baseCount);
        const ModId target = baseIds[targetIdx];
        const SemVer installed = baseVersions[targetIdx];

        // Due forme di vincolo entrambe insoddisfabili da `installed`:
        //   forma 0: atLeast({major+1,0,0}) → installed < min;
        //   forma 1: range({0,0,0},{major,0,0}) → installed >= upper esclusivo.
        const int form = *rc::gen::inRange(0, 2);
        VersionConstraint c =
            (form == 0)
                ? VersionConstraint::atLeast(SemVer{installed.major + 1, 0, 0})
                : VersionConstraint::range(SemVer{0, 0, 0},
                                           SemVer{installed.major, 0, 0});

        incompatIds.push_back(id);
        incompatTargets.push_back(target);
        incompatFound.push_back(installed);
        mods.push_back(ResolverManifest{id, SemVer{1, 0, 0},
                                        {Dependency{target, c}}});
    }

    // Risoluzione su un ordine di input mescolato per esercitare il
    // determinismo del tie-break (non deve influenzare le esclusioni).
    {
        const auto perm =
            *rc::gen::container<std::vector<int>>(mods.size(),
                                                  rc::gen::arbitrary<int>())
                 .as("chiavi di permutazione input");
        std::vector<std::size_t> idx(mods.size());
        for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(),
                         [&](std::size_t l, std::size_t r) {
                             return perm[l] < perm[r];
                         });
        std::vector<ResolverManifest> shuffled;
        shuffled.reserve(mods.size());
        for (std::size_t i : idx) shuffled.push_back(mods[i]);
        mods = std::move(shuffled);
    }

    DependencyResolver resolver;
    const LoadPlan plan = resolver.resolve(mods);

    // (c) Tutte le mod sane e indipendenti restano nel piano di caricamento.
    for (const ModId& id : baseIds) {
        RC_ASSERT(inOrder(plan.order, id));
    }
    // L'ordine contiene ESATTAMENTE le mod sane (nessuna esclusa è in order).
    RC_ASSERT(plan.order.size() == static_cast<std::size_t>(baseCount));
    {
        const std::set<ModId> orderSet(plan.order.begin(), plan.order.end());
        const std::set<ModId> baseSet(baseIds.begin(), baseIds.end());
        RC_ASSERT(orderSet == baseSet);
    }

    // (a) Ogni mod con dipendenza mancante è esclusa con MissingDependency che
    // nomina la dipendenza assente, e non compare in order.
    for (std::size_t i = 0; i < missingIds.size(); ++i) {
        const ModId& id = missingIds[i];
        RC_ASSERT(!inOrder(plan.order, id));  // (d)
        const Exclusion* ex = findExclusion(plan, id);
        RC_ASSERT(ex != nullptr);
        RC_ASSERT(ex->reason.kind ==
                  ExclusionReason::Kind::MissingDependency);
        RC_ASSERT(ex->reason.dependency == missingGhosts[i]);
    }

    // (b) Ogni mod con dipendenza di versione incompatibile è esclusa con
    // IncompatibleDependency, nomina la dipendenza offendente e riporta la
    // versione trovata; non compare in order.
    for (std::size_t i = 0; i < incompatIds.size(); ++i) {
        const ModId& id = incompatIds[i];
        RC_ASSERT(!inOrder(plan.order, id));  // (d)
        const Exclusion* ex = findExclusion(plan, id);
        RC_ASSERT(ex != nullptr);
        RC_ASSERT(ex->reason.kind ==
                  ExclusionReason::Kind::IncompatibleDependency);
        RC_ASSERT(ex->reason.dependency == incompatTargets[i]);
        RC_ASSERT(ex->reason.foundVersion.has_value());
        RC_ASSERT(*ex->reason.foundVersion == incompatFound[i]);
    }

    // Le esclusioni sono ESATTAMENTE le mod miss + incompat (le foglie escluse
    // non propagano l'esclusione alle mod sane).
    RC_ASSERT(plan.excluded.size() ==
              static_cast<std::size_t>(missingCount + incompatCount));

    // (d) Nessuna mod esclusa compare mai nell'ordine di caricamento.
    for (const Exclusion& ex : plan.excluded) {
        RC_ASSERT(!inOrder(plan.order, ex.mod));
    }
}

}  // namespace
