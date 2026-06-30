// tests/property18_event_exception_isolation_test.cpp
// Feature: pulse-sdk, Property 18 — isolamento delle eccezioni negli eventi.
// Validates: Requirements 7.4 (Requisito 7.4)
//
// Property 18 (design.md / Req 7.4): quando un evento viene pubblicato,
// se uno o più gestori lanciano un'eccezione, queste sono ISOLATE: la
// pubblicazione PROSEGUE invocando TUTTI i gestori registrati (i gestori che
// lanciano non interrompono la propagazione), ogni gestore che NON lancia viene
// comunque eseguito, e per OGNI gestore che lancia viene registrata
// un'indicazione dell'errore (EventBus::errors()).
//
// Strategia (RapidCheck, ≥100 iterazioni — forzate via RC_GTEST_PROP):
//   * si dichiara un tipo di evento `IsolatedEvent`;
//   * si registra un numero randomizzato N di gestori (N in [0, 64]); ciascun
//     gestore o LANCIA un'eccezione (sottoinsieme randomizzato) oppure registra
//     la propria esecuzione in una traccia condivisa e restituisce
//     `Propagation::Continue` (mai `Consumed`, così la propagazione non si
//     ferma e tutti i gestori vengono invocati);
//   * si pubblica un singolo evento e si verifica che:
//       - `emit` riporti N gestori invocati (nessun consumo — i gestori che
//         lanciano sono isolati e non arrestano la pubblicazione, Req 7.4);
//       - ogni gestore che NON lancia sia stato effettivamente eseguito
//         (la traccia contiene esattamente gli indici dei gestori non lancianti);
//       - `errors().size()` sia esattamente pari al numero di gestori lancianti
//         (un'indicazione di errore registrata per ciascuno, Req 7.4).
//
// L'EventBus è esposto pubblicamente da <pulse/events.hpp> (link a pulse::sdk).

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <vector>

#include <pulse/events.hpp>

namespace {

using pulse::EventBus;
using pulse::Propagation;

// Tipo di evento usato dalla property: il contenuto è irrilevante per
// l'isolamento delle eccezioni, conta solo il comportamento dei gestori.
struct IsolatedEvent {
    int payload = 0;
};

// --- Property 18 — isolamento delle eccezioni negli eventi ----------------
// Feature: pulse-sdk, Property 18. Validates: Requirements 7.4.
//
// Per N randomizzato (0..64) gestori registrati su un tipo dichiarato, dove un
// sottoinsieme randomizzato lancia eccezioni: emit invoca TUTTI gli N gestori
// (le eccezioni non arrestano la pubblicazione), ogni gestore non lanciante
// viene eseguito, e per ogni gestore lanciante viene registrato un errore.
RC_GTEST_PROP(Property18EventExceptionIsolation,
              ThrowingHandlersAreIsolatedAndRecorded,
              ()) {
    // Numero randomizzato di gestori, fino a 64 (Req 7: più gestori per tipo).
    const auto n = *rc::gen::inRange<std::size_t>(0, 65).as("numero di gestori");

    // Per ciascun gestore, un flag randomizzato: true => lancia un'eccezione.
    const auto throws =
        *rc::gen::container<std::vector<bool>>(n, rc::gen::arbitrary<bool>())
             .as("gestori che lanciano");

    EventBus bus;
    bus.declareEventType<IsolatedEvent>();  // (Req 7.1) tipo dichiarato.

    // Traccia condivisa degli indici dei gestori NON lancianti effettivamente
    // eseguiti; condivisa tramite shared_ptr così ogni closure scrive nello
    // stesso vettore.
    auto trace = std::make_shared<std::vector<std::size_t>>();

    std::size_t expectedThrows = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const bool willThrow = throws[i];
        if (willThrow) {
            ++expectedThrows;
        }
        auto result = bus.on<IsolatedEvent>(
            "modIsolate",
            [trace, i, willThrow](const IsolatedEvent&) -> Propagation {
                if (willThrow) {
                    // Gestore che lancia: l'eccezione deve essere isolata.
                    throw std::runtime_error("gestore lanciante");
                }
                trace->push_back(i);            // registra l'esecuzione
                return Propagation::Continue;   // non consuma: tutti invocati
            });
        // Registrazione su tipo dichiarato: deve riuscire (Req 7.1).
        RC_ASSERT(result.registered);
        RC_ASSERT(result.eventTypeDeclared);
    }

    RC_ASSERT(bus.handlerCount<IsolatedEvent>() == n);

    const std::size_t invoked = bus.emit(IsolatedEvent{42});

    // (1) Tutti i gestori sono stati invocati: i gestori lancianti sono isolati
    //     e NON arrestano la pubblicazione (Req 7.4); nessun consumo.
    RC_ASSERT(invoked == n);

    // (2) Ogni gestore che NON lancia è stato effettivamente eseguito: la
    //     traccia contiene esattamente gli indici dei gestori non lancianti,
    //     nell'ordine di registrazione.
    std::vector<std::size_t> expectedTrace;
    for (std::size_t i = 0; i < n; ++i) {
        if (!throws[i]) {
            expectedTrace.push_back(i);
        }
    }
    RC_ASSERT(*trace == expectedTrace);

    // (3) Un'indicazione di errore registrata per OGNI gestore lanciante
    //     (Req 7.4).
    RC_ASSERT(bus.errors().size() == expectedThrows);
}

}  // namespace
