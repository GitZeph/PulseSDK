// tests/eml_property8_dependency_exclusion_propagation_test.cpp
// Feature: external-mod-loading, Property 8 — Esclusione e propagazione delle
// dipendenze.
// Validates: Requirements 4.3, 4.4, 4.5, 4.6 (Requisiti 4.3, 4.4, 4.5, 4.6)
//
// Property 8 (design.md): per ogni grafo di dipendenze tra Mod_Manifest
// compatibili, `resolve_load_plan` esclude:
//   * le mod con una dipendenza MANCANTE o di versione INCOMPATIBILE, con
//     categoria di causa `DependencyUnsatisfied` (Req 4.3);
//   * tutte le mod coinvolte in un CICLO di dipendenze, con categoria di causa
//     `DependencyCycle` (Req 4.4);
//   * le mod che dipendono (direttamente o transitivamente) da una mod esclusa,
//     con categoria di causa `DependencyUnsatisfied` (Req 4.5);
// e fa proseguire tutte le mod restanti caricabili in `order`, ognuna dopo
// tutte le sue dipendenze (Req 4.6). Ogni mod individuata finisce in
// ESATTAMENTE uno tra {caricata, esclusa} (partizione, nessun duplicato).
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si genera un insieme di 1..6 mod con id unici `m0..m{n-1}` e versioni
//     SemVer con componenti piccole (0..3);
//   * ogni mod dichiara 0..3 dipendenze il cui bersaglio è scelto in un pool
//     che comprende le mod presenti E alcuni id ASSENTI (`ghost*`) → genera a
//     piacere dipendenze mancanti, vincoli di versione (eventualmente)
//     incompatibili, cicli (anche self-loop) ed esclusioni transitive;
//   * un ORACOLO INDIPENDENTE classifica ogni mod in {Loaded, Excluded+causa}
//     ricalcolando, con codice autonomo rispetto al `DependencyResolver`:
//       (1) pre-esclusione per dipendenza mancante/incompatibile + propagazione
//           transitiva;
//       (2) rilevamento dei cicli sul sottografo superstite via chiusura
//           transitiva (un nodo è membro di un ciclo sse è raggiungibile da sé
//           stesso con un cammino di lunghezza ≥1);
//       (3) seconda propagazione "dipende da una mod esclusa".
//   L'oracolo NON usa il resolver: usa solo i confronti di valore di SemVer.

#include "lifecycle/mod_loader.hpp"

#include "lifecycle/dependency_resolver.hpp"
#include "lifecycle/manifest.hpp"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using pulse::lifecycle::CauseCategory;
using pulse::lifecycle::DiagnosticEntry;
using pulse::lifecycle::ModOutcome;
using pulse::lifecycle::ResolvedLoadPlan;
using pulse::lifecycle::resolve_load_plan;
using pulse::lifecycle::SemVer;
using pulse::lifecycle::VersionConstraint;
using pulse::manifest::Dependency;
using pulse::manifest::EntryPoint;
using pulse::manifest::Manifest;
using pulse::manifest::ModType;

// --- Oracolo: confronto di valore di SemVer (indipendente dal resolver) ------
// Riproduce la semantica del vincolo: `v` soddisfa il vincolo sse `v >= min` e,
// se presente un upper bound, `v < maxExclusive`. Reimplementato qui in modo
// autonomo (non delega a VersionConstraint::satisfiedBy del resolver).
bool oracleSatisfies(const VersionConstraint& c, const SemVer& v) {
    if (v < c.min) return false;
    if (c.maxExclusive.has_value() && !(v < *c.maxExclusive)) return false;
    return true;
}

// Costruisce un Manifest minimale valido con id/version/dipendenze.
Manifest makeManifest(const std::string& id, SemVer version,
                      std::vector<Dependency> deps) {
    Manifest m;
    m.schemaVersion = 1;
    m.id = id;
    m.version = version;
    m.name = id;
    m.type = ModType::Native;
    m.entryPoints = {EntryPoint{"init", "mod_init"}};
    m.dependencies = std::move(deps);
    return m;
}

SemVer genSmallSemVer() {
    const auto a = static_cast<std::uint32_t>(*rc::gen::inRange(0, 4));
    const auto b = static_cast<std::uint32_t>(*rc::gen::inRange(0, 4));
    const auto c = static_cast<std::uint32_t>(*rc::gen::inRange(0, 4));
    return SemVer{a, b, c};
}

