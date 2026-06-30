// tests/hook_chain_test.cpp — unit test della catena di hook ordinata
// (task 6.1, Requisiti 3.1, 3.2, 3.3).
//
// Verifica la struttura dati e l'ordinamento di HookChain:
//   * inserimento ordinato per priority DESC (Req 3.2);
//   * tie-break per loadOrder ASC a parità di priorità (Req 3.3);
//   * saturazione della priorità nel dominio [0, 1000] (Req 3.2);
//   * default di priorità = 500 (Req 3.2);
//   * rimozione selettiva per owner che preserva gli altri hook (Req 2.4);
//   * determinismo: stessa sequenza indipendente dall'ordine di add (Req 3.3).
//
// L'esecuzione della catena (dispatch) è oggetto della task 6.3 e non è qui.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "hooking/hook_chain.hpp"

namespace {

using pulse::hooking::HookChain;
using pulse::hooking::HookNode;
using pulse::hooking::ModId;
using pulse::hooking::clampHookPriority;
using pulse::hooking::kHookPriorityDefault;
using pulse::hooking::kHookPriorityMax;
using pulse::hooking::kHookPriorityMin;

// Costruisce un nodo minimale (handler lasciato default-costruito: irrilevante
// per i test di solo ordinamento; l'esecuzione è coperta da dispatch_test).
HookNode makeNode(ModId owner, int priority, std::uint64_t loadOrder) {
    HookNode node;
    node.owner = std::move(owner);
    node.priority = priority;
    node.loadOrder = loadOrder;
    return node;
}

// Estrae la sequenza degli owner nell'ordine della catena.
std::vector<ModId> ownersInOrder(const HookChain& chain) {
    std::vector<ModId> owners;
    for (const auto& node : chain.orderedNodes()) {
        owners.push_back(node.owner);
    }
    return owners;
}

// --- default di HookNode: priorità 500 (Req 3.2) --------------------------
TEST(HookChain, NodeDefaultPriorityIs500) {
    HookNode node;
    EXPECT_EQ(node.priority, kHookPriorityDefault);
    EXPECT_EQ(node.priority, 500);
    EXPECT_EQ(node.loadOrder, 0u);
}

// --- ordinamento per priority DESC (Req 3.2) ------------------------------
TEST(HookChain, OrdersByPriorityDescending) {
    HookChain chain;
    chain.add(makeNode("low", 100, 0));
    chain.add(makeNode("high", 900, 1));
    chain.add(makeNode("mid", 500, 2));

    EXPECT_EQ(ownersInOrder(chain), (std::vector<ModId>{"high", "mid", "low"}));
}

// --- tie-break per loadOrder ASC a parità di priorità (Req 3.3) -----------
TEST(HookChain, TieBreaksByLoadOrderAscending) {
    HookChain chain;
    chain.add(makeNode("c", 500, 30));
    chain.add(makeNode("a", 500, 10));
    chain.add(makeNode("b", 500, 20));

    EXPECT_EQ(ownersInOrder(chain), (std::vector<ModId>{"a", "b", "c"}));
}

// --- priorità prevale sul loadOrder ---------------------------------------
TEST(HookChain, PriorityDominatesLoadOrder) {
    HookChain chain;
    // loadOrder basso ma priorità bassa -> deve finire dopo.
    chain.add(makeNode("earlyLowPrio", 200, 0));
    chain.add(makeNode("lateHighPrio", 800, 99));

    EXPECT_EQ(ownersInOrder(chain),
              (std::vector<ModId>{"lateHighPrio", "earlyLowPrio"}));
}

// --- l'ordine finale è indipendente dall'ordine di inserimento (Req 3.3) --
TEST(HookChain, OrderIndependentOfInsertionOrder) {
    HookChain forward;
    forward.add(makeNode("p900", 900, 1));
    forward.add(makeNode("p500a", 500, 2));
    forward.add(makeNode("p500b", 500, 5));
    forward.add(makeNode("p100", 100, 3));

    HookChain reverse;
    reverse.add(makeNode("p100", 100, 3));
    reverse.add(makeNode("p500b", 500, 5));
    reverse.add(makeNode("p500a", 500, 2));
    reverse.add(makeNode("p900", 900, 1));

    const std::vector<ModId> expected{"p900", "p500a", "p500b", "p100"};
    EXPECT_EQ(ownersInOrder(forward), expected);
    EXPECT_EQ(ownersInOrder(reverse), expected);
}

// --- saturazione della priorità nel dominio [0, 1000] (Req 3.2) -----------
TEST(HookChain, ClampsPriorityToValidDomain) {
    EXPECT_EQ(clampHookPriority(-50), kHookPriorityMin);
    EXPECT_EQ(clampHookPriority(5000), kHookPriorityMax);
    EXPECT_EQ(clampHookPriority(750), 750);

    HookChain chain;
    chain.add(makeNode("tooHigh", 5000, 0));   // -> 1000
    chain.add(makeNode("tooLow", -10, 1));      // -> 0
    chain.add(makeNode("mid", 500, 2));

    const auto& nodes = chain.orderedNodes();
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[0].owner, "tooHigh");
    EXPECT_EQ(nodes[0].priority, kHookPriorityMax);
    EXPECT_EQ(nodes[1].owner, "mid");
    EXPECT_EQ(nodes[2].owner, "tooLow");
    EXPECT_EQ(nodes[2].priority, kHookPriorityMin);
}

