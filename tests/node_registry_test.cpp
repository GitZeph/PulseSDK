// node_registry_test.cpp — unit test di pulse::NodeRegistry (task 28.1).
//
// Copre le semantiche osservabili del Requisito 8 (UI / node-ID):
//   * setId memorizza un node-ID stabile, ≤ 256 caratteri (Req 8.1);
//   * byId restituisce il nodo associato (Req 8.2) o std::nullopt quando l'id
//     è assente, lasciando il registro invariato (Req 8.3);
//   * un node-ID più lungo di 256 caratteri è rifiutato (Req 8.1);
//   * node-ID distinti di mod diverse sono tutti preservati (Req 8.6);
//   * una collisione (stesso id, mod diverse) mantiene entrambi i nodi
//     distinti associando ciascun id alla sua mod e segnala la collisione,
//     senza sovrascrivere alcun nodo (Req 8.7).
//
// `Node` è un tipo opaco del gioco: il registro conserva solo puntatori e non
// li dereferenzia mai, quindi qui usiamo indirizzi fittizi distinti come nodi.
#include <pulse/ui_nodes.hpp>

#include <string>

#include <gtest/gtest.h>

using pulse::NodeRegistry;
using pulse::Node;
using pulse::SetIdStatus;

namespace {

// Crea N puntatori-nodo fittizi distinti. Non vengono mai dereferenziati.
struct FakeNodes {
    // Buffer di byte: ogni elemento ha un indirizzo distinto e stabile.
    unsigned char storage[16]{};
    Node* at(std::size_t i) {
        return reinterpret_cast<Node*>(&storage[i]);
    }
};

}  // namespace

// Req 8.1: setId memorizza un node-ID stabile; idOf/byId restituiscono lo
// stesso nodo e lo stesso id su letture ripetute.
TEST(NodeRegistryTest, SetIdStoresStableId) {
    NodeRegistry reg;
    FakeNodes nodes;
    Node* n = nodes.at(0);

    auto res = reg.setId(n, "main-menu/play", "mod.alpha");
    EXPECT_EQ(res.status, SetIdStatus::Assigned);
    EXPECT_TRUE(res.assigned());
    EXPECT_FALSE(res.collision());

    // Stabilità: l'id assegnato non cambia fra letture successive.
    auto id1 = reg.idOf(n);
    auto id2 = reg.idOf(n);
    ASSERT_TRUE(id1.has_value());
    EXPECT_EQ(*id1, "main-menu/play");
    EXPECT_EQ(id1, id2);

    // byId restituisce esattamente il nodo registrato (Req 8.2).
    auto found = reg.byId("main-menu/play");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, n);

    // Ri-assegnare lo stesso id allo stesso nodo è idempotente (stabile).
    auto again = reg.setId(n, "main-menu/play", "mod.alpha");
    EXPECT_EQ(again.status, SetIdStatus::AlreadyAssigned);
    EXPECT_EQ(reg.count("main-menu/play"), 1u);
}

// Req 8.3: byId su un id assente restituisce std::nullopt senza modificare il
// registro.
TEST(NodeRegistryTest, ByIdAbsentReturnsNullopt) {
    NodeRegistry reg;
    FakeNodes nodes;

    EXPECT_FALSE(reg.contains("missing"));
    EXPECT_EQ(reg.byId("missing"), std::nullopt);

    // Anche dopo aver registrato altri id, un id assente resta nullopt e il
    // registro non è alterato dalla richiesta.
    reg.setId(nodes.at(0), "present", "mod.alpha");
    EXPECT_EQ(reg.byId("still-missing"), std::nullopt);
    EXPECT_EQ(reg.count("still-missing"), 0u);
    EXPECT_TRUE(reg.contains("present"));
}

