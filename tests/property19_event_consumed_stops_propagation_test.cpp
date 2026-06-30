// tests/property19_event_consumed_stops_propagation_test.cpp
// Feature: pulse-sdk, Property 19 — il consumo di un evento interrompe la
// propagazione ai gestori successivi.
// Validates: Requirements 7.6 (Requisito 7.6)
//
// Property 19 (design.md / Req 7.6): quando un gestore restituisce
// `Propagation::Consumed`, l'EventBus interrompe la propagazione: i gestori
// registrati DOPO il primo che consuma NON vengono invocati. I gestori fino al
// primo consumatore (incluso) sono invocati nell'ordine di registrazione.
//
// Strategia (RapidCheck, ≥100 iterazioni — forzate via RC_PARAMS/numTests):
//   * si dichiara un tipo di evento `ConsumeEvent`;
//   * si registra un numero randomizzato N di gestori (N in [0, 64]);
//   * si sceglie una posizione randomizzata `k` del consumatore: o un indice in
//     [0, N-1] (incluse la prima e l'ultima posizione), oppure "nessuno"
//     (nessun gestore consuma) — così sono coperti i casi limite no-consumer,
//     primo e ultimo;
//   * il gestore in posizione `k` restituisce `Propagation::Consumed`, tutti
//     gli altri `Propagation::Continue`; ciascun gestore appende il proprio
//     indice di registrazione a una traccia condivisa;
//   * si pubblica un singolo evento e si verifica che:
//       - il numero di gestori invocati riportato da `emit` sia (k + 1) quando
//         esiste un consumatore, oppure N quando nessuno consuma;
//       - la traccia condivisa sia esattamente 0, 1, ..., (numero invocati - 1)
//         (ordine di registrazione, propagazione interrotta dopo il consumo);
//       - i gestori dopo il primo consumatore non compaiano MAI nella traccia.

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

// Tipo di evento usato dalla property: il contenuto è irrilevante, conta solo
// quale gestore consuma e quali vengono invocati.
struct ConsumeEvent {
    int payload = 0;
};

// --- Property 19 — il consumo interrompe la propagazione ------------------
// Feature: pulse-sdk, Property 19. Validates: Requirements 7.6.
//
// Per N randomizzato (0..64) gestori e una posizione randomizzata del
// consumatore (o nessuno), emit invoca i gestori in ordine fino al primo che
// restituisce Consumed (incluso) e poi si ferma.
RC_GTEST_PROP(Property19EventConsumedStopsPropagation,
              ConsumedHandlerStopsPropagation,
              ()) {
    // Numero randomizzato di gestori, fino a 64 (Req 7: più gestori per tipo).
    const auto n = *rc::gen::inRange<std::size_t>(0, 65).as("numero di gestori");

    // Posizione del consumatore: -1 => nessun consumatore; altrimenti l'indice
    // (0..N-1) del gestore che restituisce Consumed. Copre no-consumer, prima
    // e ultima posizione.
    const auto consumerPos =
        *rc::gen::inRange<long long>(-1, static_cast<long long>(n))
             .as("posizione del consumatore (-1 = nessuno)");

    EventBus bus;
    bus.declareEventType<ConsumeEvent>();  // (Req 7.1) tipo dichiarato.

    // Traccia condivisa degli indici di invocazione.
    auto trace = std::make_shared<std::vector<std::size_t>>();

    for (std::size_t i = 0; i < n; ++i) {
        const bool consumes =
            (consumerPos >= 0) && (static_cast<std::size_t>(consumerPos) == i);
        auto result = bus.on<ConsumeEvent>(
            "modConsume",
            [trace, i, consumes](const ConsumeEvent&) {
                trace->push_back(i);  // appende l'indice di registrazione
                return consumes ? Propagation::Consumed : Propagation::Continue;
            });
        // Registrazione su tipo dichiarato: deve riuscire (Req 7.1).
        RC_ASSERT(result.registered);
        RC_ASSERT(result.eventTypeDeclared);
    }

    RC_ASSERT(bus.handlerCount<ConsumeEvent>() == n);

    const std::size_t invoked = bus.emit(ConsumeEvent{7});

    // Numero atteso di gestori invocati: fino al primo consumatore (incluso),
    // altrimenti tutti.
    const std::size_t expectedInvoked =
        (consumerPos >= 0) ? (static_cast<std::size_t>(consumerPos) + 1) : n;

    // emit riporta esattamente i gestori invocati (Req 7.6).
    RC_ASSERT(invoked == expectedInvoked);

    // La traccia riflette gli stessi gestori invocati, in ordine.
    RC_ASSERT(trace->size() == expectedInvoked);

    // Sequenza attesa: 0, 1, ..., expectedInvoked-1 — i gestori dopo il primo
    // consumatore non sono mai stati invocati (propagazione interrotta).
    std::vector<std::size_t> expectedTrace(expectedInvoked);
    std::iota(expectedTrace.begin(), expectedTrace.end(), std::size_t{0});
    RC_ASSERT(*trace == expectedTrace);

    // Verifica esplicita: nessun indice oltre il primo consumatore compare
    // nella traccia.
    if (consumerPos >= 0) {
        for (const std::size_t idx : *trace) {
            RC_ASSERT(idx <= static_cast<std::size_t>(consumerPos));
        }
    }
}

}  // namespace
