// tests/eml_property7_topological_transitive_load_order_test.cpp
// Feature: external-mod-loading, Property 7 — Ordine di caricamento topologico
// e transitivo.
// Validates: Requirements 4.2 (Requisiti 4.2)
//
// Property 7 (design.md): in `LoadPlan.order` (qui `ResolvedLoadPlan.order`)
// OGNI mod compare DOPO tutte le mod da cui dipende, DIRETTAMENTE o
// TRANSITIVAMENTE; e si caricano SOLO le mod presenti in `order` (le mod
// escluse — dipendenza mancante o propagazione transitiva dell'esclusione —
// sono assenti da `order`).
//
// Strategia (RapidCheck, ≥100 iterazioni — forzate via RC_GTEST_PROP default
// che esegue 100 casi):
//   * si genera un grafo di dipendenze ACICLICO per costruzione: le mod sono
//     `m0..m(n-1)` e una mod `i` può dipendere SOLO da mod `j < i` (gli archi
//     vanno sempre verso indici inferiori → nessun ciclo);
//   * con piccola probabilità una mod dichiara una dipendenza da un id
//     GARANTITAMENTE assente ("ghost.*"), per esercitare l'esclusione (Req 4.3)
//     e la sua propagazione transitiva (Req 4.5) e dunque la clausola
//     "si caricano solo le mod in order";
//   * i vincoli di versione sono `any()` e tutte le versioni sono 1.0.0, così
//     l'unica causa di esclusione possibile è la dipendenza mancante/esclusa
//     (niente incompatibilità di versione, niente cicli).
//
// Oracolo INDIPENDENTE da `resolve_load_plan`:
//   * `excluded` = chiusura a punto fisso di { mod con almeno una dipendenza
//     verso un id non presente nel set } ∪ { mod che dipende da una mod
//     esclusa };
//   * `loadable` = tutte le mod non escluse;
//   * la relazione di dipendenza TRANSITIVA è ricalcolata autonomamente con una
//     visita (DFS) sul grafo delle dipendenze dirette, ristretta a `loadable`.
//
// Header del loader in loader/lifecycle/ (include relativo alla radice loader/);
// la logica è in mod_loader.cpp (compilata in pulse::loader).

#include "lifecycle/mod_loader.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "lifecycle/dependency_resolver.hpp"
#include "lifecycle/manifest.hpp"

namespace {

using pulse::lifecycle::DiagnosticEntry;
using pulse::lifecycle::ResolvedLoadPlan;
using pulse::lifecycle::SemVer;
using pulse::lifecycle::VersionConstraint;
using pulse::lifecycle::resolve_load_plan;
using pulse::manifest::Dependency;
using pulse::manifest::EntryPoint;
using pulse::manifest::Manifest;
using pulse::manifest::ModType;

// Manifest minimale valido: id, versione 1.0.0, dipendenze con vincolo any().
Manifest makeManifest(const std::string& id, std::vector<Dependency> deps) {
    Manifest m;
    m.schemaVersion = 1;
    m.id = id;
    m.version = SemVer{1, 0, 0};
    m.name = id;
    m.type = ModType::Native;
    m.entryPoints = {EntryPoint{"init", "mod_init"}};
    m.dependencies = std::move(deps);
    return m;
}

int indexOf(const std::vector<std::string>& order, const std::string& mod) {
    auto it = std::find(order.begin(), order.end(), mod);
    return it == order.end() ? -1 : static_cast<int>(it - order.begin());
}

// --- Property 7 — ordine di caricamento topologico e transitivo ------------
// Feature: external-mod-loading, Property 7.
// Validates: Requirements 4.2.
RC_GTEST_PROP(EmlProperty7TopologicalTransitiveLoadOrder,
              EveryModAfterAllItsTransitiveDepsAndOnlyOrderLoaded,
              ()) {
    // --- Genera un grafo ACICLICO: arco i -> j solo se j < i --------------
    const int n = *rc::gen::inRange(0, 9);
    std::vector<std::string> ids;
    ids.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) ids.push_back("m" + std::to_string(i));
    const std::set<std::string> idSet(ids.begin(), ids.end());

    // Dipendenze dirette per ogni mod (verso indici inferiori → aciclico),
    // più eventuali dipendenze da id "ghost" assenti.
    std::map<std::string, std::vector<std::string>> directDeps;
    std::vector<Manifest> compatible;
    compatible.reserve(static_cast<std::size_t>(n));

