// tests/event_bus_test.cpp — unit test dell'EventBus dello SDK (task 18.1).
//
// Verifica i comportamenti del sistema di eventi (Requisito 7):
//   * 7.1/7.2 registrazione SOLO per tipi dichiarati; il tipo non dichiarato è
//             rifiutato con errore e senza registrare il gestore;
//   * 7.3     invocazione dei gestori nell'ORDINE DI REGISTRAZIONE;
//   * 7.4     isolamento delle eccezioni con prosecuzione + registrazione
//             dell'indicazione di errore;
//   * 7.6     stop della propagazione su Propagation::Consumed;
//   * 7.5     unregisterMod deregistra tutti i gestori di una mod (su disable).
//
// Header-only: include l'header pubblico dello SDK <pulse/events.hpp>.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <pulse/events.hpp>

namespace {

// --- Tipi di evento di prova ----------------------------------------------
struct PlayerJump {
    int height = 0;
};

struct LevelLoaded {
    std::string name;
};

// Tipo MAI dichiarato sul bus: usato per verificare il rifiuto (Req 7.2).
struct UndeclaredEvent {
    int value = 0;
};

using pulse::EventBus;
using pulse::Propagation;

// --- Req 7.1: registrazione per un tipo dichiarato riesce ------------------
TEST(EventBus, RegisterHandlerForDeclaredTypeSucceeds) {
    EventBus bus;
    bus.declareEventType<PlayerJump>();
    EXPECT_TRUE(bus.isEventTypeDeclared<PlayerJump>());

    auto result = bus.on<PlayerJump>(
        "modA", [](const PlayerJump&) { return Propagation::Continue; });

    EXPECT_TRUE(result.registered);
    EXPECT_TRUE(result.eventTypeDeclared);
    EXPECT_EQ(bus.handlerCount<PlayerJump>(), 1u);
}

// --- Req 7.1: più gestori per lo stesso tipo sono ammessi ------------------
TEST(EventBus, MultipleHandlersForSameTypeAllowed) {
    EventBus bus;
    bus.declareEventType<PlayerJump>();

    bus.on<PlayerJump>("modA",
                       [](const PlayerJump&) { return Propagation::Continue; });
    bus.on<PlayerJump>("modA",
                       [](const PlayerJump&) { return Propagation::Continue; });

    EXPECT_EQ(bus.handlerCount<PlayerJump>(), 2u);
}

// --- Req 7.2: registrazione per un tipo NON dichiarato è rifiutata ---------
TEST(EventBus, RegisterHandlerForUndeclaredTypeIsRejected) {
    EventBus bus;
    // Nessuna dichiarazione di UndeclaredEvent.
    EXPECT_FALSE(bus.isEventTypeDeclared<UndeclaredEvent>());

    auto result = bus.on<UndeclaredEvent>(
        "modA", [](const UndeclaredEvent&) { return Propagation::Continue; });

    EXPECT_FALSE(result.registered);              // gestore NON registrato
    EXPECT_FALSE(result.eventTypeDeclared);       // errore: tipo non dichiarato
    EXPECT_EQ(bus.handlerCount<UndeclaredEvent>(), 0u);
}

// --- Req 7.3: i gestori sono invocati in ordine di registrazione -----------
TEST(EventBus, HandlersInvokedInRegistrationOrder) {
    EventBus bus;
    bus.declareEventType<PlayerJump>();

    std::vector<int> order;
    bus.on<PlayerJump>("modA", [&order](const PlayerJump&) {
        order.push_back(1);
        return Propagation::Continue;
    });
    bus.on<PlayerJump>("modB", [&order](const PlayerJump&) {
        order.push_back(2);
        return Propagation::Continue;
    });
    bus.on<PlayerJump>("modC", [&order](const PlayerJump&) {
        order.push_back(3);
        return Propagation::Continue;
    });

    const std::size_t invoked = bus.emit(PlayerJump{10});

    EXPECT_EQ(invoked, 3u);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

// --- Req 7.3: il gestore riceve i dati corretti dell'evento ----------------
TEST(EventBus, HandlerReceivesEventPayload) {
    EventBus bus;
    bus.declareEventType<LevelLoaded>();

    std::string seen;
    bus.on<LevelLoaded>("modA", [&seen](const LevelLoaded& e) {
        seen = e.name;
        return Propagation::Continue;
    });

    bus.emit(LevelLoaded{"Stereo Madness"});
    EXPECT_EQ(seen, "Stereo Madness");
}

// --- Req 7.4: un'eccezione è isolata e la pubblicazione prosegue -----------
TEST(EventBus, HandlerExceptionIsIsolatedAndPublicationContinues) {
    EventBus bus;
    bus.declareEventType<PlayerJump>();

    std::vector<int> order;
    bus.on<PlayerJump>("modA", [&order](const PlayerJump&) {
        order.push_back(1);
        return Propagation::Continue;
    });
    bus.on<PlayerJump>("modBroken", [](const PlayerJump&) -> Propagation {
        throw std::runtime_error("boom");
    });
    bus.on<PlayerJump>("modC", [&order](const PlayerJump&) {
        order.push_back(3);
        return Propagation::Continue;
    });

    const std::size_t invoked = bus.emit(PlayerJump{1});

    // Tutti e tre i gestori sono stati invocati: l'eccezione non ha interrotto
    // la pubblicazione (Req 7.4).
    EXPECT_EQ(invoked, 3u);
    EXPECT_EQ(order, (std::vector<int>{1, 3}));

    // L'indicazione dell'errore è stata registrata (Req 7.4).
    ASSERT_EQ(bus.errors().size(), 1u);
    EXPECT_EQ(bus.errors().front().owner, "modBroken");
    EXPECT_EQ(bus.errors().front().what, "boom");
}

// --- Req 7.6: Propagation::Consumed interrompe la propagazione -------------
TEST(EventBus, ConsumedStopsPropagation) {
    EventBus bus;
    bus.declareEventType<PlayerJump>();

    std::vector<int> order;
    bus.on<PlayerJump>("modA", [&order](const PlayerJump&) {
        order.push_back(1);
        return Propagation::Continue;
    });
    bus.on<PlayerJump>("modConsumer", [&order](const PlayerJump&) {
        order.push_back(2);
        return Propagation::Consumed;  // consuma l'evento (Req 7.6)
    });
    bus.on<PlayerJump>("modC", [&order](const PlayerJump&) {
        order.push_back(3);  // NON deve essere invocato
        return Propagation::Continue;
    });

    const std::size_t invoked = bus.emit(PlayerJump{1});

    EXPECT_EQ(invoked, 2u);  // il terzo gestore non è stato invocato
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
}

// --- Req 7.5: unregisterMod deregistra tutti i gestori di una mod ----------
TEST(EventBus, UnregisterModRemovesAllHandlersOfThatMod) {
    EventBus bus;
    bus.declareEventType<PlayerJump>();
    bus.declareEventType<LevelLoaded>();

    bus.on<PlayerJump>("modA",
                       [](const PlayerJump&) { return Propagation::Continue; });
    bus.on<LevelLoaded>("modA",
                        [](const LevelLoaded&) { return Propagation::Continue; });
    bus.on<PlayerJump>("modB",
                       [](const PlayerJump&) { return Propagation::Continue; });

    EXPECT_EQ(bus.handlersForMod("modA"), 2u);
    EXPECT_EQ(bus.handlersForMod("modB"), 1u);

    const std::size_t removed = bus.unregisterMod("modA");

    EXPECT_EQ(removed, 2u);
    EXPECT_EQ(bus.handlersForMod("modA"), 0u);
    // I gestori di altre mod restano intatti.
    EXPECT_EQ(bus.handlersForMod("modB"), 1u);
    EXPECT_EQ(bus.handlerCount<PlayerJump>(), 1u);
    EXPECT_EQ(bus.handlerCount<LevelLoaded>(), 0u);
}

// --- Req 7.5: dopo unregisterMod i gestori non vengono più invocati --------
TEST(EventBus, HandlersNotInvokedAfterUnregisterMod) {
    EventBus bus;
    bus.declareEventType<PlayerJump>();

    int calls = 0;
    bus.on<PlayerJump>("modA", [&calls](const PlayerJump&) {
        ++calls;
        return Propagation::Continue;
    });

    bus.emit(PlayerJump{1});
    EXPECT_EQ(calls, 1);

    bus.unregisterMod("modA");
    const std::size_t invoked = bus.emit(PlayerJump{1});

    EXPECT_EQ(invoked, 0u);
    EXPECT_EQ(calls, 1);  // invariato: nessuna ulteriore invocazione
}

// --- emit su un tipo dichiarato ma senza gestori non fa nulla --------------
TEST(EventBus, EmitWithNoHandlersIsNoOp) {
    EventBus bus;
    bus.declareEventType<PlayerJump>();
    EXPECT_EQ(bus.emit(PlayerJump{1}), 0u);
}

}  // namespace
