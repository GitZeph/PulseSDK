// tests/property7_install_failure_atomicity_test.cpp
// Feature: pulse-sdk, Property 7 — Atomicità del fallimento di installazione.
// Validates: Requirements 2.5 (Requisiti 2.5)
//
// Property 7 (design.md / Req 2.5): «IF l'installazione di un hook fallisce
// dopo un massimo di 3 tentativi, THEN annullare tutte le modifiche parziali,
// lasciare invariato il codice della funzione bersaglio e restituire alla Mod
// un esito di errore che indica la funzione e la causa del fallimento.»
//
// Su scenari randomizzati, quando un'installazione (singola) o un batch
// (installAll) fallisce definitivamente dopo i 3 tentativi, l'Hooking Engine
// non lascia ALCUNA modifica parziale:
//   * le funzioni bersaglio restano invariate (nessun detour sul backend);
//   * il conteggio totale degli hook torna allo stato pre-chiamata (per un
//     singolo install fallito: 0 hook aggiunti per quel bersaglio; per
//     installAll: l'intero batch è annullato → totalHooks torna al valore
//     pre-batch, tipicamente 0);
//   * viene restituito un esito di errore che riporta funzione + causa;
//   * il numero di tentativi di install sul backend per un bersaglio che
//     fallisce in modo persistente è ESATTAMENTE 3 (kMaxInstallAttempts).
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * proprietà (a) — singolo install: owner/funzione/indirizzo randomizzati,
//     backend forzato a fallire ogni install (failAllInstalls): l'esito è di
//     errore, esattamente 3 tentativi, 0 hook residui, funzione invariata;
//   * proprietà (b) — batch atomico: si genera un batch su funzioni DISTINTE
//     mescolando bersagli che riescono e bersagli con fallimento iniettato
//     (failInstallAt). Se almeno un bersaglio fallisce, l'intero batch viene
//     annullato (rollback): totalHooks/installedTargets tornano a 0, nessuna
//     funzione resta hookata, l'errore riporta la funzione fallita + causa e
//     i tentativi sul backend del bersaglio fallito sono esattamente 3. Se
//     nessun bersaglio fallisce, tutti gli hook risultano installati.
//
// Usa il FakeBackend in-memory (loader/hooking/) con i suoi fallimenti
// iniettabili per pilotare in modo deterministico install/retry/rollback senza
// patchare codice macchina reale.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hooking/fake_backend.hpp"
#include "hooking/hook_engine.hpp"

namespace {

using pulse::hooking::FakeBackend;
using pulse::hooking::HookEngine;
using pulse::hooking::HookRequest;
using pulse::hooking::kMaxInstallAttempts;

int g_detour = 0;
void* const kDetour = &g_detour;

// Descrittore randomizzato di una richiesta di hook in un batch.
struct ReqSpec {
    std::string owner;
    std::string functionName;
    bool fails = false;  // se true, il bersaglio ha un fallimento iniettato
};

// Indirizzo bersaglio univoco e non nullo derivato dall'indice nel batch, così
// ogni richiesta agisce su una funzione distinta (ogni hook è "fresco" e tenta
// il backend esattamente una volta in caso di successo).
std::uintptr_t targetForIndex(std::size_t index) noexcept {
    return static_cast<std::uintptr_t>(0x10000 + (index + 1) * 0x1000);
}

HookRequest makeReq(const ReqSpec& spec, std::uintptr_t target) {
    HookRequest req;
    req.owner = spec.owner;
    req.functionName = spec.functionName;
    req.target = target;
    req.detour = kDetour;
    return req;
}

// --- Property 7 (a) — singolo install: atomicità + limite di 3 tentativi ----
// Feature: pulse-sdk, Property 7. Validates: Requirements 2.5.
//
// Un install che fallisce in modo persistente non lascia modifiche parziali e
// tenta il backend esattamente kMaxInstallAttempts (3) volte, restituendo un
// esito di errore con funzione + causa.
RC_GTEST_PROP(Property7InstallFailureAtomicity,
              SingleFailingInstallIsAtomicAndBoundedToThreeAttempts,
              ()) {
    const auto owner = *rc::gen::arbitrary<std::string>().as("owner");
    const auto functionName =
        *rc::gen::arbitrary<std::string>().as("nome funzione");
    // Indirizzo bersaglio non nullo randomizzato.
    const auto target = static_cast<std::uintptr_t>(
        *rc::gen::inRange<std::uint64_t>(1, 0x7fffffff).as("indirizzo")) *
        0x1000ULL;

    FakeBackend backend;
    backend.failAllInstalls(true);  // ogni tentativo di install fallisce
    HookEngine engine{backend};

    HookRequest req;
    req.owner = owner;
    req.functionName = functionName;
    req.target = target;
    req.detour = kDetour;

    const auto outcome = engine.install(req);

    // Esito di errore con funzione + causa (Req 2.5).
    RC_ASSERT(!outcome.installed);
    RC_ASSERT(outcome.error.has_value());
    RC_ASSERT(outcome.error->function == functionName);
    RC_ASSERT(!outcome.error->cause.message.empty());

    // Limite di tentativi: esattamente kMaxInstallAttempts (3).
    RC_ASSERT(outcome.attempts == static_cast<std::size_t>(kMaxInstallAttempts));
    RC_ASSERT(backend.installAttempts() ==
              static_cast<std::size_t>(kMaxInstallAttempts));

    // Atomicità: nessuna modifica parziale, funzione bersaglio invariata.
    RC_ASSERT(engine.totalHooks() == 0u);
    RC_ASSERT(engine.installedTargets() == 0u);
    RC_ASSERT(!engine.isTargetInstalled(target));
    RC_ASSERT(backend.installedCount() == 0u);
    RC_ASSERT(!backend.isInstalled(target));
}

// --- Property 7 (b) — batch installAll: rollback atomico del batch ----------
// Feature: pulse-sdk, Property 7. Validates: Requirements 2.5.
//
// Si genera un batch su funzioni distinte mescolando bersagli che riescono e
// bersagli con fallimento iniettato. Al primo fallimento definitivo l'intero
// batch è annullato (rollback) e lo stato torna identico al pre-batch; se
// nessun bersaglio fallisce, tutti gli hook risultano installati.
RC_GTEST_PROP(Property7InstallFailureAtomicity,
              FailingBatchIsRolledBackEntirelyAndAttemptBoundHolds,
              ()) {
    const auto specs =
        *rc::gen::container<std::vector<ReqSpec>>(
             rc::gen::construct<ReqSpec>(rc::gen::arbitrary<std::string>(),
                                         rc::gen::arbitrary<std::string>(),
                                         rc::gen::arbitrary<bool>()))
             .as("richieste del batch");

    FakeBackend backend;
    HookEngine engine{backend};

    // Costruisce il batch con bersagli univoci e inietta i fallimenti scelti.
    std::vector<HookRequest> batch;
    batch.reserve(specs.size());
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const std::uintptr_t target = targetForIndex(i);
        batch.push_back(makeReq(specs[i], target));
        if (specs[i].fails) {
            backend.failInstallAt(target);
        }
    }

