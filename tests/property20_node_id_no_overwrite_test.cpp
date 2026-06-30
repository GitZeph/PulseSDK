// tests/property20_node_id_no_overwrite_test.cpp
// Feature: pulse-sdk, Property 20 — Assenza di sovrascrittura dei node-ID.
// Validates: Requirements 8.6, 8.7 (Requisiti 8.6, 8.7)
//
// Property 20 (design.md / Req 8.6, 8.7): per ogni coppia di mod che assegnano
// node-ID a nodi della stessa interfaccia, i node-ID DISTINTI sono tutti
// preservati; se due mod assegnano lo STESSO node-ID, entrambi i nodi restano
// distinti associati alla rispettiva mod e la collisione è segnalata, SENZA
// sovrascrivere alcun nodo.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si genera un numero di mod e un insieme randomizzato di assegnazioni
//     (modId, nodeAddress, id); ogni nodo è modellato da un indirizzo fittizio
//     DISTINTO (offset in un buffer), perché `Node` è un tipo opaco del gioco
//     che il registro non dereferenzia mai;
//   * gli id sono scelti da un piccolo alfabeto per forzare frequentemente
//     collisioni (stesso id da mod diverse);
//   * si calcola lo stato atteso direttamente dall'insieme generato e si
//     verificano gli invarianti contro il NodeRegistry reale.
//
// Invarianti verificati:
//   (a) ogni node-ID DISTINTO assegnato resta presente nel registro;
//   (b) per la PRIMA assegnazione (modId, nodo) di un dato id, `byId(id)`
//       restituisce sempre quel nodo: nessuna assegnazione successiva
//       sovrascrive il primo registrante (Req 8.7 — nessun overwrite);
//   (c) ogni coppia (id, modId) effettivamente assegnata resta risolvibile via
//       `byId(id, modId)` e punta al nodo che quella mod ha registrato per
//       prima sotto quell'id;
//   (d) quando due nodi distinti condividono un id, la collisione è segnalata
//       (`hasCollision(id)` true, `count(id) >= 2`) e i nodi restano distinti;
//   (e) il numero di nodi distinti registrati sotto un id (`count(id)`) è pari
//       al numero di nodi distinti effettivamente assegnati a quell'id.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <pulse/ui_nodes.hpp>