// Classificazione attesa di una mod individuata: caricata, oppure esclusa con
// esattamente una categoria di causa dell'insieme chiuso (Req 4.3/4.4/4.5).
struct Oracle {
    std::set<std::string> loaded;                       // → presenti in `order`
    std::map<std::string, CauseCategory> excludedCause;  // mod esclusa → causa
};

// Oracolo INDIPENDENTE: ricalcola la partizione {loaded | excluded(+causa)}
// replicando la specifica (pre-esclusione + propagazione, cicli, propagazione)
// con codice autonomo rispetto al DependencyResolver.
Oracle classify(const std::vector<Manifest>& mods) {
    std::set<std::string> present;
    std::map<std::string, SemVer> versionById;
    std::map<std::string, std::vector<Dependency>> depsById;
    for (const Manifest& m : mods) {
        present.insert(m.id);
        versionById[m.id] = m.version;
        depsById[m.id] = m.dependencies;
    }

    // --- Fase 1: pre-esclusione per dipendenza mancante/incompatibile -------
    std::set<std::string> unsat;  // "dipendenza non soddisfatta" (Req 4.3/4.5)
    for (const std::string& v : present) {
        for (const Dependency& d : depsById[v]) {
            const bool missing = present.count(d.id) == 0;
            const bool incompatible =
                !missing && !oracleSatisfies(d.versionConstraint, versionById[d.id]);
            if (missing || incompatible) {
                unsat.insert(v);
                break;
            }
        }
    }

    // Propagazione transitiva: dipendere da una mod già non-soddisfatta esclude.
    auto propagateUnsatFrom = [&](const std::set<std::string>& excludedSet) {
        bool changed = true;
        while (changed) {
            changed = false;
            for (const std::string& v : present) {
                if (unsat.count(v) != 0) continue;
                for (const Dependency& d : depsById[v]) {
                    if (present.count(d.id) != 0 && excludedSet.count(d.id) != 0) {
                        unsat.insert(v);
                        changed = true;
                        break;
                    }
                }
            }
        }
    };
    propagateUnsatFrom(unsat);

    // Sottografo superstite dopo la fase 1.
    std::set<std::string> survivors;
    for (const std::string& v : present)
        if (unsat.count(v) == 0) survivors.insert(v);

    // --- Fase 2: cicli via chiusura transitiva sul sottografo superstite ----
    // Arco v -> d sse d è una dipendenza presente e superstite (incl. self).
    std::map<std::string, std::set<std::string>> reach;  // chiusura transitiva
    for (const std::string& v : survivors) {
        for (const Dependency& d : depsById[v]) {
            if (survivors.count(d.id) != 0) reach[v].insert(d.id);
        }
    }
    // Chiusura transitiva (Floyd–Warshall su nodi superstiti).
    for (const std::string& k : survivors)
        for (const std::string& i : survivors) {
            if (reach[i].count(k) == 0) continue;
            for (const std::string& j : reach[k]) reach[i].insert(j);
        }
    // Membro di ciclo sse raggiungibile da sé stesso (cammino di lunghezza ≥1).
    std::set<std::string> cycle;
    for (const std::string& v : survivors)
        if (reach[v].count(v) != 0) cycle.insert(v);

    // --- Fase 3: seconda propagazione "dipende da una mod esclusa" ----------
    // Le mod escluse sono ora unsat ∪ cycle; chi vi dipende è "non soddisfatta".
    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (const std::string& v : present) {
                if (unsat.count(v) != 0 || cycle.count(v) != 0) continue;
                for (const Dependency& d : depsById[v]) {
                    if (present.count(d.id) != 0 &&
                        (unsat.count(d.id) != 0 || cycle.count(d.id) != 0)) {
                        unsat.insert(v);
                        changed = true;
                        break;
                    }
                }
            }
        }
    }

    Oracle o;
    for (const std::string& v : present) {
        if (cycle.count(v) != 0) {
            o.excludedCause[v] = CauseCategory::DependencyCycle;       // Req 4.4
        } else if (unsat.count(v) != 0) {
            o.excludedCause[v] = CauseCategory::DependencyUnsatisfied;  // Req 4.3/4.5
        } else {
            o.loaded.insert(v);                                        // Req 4.6
        }
    }
    return o;
}

