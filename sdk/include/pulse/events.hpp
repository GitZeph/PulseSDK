// pulse/events.hpp — header pubblico del sistema di eventi dello SDK Pulse
// (Layer 5, Requisito 7).
//
// Espone l'`EventBus` ai Developer di mod. L'implementazione (header-only,
// template) vive sotto sdk/events/event_bus.hpp ed è inclusa qui; questo file
// è il punto d'ingresso pubblico, raggiungibile via la include path di
// `pulse::sdk` come <pulse/events.hpp>.
//
// API principali:
//   * EventBus::declareEventType<E>()       dichiara un tipo di evento (Req 7.1)
//   * EventBus::on<E>(modId, handler)        registra un gestore (Req 7.1, 7.2)
//   * EventBus::emit<E>(event)               pubblica un evento (Req 7.3, 7.4, 7.6)
//   * EventBus::unregisterMod(modId)         deregistra i gestori di una mod (Req 7.5)
//   * Propagation{Continue, Consumed}        controllo della propagazione (Req 7.6)
//
// Esempio:
//   struct PlayerJump { int height; };
//   pulse::EventBus bus;
//   bus.declareEventType<PlayerJump>();                 // (Req 7.1)
//   bus.on<PlayerJump>("myMod", [](const PlayerJump& e) {
//       // ... reagisci all'evento ...
//       return pulse::Propagation::Continue;            // o Consumed (Req 7.6)
//   });
//   bus.emit(PlayerJump{42});                            // (Req 7.3)
//
// Stack: C++20/23 (Requisito 26.1). Header-only, nessuna dipendenza esterna.
#ifndef PULSE_EVENTS_HPP
#define PULSE_EVENTS_HPP

// L'implementazione è collocata sotto sdk/events/ (codice del sistema di
// eventi) ed esposta da questo header pubblico tramite include relativo, così
// non dipende dalla presenza di sdk/events/ nella include path pubblica.
#include "../../events/event_bus.hpp"

namespace pulse {

// Promuove i tipi pubblici del sistema di eventi nel namespace `pulse`, in
// modo che i Developer usino `pulse::EventBus`, `pulse::Propagation`, ecc.
using events::EventBus;
using events::HandlerError;
using events::HandlerId;
using events::ModId;
using events::OnResult;
using events::Propagation;

}  // namespace pulse

#endif  // PULSE_EVENTS_HPP
