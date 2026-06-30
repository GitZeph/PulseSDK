// tests/property9_topological_load_order_test.cpp
// Feature: pulse-sdk, Property 9 — Ordine topologico di caricamento.
// Validates: Requirements 4.1 (Requisito 4.1)
//
// Property 9 (design.md / Req 4.1): per ogni grafo di dipendenze ACICLICO
// generato in modo casuale, `DependencyResolver::resolve(...).order` deve:
//   (a) collocare ogni mod DOPO tutte le sue dipendenze (ordine topologico
//       valido, Req 4.1);
//   (b) contenere ESATTAMENTE le mod risolvibili — in un DAG con vincoli di
//       versione sempre soddisfatti nessuna mod è esclusa, quindi `order` è
//       una permutazione dell'intero insieme di input e `excluded` è vuoto;
//   (c) essere DETERMINISTICO rispetto all'ordine di input: permutare l'ordine
//       dei manifest in ingresso produce sempre lo stesso `order` in uscita.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si genera un DAG garantito aciclico costruendo `n` mod e consentendo a
//     ciascuna mod di indice `i` di dipendere SOLO da mod di indice `j < i`
//     (gli archi puntano sempre a indici inferiori ⇒ nessun ciclo possibile);
//   * tutte le versioni sono compatibili (vincolo `any()`), così nessuna mod
//     viene esclusa e `order` deve coincidere come insieme con l'input;
//   * si verifica l'invariante topologica a coppie (ogni dipendenza precede il
//     suo dipendente) e la completezza dell'insieme;
//   * si costruisce una permutazione casuale dell'input e si verifica che
//     `resolve` produca un `order` identico (determinismo, parte (c)).

#include "lifecycle/dependency_resolver.hpp"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using pulse::lifecycle::Dependency;
using pulse::lifecycle::DependencyResolver;
using pulse::lifecycle::LoadPlan;
using pulse::lifecycle::ModId;
using pulse::lifecycle::ResolverManifest;
using pulse::lifecycle::SemVer;
using pulse::lifecycle::VersionConstraint;

// Id stabile e univoco per l'indice del nodo nel grafo generato.
ModId makeId(int i) { return "mod" + std::to_string(i); }

// Indice della prima occorrenza di `id` in `order`, o -1 se assente.
int posOf(const std::vector<ModId>& order, const ModId& id) {
    auto it = std::find(order.begin(), order.end(), id);
    return it == order.end() ? -1 : static_cast<int>(it - order.begin());
}

// Genera un DAG casuale: `n` mod dove la mod di indice i può dipendere solo da
// mod di indice j < i. Gli archi puntano sempre a indici inferiori, quindi il
// grafo è aciclico per costruzione e tutte le dipendenze esistono e sono
// compatibili (vincolo any()).
std::vector<ResolverManifest> generateDag() {
    const int n = *rc::gen::inRange(0, 25).as("numero di mod");
    std::vector<ResolverManifest> mods;
    mods.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        ResolverManifest m;
        m.id = makeId(i);
        m.version = SemVer{1, 0, 0};
        if (i > 0) {
            // Sottoinsieme (senza duplicati) degli indici < i come dipendenze.
            const auto deps = *rc::gen::container<std::set<int>>(
                                   rc::gen::inRange(0, i))
                                   .as("dipendenze di " + m.id);
            for (int j : deps) {
                m.dependencies.push_back(
                    Dependency{makeId(j), VersionConstraint::any()});
            }
        }
        mods.push_back(std::move(m));
    }
    return mods;
}

// --- Property 9 — ordine topologico valido, completo e deterministico ------
// Feature: pulse-sdk, Property 9. Validates: Requirements 4.1.
RC_GTEST_PROP(Property9TopologicalLoadOrder,
              OrderIsTopologicalCompleteAndInputPermutationInvariant,
              ()) {
    const std::vector<ResolverManifest> mods = generateDag();
    const std::size_t n = mods.size();

    DependencyResolver resolver;
    const LoadPlan plan = resolver.resolve(mods);

    // (b) In un DAG con dipendenze sempre soddisfatte nessuna mod è esclusa:
    // `order` contiene esattamente tutte le mod di input.
    RC_ASSERT(plan.excluded.empty());
    RC_ASSERT(plan.order.size() == n);

    // `order` è una permutazione dell'insieme di input (nessun duplicato,
    // stessi elementi).
    std::set<ModId> inputIds;
    for (const auto& m : mods) inputIds.insert(m.id);
    std::set<ModId> orderIds(plan.order.begin(), plan.order.end());
    RC_ASSERT(orderIds.size() == plan.order.size());  // nessun duplicato
    RC_ASSERT(orderIds == inputIds);                   // stessi elementi

    // (a) Invariante topologica: ogni dipendenza precede il proprio dipendente.
    for (const auto& m : mods) {
        const int dependentPos = posOf(plan.order, m.id);
        RC_ASSERT(dependentPos >= 0);
        for (const auto& d : m.dependencies) {
            const int depPos = posOf(plan.order, d.id);
            RC_ASSERT(depPos >= 0);
            RC_ASSERT(depPos < dependentPos);
        }
    }

    // (c) Determinismo rispetto all'ordine di input: permutare i manifest in
    // ingresso non cambia l'`order` prodotto.
    std::vector<std::size_t> perm(n);
    for (std::size_t i = 0; i < n; ++i) perm[i] = i;
    const auto permKeys =
        *rc::gen::container<std::vector<int>>(n, rc::gen::arbitrary<int>())
             .as("chiavi di permutazione");
    std::stable_sort(perm.begin(), perm.end(),
                     [&](std::size_t l, std::size_t r) {
                         return permKeys[l] < permKeys[r];
                     });

    std::vector<ResolverManifest> permuted;
    permuted.reserve(n);
    for (std::size_t idx : perm) permuted.push_back(mods[idx]);

    const LoadPlan permutedPlan = resolver.resolve(permuted);
    RC_ASSERT(permutedPlan.order == plan.order);
}

}  // namespace
