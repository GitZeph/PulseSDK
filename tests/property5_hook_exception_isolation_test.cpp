// tests/property5_hook_exception_isolation_test.cpp
// Feature: pulse-sdk, Property 5 — isolamento delle eccezioni nella catena di
// hook.
// Validates: Requirements 3.5 (Requisito 3.5)
//
// Property 5 (design.md / Req 3.5): per ogni catena di hook in cui uno o più
// gestori terminano in modo anomalo (lanciano un'eccezione), l'esecuzione della
// catena:
//   (a) ISOLA l'errore — l'eccezione NON si propaga al chiamante (il gioco) né
//       agli altri gestori (dispatch non rilancia);
//   (b) PROSEGUE la catena invocando il gestore successivo, così OGNI gestore
//       NON lanciante viene comunque eseguito nell'ordine della catena e —
//       poiché i gestori non lancianti invocano callNext() — la catena
//       COMPLETA raggiungendo la funzione originale (Req 3.1/3.5);
//   (c) REGISTRA un evento di errore per OGNI gestore lanciante, identificando
//       la mod (owner) e la funzione coinvolte.
//
// Strategia (RapidCheck, ≥100 iterazioni — il default di RC_GTEST_PROP):
//   * si genera una catena randomizzata di N hook (N in [0, 64]) sulla stessa
//     funzione, con priorità casuali (incluse fuori dominio per esercitare la
//     saturazione) e loadOrder UNIVOCO (= indice di inserimento), così l'ordine
//     di esecuzione è totalmente determinato;
//   * un sottoinsieme randomizzato di gestori LANCIA un'eccezione; gli altri
//     registrano la propria esecuzione e invocano callNext();
//   * TUTTI i gestori registrano il proprio owner all'ingresso (sia i lancianti
//     sia i non lancianti), così la traccia documenta l'ordine di invocazione;
//   * si esegue dispatch una sola volta e si verifica (a), (b) e (c).
//
// Il test usa direttamente HookChain::dispatch + HookContext::callNext() della
// catena di hook (loader/hooking/), modellando la funzione originale tramite
// OriginalFn — esattamente il trampolino invocato dall'ultimo callNext()
// (Req 3.1). L'isolamento delle eccezioni è una proprietà della catena, non del
// backend di hooking sottostante.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "hooking/hook_chain.hpp"

