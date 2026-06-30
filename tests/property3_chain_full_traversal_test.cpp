// tests/property3_chain_full_traversal_test.cpp
// Feature: pulse-sdk, Property 3 — Concatenamento completo della catena (2..64).
// Validates: Requirements 3.1 (Requisiti 3.1)
//
// Property 3 (design.md / Req 3.1): per ogni numero N di hook con 2 ≤ N ≤ 64
// sulla stessa funzione, l'esecuzione della catena visita TUTTI gli N gestori
// nell'ordine stabilito (priority DESC, poi loadOrder ASC) e TERMINA invocando
// la funzione originale quando l'ultimo gestore chiama il successivo.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si genera N in [2, 64] e, per ciascuno degli N nodi, una priorità grezza
//     casuale (anche fuori dal dominio [0, 1000], per esercitare la saturazione
//     applicata da `add`); il `loadOrder` di ogni nodo è UNIVOCO (= indice di
//     creazione), così l'ordine della catena è totale e completamente
//     determinato dai dati — l'ordine atteso si legge da `orderedNodes()`;
//   * ogni nodo riceve un gestore "trasparente" che registra il proprio owner
//     e poi invoca `callNext()`, propagando lungo l'intera catena;
//   * la funzione originale (trampolino) è modellata dal FakeBackend in-memory
//     (loader/hooking/): si semina un bersaglio, lo si installa per ottenere un
//     `Trampoline` valido e l'`OriginalFn` di dispatch registra l'esecuzione
//     dell'originale verificando che il trampolino sia valido (Req 3.1);
//   * dopo `dispatch`, si verifica che la traccia di esecuzione coincida
//     esattamente con gli owner in ordine di `orderedNodes()` seguiti da un
//     singolo evento dell'originale in coda — cioè ogni nodo è invocato una e
//     una sola volta, nell'ordine corretto, e l'originale è eseguito per ultimo.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "hooking/fake_backend.hpp"
#include "hooking/hook_chain.hpp"

namespace {

using pulse::hooking::FakeBackend;
using pulse::hooking::HookChain;
using pulse::hooking::HookContext;
using pulse::hooking::HookEventSink;
using pulse::hooking::HookNode;
using pulse::hooking::ModId;
using pulse::hooking::OriginalFn;
using pulse::hooking::Trampoline;

// Marcatore registrato dall'originale (trampolino) al termine della catena.
constexpr const char* kOriginalMarker = "<original>";

// Traccia dell'ordine di esecuzione: ogni gestore appende il proprio owner;
// l'originale appende kOriginalMarker.
struct Trace {
    std::vector<std::string> events;
};

// Costruisce un nodo "trasparente": registra l'owner e propaga con callNext().
HookNode passThrough(Trace& trace, ModId owner, int priority,
                     std::uint64_t loadOrder) {
    HookNode node;
    node.owner = owner;
    node.priority = priority;
    node.loadOrder = loadOrder;
    node.handler = [&trace, owner](HookContext& ctx) {
        trace.events.push_back(owner);
        ctx.callNext();
    };
    return node;
}

// Sequenza attesa di owner: l'ordine deterministico della catena.
std::vector<std::string> expectedOwners(const HookChain& chain) {
    std::vector<std::string> owners;
    owners.reserve(chain.size());
    for (const auto& node : chain.orderedNodes()) {
        owners.push_back(node.owner);
    }
    return owners;
}

// --- Property 3 — concatenamento completo (2..64) -------------------------
// Feature: pulse-sdk, Property 3. Validates: Requirements 3.1.
//
// Per ogni catena randomizzata di N hook con 2 ≤ N ≤ 64, quando ogni gestore
// invoca callNext() l'esecuzione visita tutti gli N gestori nell'ordine
// stabilito e termina invocando l'originale (modellato dal trampolino del
// FakeBackend) esattamente una volta in coda.
RC_GTEST_PROP(Property3ChainFullTraversal,
              VisitsEveryHandlerInOrderThenOriginal,
              ()) {
    // N nel dominio richiesto [2, 64] (inRange ha estremo superiore esclusivo).
    const int n = *rc::gen::inRange(2, 65).as("numero di hook N (2..64)");

    // Priorità grezze casuali (fuori da [0,1000] incluso, per la saturazione).
    const auto priorities =
        *rc::gen::container<std::vector<int>>(
             static_cast<std::size_t>(n), rc::gen::inRange(-200, 1201))
             .as("priorità grezze dei nodi");

    // Costruisce la catena: owner univoco per nodo, loadOrder univoco (= indice)
    // così l'ordine è totale e deterministico.
    Trace trace;
    HookChain chain;
    for (int i = 0; i < n; ++i) {
        chain.add(passThrough(trace, "m" + std::to_string(i), priorities[i],
                              static_cast<std::uint64_t>(i)));
    }
    RC_ASSERT(chain.size() == static_cast<std::size_t>(n));

    // L'originale è il trampolino restituito dal FakeBackend per il bersaglio.
    FakeBackend backend;
    const std::uintptr_t target = 0x4000;
    backend.seedOriginal(target, FakeBackend::Bytes(16, 0xAB));
    int detourMarker = 0;
    auto installed = backend.install(target, &detourMarker);
    RC_ASSERT(installed.has_value());  // install riuscito → trampolino valido
    const Trampoline trampoline = std::move(installed).value();
    RC_ASSERT(trampoline.valid());

    // OriginalFn: registra l'esecuzione dell'originale verificando il trampolino.
    const OriginalFn original = [&trace, &trampoline] {
        // Il trampolino verso l'originale deve essere valido al momento della
        // chiamata finale della catena (Req 3.1).
        RC_ASSERT(trampoline.valid());
        trace.events.emplace_back(kOriginalMarker);
    };

    // Sink diagnostico: in un concatenamento completo (tutti chiamano next e
    // nessuno lancia) NON deve essere registrato alcun evento.
    std::vector<std::string> logs;
    const HookEventSink sink = [&logs](std::string_view msg) {
        logs.emplace_back(msg);
    };

    // Ordine atteso PRIMA del dispatch (la catena non è mutata da dispatch).
    std::vector<std::string> expected = expectedOwners(chain);
    expected.emplace_back(kOriginalMarker);

    chain.dispatch("MenuLayer::init", original, sink);

    // (1) Ogni nodo è invocato esattamente una volta + originale una volta:
    //     dimensione totale della traccia = N + 1.
    RC_ASSERT(trace.events.size() == static_cast<std::size_t>(n) + 1);

    // (2) La traccia coincide con gli owner nell'ordine della catena, con
    //     l'originale come ultimo evento (concatenamento completo, Req 3.1).
    RC_ASSERT(trace.events == expected);

    // (3) L'originale è eseguito esattamente una volta ed è l'ULTIMO evento.
    RC_ASSERT(trace.events.back() == kOriginalMarker);

    // (4) Concatenamento "pulito": nessun evento di interruzione/errore.
    RC_ASSERT(logs.empty());
}

}  // namespace
