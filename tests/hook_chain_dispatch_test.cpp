// tests/hook_chain_dispatch_test.cpp — unit test dell'esecuzione della catena
// di hook (task 6.3, Requisiti 3.1, 3.4, 3.5).
//
// Verifica HookChain::dispatch + HookContext::callNext():
//   * concatenamento completo: i gestori sono eseguiti nell'ordine della
//     catena (priority DESC, loadOrder ASC) e l'ultimo callNext() invoca la
//     funzione originale (Req 3.1), con supporto da 2 ad almeno 64 hook;
//   * stop senza next: un gestore che ritorna senza chiamare callNext()
//     interrompe la catena (gestori rimanenti e originale NON eseguiti) e
//     registra un evento di interruzione con mod + funzione (Req 3.4);
//   * isolamento delle eccezioni: un gestore che lancia un'eccezione è isolato,
//     si registra un evento di errore con mod + funzione e la catena prosegue
//     invocando il gestore successivo (Req 3.5).

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "hooking/hook_chain.hpp"

namespace {

using pulse::hooking::HookChain;
using pulse::hooking::HookContext;
using pulse::hooking::HookEventSink;
using pulse::hooking::HookNode;
using pulse::hooking::ModId;
using pulse::hooking::OriginalFn;

// Traccia globale (per test) dell'ordine di esecuzione di gestori e originale.
// Ogni gestore appende il proprio owner; l'originale appende "<original>".
struct Trace {
    std::vector<std::string> events;
    std::vector<std::string> logs;

    void record(std::string e) { events.push_back(std::move(e)); }
    HookEventSink sink() {
        return [this](std::string_view msg) { logs.push_back(std::string(msg)); };
    }
    OriginalFn original() {
        return [this] { events.emplace_back("<original>"); };
    }
};

// Costruisce un nodo con un gestore che registra l'esecuzione e poi invoca
// callNext() (gestore "trasparente" che propaga lungo la catena).
HookNode passThrough(Trace& trace, ModId owner, int priority,
                     std::uint64_t loadOrder) {
    HookNode node;
    node.owner = owner;
    node.priority = priority;
    node.loadOrder = loadOrder;
    node.handler = [&trace, owner](HookContext& ctx) {
        trace.record(owner);
        ctx.callNext();
    };
    return node;
}

// --- concatenamento completo a 2 hook: ordine + originale in coda (Req 3.1) -
TEST(HookChainDispatch, TwoHooksChainThenOriginal) {
    Trace trace;
    HookChain chain;
    chain.add(passThrough(trace, "high", 800, 0));
    chain.add(passThrough(trace, "low", 200, 1));

    chain.dispatch("MenuLayer::init", trace.original(), trace.sink());

    EXPECT_EQ(trace.events,
              (std::vector<std::string>{"high", "low", "<original>"}));
    EXPECT_TRUE(trace.logs.empty());  // nessun evento di interruzione/errore
}

// --- l'ordine di esecuzione segue priority DESC, poi loadOrder ASC ---------
TEST(HookChainDispatch, ExecutionFollowsChainOrder) {
    Trace trace;
    HookChain chain;
    // Inseriti in ordine "mescolato"; l'esecuzione deve seguire l'ordinamento.
    chain.add(passThrough(trace, "p500b", 500, 5));
    chain.add(passThrough(trace, "p900", 900, 1));
    chain.add(passThrough(trace, "p500a", 500, 2));
    chain.add(passThrough(trace, "p100", 100, 3));

    chain.dispatch("Target::fn", trace.original(), trace.sink());

    EXPECT_EQ(trace.events,
              (std::vector<std::string>{"p900", "p500a", "p500b", "p100",
                                        "<original>"}));
}

// --- catena vuota: dispatch invoca direttamente l'originale ----------------
TEST(HookChainDispatch, EmptyChainInvokesOriginal) {
    Trace trace;
    HookChain chain;

    chain.dispatch("Target::fn", trace.original(), trace.sink());

    EXPECT_EQ(trace.events, (std::vector<std::string>{"<original>"}));
}

// --- catena ampia (>=64 hook): ordine completo + originale (Req 3.1) -------
TEST(HookChainDispatch, LargeChainOf64ReachesOriginal) {
    Trace trace;
    HookChain chain;
    constexpr int kCount = 64;
    // priority decrescente con l'indice -> ordine di esecuzione = m0, m1, ...
    for (int i = 0; i < kCount; ++i) {
        chain.add(passThrough(trace, "m" + std::to_string(i), kCount - i,
                              static_cast<std::uint64_t>(i)));
    }

    chain.dispatch("Target::fn", trace.original(), trace.sink());

    ASSERT_EQ(trace.events.size(), static_cast<std::size_t>(kCount) + 1);
    for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(trace.events[static_cast<std::size_t>(i)],
                  "m" + std::to_string(i));
    }
    EXPECT_EQ(trace.events.back(), "<original>");
}