namespace {

using pulse::hooking::HookChain;
using pulse::hooking::HookContext;
using pulse::hooking::HookEventSink;
using pulse::hooking::HookNode;
using pulse::hooking::OriginalFn;

// Nome (con delimitatori) della mod proprietaria del nodo i-esimo. I
// delimitatori '#' evitano ambiguità di sottostringa quando si cerca l'owner
// nei messaggi di log (es. "mod#1#" non è sottostringa di "mod#12#").
std::string ownerOf(std::size_t i) {
    return "mod#" + std::to_string(i) + "#";
}

// Funzione bersaglio fissa: il suo nome deve comparire negli eventi di errore.
constexpr std::string_view kFunctionName = "TargetLayer::update";

// --- Property 5 — isolamento delle eccezioni nella catena di hook ----------
// Feature: pulse-sdk, Property 5. Validates: Requirements 3.5.
//
// Per una catena randomizzata di N hook (0..64) in cui un sottoinsieme casuale
// lancia eccezioni: dispatch non rilancia (isolamento), ogni gestore non
// lanciante viene eseguito nell'ordine della catena, la catena raggiunge la
// funzione originale e per ogni gestore lanciante viene registrato un evento di
// errore con mod + funzione.
RC_GTEST_PROP(Property5HookExceptionIsolation,
              ThrowingHandlersAreIsolatedAndChainCompletes,
              ()) {
    // Numero randomizzato di hook concatenati: fino a 64 (Req 3 / Property 3).
    const auto n =
        *rc::gen::inRange<std::size_t>(0, 65).as("numero di hook");

    // Sottoinsieme randomizzato di gestori che lanciano un'eccezione.
    const auto throwsFlags =
        *rc::gen::container<std::vector<bool>>(n, rc::gen::arbitrary<bool>())
             .as("gestori che lanciano");

    // Priorità randomizzate, incluse fuori dal dominio [0, 1000] per esercitare
    // la saturazione applicata da add (Req 3.2). Non influenzano l'isolamento,
    // solo l'ordine di esecuzione.
    const auto priorities =
        *rc::gen::container<std::vector<int>>(n, rc::gen::inRange(-200, 1201))
             .as("priorità");

    // Traccia condivisa degli owner all'ingresso di ciascun gestore (ordine di
    // invocazione effettivo); condivisa via shared_ptr così ogni closure scrive
    // nello stesso vettore.
    auto trace = std::make_shared<std::vector<std::string>>();

    HookChain chain;
    for (std::size_t i = 0; i < n; ++i) {
        const std::string owner = ownerOf(i);
        const bool willThrow = throwsFlags[i];

        HookNode node;
        node.owner = owner;
        node.priority = priorities[i];
        node.loadOrder = static_cast<std::uint64_t>(i);  // univoco => ordine totale
        node.handler = [trace, owner, willThrow](HookContext& ctx) {
            trace->push_back(owner);  // registra l'invocazione (sempre)
            if (willThrow) {
                // Gestore che termina in modo anomalo: l'errore deve essere
                // isolato e la catena deve proseguire col successivo.
                throw std::runtime_error("kaboom in " + owner);
            }
            ctx.callNext();  // gestore trasparente: prosegue la catena
        };
        chain.add(std::move(node));
    }

    // Ordine di esecuzione atteso e mappa owner -> lancia? derivati dalla vista
    // ordinata della catena (priority DESC, loadOrder ASC). L'ordinamento è già
    // validato dalla Property 2: qui è la sorgente di verità per l'ordine.
    std::vector<std::string> expectedAllOrder;
    std::vector<std::string> expectedNonThrowingOrder;
    std::unordered_set<std::string> throwingOwners;
    expectedAllOrder.reserve(n);
    for (const HookNode& node : chain.orderedNodes()) {
        expectedAllOrder.push_back(node.owner);
        // L'indice è codificato nell'owner; ricaviamo il flag dalla generazione.
        // Estrae l'indice tra i delimitatori '#'.
        const std::size_t start = node.owner.find('#') + 1;
        const std::size_t end = node.owner.find('#', start);
        const std::size_t idx = static_cast<std::size_t>(
            std::stoul(node.owner.substr(start, end - start)));
        if (throwsFlags[idx]) {
            throwingOwners.insert(node.owner);
        } else {
            expectedNonThrowingOrder.push_back(node.owner);
        }
    }

    // Esecuzione della catena: cattura eventi diagnostici e l'invocazione
    // dell'originale (trampolino invocato dall'ultimo callNext(), Req 3.1).
    std::vector<std::string> logs;
    bool originalReached = false;
    const HookEventSink sink = [&logs](std::string_view msg) {
        logs.emplace_back(msg);
    };
    const OriginalFn original = [&originalReached] { originalReached = true; };

    // (a) Isolamento: dispatch NON deve propagare alcuna eccezione al chiamante
    //     (il gioco resta protetto, Req 3.5).
    bool dispatchThrew = false;
    try {
        chain.dispatch(kFunctionName, original, sink);
    } catch (...) {
        dispatchThrew = true;
    }
    RC_ASSERT(!dispatchThrew);

    // (b) La catena prosegue oltre ogni gestore lanciante: TUTTI i gestori sono
    //     invocati una sola volta nell'ordine della catena (i lancianti non
    //     interrompono la propagazione).
    RC_ASSERT(*trace == expectedAllOrder);

    // (b') In particolare, ogni gestore NON lanciante è eseguito nell'ordine
    //      della catena.
    std::vector<std::string> actualNonThrowing;
    actualNonThrowing.reserve(trace->size());
    for (const std::string& owner : *trace) {
        if (throwingOwners.find(owner) == throwingOwners.end()) {
            actualNonThrowing.push_back(owner);
        }
    }
    RC_ASSERT(actualNonThrowing == expectedNonThrowingOrder);

    // (b'') La catena COMPLETA raggiungendo la funzione originale: i gestori non
    //       lancianti invocano callNext() e i lancianti sono isolati, quindi
    //       l'ultimo callNext() (oltre l'ultimo gestore) raggiunge l'originale
    //       (vale anche per la catena vuota). (Req 3.1/3.5)
    RC_ASSERT(originalReached);

    // (c) Un evento di errore registrato per OGNI gestore lanciante,
    //     identificando mod (owner) e funzione (Req 3.5). I gestori non
    //     lancianti non producono eventi di interruzione (chiamano callNext),
    //     quindi i log sono esattamente gli eventi di errore isolato.
    RC_ASSERT(logs.size() == throwingOwners.size());
    for (const std::string& owner : throwingOwners) {
        // L'owner deve comparire (tra apici) in esattamente un evento di errore,
        // insieme al nome della funzione coinvolta.
        const std::string quotedOwner = "'" + owner + "'";
        std::size_t matches = 0;
        for (const std::string& log : logs) {
            const bool hasOwner = log.find(quotedOwner) != std::string::npos;
            const bool hasFunction =
                log.find(std::string(kFunctionName)) != std::string::npos;
            if (hasOwner && hasFunction) {
                ++matches;
            }
        }
        RC_ASSERT(matches == 1u);
    }
}

}  // namespace
