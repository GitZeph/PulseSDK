// pulse/ui.hpp — header pubblico aggregato del sistema di UI dello SDK Pulse
// (Layer 5, Requisito 8).
//
// Espone ai Developer di mod sia il registro dei node-ID (NodeRegistry) sia il
// sistema di layout (ILayout e i layout concreti). Le implementazioni
// (header-only) vivono sotto sdk/ui/ e sono incluse qui; questo file è il
// punto d'ingresso pubblico aggregato, raggiungibile via la include path di
// `pulse::sdk` come <pulse/ui.hpp>.
//
// API principali del layout:
//   * ILayout::apply(container)        dispone i figli entro 16 ms (Req 8.4);
//        su regole in conflitto/non valide applica un fallback che mantiene i
//        figli nei limiti del contenitore e segnala il contenitore (Req 8.5).
//   * RowLayout                         layout concreto a riga orizzontale.
//   * INodeGeometry                     adattatore di geometria iniettabile
//        (host-testabile) usato da apply() per riposizionare i figli senza
//        dereferenziare il tipo opaco `Node`.
//
// Esempio:
//   pulse::RowLayout layout{geometry, /*spacing*/ 4.0};
//   layout.apply(container);
//   if (layout.lastResult().fellBack()) { /* contenitore segnalato (Req 8.5) */ }
//
// Stack: C++20/23 (Requisito 26.1). Header-only, nessuna dipendenza esterna.
#ifndef PULSE_UI_HPP
#define PULSE_UI_HPP

// Implementazioni header-only collocate sotto sdk/ui/, incluse via percorso
// relativo così da non dipendere dalla presenza di sdk/ui/ nella include path
// pubblica.
#include "../../ui/layout.hpp"
#include "../../ui/node_id.hpp"

namespace pulse {

// Registro dei node-ID (Req 8.1, 8.2, 8.3, 8.6, 8.7) — vedi anche
// <pulse/ui_nodes.hpp>.
using ui::ModId;
using ui::Node;
using ui::NodeRegistry;
using ui::SetIdResult;
using ui::SetIdStatus;

// Sistema di layout (Req 8.4, 8.5).
using ui::ILayout;
using ui::ILayoutSink;
using ui::INodeGeometry;
using ui::kLayoutBudgetMs;
using ui::LayoutBase;
using ui::LayoutClock;
using ui::LayoutResult;
using ui::LayoutStatus;
using ui::Rect;
using ui::RowLayout;

}  // namespace pulse

#endif  // PULSE_UI_HPP
