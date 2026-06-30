// tests/property11_dependency_cycle_isolation_test.cpp
// Feature: pulse-sdk, Property 11 — Rilevamento e isolamento dei cicli.
// Validates: Requirements 4.3 (Requisito 4.3)
//
// Property 11 (design.md / Req 4.3): per ogni grafo di dipendenze contenente
// almeno un ciclo, tutte e sole le mod coinvolte in un ciclo sono escluse dal
// caricamento e segnalate (kind == DependencyCycle, con l'elenco dei membri),
// mentre il sottografo aciclico restante è caricato in ordine topologico
// valido.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si costruisce un grafo randomizzato che CONTIENE INTENZIONALMENTE uno o
//     più cicli di dipendenze. Ogni ciclo è un gruppo disgiunto di k>=2 nodi
//     concatenati ad anello (g_0->g_1->...->g_{k-1}->g_0); essendo gruppi
//     disgiunti senza archi incrociati, ogni gruppo è esattamente una SCC e i
//     suoi membri sono noti al test;
//   * in parallelo si genera un insieme di mod "libere" che NON appartengono ad
//     alcun ciclo e NON dipendono da alcun membro di un ciclo (dipendono solo
//     da altre mod libere di indice inferiore → sottografo aciclico): queste
//     devono caricarsi tutte;
//   * l'ordine dei manifest in ingresso è mescolato da una permutazione casuale
//     per esercitare l'indipendenza dall'ordine d'ingresso del resolver.
//
// Invarianti verificate ad ogni iterazione:
//   (1) ogni membro di un ciclo iniettato è escluso con kind==DependencyCycle e
//       l'insieme `cycle` riportato coincide con i membri del proprio ciclo;
//   (2) nessun membro di un ciclo compare in `order`;
//   (3) tutte le mod libere (acicliche, indipendenti dai cicli) compaiono in
//       `order`, ed è l'unico contenuto di `order` (|order| == #mod libere);
//   (4) per ogni dipendenza tra mod libere, la dipendenza precede il dipendente
//       in `order` (ordine topologico valido sul sottografo superstite).
//
// Tutte le dipendenze usano vincolo any() e versione uniforme 1.0.0 così da
// isolare il fenomeno del ciclo (nessuna esclusione per dipendenza
// mancante/incompatibile interferisce con le asserzioni).

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <set>
#include <string>
#include <vector>

#include "lifecycle/dependency_resolver.hpp"

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

int posOf(const std::vector<ModId>& order, const ModId& id) {
    auto it = std::find(order.begin(), order.end(), id);
    return it == order.end() ? -1 : static_cast<int>(it - order.begin());
}

bool inOrder(const std::vector<ModId>& order, const ModId& id) {
    return posOf(order, id) >= 0;
}

const Exclusion* findExclusion(const LoadPlan& plan, const ModId& id) {
    for (const auto& e : plan.excluded) {
        if (e.mod == id) return &e;
    }
    return nullptr;
}

