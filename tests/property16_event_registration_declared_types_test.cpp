// tests/property16_event_registration_declared_types_test.cpp
// Feature: pulse-sdk, Property 16 — Registrazione eventi solo per tipi
// dichiarati.
// Validates: Requirements 7.2 (Requisito 7.2)
//
// Property 16 (design.md / Req 7.2): per ogni tipo di evento la registrazione
// di un gestore tramite `EventBus::on<E>(...)` ha successo SE E SOLO SE il tipo
// E è stato precedentemente dichiarato con `declareEventType<E>()`. Se il tipo
// NON è dichiarato la registrazione è rifiutata:
//   * `OnResult::registered == false`;
//   * `OnResult::eventTypeDeclared == false` (errore "tipo non dichiarato");
//   * nessun gestore viene aggiunto (`handlerCount<E>()` resta invariato).
// Per i tipi dichiarati la registrazione ha successo (`registered == true`) e
// `handlerCount<E>()` riflette ESCLUSIVAMENTE le registrazioni riuscite.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
// i tipi di evento C++ sono statici, quindi la casualità è guidata su un POOL
// FISSO di tipi distinti `EventTag<N>` (N = 0..kPoolSize-1). Si genera una
// sequenza randomizzata di operazioni — `Declare(tag)` oppure
// `Register(tag)` — su tag scelti a caso nel pool. La dispatch da indice di tag
// (runtime) alla chiamata templata (compile-time) avviene tramite tabelle di
// funzioni costruite per espansione di `std::index_sequence`.
//
// A fianco del bus reale si mantiene un MODELLO (insieme dei tag dichiarati +
// conteggio dei gestori per tag) e dopo OGNI operazione si verifica:
//   * `on<E>` ha avuto successo  <=>  E era dichiarato al momento della chiamata;
//   * una registrazione rifiutata riporta `eventTypeDeclared == false` e NON
//     incrementa il conteggio dei gestori;
//   * `handlerCount<E>()` del bus coincide col numero di registrazioni riuscite
//     modellate (i gestori contano solo le registrazioni andate a buon fine);
//   * `isEventTypeDeclared<E>()` coincide con lo stato del modello.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <array>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

#include <pulse/events.hpp>

