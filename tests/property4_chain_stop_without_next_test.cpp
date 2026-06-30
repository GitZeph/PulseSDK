// tests/property4_chain_stop_without_next_test.cpp
// Feature: pulse-sdk, Property 4 — Interruzione della catena senza next.
// Validates: Requirements 3.4 (Requisiti 3.4)
//
// Property 4 (design.md / Req 3.4): data una catena di hook generata in modo
// randomizzato, se il gestore in una posizione casuale K (nell'ordine
// deterministico della catena) ritorna SENZA invocare `callNext()`, allora:
//   (a) i gestori nelle posizioni successive a K NON vengono mai eseguiti;
//   (b) la funzione originale (trampolino) NON viene mai eseguita;
//   (c) i gestori nelle posizioni 0..K vengono eseguiti esattamente una volta,
//       nell'ordine della catena (priority DESC, poi loadOrder ASC);
//   (d) viene registrato un evento di interruzione che identifica la mod che
//       ha interrotto la catena e la funzione bersaglio.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si genera una catena di N hook (1..64); ogni nodo riceve un owner
//     UNIVOCO ("m{i}") e un `loadOrder` UNIVOCO (= indice), così l'ordine della
//     catena è totale e deterministico; le priorità sono casuali nel dominio
//     valido [0, 1000];
//   * si calcola l'ordine effettivo della catena via `orderedNodes()` e si
//     sceglie una posizione casuale K in [0, N); il nodo in posizione ordinata
//     K diventa lo "stopper" (registra l'esecuzione ma NON chiama callNext()),
//     tutti gli altri sono "trasparenti" (registrano e poi callNext());
//   * si esegue `dispatch` con un originale che registra "<original>" e un sink
//     che cattura gli eventi diagnostici, e si verificano le invarianti (a)-(d).
//
// Usa il modello in-memory della catena (loader/hooking/hook_chain.hpp); il
// FakeBackend non è necessario perché `dispatch` opera sui gestori e su un
// trampolino `OriginalFn` opaco, indipendente dal backend di hooking.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstddef>
#include <cstdint>
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

// Marcatore registrato dall'originale (trampolino) nella traccia di esecuzione.
constexpr const char* kOriginalMarker = "<original>";

// Traccia condivisa: ordine di esecuzione di gestori/originale + log eventi.
struct Trace {
    std::vector<std::string> events;
    std::vector<std::string> logs;

    void record(std::string e) { events.push_back(std::move(e)); }

    HookEventSink sink() {
        return [this](std::string_view msg) {
            logs.push_back(std::string(msg));
        };
    }
    OriginalFn original() {
        return [this] { events.emplace_back(kOriginalMarker); };
    }
};

// Costruisce un nodo "trasparente": registra l'owner e poi cede al successivo.
HookNode passThrough(Trace& trace, ModId owner, int priority,
                     std::uint64_t loadOrder) {
    HookNode node;
    node.owner = std::move(owner);
    node.priority = priority;
    node.loadOrder = loadOrder;
    ModId capturedOwner = node.owner;
    node.handler = [&trace, capturedOwner](HookContext& ctx) {
        trace.record(capturedOwner);
        ctx.callNext();
    };
    return node;
}

// Costruisce un nodo "stopper": registra l'owner ma NON chiama callNext(),
// interrompendo la catena (Req 3.4).
HookNode stopper(Trace& trace, ModId owner, int priority,
                 std::uint64_t loadOrder) {
    HookNode node;
    node.owner = std::move(owner);
    node.priority = priority;
    node.loadOrder = loadOrder;
    ModId capturedOwner = node.owner;
    node.handler = [&trace, capturedOwner](HookContext&) {
        trace.record(capturedOwner);
        // Ritorna senza invocare callNext(): la catena si interrompe qui.
    };
    return node;
}

// --- Property 4 — interruzione della catena senza next (Req 3.4) -----------
// Feature: pulse-sdk, Property 4. Validates: Requirements 3.4.
RC_GTEST_PROP(Property4ChainStopWithoutNext,
              HandlerWithoutCallNextStopsChainAndOriginal,
              ()) {
    // Numero di hook nella catena: da 1 a 64.
    const int n = *rc::gen::inRange(1, 65).as("numero di hook");

    // Priorità casuali nel dominio valido [0, 1000] (Req 3.2): l'ordinamento
    // resta totale grazie al loadOrder univoco usato come tie-break.
    const auto priorities =
        *rc::gen::container<std::vector<int>>(
             static_cast<std::size_t>(n), rc::gen::inRange(0, 1001))
             .as("priorità dei nodi");

    const auto count = static_cast<std::size_t>(n);

    // Owner univoci ("m{i}") e loadOrder univoco (= indice): ordine totale.
    const auto ownerOf = [](std::size_t i) {
        return "m" + std::to_string(i);
    };

    // (1) Determina l'ordine effettivo della catena costruendo una catena di
    //     sondaggio (gli handler non influenzano l'ordinamento).
    Trace probeTrace;  // non eseguita: serve solo a costruire i nodi.
    HookChain orderChain;
    for (std::size_t i = 0; i < count; ++i) {
        orderChain.add(passThrough(probeTrace, ownerOf(i), priorities[i],
                                   static_cast<std::uint64_t>(i)));
    }

    std::vector<ModId> orderedOwners;
    orderedOwners.reserve(count);
    for (const auto& node : orderChain.orderedNodes()) {
        orderedOwners.push_back(node.owner);
    }
    RC_ASSERT(orderedOwners.size() == count);

    // (2) Sceglie la posizione ordinata K dell'interruttore in [0, N).
    const auto k = static_cast<std::size_t>(
        *rc::gen::inRange<int>(0, n).as("indice di interruzione K"));
    const ModId stopperOwner = orderedOwners[k];

    // (3) Costruisce la catena reale: il nodo in posizione ordinata K è lo
    //     stopper, tutti gli altri sono trasparenti.
    Trace trace;
    HookChain chain;
    for (std::size_t i = 0; i < count; ++i) {
        if (ownerOf(i) == stopperOwner) {
            chain.add(stopper(trace, ownerOf(i), priorities[i],
                              static_cast<std::uint64_t>(i)));
        } else {
            chain.add(passThrough(trace, ownerOf(i), priorities[i],
                                  static_cast<std::uint64_t>(i)));
        }
    }

    const std::string functionName = "MenuLayer::init";
    chain.dispatch(functionName, trace.original(), trace.sink());

    // (c) I gestori 0..K sono eseguiti, in ordine; lo stopper è l'ultimo.
    std::vector<std::string> expectedEvents(orderedOwners.begin(),
                                            orderedOwners.begin() +
                                                static_cast<std::ptrdiff_t>(k) +
                                                1);
    RC_ASSERT(trace.events == expectedEvents);

    // (b) L'originale non è mai stato invocato.
    for (const auto& e : trace.events) {
        RC_ASSERT(e != kOriginalMarker);
    }

    // (a) I gestori dopo K non sono stati eseguiti (deducibile da (c), ma reso
    //     esplicito): nessun owner in posizione > K compare nella traccia.
    for (std::size_t j = k + 1; j < count; ++j) {
        const ModId& laterOwner = orderedOwners[j];
        for (const auto& e : trace.events) {
            RC_ASSERT(e != laterOwner);
        }
    }

    // (d) Registrato un evento di interruzione con mod + funzione (Req 3.4).
    RC_ASSERT(trace.logs.size() == 1u);
    RC_ASSERT(trace.logs[0].find(stopperOwner) != std::string::npos);
    RC_ASSERT(trace.logs[0].find(functionName) != std::string::npos);
}

}  // namespace
