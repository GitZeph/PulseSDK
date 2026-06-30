// loader/lifecycle/dependency_resolver.cpp — implementazione del
// DependencyResolver (Requisito 4.1, 4.2, 4.3).
//
// Vedi dependency_resolver.hpp per la descrizione dell'algoritmo. In sintesi:
//   1. pre-esclusione delle mod con dipendenze mancanti o incompatibili
//      (Req 4.2), propagata transitivamente alle mod che ne dipendono;
//   2. rilevamento dei cicli via componenti fortemente connesse (SCC): i membri
//      di ogni ciclo sono esclusi e riportati con l'elenco esatto dei membri
//      del proprio ciclo (Req 4.3); le mod che dipendono da un ciclo (ma non vi
//      appartengono) sono escluse come dipendenti di una mod esclusa (Req 4.2);
//   3. Kahn topological sort sul sottografo aciclico superstite con tie-break
//      deterministico per `ModId` (Req 4.1, determinismo IMP-02).
#include "lifecycle/dependency_resolver.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse::lifecycle {

namespace {

// Indicizza i manifest per id (ordine deterministico). In caso di id duplicati
// prevale la prima dichiarazione (identità di mod univoca, Req 16.1).
std::map<ModId, const ResolverManifest*> indexById(
    const std::vector<ResolverManifest>& installed) {
    std::map<ModId, const ResolverManifest*> byId;
    for (const auto& m : installed) {
        byId.emplace(m.id, &m);  // emplace non sovrascrive una chiave esistente
    }
    return byId;
}

// Dipendenze attive, deduplicate e ordinate deterministicamente, di una mod.
std::vector<ModId> activeDepsOf(const ResolverManifest& manifest,
                                const std::set<ModId>& active) {
    std::vector<ModId> deps;
    std::unordered_set<ModId> seen;
    for (const auto& dep : manifest.dependencies) {
        if (active.count(dep.id) == 0) continue;
        if (!seen.insert(dep.id).second) continue;
        deps.push_back(dep.id);
    }
    std::sort(deps.begin(), deps.end());
    return deps;
}

}  // namespace

