// tests/property17_event_registration_order_test.cpp
// Feature: pulse-sdk, Property 17 — invocazione dei gestori di evento in
// ordine di registrazione.
// Validates: Requirements 7.3 (Requisito 7.3)
//
// Property 17 (design.md / Req 7.3): quando un evento viene pubblicato,
// l'EventBus invoca i gestori registrati per quel tipo NELL'ORDINE DI
// REGISTRAZIONE, dal primo registrato all'ultimo.
//
// Strategia (RapidCheck, ≥100 iterazioni — forzate via RC_PARAMS/numTests):
//   * si dichiara un tipo di evento `OrderEvent`;
//   * si registra un numero randomizzato N di gestori (N in [0, 64]); ciascun
//     gestore cattura il proprio indice di registrazione `i` e, quando
//     invocato, appende `i` a una traccia condivisa, restituendo sempre
//     `Propagation::Continue` (nessun consumo, così tutti i gestori vengono
//     invocati);
//   * si pubblica un singolo evento e si verifica che la traccia risultante sia
//     esattamente la sequenza 0, 1, 2, ..., N-1 — cioè i gestori sono stati
//     invocati nell'ordine in cui erano stati registrati (Req 7.3);
//   * si verifica inoltre che `emit` riporti N gestori invocati e che ogni
//     registrazione sia andata a buon fine (tipo dichiarato, Req 7.1).
//
// L'EventBus è esposto pubblicamente da <pulse/events.hpp> (link a pulse::sdk).

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstddef>
#include <memory>
#include <numeric>
#include <vector>

#include <pulse/events.hpp>

namespace {

using pulse::EventBus;
using pulse::Propagation;

// Tipo di evento usato dalla property: il contenuto è irrilevante per
// l'ordinamento, conta solo l'ordine di invocazione dei gestori.
struct OrderEvent {
    int payload = 0;
};

// --- Property 17 — invocazione in ordine di registrazione -----------------
// Feature: pulse-sdk, Property 17. Validates: Requirements 7.3.
//
// Per N randomizzato (0..64) gestori registrati su un tipo dichiarato, emit
// li invoca nell'ordine di registrazione: la traccia condivisa risultante è
// esattamente 0, 1, ..., N-1.
RC_GTEST_PROP(Property17EventRegistrationOrder,
              HandlersInvokedInRegistrationOrder,
              ()) {
    // Numero randomizzato di gestori, fino a 64 (Req 7: più gestori per tipo).
    const auto n = *rc::gen::inRange<std::size_t>(0, 65).as("numero di gestori");

    EventBus bus;
    bus.declareEventType<OrderEvent>();  // (Req 7.1) tipo dichiarato.

    // Traccia condivisa degli indici di invocazione; condivisa tra i gestori
    // tramite shared_ptr così ogni closure scrive nello stesso vettore.
    auto trace = std::make_shared<std::vector<std::size_t>>();

    for (std::size_t i = 0; i < n; ++i) {
        auto result = bus.on<OrderEvent>(
            "modOrder",
            [trace, i](const OrderEvent&) {
                trace->push_back(i);              // appende l'indice di registrazione
                return Propagation::Continue;     // non consuma: tutti invocati
            });
        // Registrazione su tipo dichiarato: deve riuscire (Req 7.1).
        RC_ASSERT(result.registered);
        RC_ASSERT(result.eventTypeDeclared);
    }

    RC_ASSERT(bus.handlerCount<OrderEvent>() == n);

    const std::size_t invoked = bus.emit(OrderEvent{42});

    // Tutti i gestori sono stati invocati (nessun consumo).
    RC_ASSERT(invoked == n);
    RC_ASSERT(trace->size() == n);

    // Sequenza attesa: 0, 1, ..., N-1 (ordine di registrazione, Req 7.3).
    std::vector<std::size_t> expected(n);
    std::iota(expected.begin(), expected.end(), std::size_t{0});
    RC_ASSERT(*trace == expected);
}

}  // namespace