namespace {

using pulse::EventBus;
using pulse::ModId;
using pulse::OnResult;
using pulse::Propagation;

// Pool fisso di tipi di evento distinti. `EventTag<N>` genera un tipo C++
// distinto per ciascun N: type_index(typeid(EventTag<N>)) è univoco, così il
// registro dei tipi dichiarati dell'EventBus li distingue correttamente.
template <std::size_t N>
struct EventTag {
    int value = 0;
};

constexpr std::size_t kPoolSize = 8;

// Gestore non-op: l'identità del gestore è irrilevante per la Property 16
// (conta solo se la registrazione è ammessa). Restituisce sempre Continue.
template <std::size_t N>
Propagation noopHandler(const EventTag<N>&) {
    return Propagation::Continue;
}

// --- Tabelle di dispatch indice-di-tag -> chiamata templata ----------------
// Costruite una sola volta per espansione di index_sequence: mappano un indice
// di tag noto a runtime sulla corrispondente istanziazione template.

using DeclareFn = void (*)(EventBus&);
using IsDeclaredFn = bool (*)(const EventBus&);
using OnFn = OnResult (*)(EventBus&, const ModId&);
using HandlerCountFn = std::size_t (*)(const EventBus&);

template <std::size_t N>
void declareImpl(EventBus& bus) {
    bus.declareEventType<EventTag<N>>();
}

template <std::size_t N>
bool isDeclaredImpl(const EventBus& bus) {
    return bus.isEventTypeDeclared<EventTag<N>>();
}

template <std::size_t N>
OnResult onImpl(EventBus& bus, const ModId& owner) {
    return bus.on<EventTag<N>>(
        owner, std::function<Propagation(const EventTag<N>&)>(&noopHandler<N>));
}

template <std::size_t N>
std::size_t handlerCountImpl(const EventBus& bus) {
    return bus.handlerCount<EventTag<N>>();
}

template <std::size_t... Ns>
std::array<DeclareFn, kPoolSize> makeDeclareTable(std::index_sequence<Ns...>) {
    return {{&declareImpl<Ns>...}};
}

template <std::size_t... Ns>
std::array<IsDeclaredFn, kPoolSize> makeIsDeclaredTable(
    std::index_sequence<Ns...>) {
    return {{&isDeclaredImpl<Ns>...}};
}

template <std::size_t... Ns>
std::array<OnFn, kPoolSize> makeOnTable(std::index_sequence<Ns...>) {
    return {{&onImpl<Ns>...}};
}

template <std::size_t... Ns>
std::array<HandlerCountFn, kPoolSize> makeHandlerCountTable(
    std::index_sequence<Ns...>) {
    return {{&handlerCountImpl<Ns>...}};
}

const std::array<DeclareFn, kPoolSize> kDeclare =
    makeDeclareTable(std::make_index_sequence<kPoolSize>{});
const std::array<IsDeclaredFn, kPoolSize> kIsDeclared =
    makeIsDeclaredTable(std::make_index_sequence<kPoolSize>{});
const std::array<OnFn, kPoolSize> kOn =
    makeOnTable(std::make_index_sequence<kPoolSize>{});
const std::array<HandlerCountFn, kPoolSize> kHandlerCount =
    makeHandlerCountTable(std::make_index_sequence<kPoolSize>{});

// Un'operazione randomizzata sul bus: dichiarazione o tentativo di
// registrazione su un tag del pool.
enum class OpKind { Declare, Register };

struct Op {
    OpKind kind = OpKind::Declare;
    std::size_t tag = 0;  // indice nel pool [0, kPoolSize)
};

// --- Property 16 -----------------------------------------------------------
// Feature: pulse-sdk, Property 16. Validates: Requirements 7.2.
//
// Esegue una sequenza randomizzata di Declare/Register sul pool fisso di tipi e
// verifica, dopo ogni operazione, l'invariante "registrazione ammessa sse tipo
// dichiarato" e che i conteggi dei gestori riflettano solo le registrazioni
// riuscite.
RC_GTEST_PROP(Property16EventRegistration,
              RegistrationSucceedsIffDeclared,
              ()) {
    // Sequenza randomizzata di operazioni (fino a 128). L'ordine casuale
    // produce naturalmente tentativi di registrazione PRIMA della
    // dichiarazione (tipo non dichiarato, Req 7.2) e dopo (tipo dichiarato).
    const auto ops = *rc::gen::container<std::vector<Op>>(
                          rc::gen::construct<Op>(
                              rc::gen::element(OpKind::Declare,
                                               OpKind::Register),
                              rc::gen::inRange<std::size_t>(0, kPoolSize)))
                          .as("sequenza di operazioni");

    EventBus bus;

    // Modello di riferimento: stato dichiarato e conteggio dei gestori riusciti
    // per ciascun tag del pool.
    std::array<bool, kPoolSize> declared{};
    std::array<std::size_t, kPoolSize> expectedHandlers{};

    const ModId owner = "modProperty16";

    for (const Op& op : ops) {
        const std::size_t t = op.tag;

        if (op.kind == OpKind::Declare) {
            kDeclare[t](bus);
            declared[t] = true;  // idempotente lato bus e lato modello
        } else {
            const bool wasDeclared = declared[t];
            const std::size_t before = kHandlerCount[t](bus);
            const OnResult result = kOn[t](bus, owner);

            // (1) on<E> riesce SSE E era dichiarato al momento della chiamata.
            RC_ASSERT(result.registered == wasDeclared);

            if (wasDeclared) {
                // Registrazione riuscita: tipo dichiarato, gestore aggiunto.
                RC_ASSERT(result.eventTypeDeclared);
                RC_ASSERT(kHandlerCount[t](bus) == before + 1);
                ++expectedHandlers[t];
            } else {
                // Rifiuto (Req 7.2): errore "tipo non dichiarato", nessun
                // gestore aggiunto.
                RC_ASSERT(!result.eventTypeDeclared);
                RC_ASSERT(kHandlerCount[t](bus) == before);
            }
        }

        // Invarianti dopo ogni operazione: stato dichiarato e conteggi gestori
        // del bus coincidono col modello (i gestori contano SOLO le
        // registrazioni riuscite).
        for (std::size_t i = 0; i < kPoolSize; ++i) {
            RC_ASSERT(kIsDeclared[i](bus) == declared[i]);
            RC_ASSERT(kHandlerCount[i](bus) == expectedHandlers[i]);
        }
    }
}

}  // namespace