namespace {

using pulse::Node;
using pulse::NodeRegistry;
using pulse::SetIdStatus;

// Descrittore di un'assegnazione generata: indice della mod proprietaria,
// indice del nodo (indirizzo fittizio distinto) e indice del node-ID
// nell'alfabeto. Gli indici sono risolti in valori stabili dal test.
struct Assignment {
    int modIdx;
    int nodeIdx;
    int idIdx;
};

// Identità mod stabile a partire dall'indice.
std::string modName(int idx) { return "mod" + std::to_string(idx); }

// node-ID stabile a partire dall'indice (alfabeto piccolo per forzare collisioni).
std::string nodeId(int idx) { return "node-id-" + std::to_string(idx); }

// --- Property 20 — assenza di sovrascrittura dei node-ID ------------------
// Feature: pulse-sdk, Property 20. Validates: Requirements 8.6, 8.7.
RC_GTEST_PROP(Property20NodeIdNoOverwrite,
              DistinctIdsPreservedAndCollisionsKeepBothWithoutOverwrite,
              ()) {
    // Domini: numero di mod, di nodi fittizi distinti e di node-ID disponibili.
    const int numMods = *rc::gen::inRange(2, 5).as("numero di mod");
    const int numNodes = *rc::gen::inRange(2, 12).as("numero di nodi distinti");
    const int numIds = *rc::gen::inRange(1, 5).as("numero di node-ID distinti");

    // Buffer di byte: ogni indice ha un indirizzo distinto e stabile da usare
    // come `Node*` fittizio. Mai dereferenziato dal registro.
    std::vector<unsigned char> storage(static_cast<std::size_t>(numNodes), 0);
    auto nodeAt = [&](int idx) -> Node* {
        return reinterpret_cast<Node*>(&storage[static_cast<std::size_t>(idx)]);
    };

    // Insieme randomizzato di assegnazioni (modId, nodeAddress, id). Dimensione
    // libera (anche vuota) per esercitare anche i casi degeneri.
    const auto assignments = *rc::gen::container<std::vector<Assignment>>(
        rc::gen::construct<Assignment>(
            rc::gen::inRange(0, numMods),
            rc::gen::inRange(0, numNodes),
            rc::gen::inRange(0, numIds)))
                                  .as("assegnazioni (modIdx, nodeIdx, idIdx)");

    NodeRegistry reg;

    // Stato atteso ricavato dall'insieme generato, rispettando l'ordine di
    // inserimento (necessario per l'invariante "nessuna sovrascrittura").
    //
    // Il registro crea una NUOVA voce `(modId, nodo)` sotto un id solo quando
    // quel nodo non è ancora presente sotto l'id; se il nodo è già presente
    // (anche per una mod diversa) l'esito è idempotente (AlreadyAssigned) e
    // nessuna voce viene aggiunta. Modelliamo quindi le voci ESATTAMENTE come
    // il registro: per ciascun id, la sequenza ordinata delle voci create.
    //
    // expectedNodesForId[id]  -> insieme dei nodi distinti già presenti sotto id.
    // expectedEntries[id]     -> sequenza ordinata di voci (modIdx, nodeIdx).
    // assignedNodeIds         -> insieme dei node-ID effettivamente assegnati.
    std::map<int, std::set<int>> expectedNodesForId;
    std::map<int, std::vector<std::pair<int, int>>> expectedEntries;
    std::set<int> assignedNodeIds;

    for (const Assignment& a : assignments) {
        Node* node = nodeAt(a.nodeIdx);
        const auto res = reg.setId(node, nodeId(a.idIdx), modName(a.modIdx));

        // Calcola lo stato atteso PRIMA di registrare l'effetto, così da
        // confrontare lo status restituito con la nostra previsione.
        auto& nodesForId = expectedNodesForId[a.idIdx];
        const bool nodeAlreadyUnderId = nodesForId.count(a.nodeIdx) > 0;
        const bool idAlreadyUsed = !nodesForId.empty();

        if (nodeAlreadyUnderId) {
            // Ri-assegnazione idempotente: il nodo è già presente sotto l'id
            // (indipendentemente dalla mod). Nessuna nuova voce.
            RC_ASSERT(res.status == SetIdStatus::AlreadyAssigned);
        } else if (idAlreadyUsed) {
            // id già usato da un nodo DIVERSO -> collisione segnalata, ma il
            // nodo è comunque mantenuto distinto (Req 8.7).
            RC_ASSERT(res.status == SetIdStatus::Collision);
            RC_ASSERT(res.collision());
            RC_ASSERT(res.assigned());
            RC_ASSERT(!res.message.empty());
        } else {
            // Primo uso dell'id -> assegnazione pulita.
            RC_ASSERT(res.status == SetIdStatus::Assigned);
        }

        // Aggiorna lo stato atteso. La voce è creata solo per un nodo NUOVO
        // sotto l'id (specchio della logica del registro).
        if (!nodeAlreadyUnderId) {
            expectedEntries[a.idIdx].emplace_back(a.modIdx, a.nodeIdx);
            nodesForId.insert(a.nodeIdx);
        }
        assignedNodeIds.insert(a.idIdx);
    }

    // (a) Ogni node-ID DISTINTO assegnato è preservato nel registro.
    for (int idIdx : assignedNodeIds) {
        RC_ASSERT(reg.contains(nodeId(idIdx)));
    }

    // (e) count(id) == numero di nodi distinti registrati sotto quell'id; e
    // (b) byId(id) restituisce SEMPRE il primo registrante (mai sovrascritto).
    for (const auto& [idIdx, entries] : expectedEntries) {
        const std::string id = nodeId(idIdx);

        RC_ASSERT(reg.count(id) == entries.size());

        // (d) la collisione è segnalata sse esistono ≥ 2 nodi distinti.
        RC_ASSERT(reg.hasCollision(id) == (entries.size() > 1));

        // (b) il primo registrante resta restituito da byId(id): nessun
        // overwrite, qualunque assegnazione successiva sia avvenuta.
        const auto primary = reg.byId(id);
        RC_ASSERT(primary.has_value());
        RC_ASSERT(*primary == nodeAt(entries.front().second));
    }

    // (c) per ogni mod che ha introdotto almeno una voce sotto un id, la coppia
    // (id, modId) resta risolvibile e punta al PRIMO nodo che quella mod ha
    // introdotto sotto quell'id (nessuna sovrascrittura della voce iniziale).
    for (const auto& [idIdx, entries] : expectedEntries) {
        const std::string id = nodeId(idIdx);

        // Prima voce per ciascuna mod, nell'ordine di creazione.
        std::map<int, int> firstNodeByMod;  // modIdx -> nodeIdx
        for (const auto& [modIdx, nodeIdx] : entries) {
            firstNodeByMod.try_emplace(modIdx, nodeIdx);
        }

        for (const auto& [modIdx, nodeIdx] : firstNodeByMod) {
            const auto resolved = reg.byId(id, modName(modIdx));
            RC_ASSERT(resolved.has_value());
            RC_ASSERT(*resolved == nodeAt(nodeIdx));
        }
    }

    // (d) i nodi che condividono un id restano DISTINTI: i puntatori risolti
    // per le diverse (id, mod) non vengono mai fusi né superano il numero di
    // nodi distinti registrati sotto l'id.
    for (const auto& [idIdx, entries] : expectedEntries) {
        const std::string id = nodeId(idIdx);
        std::set<Node*> resolved;
        for (int modIdx = 0; modIdx < numMods; ++modIdx) {
            auto r = reg.byId(id, modName(modIdx));
            if (r.has_value()) {
                resolved.insert(*r);
            }
        }
        RC_ASSERT(resolved.size() <= entries.size());
    }
}

}  // namespace