// --- stop senza next: catena interrotta + log di interruzione (Req 3.4) ----
TEST(HookChainDispatch, HandlerWithoutCallNextStopsChain) {
    Trace trace;
    HookChain chain;
    chain.add(passThrough(trace, "first", 900, 0));

    // 'stopper' registra l'esecuzione ma NON invoca callNext().
    HookNode stopper;
    stopper.owner = "stopper";
    stopper.priority = 500;
    stopper.loadOrder = 1;
    stopper.handler = [&trace](HookContext&) { trace.record("stopper"); };
    chain.add(std::move(stopper));

    chain.add(passThrough(trace, "never", 100, 2));

    chain.dispatch("MenuLayer::init", trace.original(), trace.sink());

    // 'first' e 'stopper' eseguiti; 'never' e l'originale NON eseguiti.
    EXPECT_EQ(trace.events, (std::vector<std::string>{"first", "stopper"}));

    // Evento di interruzione registrato con mod + funzione (Req 3.4).
    ASSERT_EQ(trace.logs.size(), 1u);
    EXPECT_NE(trace.logs[0].find("stopper"), std::string::npos);
    EXPECT_NE(trace.logs[0].find("MenuLayer::init"), std::string::npos);
}

// --- isolamento eccezioni: errore isolato + prosecuzione catena (Req 3.5) --
TEST(HookChainDispatch, ThrowingHandlerIsIsolatedAndChainContinues) {
    Trace trace;
    HookChain chain;
    chain.add(passThrough(trace, "before", 900, 0));

    // 'boom' registra l'esecuzione e poi lancia: l'eccezione deve essere
    // isolata e la catena deve proseguire col gestore successivo.
    HookNode boom;
    boom.owner = "boom";
    boom.priority = 500;
    boom.loadOrder = 1;
    boom.handler = [&trace](HookContext&) {
        trace.record("boom");
        throw std::runtime_error("kaboom");
    };
    chain.add(std::move(boom));

    chain.add(passThrough(trace, "after", 100, 2));

    // dispatch NON deve propagare l'eccezione al chiamante (gioco protetto).
    EXPECT_NO_THROW(
        chain.dispatch("Player::update", trace.original(), trace.sink()));

    // La catena prosegue: 'after' e l'originale vengono comunque eseguiti.
    EXPECT_EQ(trace.events,
              (std::vector<std::string>{"before", "boom", "after",
                                        "<original>"}));

    // Evento di errore registrato con mod + funzione (Req 3.5).
    ASSERT_EQ(trace.logs.size(), 1u);
    EXPECT_NE(trace.logs[0].find("boom"), std::string::npos);
    EXPECT_NE(trace.logs[0].find("Player::update"), std::string::npos);
}

// --- eccezione nell'ULTIMO gestore: prosegue fino all'originale (Req 3.5) --
TEST(HookChainDispatch, ThrowingLastHandlerStillReachesOriginal) {
    Trace trace;
    HookChain chain;
    chain.add(passThrough(trace, "first", 900, 0));

    HookNode boom;
    boom.owner = "lastBoom";
    boom.priority = 100;
    boom.loadOrder = 1;
    boom.handler = [&trace](HookContext&) {
        trace.record("lastBoom");
        throw std::runtime_error("late");
    };
    chain.add(std::move(boom));

    chain.dispatch("Target::fn", trace.original(), trace.sink());

    // Senza gestori successivi, la prosecuzione raggiunge l'originale.
    EXPECT_EQ(trace.events,
              (std::vector<std::string>{"first", "lastBoom", "<original>"}));
    ASSERT_EQ(trace.logs.size(), 1u);
    EXPECT_NE(trace.logs[0].find("lastBoom"), std::string::npos);
}

// --- callNext idempotente: una seconda invocazione non riesegue la coda -----
TEST(HookChainDispatch, DoubleCallNextDoesNotRunTailTwice) {
    Trace trace;
    HookChain chain;

    HookNode twice;
    twice.owner = "twice";
    twice.priority = 900;
    twice.loadOrder = 0;
    twice.handler = [&trace](HookContext& ctx) {
        trace.record("twice");
        ctx.callNext();
        ctx.callNext();  // seconda invocazione: deve essere ignorata
    };
    chain.add(std::move(twice));
    chain.add(passThrough(trace, "tail", 100, 1));

    chain.dispatch("Target::fn", trace.original(), trace.sink());

    // 'tail' e l'originale eseguiti UNA sola volta.
    EXPECT_EQ(trace.events,
              (std::vector<std::string>{"twice", "tail", "<original>"}));
}

// --- nessun sink (log nullo): dispatch resta robusto -----------------------
TEST(HookChainDispatch, NullSinkIsSafe) {
    Trace trace;
    HookChain chain;

    HookNode stopper;
    stopper.owner = "stopper";
    stopper.priority = 500;
    stopper.loadOrder = 0;
    stopper.handler = [&trace](HookContext&) { trace.record("stopper"); };
    chain.add(std::move(stopper));

    // log == nullptr: l'evento di interruzione è semplicemente scartato.
    EXPECT_NO_THROW(chain.dispatch("Target::fn", trace.original(), nullptr));
    EXPECT_EQ(trace.events, (std::vector<std::string>{"stopper"}));
}

}  // namespace
