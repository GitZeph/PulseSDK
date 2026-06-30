// tests/property2_hook_chain_determinism_test.cpp
// Feature: pulse-sdk, Property 2 — Determinismo dell'ordinamento della catena
// di hook.
// Validates: Requirements 3.2, 3.3 (Requisiti 3.2, 3.3)
//
// Property 2 (design.md / Req 3.2, 3.3): per ogni insieme randomizzato di
// HookNode (owner, priority, loadOrder casuali) la sequenza prodotta da
// `HookChain::orderedNodes()` deve essere:
//   (a) ORDINATA per priority DECRESCENTE e, a parità, per loadOrder
//       CRESCENTE (Req 3.2);
//   (b) DETERMINISTICA / RIPETIBILE — costruire la catena inserendo gli
//       stessi nodi in un ordine di inserimento qualsiasi (mescolato) produce
//       sempre la medesima sequenza finale, così lo stesso insieme di mod
//       genera sempre la stessa sequenza di esecuzione (Req 3.3).
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si genera un insieme randomizzato di nodi; ciascun nodo riceve un
//     `loadOrder` UNIVOCO (l'ordine di caricamento risolto dal Dependency
//     Resolver è univoco per mod, Req 3.3), così la relazione d'ordine è
//     totale e l'ordinamento è completamente determinato dai dati;
//   * le priorità generate includono valori fuori dal dominio [0, 1000] per
//     esercitare anche la saturazione applicata da `add` (Req 3.2);
//   * si costruiscono due catene con gli STESSI nodi ma ordini di inserimento
//     diversi (uno "in avanti", uno mescolato da una permutazione casuale) e
//     si verifica che `orderedNodes()` coincida elemento per elemento;
//   * si verifica inoltre l'invariante a coppie `precedes(prev, next)` e che
//     la sequenza sia ordinata per priority DESC poi loadOrder ASC.
//
// L'handler dei nodi è lasciato default-costruito: l'ordinamento dipende solo
// da (priority, loadOrder) e il test resta indipendente dalla firma di
// HandlerFn (che la task 6.3 — dispatch — potrebbe cambiare).

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <string>
#include <tuple>
#include <vector>

#include "hooking/hook_chain.hpp"

