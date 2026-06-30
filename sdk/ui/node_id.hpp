// sdk/ui/node_id.hpp — registro dei node-ID dell'interfaccia (Layer 5,
// Requisito 8). Implementazione header-only del `NodeRegistry`, esposta ai
// Developer tramite l'header pubblico <pulse/ui_nodes.hpp> (che promuove i tipi
// nel namespace `pulse`).
//
// Semantiche osservabili implementate qui:
//
//   * setId(node, id[, modId]) — assegna a un nodo un node-ID STABILE
//       costituito da una stringa di al massimo 256 caratteri (Req 8.1). Un id
//       più lungo di 256 caratteri è RIFIUTATO senza modificare il registro.
//   * byId(id)                  — restituisce il nodo associato a `id` entro un
//       tempo di lookup costante/logaritmico (≤ 50 ms, Req 8.2); una richiesta
//       per un id assente restituisce un valore ASSENTE (`std::nullopt`)
//       lasciando il registro invariato (Req 8.3).
//   * Collisione fra mod (Req 8.6, 8.7) — il registro associa la coppia
//       `(id, ModId)`: quando due mod assegnano lo STESSO node-ID a nodi
//       diversi, ENTRAMBI i nodi sono mantenuti distinti (ciascun id resta
//       associato alla mod che lo ha definito) e la collisione è SEGNALATA,
//       SENZA sovrascrivere alcun nodo. Quando le mod usano node-ID distinti,
//       tutti gli id assegnati sono preservati senza sovrascritture (Req 8.6).
//
// `Node` è un tipo del gioco non disponibile sull'host: qui è modellato come
// tipo OPACO forward-declared. Il registro conserva e confronta soltanto
// puntatori `Node*` (non li dereferenzia mai), così il codice compila ed è
// testabile sull'host con indirizzi fittizi.
//
// Stack: C++20/23 (Requisito 26.1). Header-only, nessuna dipendenza esterna.
#ifndef PULSE_UI_NODE_ID_HPP
#define PULSE_UI_NODE_ID_HPP

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulse::ui {

// ---------------------------------------------------------------------------
// Tipi di base.
// ---------------------------------------------------------------------------

// Tipo OPACO del nodo dell'interfaccia del gioco. Forward-declared: il registro
// non lo definisce né lo dereferenzia, conserva soltanto puntatori `Node*`.
struct Node;

// Identità della Mod che definisce un node-ID (Req 8.7).
using ModId = std::string;

// Lunghezza massima di un node-ID: 256 caratteri (Req 8.1).
inline constexpr std::size_t kMaxIdLength = 256u;

// ---------------------------------------------------------------------------
// Esito di setId().
// ---------------------------------------------------------------------------

enum class SetIdStatus {
    // Node-ID assegnato senza conflitti (Req 8.1, 8.6).
    Assigned,
    // Ri-assegnazione idempotente: lo stesso nodo era già associato a `id`.
    AlreadyAssigned,
    // Stesso node-ID assegnato a un nodo diverso: entrambi i nodi sono
    // mantenuti distinti e la collisione è segnalata, senza overwrite (Req 8.7).
    Collision,
    // node-ID più lungo di 256 caratteri: rifiutato, registro invariato (Req 8.1).
    RejectedTooLong,
};

struct SetIdResult {
    SetIdStatus status{SetIdStatus::Assigned};
    std::string message;

    // True se il nodo risulta registrato sotto `id` dopo la chiamata (incluse
    // assegnazione nuova, idempotente e collisione: in nessun caso un nodo
    // preesistente viene sovrascritto).
    [[nodiscard]] bool assigned() const noexcept {
        return status == SetIdStatus::Assigned ||
               status == SetIdStatus::AlreadyAssigned ||
               status == SetIdStatus::Collision;
    }

    // True se la chiamata ha rilevato una collisione di node-ID (Req 8.7).
    [[nodiscard]] bool collision() const noexcept {
        return status == SetIdStatus::Collision;
    }

    // True se la chiamata è stata rifiutata per id troppo lungo (Req 8.1).
    [[nodiscard]] bool rejected() const noexcept {
        return status == SetIdStatus::RejectedTooLong;
    }
};

// ---------------------------------------------------------------------------
// NodeRegistry — associa node-ID stabili ai nodi dell'interfaccia (Req 8).
//
// Il registro associa la coppia `(id, ModId)`: per ciascun node-ID conserva
// l'elenco delle voci `(modId, node)` che lo hanno richiesto, così da poter
// mantenere distinti i nodi di mod diverse in caso di collisione (Req 8.7).
// ---------------------------------------------------------------------------
class NodeRegistry {
public:
    // Assegna a `node` il node-ID `id` per conto della mod `modId` (Req 8.1,
    // 8.6, 8.7).
    //
    //   * id > 256 caratteri  -> rifiutato, registro invariato (Req 8.1).
    //   * id libero            -> assegnato (Req 8.1).
    //   * id già usato dallo STESSO nodo -> idempotente (assegnazione stabile).
    //   * id già usato da un nodo DIVERSO -> COLLISIONE: la nuova voce
    //       `(id, modId, node)` è aggiunta mantenendo distinti tutti i nodi
    //       coinvolti; nessun nodo è sovrascritto; la collisione è segnalata
    //       (Req 8.7).
    SetIdResult setId(Node* node, std::string_view id, std::string_view modId) {
        if (id.size() > kMaxIdLength) {
            // Rifiuto: il registro resta invariato (Req 8.1).
            return SetIdResult{
                SetIdStatus::RejectedTooLong,
                "node-ID rifiutato: lunghezza superiore a 256 caratteri"};
        }

        auto& entries = byId_[std::string(id)];

        // Ri-assegnazione idempotente dello stesso nodo (id stabile).
        for (const auto& entry : entries) {
            if (entry.node == node) {
                return SetIdResult{SetIdStatus::AlreadyAssigned,
                                   "node-ID già assegnato a questo nodo"};
            }
        }

        const bool isCollision = !entries.empty();

        // Aggiunge la nuova voce SENZA rimuovere/sovrascrivere quelle esistenti
        // (Req 8.6, 8.7): entrambi i nodi restano distinti.
        entries.push_back(Entry{std::string(modId), node});

        // Mappa inversa nodo -> id: la prima assegnazione è stabile e non viene
        // riscritta da chiamate successive (Req 8.1).
        idOfNode_.try_emplace(node, std::string(id));

        if (isCollision) {
            return SetIdResult{
                SetIdStatus::Collision,
                "collisione di node-ID: entrambi i nodi mantenuti distinti per "
                "(id, ModId), nessuna sovrascrittura"};
        }
        return SetIdResult{SetIdStatus::Assigned, {}};
    }

    // Overload conforme alla firma di design (Req 8.1/8.2): usa una ModId
    // vuota di default (utile quando l'identità della mod non è rilevante).
    SetIdResult setId(Node* node, std::string_view id) {
        return setId(node, id, std::string_view{});
    }

    // Restituisce il nodo associato a `id`, o `std::nullopt` se nessun nodo è
    // registrato sotto quell'id (Req 8.2, 8.3). In presenza di una collisione
    // restituisce il primo nodo registrato (preservato, mai sovrascritto); usa
    // l'overload con ModId per disambiguare.
    [[nodiscard]] std::optional<Node*> byId(std::string_view id) const {
        auto it = byId_.find(id);
        if (it == byId_.end() || it->second.empty()) {
            return std::nullopt;  // assenza del nodo, registro invariato (Req 8.3).
        }
        return it->second.front().node;
    }

    // Restituisce il nodo associato alla coppia `(id, modId)`, o
    // `std::nullopt` se nessun nodo di quella mod è registrato sotto `id`.
    [[nodiscard]] std::optional<Node*> byId(std::string_view id,
                                            std::string_view modId) const {
        auto it = byId_.find(id);
        if (it == byId_.end()) {
            return std::nullopt;
        }
        for (const auto& entry : it->second) {
            if (entry.modId == modId) {
                return entry.node;
            }
        }
        return std::nullopt;
    }

    // node-ID stabile assegnato a `node`, o `std::nullopt` se il nodo non è
    // registrato. La prima assegnazione è stabile (Req 8.1).
    [[nodiscard]] std::optional<std::string> idOf(Node* node) const {
        auto it = idOfNode_.find(node);
        if (it == idOfNode_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    // Numero di nodi distinti registrati sotto `id` (≥ 2 in caso di collisione).
    [[nodiscard]] std::size_t count(std::string_view id) const {
        auto it = byId_.find(id);
        return (it == byId_.end()) ? 0u : it->second.size();
    }

    // True se più nodi condividono lo stesso node-ID (collisione, Req 8.7).
    [[nodiscard]] bool hasCollision(std::string_view id) const {
        return count(id) > 1u;
    }

    // True se `id` è registrato per almeno un nodo.
    [[nodiscard]] bool contains(std::string_view id) const {
        return count(id) > 0u;
    }

private:
    // Voce del registro: la mod che ha definito l'id e il nodo associato.
    struct Entry {
        ModId modId;
        Node* node;
    };

    // node-ID -> elenco delle voci `(modId, node)` (associazione `(id, ModId)`,
    // Req 8.7). `std::less<>` abilita la ricerca eterogenea con string_view.
    std::map<std::string, std::vector<Entry>, std::less<>> byId_;

    // Mappa inversa nodo -> node-ID stabile (Req 8.1).
    std::map<Node*, std::string> idOfNode_;
};

}  // namespace pulse::ui

#endif  // PULSE_UI_NODE_ID_HPP