LoadPlan DependencyResolver::resolve(
    const std::vector<ResolverManifest>& installed) const {
    LoadPlan plan;

    const std::map<ModId, const ResolverManifest*> byId = indexById(installed);

    // ------------------------------------------------------------------
    // Fase 1 — pre-esclusione per dipendenze mancanti o incompatibili
    // (Req 4.2). Iteriamo a punto fisso per propagare l'esclusione alle mod
    // che dipendono (anche transitivamente) da una mod esclusa.
    // ------------------------------------------------------------------
    std::map<ModId, ExclusionReason> excludedReason;

    for (const auto& [id, manifest] : byId) {
        for (const auto& dep : manifest->dependencies) {
            auto it = byId.find(dep.id);
            if (it == byId.end()) {
                ExclusionReason r;
                r.kind = ExclusionReason::Kind::MissingDependency;
                r.dependency = dep.id;
                excludedReason.emplace(id, std::move(r));
                break;
            }
            const SemVer& installedVer = it->second->version;
            if (!dep.versionConstraint.satisfiedBy(installedVer)) {
                ExclusionReason r;
                r.kind = ExclusionReason::Kind::IncompatibleDependency;
                r.dependency = dep.id;
                r.foundVersion = installedVer;
                excludedReason.emplace(id, std::move(r));
                break;
            }
        }
    }

    // Funzione di propagazione transitiva a punto fisso: una mod che dipende da
    // una mod già esclusa è a sua volta esclusa (la dipendenza non si caricherà).
    auto propagateExclusion = [&]() {
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& [id, manifest] : byId) {
                if (excludedReason.count(id) != 0) continue;
                for (const auto& dep : manifest->dependencies) {
                    if (excludedReason.count(dep.id) != 0) {
                        ExclusionReason r;
                        r.kind = ExclusionReason::Kind::DependencyExcluded;
                        r.dependency = dep.id;
                        excludedReason.emplace(id, std::move(r));
                        changed = true;
                        break;
                    }
                }
            }
        }
    };
    propagateExclusion();

    // Sottografo attivo (mod non ancora escluse).
    auto computeActive = [&]() {
        std::set<ModId> active;
        for (const auto& [id, manifest] : byId) {
            if (excludedReason.count(id) == 0) active.insert(id);
        }
        return active;
    };

    // ------------------------------------------------------------------
    // Fase 2 — rilevamento dei cicli via SCC (Tarjan) sul sottografo attivo.
    // Arco node -> dep (la mod dipende dalla dipendenza). Una SCC con più di un
    // nodo (o un nodo con self-loop) costituisce un ciclo di dipendenze.
    // ------------------------------------------------------------------
    {
        const std::set<ModId> active = computeActive();

        // Adiacenza orientata node -> dipendenze attive.
        std::map<ModId, std::vector<ModId>> adj;
        std::set<std::pair<ModId, ModId>> selfLoop;  // dipendenza diretta su sé stessi
        for (const ModId& id : active) {
            std::vector<ModId> deps = activeDepsOf(*byId.at(id), active);
            for (const ModId& d : deps) {
                if (d == id) selfLoop.insert({id, id});
            }
            adj[id] = std::move(deps);
        }

        // Tarjan SCC (iterativo per evitare overflow dello stack su grafi grandi).
        std::unordered_map<ModId, int> index;
        std::unordered_map<ModId, int> lowlink;
        std::unordered_set<ModId> onStack;
        std::vector<ModId> tarjanStack;
        std::vector<std::vector<ModId>> sccs;
        int counter = 0;

        std::function<void(const ModId&)> strongconnect = [&](const ModId& v) {
            index[v] = counter;
            lowlink[v] = counter;
            ++counter;
            tarjanStack.push_back(v);
            onStack.insert(v);

            for (const ModId& w : adj[v]) {
                if (index.find(w) == index.end()) {
                    strongconnect(w);
                    lowlink[v] = std::min(lowlink[v], lowlink[w]);
                } else if (onStack.count(w) != 0) {
                    lowlink[v] = std::min(lowlink[v], index[w]);
                }
            }

            if (lowlink[v] == index[v]) {
                std::vector<ModId> scc;
                while (true) {
                    ModId w = tarjanStack.back();
                    tarjanStack.pop_back();
                    onStack.erase(w);
                    scc.push_back(w);
                    if (w == v) break;
                }
                sccs.push_back(std::move(scc));
            }
        };

        for (const ModId& id : active) {
            if (index.find(id) == index.end()) strongconnect(id);
        }

        // Marca come membri di ciclo le SCC con > 1 nodo o con self-loop.
        for (auto& scc : sccs) {
            const bool isCycle =
                scc.size() > 1 ||
                (scc.size() == 1 && selfLoop.count({scc.front(), scc.front()}) != 0);
            if (!isCycle) continue;

            std::vector<ModId> members = scc;
            std::sort(members.begin(), members.end());
            for (const ModId& m : members) {
                ExclusionReason r;
                r.kind = ExclusionReason::Kind::DependencyCycle;
                r.cycle = members;  // membri del ciclo specifico (Req 4.3)
                excludedReason.emplace(m, std::move(r));
            }
        }
    }

    // Le mod che dipendono dai cicli appena esclusi vanno escluse a loro volta
    // (come dipendenti di una mod esclusa, Req 4.2).
    propagateExclusion();

    // ------------------------------------------------------------------
    // Fase 3 — Kahn topological sort sul sottografo ora aciclico (Req 4.1).
    // Arco dipendenza -> dipendente: l'in-degree di un nodo è il numero delle
    // sue dipendenze attive. Tie-break deterministico: ModId lessicografico.
    // ------------------------------------------------------------------
    const std::set<ModId> active = computeActive();

    std::map<ModId, int> inDegree;
    std::map<ModId, std::vector<ModId>> dependents;  // dipendenza -> dipendenti

    for (const ModId& id : active) {
        std::vector<ModId> deps = activeDepsOf(*byId.at(id), active);
        inDegree[id] = static_cast<int>(deps.size());
        for (const ModId& d : deps) {
            dependents[d].push_back(id);
        }
    }

    std::priority_queue<ModId, std::vector<ModId>, std::greater<ModId>> readyQueue;
    for (const ModId& id : active) {
        if (inDegree[id] == 0) readyQueue.push(id);
    }

    while (!readyQueue.empty()) {
        ModId id = readyQueue.top();
        readyQueue.pop();
        plan.order.push_back(id);

        auto it = dependents.find(id);
        if (it != dependents.end()) {
            std::vector<ModId> deps = it->second;
            std::sort(deps.begin(), deps.end());
            for (const ModId& dependent : deps) {
                if (--inDegree[dependent] == 0) readyQueue.push(dependent);
            }
        }
    }

    // ------------------------------------------------------------------
    // Composizione dell'elenco esclusioni, ordinato per id della mod esclusa.
    // ------------------------------------------------------------------
    for (auto& [id, reason] : excludedReason) {
        plan.excluded.push_back(Exclusion{id, std::move(reason)});
    }

    return plan;
}

}  // namespace pulse::lifecycle