// --- Property 11 — i cicli sono isolati, il resto carica topologicamente ----
// Feature: pulse-sdk, Property 11. Validates: Requirements 4.3.
RC_GTEST_PROP(Property11DependencyCycleIsolation,
              CyclesExcludedAndAcyclicRestLoads,
              ()) {
    // ---- Cicli iniettati: gruppi disgiunti di k>=2 nodi ad anello ----------
    const int numCycles = *rc::gen::inRange(1, 6).as("numero di cicli iniettati");

    std::vector<ResolverManifest> mods;
    std::vector<std::vector<ModId>> cycleGroups;  // membri noti per ciclo

    for (int ci = 0; ci < numCycles; ++ci) {
        const int k = *rc::gen::inRange(2, 6).as("dimensione del ciclo");
        std::vector<ModId> group;
        group.reserve(static_cast<std::size_t>(k));
        for (int p = 0; p < k; ++p) {
            group.push_back("cyc" + std::to_string(ci) + "_" + std::to_string(p));
        }
        // Anello: g_p dipende da g_{(p+1) % k} → ciclo diretto, SCC = gruppo.
        for (int p = 0; p < k; ++p) {
            Dependency d{group[static_cast<std::size_t>((p + 1) % k)],
                         VersionConstraint::any()};
            mods.push_back(
                ResolverManifest{group[static_cast<std::size_t>(p)],
                                 SemVer{1, 0, 0},
                                 std::vector<Dependency>{d}});
        }
        cycleGroups.push_back(std::move(group));
    }

    // ---- Mod libere: sottografo aciclico, indipendente dai cicli -----------
    const int numFree = *rc::gen::inRange(0, 9).as("numero di mod libere");
    std::vector<ModId> freeIds;
    freeIds.reserve(static_cast<std::size_t>(numFree));
    for (int i = 0; i < numFree; ++i) {
        freeIds.push_back("free" + std::to_string(i));
    }

    // Le dipendenze di ciascuna mod libera puntano SOLO a mod libere di indice
    // strettamente inferiore (aciclico per costruzione, nessun arco verso i
    // cicli). Le memorizziamo per verificare l'ordine topologico dopo.
    std::vector<std::vector<ModId>> freeDeps(static_cast<std::size_t>(numFree));
    for (int i = 0; i < numFree; ++i) {
        std::vector<Dependency> deps;
        for (int j = 0; j < i; ++j) {
            const bool include = *rc::gen::arbitrary<bool>();
            if (include) {
                deps.push_back(Dependency{freeIds[static_cast<std::size_t>(j)],
                                          VersionConstraint::any()});
                freeDeps[static_cast<std::size_t>(i)].push_back(
                    freeIds[static_cast<std::size_t>(j)]);
            }
        }
        mods.push_back(ResolverManifest{freeIds[static_cast<std::size_t>(i)],
                                        SemVer{1, 0, 0}, std::move(deps)});
    }

    // ---- Mescola l'ordine dei manifest (indipendenza dall'ordine) ----------
    const std::size_t total = mods.size();
    std::vector<std::size_t> perm(total);
    std::iota(perm.begin(), perm.end(), std::size_t{0});
    const auto shuffleKeys =
        *rc::gen::container<std::vector<int>>(total, rc::gen::arbitrary<int>())
             .as("chiavi di permutazione dei manifest");
    std::stable_sort(perm.begin(), perm.end(),
                     [&](std::size_t l, std::size_t r) {
                         return shuffleKeys[l] < shuffleKeys[r];
                     });
    std::vector<ResolverManifest> shuffled;
    shuffled.reserve(total);
    for (std::size_t idx : perm) shuffled.push_back(mods[idx]);

    // ---- Risoluzione -------------------------------------------------------
    DependencyResolver resolver;
    const LoadPlan plan = resolver.resolve(shuffled);

    // (1)+(2) Ogni membro di ciclo è escluso come DependencyCycle, riporta
    // l'esatto insieme dei membri del proprio ciclo e non compare in order.
    for (const auto& group : cycleGroups) {
        const std::set<ModId> groupSet(group.begin(), group.end());
        for (const ModId& member : group) {
            RC_ASSERT(!inOrder(plan.order, member));

            const Exclusion* ex = findExclusion(plan, member);
            RC_ASSERT(ex != nullptr);
            RC_ASSERT(ex->reason.kind ==
                      ExclusionReason::Kind::DependencyCycle);

            const std::set<ModId> reported(ex->reason.cycle.begin(),
                                            ex->reason.cycle.end());
            RC_ASSERT(reported == groupSet);
        }
    }

    // (3) Tutte le mod libere caricano, e sono l'UNICO contenuto di order.
    for (const ModId& id : freeIds) {
        RC_ASSERT(inOrder(plan.order, id));
    }
    RC_ASSERT(plan.order.size() == static_cast<std::size_t>(numFree));

    // (4) Ordine topologico valido sul sottografo libero: ogni dipendenza
    // precede il dipendente.
    for (int i = 0; i < numFree; ++i) {
        for (const ModId& d : freeDeps[static_cast<std::size_t>(i)]) {
            RC_ASSERT(posOf(plan.order, d) <
                      posOf(plan.order, freeIds[static_cast<std::size_t>(i)]));
        }
    }
}

}  // namespace