    for (int i = 0; i < n; ++i) {
        std::vector<Dependency> deps;
        std::vector<std::string> depIds;

        // Dipendenze interne verso mod j < i (subset casuale, niente cicli).
        for (int j = 0; j < i; ++j) {
            if (*rc::gen::arbitrary<bool>()) {
                deps.push_back(Dependency{ids[static_cast<std::size_t>(j)],
                                          VersionConstraint::any()});
                depIds.push_back(ids[static_cast<std::size_t>(j)]);
            }
        }

        // Con piccola probabilità: dipendenza da un id garantito assente.
        if (*rc::gen::inRange(0, 5) == 0) {
            const std::string ghost = "ghost." + std::to_string(i);
            deps.push_back(Dependency{ghost, VersionConstraint::any()});
            depIds.push_back(ghost);
        }

        directDeps[ids[static_cast<std::size_t>(i)]] = depIds;
        compatible.push_back(makeManifest(ids[static_cast<std::size_t>(i)],
                                          std::move(deps)));
    }

    // --- Oracolo INDIPENDENTE: insieme delle mod escluse a punto fisso -----
    // Una mod è esclusa se ha una dipendenza verso un id non presente nel set,
    // oppure se dipende (anche transitivamente) da una mod esclusa (Req 4.3/4.5).
    std::set<std::string> excludedSet;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const std::string& id : ids) {
            if (excludedSet.count(id)) continue;
            for (const std::string& d : directDeps[id]) {
                const bool depMissing = idSet.find(d) == idSet.end();
                const bool depExcluded = excludedSet.count(d) != 0;
                if (depMissing || depExcluded) {
                    excludedSet.insert(id);
                    changed = true;
                    break;
                }
            }
        }
    }

    std::set<std::string> loadableSet;
    for (const std::string& id : ids)
        if (!excludedSet.count(id)) loadableSet.insert(id);

    // --- Oracolo INDIPENDENTE: chiusura transitiva delle dipendenze --------
    // transitiveDeps[id] = tutte le mod (presenti nel set) raggiungibili
    // seguendo gli archi di dipendenza, calcolata con una DFS autonoma.
    std::map<std::string, std::set<std::string>> transitiveDeps;
    for (const std::string& start : ids) {
        std::set<std::string> reached;
        std::vector<std::string> stack(directDeps[start].begin(),
                                       directDeps[start].end());
        while (!stack.empty()) {
            const std::string cur = stack.back();
            stack.pop_back();
            if (idSet.find(cur) == idSet.end()) continue;  // ignora ghost
            if (!reached.insert(cur).second) continue;
            for (const std::string& d : directDeps[cur]) stack.push_back(d);
        }
        transitiveDeps[start] = std::move(reached);
    }

    // --- Esecuzione sotto test --------------------------------------------
    ResolvedLoadPlan plan = resolve_load_plan(compatible, nullptr);

    // (1) Si caricano SOLO le mod caricabili: order è esattamente loadableSet,
    //     ogni id una sola volta, nessun extra (Req 4.2 — "solo le mod in order").
    std::set<std::string> orderSet;
    for (const std::string& m : plan.order) {
        RC_ASSERT(idSet.find(m) != idSet.end());      // nessun id sconosciuto
        RC_ASSERT(orderSet.insert(m).second);          // nessun duplicato
    }
    RC_ASSERT(orderSet == loadableSet);

    // (2) Le mod ESCLUSE sono ASSENTI da order (Req 4.2/4.3/4.5).
    for (const std::string& ex : excludedSet)
        RC_ASSERT(indexOf(plan.order, ex) == -1);

    // (3) ORDINE topologico/transitivo: ogni mod compare DOPO tutte le mod da
    //     cui dipende direttamente o transitivamente che sono anch'esse
    //     caricate (Req 4.2).
    for (const std::string& m : plan.order) {
        const int mi = indexOf(plan.order, m);
        for (const std::string& d : transitiveDeps[m]) {
            if (!loadableSet.count(d)) continue;  // dep esclusa: non in order
            const int di = indexOf(plan.order, d);
            RC_ASSERT(di != -1);                  // la dep caricabile è in order
            RC_ASSERT(di < mi);                   // compare PRIMA della mod
        }
    }
}

}  // namespace