// Req 8.1: un node-ID di esattamente 256 caratteri è accettato; uno di 257 è
// rifiutato e il registro resta invariato.
TEST(NodeRegistryTest, IdLengthBoundaryRejectedOver256) {
    NodeRegistry reg;
    FakeNodes nodes;

    const std::string id256(256, 'a');
    auto ok = reg.setId(nodes.at(0), id256, "mod.alpha");
    EXPECT_EQ(ok.status, SetIdStatus::Assigned);
    EXPECT_TRUE(reg.byId(id256).has_value());

    const std::string id257(257, 'b');
    auto rejected = reg.setId(nodes.at(1), id257, "mod.alpha");
    EXPECT_EQ(rejected.status, SetIdStatus::RejectedTooLong);
    EXPECT_TRUE(rejected.rejected());
    EXPECT_FALSE(rejected.assigned());

    // Registro invariato: l'id troppo lungo non è stato memorizzato.
    EXPECT_FALSE(reg.contains(id257));
    EXPECT_EQ(reg.byId(id257), std::nullopt);
    EXPECT_EQ(reg.idOf(nodes.at(1)), std::nullopt);
}

// Req 8.6: node-ID distinti assegnati da mod diverse alla stessa interfaccia
// sono tutti preservati, senza sovrascritture.
TEST(NodeRegistryTest, DistinctIdsFromDifferentModsPreserved) {
    NodeRegistry reg;
    FakeNodes nodes;
    Node* a = nodes.at(0);
    Node* b = nodes.at(1);

    EXPECT_EQ(reg.setId(a, "alpha-button", "mod.alpha").status,
              SetIdStatus::Assigned);
    EXPECT_EQ(reg.setId(b, "beta-button", "mod.beta").status,
              SetIdStatus::Assigned);

    // Entrambi gli id sono preservati e puntano ai rispettivi nodi.
    auto fa = reg.byId("alpha-button");
    auto fb = reg.byId("beta-button");
    ASSERT_TRUE(fa.has_value());
    ASSERT_TRUE(fb.has_value());
    EXPECT_EQ(*fa, a);
    EXPECT_EQ(*fb, b);
    EXPECT_FALSE(reg.hasCollision("alpha-button"));
    EXPECT_FALSE(reg.hasCollision("beta-button"));
}

// Req 8.7: due mod assegnano lo STESSO node-ID a nodi diversi. Entrambi i nodi
// restano distinti, ciascun id è associato alla sua mod, la collisione è
// segnalata e nessun nodo viene sovrascritto.
TEST(NodeRegistryTest, SameIdCollisionKeepsBothDistinctWithoutOverwrite) {
    NodeRegistry reg;
    FakeNodes nodes;
    Node* fromAlpha = nodes.at(0);
    Node* fromBeta = nodes.at(1);

    // mod.alpha assegna "shared-id".
    auto first = reg.setId(fromAlpha, "shared-id", "mod.alpha");
    EXPECT_EQ(first.status, SetIdStatus::Assigned);
    EXPECT_FALSE(first.collision());

    // mod.beta assegna lo stesso "shared-id" a un nodo diverso -> collisione.
    auto second = reg.setId(fromBeta, "shared-id", "mod.beta");
    EXPECT_EQ(second.status, SetIdStatus::Collision);
    EXPECT_TRUE(second.collision());
    EXPECT_TRUE(second.assigned());        // anche il secondo nodo è mantenuto.
    EXPECT_FALSE(second.message.empty());  // la collisione è segnalata.

    // Entrambi i nodi restano distinti e nessuno è stato sovrascritto.
    EXPECT_TRUE(reg.hasCollision("shared-id"));
    EXPECT_EQ(reg.count("shared-id"), 2u);

    // Ciascun id resta associato alla mod che lo ha definito (Req 8.7).
    auto alphaNode = reg.byId("shared-id", "mod.alpha");
    auto betaNode = reg.byId("shared-id", "mod.beta");
    ASSERT_TRUE(alphaNode.has_value());
    ASSERT_TRUE(betaNode.has_value());
    EXPECT_EQ(*alphaNode, fromAlpha);
    EXPECT_EQ(*betaNode, fromBeta);
    EXPECT_NE(*alphaNode, *betaNode);

    // Il nodo registrato per primo non è stato sovrascritto: byId(id) lo
    // restituisce ancora.
    auto primary = reg.byId("shared-id");
    ASSERT_TRUE(primary.has_value());
    EXPECT_EQ(*primary, fromAlpha);
}