// --- inserimento stabile a parità di priority e loadOrder -----------------
TEST(HookChain, StableForEqualKeys) {
    HookChain chain;
    chain.add(makeNode("first", 500, 7));
    chain.add(makeNode("second", 500, 7));
    chain.add(makeNode("third", 500, 7));

    EXPECT_EQ(ownersInOrder(chain),
              (std::vector<ModId>{"first", "second", "third"}));
}

// --- rimozione selettiva per owner (Req 2.4) ------------------------------
TEST(HookChain, RemoveSelectiveByOwnerPreservesOthers) {
    HookChain chain;
    chain.add(makeNode("keepA", 900, 0));
    chain.add(makeNode("dropMe", 700, 1));
    chain.add(makeNode("keepB", 500, 2));

    const std::size_t removed = chain.remove("dropMe");
    EXPECT_EQ(removed, 1u);
    EXPECT_EQ(chain.size(), 2u);
    EXPECT_FALSE(chain.contains("dropMe"));
    EXPECT_EQ(ownersInOrder(chain), (std::vector<ModId>{"keepA", "keepB"}));
}

// --- remove elimina TUTTI gli hook di una mod -----------------------------
TEST(HookChain, RemoveDropsAllHooksOfOwner) {
    HookChain chain;
    chain.add(makeNode("modX", 900, 0));
    chain.add(makeNode("modY", 800, 1));
    chain.add(makeNode("modX", 100, 2));

    const std::size_t removed = chain.remove("modX");
    EXPECT_EQ(removed, 2u);
    EXPECT_EQ(ownersInOrder(chain), (std::vector<ModId>{"modY"}));
}

// --- remove di un owner assente non altera la catena ----------------------
TEST(HookChain, RemoveAbsentOwnerIsNoOp) {
    HookChain chain;
    chain.add(makeNode("a", 500, 0));
    chain.add(makeNode("b", 400, 1));

    const std::size_t removed = chain.remove("ghost");
    EXPECT_EQ(removed, 0u);
    EXPECT_EQ(chain.size(), 2u);
    EXPECT_EQ(ownersInOrder(chain), (std::vector<ModId>{"a", "b"}));
}

// --- l'invariante di ordinamento sopravvive a remove ----------------------
TEST(HookChain, OrderingInvariantHoldsAfterRemove) {
    HookChain chain;
    chain.add(makeNode("p900", 900, 0));
    chain.add(makeNode("p500", 500, 1));
    chain.add(makeNode("p700", 700, 2));
    chain.add(makeNode("p300", 300, 3));

    chain.remove("p700");

    const auto& nodes = chain.orderedNodes();
    for (std::size_t i = 1; i < nodes.size(); ++i) {
        EXPECT_TRUE(HookChain::precedes(nodes[i - 1], nodes[i]))
            << "violazione dell'ordinamento a indice " << i;
    }
    EXPECT_EQ(ownersInOrder(chain),
              (std::vector<ModId>{"p900", "p500", "p300"}));
}

// --- supporta una catena ampia (>=64 hook) mantenendo l'ordine (Req 3.1) --
TEST(HookChain, SupportsLargeChainOrdered) {
    HookChain chain;
    // Inserisce 64 hook con priorità decrescente in ordine di inserimento
    // "mescolato" (loadOrder crescente, priority decrescente al contrario).
    constexpr int kCount = 64;
    for (int i = 0; i < kCount; ++i) {
        // priorità = i*10 (saturata a 1000), loadOrder = i.
        chain.add(makeNode("m" + std::to_string(i), i * 10,
                           static_cast<std::uint64_t>(i)));
    }

    EXPECT_EQ(chain.size(), static_cast<std::size_t>(kCount));
    const auto& nodes = chain.orderedNodes();
    for (std::size_t i = 1; i < nodes.size(); ++i) {
        EXPECT_TRUE(HookChain::precedes(nodes[i - 1], nodes[i]))
            << "violazione dell'ordinamento a indice " << i;
    }
}

// --- empty/clear ----------------------------------------------------------
TEST(HookChain, EmptyAndClear) {
    HookChain chain;
    EXPECT_TRUE(chain.empty());

    chain.add(makeNode("a", 500, 0));
    EXPECT_FALSE(chain.empty());

    chain.clear();
    EXPECT_TRUE(chain.empty());
    EXPECT_EQ(chain.size(), 0u);
}

}  // namespace