namespace {

using pulse::hooking::clampHookPriority;
using pulse::hooking::HookChain;
using pulse::hooking::HookNode;
using pulse::hooking::ModId;

// Descrittore di un nodo generato: owner + priorità grezza (il loadOrder è
// assegnato dal test in modo univoco per garantire un ordine totale).
struct NodeSpec {
    std::string owner;
    int priority;
};

// Confronto strutturale di un nodo: (owner, priority, loadOrder). L'handler è
// escluso (non confrontabile e irrilevante per l'ordinamento).
using NodeKey = std::tuple<ModId, int, std::uint64_t>;

NodeKey keyOf(const HookNode& node) {
    return {node.owner, node.priority, node.loadOrder};
}

std::vector<NodeKey> keysOf(const HookChain& chain) {
    std::vector<NodeKey> keys;
    keys.reserve(chain.size());
    for (const auto& node : chain.orderedNodes()) {
        keys.push_back(keyOf(node));
    }
    return keys;
}

// Costruisce i nodi dai descrittori assegnando loadOrder = indice (univoco).
std::vector<HookNode> buildNodes(const std::vector<NodeSpec>& specs) {
    std::vector<HookNode> nodes;
    nodes.reserve(specs.size());
    for (std::size_t i = 0; i < specs.size(); ++i) {
        HookNode node;
        node.owner = specs[i].owner;
        node.priority = specs[i].priority;
        node.loadOrder = static_cast<std::uint64_t>(i);
        // node.handler resta default-costruito (vuoto).
        nodes.push_back(std::move(node));
    }
    return nodes;
}

// Inserisce i nodi nella catena seguendo l'ordine indicato da `order`.
HookChain buildChain(const std::vector<HookNode>& nodes,
                     const std::vector<std::size_t>& order) {
    HookChain chain;
    for (std::size_t idx : order) {
        chain.add(nodes[idx]);
    }
    return chain;
}

// --- Property 2 (b) — indipendenza dall'ordine di inserimento -------------
// Feature: pulse-sdk, Property 2. Validates: Requirements 3.2, 3.3.
//
// Stessi nodi inseriti in ordine "in avanti" e in un ordine mescolato da una
// permutazione casuale: la sequenza ordinata finale deve essere identica
// (determinismo, Req 3.3) e rispettare l'invariante di ordinamento (Req 3.2).
RC_GTEST_PROP(Property2HookChainDeterminism,
              OrderIsInsertionIndependentAndSorted,
              ()) {
    // Insieme randomizzato di nodi: dimensione fino a 64 hook concatenati.
    const auto specs = *rc::gen::container<std::vector<NodeSpec>>(
        rc::gen::construct<NodeSpec>(
            rc::gen::arbitrary<std::string>(),
            // Include valori fuori [0, 1000] per esercitare la saturazione.
            rc::gen::inRange(-200, 1201)))
                           .as("nodi generati");

    const std::vector<HookNode> nodes = buildNodes(specs);
    const std::size_t n = nodes.size();

    // Ordine "in avanti": 0, 1, ..., n-1.
    std::vector<std::size_t> forwardOrder(n);
    std::iota(forwardOrder.begin(), forwardOrder.end(), std::size_t{0});

    // Ordine mescolato: permutazione indotta da chiavi casuali (copre molti
    // ordini di inserimento, incluso il rovesciato).
    std::vector<std::size_t> shuffledOrder = forwardOrder;
    const auto shuffleKeys =
        *rc::gen::container<std::vector<int>>(n, rc::gen::arbitrary<int>())
             .as("chiavi di permutazione");
    std::stable_sort(shuffledOrder.begin(), shuffledOrder.end(),
                     [&](std::size_t l, std::size_t r) {
                         return shuffleKeys[l] < shuffleKeys[r];
                     });

    const HookChain forwardChain = buildChain(nodes, forwardOrder);
    const HookChain shuffledChain = buildChain(nodes, shuffledOrder);

    // (b) Determinismo: stessa sequenza indipendente dall'ordine di add.
    RC_ASSERT(keysOf(forwardChain) == keysOf(shuffledChain));

    // La catena contiene esattamente i nodi inseriti.
    RC_ASSERT(forwardChain.size() == n);

    // (a) Invariante di ordinamento: priority DESC, poi loadOrder ASC.
    const auto& ordered = forwardChain.orderedNodes();
    for (std::size_t i = 1; i < ordered.size(); ++i) {
        const HookNode& prev = ordered[i - 1];
        const HookNode& cur = ordered[i];
        // loadOrder univoco => relazione d'ordine stretta e totale.
        RC_ASSERT(HookChain::precedes(prev, cur));
        if (prev.priority == cur.priority) {
            RC_ASSERT(prev.loadOrder < cur.loadOrder);  // tie-break ASC
        } else {
            RC_ASSERT(prev.priority > cur.priority);     // priority DESC
        }
        // Le priorità memorizzate sono nel dominio saturato [0, 1000].
        RC_ASSERT(cur.priority >= pulse::hooking::kHookPriorityMin);
        RC_ASSERT(cur.priority <= pulse::hooking::kHookPriorityMax);
    }
}

// --- Property 2 (b) — ripetibilità tra costruzioni ripetute ---------------
// Feature: pulse-sdk, Property 2. Validates: Requirements 3.2, 3.3.
//
// Costruire due volte la catena con lo stesso insieme di nodi e lo stesso
// ordine di inserimento produce la medesima sequenza: lo stesso insieme di mod
// genera sempre la stessa sequenza di esecuzione tra run successivi (Req 3.3).
RC_GTEST_PROP(Property2HookChainDeterminism,
              RepeatedBuildsYieldIdenticalSequence,
              ()) {
    const auto specs = *rc::gen::container<std::vector<NodeSpec>>(
        rc::gen::construct<NodeSpec>(
            rc::gen::arbitrary<std::string>(),
            rc::gen::inRange(-200, 1201)))
                           .as("nodi generati");

    const std::vector<HookNode> nodes = buildNodes(specs);
    const std::size_t n = nodes.size();

    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), std::size_t{0});

    const HookChain first = buildChain(nodes, order);
    const HookChain second = buildChain(nodes, order);

    RC_ASSERT(keysOf(first) == keysOf(second));
}

}  // namespace
