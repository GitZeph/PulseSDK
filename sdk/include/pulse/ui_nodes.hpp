// pulse/ui_nodes.hpp — header pubblico del registro dei node-ID dell'interfaccia
// (Layer 5, Requisito 8).
//
// Espone il `NodeRegistry` ai Developer di mod. L'implementazione (header-only)
// vive sotto sdk/ui/node_id.hpp ed è inclusa qui; questo file è il punto
// d'ingresso pubblico, raggiungibile via la include path di `pulse::sdk` come
// <pulse/ui_nodes.hpp>.
//
// API principali:
//   * NodeRegistry::setId(node, id[, modId])  assegna un node-ID stabile,
//        ≤ 256 caratteri (Req 8.1); su id già usato da un nodo diverso segnala
//        la collisione mantenendo entrambi i nodi distinti (Req 8.6, 8.7).
//   * NodeRegistry::byId(id[, modId])          restituisce il nodo o nullopt se
//        assente (Req 8.2, 8.3).
//
// Esempio:
//   pulse::NodeRegistry reg;
//   reg.setId(node, "main-menu/play-button", "mod.alpha");   // (Req 8.1)
//   if (auto n = reg.byId("main-menu/play-button")) { /* ... */ }  // (Req 8.2)
//
// Stack: C++20/23 (Requisito 26.1). Header-only, nessuna dipendenza esterna.
#ifndef PULSE_UI_NODES_HPP
#define PULSE_UI_NODES_HPP

// L'implementazione è collocata sotto sdk/ui/ (codice del sistema di UI) ed
// esposta da questo header pubblico tramite include relativo, così non dipende
// dalla presenza di sdk/ui/ nella include path pubblica.
#include "../../ui/node_id.hpp"

namespace pulse {

// Promuove i tipi pubblici del registro dei node-ID nel namespace `pulse`, in
// modo che i Developer usino `pulse::NodeRegistry`, `pulse::SetIdResult`, ecc.
using ui::ModId;
using ui::Node;
using ui::NodeRegistry;
using ui::SetIdResult;
using ui::SetIdStatus;

}  // namespace pulse

#endif  // PULSE_UI_NODES_HPP