    // Stato pre-batch: nessun hook installato.
    RC_ASSERT(engine.totalHooks() == 0u);
    RC_ASSERT(engine.installedTargets() == 0u);

    // Indice della prima richiesta destinata a fallire (se esiste).
    bool hasFailure = false;
    std::size_t firstFailIndex = 0;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        if (specs[i].fails) {
            hasFailure = true;
            firstFailIndex = i;
            break;
        }
    }

    const auto outcome = engine.installAll(batch);

    if (hasFailure) {
        // Esito di errore con la funzione fallita + causa (Req 2.5).
        RC_ASSERT(!outcome.installed);
        RC_ASSERT(outcome.error.has_value());
        RC_ASSERT(outcome.error->function == specs[firstFailIndex].functionName);
        RC_ASSERT(!outcome.error->cause.message.empty());

        // Rollback atomico: lo stato torna identico al pre-batch (0 hook).
        RC_ASSERT(engine.totalHooks() == 0u);
        RC_ASSERT(engine.installedTargets() == 0u);
        RC_ASSERT(backend.installedCount() == 0u);
        for (std::size_t i = 0; i < specs.size(); ++i) {
            RC_ASSERT(!engine.isTargetInstalled(targetForIndex(i)));
            RC_ASSERT(!backend.isInstalled(targetForIndex(i)));
        }

        // Limite di tentativi: i bersagli precedenti riescono in 1 tentativo
        // ciascuno, il bersaglio fallito ne consuma esattamente 3 e nulla viene
        // tentato dopo di esso. Quindi:
        //   tentativi totali == firstFailIndex + kMaxInstallAttempts
        // e il bersaglio fallito ha esattamente kMaxInstallAttempts tentativi.
        const std::size_t expectedAttempts =
            firstFailIndex + static_cast<std::size_t>(kMaxInstallAttempts);
        RC_ASSERT(outcome.attempts == expectedAttempts);
        RC_ASSERT(backend.installAttempts() == expectedAttempts);
        RC_ASSERT(backend.installAttempts() - firstFailIndex ==
                  static_cast<std::size_t>(kMaxInstallAttempts));
    } else {
        // Nessun fallimento: tutti gli hook installati sulle funzioni distinte.
        RC_ASSERT(outcome.installed);
        RC_ASSERT(!outcome.error.has_value());
        RC_ASSERT(engine.totalHooks() == specs.size());
        RC_ASSERT(engine.installedTargets() == specs.size());
        RC_ASSERT(backend.installedCount() == specs.size());
        // Ogni bersaglio fresco ha richiesto esattamente un tentativo.
        RC_ASSERT(backend.installAttempts() == specs.size());
    }
}

}  // namespace