// --- Property 8 — esclusione e propagazione delle dipendenze ----------------
// Feature: external-mod-loading, Property 8.
// Validates: Requirements 4.3, 4.4, 4.5, 4.6.
RC_GTEST_PROP(EmlProperty8DependencyExclusionPropagation,
              ExcludedWithCorrectCauseAndRemainingLoadInOrder, ()) {
    const int n = *rc::gen::inRange(1, 7);        // 1..6 mod presenti
    const int numGhost = *rc::gen::inRange(0, 3);  // 0..2 id assenti (ghost)

    auto idOf = [](int i) { return "m" + std::to_string(i); };
    auto ghostOf = [](int g) { return "ghost" + std::to_string(g); };

    std::vector<SemVer> versions(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) versions[static_cast<std::size_t>(i)] = genSmallSemVer();

    std::vector<Manifest> mods;
    mods.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const int k = *rc::gen::inRange(0, 4);  // 0..3 dipendenze
        std::vector<Dependency> deps;
        deps.reserve(static_cast<std::size_t>(k));
        for (int j = 0; j < k; ++j) {
            const int t = *rc::gen::inRange(0, n + numGhost);
            std::string depId = (t < n) ? idOf(t) : ghostOf(t - n);

            // Vincolo: any() oppure atLeast(reqMin) — quest'ultimo può risultare
            // incompatibile col bersaglio (genera Req 4.3).
            VersionConstraint c = VersionConstraint::any();
            if (*rc::gen::arbitrary<bool>()) {
                c = VersionConstraint::atLeast(genSmallSemVer());
            }
            deps.push_back(Dependency{std::move(depId), c});
        }
        mods.push_back(makeManifest(idOf(i), versions[static_cast<std::size_t>(i)],
                                    std::move(deps)));
    }

    // --- Oracolo indipendente ---------------------------------------------
    const Oracle oracle = classify(mods);

    // --- Sistema sotto test ------------------------------------------------
    const ResolvedLoadPlan plan = resolve_load_plan(mods, nullptr);

    // (1) `order` contiene ESATTAMENTE le mod caricabili (Req 4.6).
    std::set<std::string> orderSet(plan.order.begin(), plan.order.end());
    RC_ASSERT(orderSet == oracle.loaded);
    // Nessun duplicato in `order` (ogni Mod_Id al più una volta).
    RC_ASSERT(orderSet.size() == plan.order.size());

    // (2) Ordine topologico: ogni mod in `order` compare dopo le sue dipendenze
    //     (presenti e caricabili) (Req 4.6 → coerenza con 4.2).
    std::map<std::string, std::size_t> orderIndex;
    for (std::size_t i = 0; i < plan.order.size(); ++i) orderIndex[plan.order[i]] = i;
    for (const Manifest& m : mods) {
        if (orderSet.count(m.id) == 0) continue;
        for (const Dependency& d : m.dependencies) {
            auto it = orderIndex.find(d.id);
            if (it != orderIndex.end()) {
                RC_ASSERT(it->second < orderIndex.at(m.id));
            }
        }
    }

    // (3) Le mod escluse coincidono ESATTAMENTE con quelle dell'oracolo, con la
    //     categoria di causa corretta e senza duplicati (Req 4.3/4.4/4.5).
    std::map<std::string, CauseCategory> planExcluded;
    for (const DiagnosticEntry& e : plan.excluded) {
        // Ogni esclusione è osservabile come {Excluded + esattamente una causa}.
        RC_ASSERT(e.outcome == ModOutcome::Excluded);
        RC_ASSERT(e.cause.has_value());
        RC_ASSERT(!e.message.empty());
        // Nessun Mod_Id escluso compare due volte.
        RC_ASSERT(planExcluded.find(e.modId) == planExcluded.end());
        planExcluded.emplace(e.modId, *e.cause);
    }
    RC_ASSERT(planExcluded.size() == oracle.excludedCause.size());
    for (const auto& [mod, cause] : oracle.excludedCause) {
        auto it = planExcluded.find(mod);
        RC_ASSERT(it != planExcluded.end());
        RC_ASSERT(it->second == cause);
    }

    // (4) Partizione completa: ogni mod individuata è in ESATTAMENTE uno tra
    //     {caricata, esclusa}; unione = tutte le mod, intersezione vuota.
    RC_ASSERT(orderSet.size() + planExcluded.size() == static_cast<std::size_t>(n));
    for (const Manifest& m : mods) {
        const bool inOrder = orderSet.count(m.id) != 0;
        const bool inExcluded = planExcluded.count(m.id) != 0;
        RC_ASSERT(inOrder != inExcluded);  // XOR: esattamente uno dei due
    }
}

}  // namespace
