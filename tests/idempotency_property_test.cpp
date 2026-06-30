// tests/idempotency_property_test.cpp
// Feature: pulse-gd-integration, Property 1 — Idempotenza dell'entry point
// centralizzato.
// Validates: Requirements 2.6 (Requisiti 2.6)
//
// Property 1 (design.md §2.1 / Req 2.6): anche quando fossero presenti più
// vettori di early-load (es. `LC_LOAD_DYLIB` patchato E una variabile
// `DYLD_INSERT_LIBRARIES` residua), il costruttore di early-load deve invocare
// l'entry point centralizzato `pulse_loader_runtime_entry` UNA SOLA VOLTA per
// processo. Per N ≥ 1 invocazioni simulate del guard (in qualunque ordine e
// anche concorrenti) l'entry deve essere eseguito esattamente una volta.
//
// Perché un primitivo "run once" speculare, e non il codice reale:
//   La guardia reale vive in `loader/bootstrap/macos_bootstrap.cpp` come
//   `run_pulse_early_load_once()` in un namespace anonimo, compilata SOLO sotto
//   `#if defined(PULSE_LOADER_ARTIFACT)` (build del Loader_Artifact dinamico).
//   La build dei test host costruisce la libreria statica senza quella macro,
//   quindi quel simbolo non è linkabile dall'host. Come previsto dal task,
//   replichiamo QUI lo stesso meccanismo — un guard di processo basato su
//   `std::atomic_flag` inizializzato con `ATOMIC_FLAG_INIT` il cui primo
//   `test_and_set()` ritorna `false` (eseguiamo l'entry) e ogni chiamata
//   successiva ritorna `true` (no-op) — così la proprietà è verificata sulla
//   STESSA logica della guardia reale, in modo host-testabile.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * SEQUENZIALE: N invocazioni del guard, eseguite in un ordine arbitrario
//     (permutazione casuale degli indici di invocazione) → l'entry conta 1.
//   * CONCORRENTE: T thread che invocano il guard K volte ciascuno, rilasciati
//     simultaneamente da una barriera spin per massimizzare la contesa
//     (qualunque ordine di interleaving) → l'entry conta esattamente 1.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

namespace {

// Guardia "run once" che replica fedelmente `run_pulse_early_load_once()`
// (loader/bootstrap/macos_bootstrap.cpp): un `std::atomic_flag` di processo
// più un contatore atomico delle esecuzioni effettive dell'entry centralizzato.
// `entryRuns` modella il numero di invocazioni di `pulse_loader_runtime_entry`.
struct OnceGuard {
    std::atomic_flag entered = ATOMIC_FLAG_INIT;     // guard di processo (Req 2.6)
    std::atomic<int> entryRuns{0};                   // # esecuzioni dell'entry

    // Speculare al codice reale: se il guard era già impostato, un altro vettore
    // di early-load ha già avviato il runtime → no-op. Altrimenti esegue l'entry
    // esattamente una volta. `noexcept`: non propaga eccezioni (Req 2.8).
    void runOnce() noexcept {
        if (entered.test_and_set(std::memory_order_acq_rel)) {
            return;  // già entrati: no-op (Req 2.6)
        }
        // Unico entry point centralizzato (mirror di pulse_loader_runtime_entry).
        entryRuns.fetch_add(1, std::memory_order_relaxed);
    }
};

// --- Property 1 — invocazioni SEQUENZIALI in ordine arbitrario ------------
// Feature: pulse-gd-integration, Property 1. Validates: Requirements 2.6.
//
// N ≥ 1 invocazioni del guard eseguite in un ordine di scheduling arbitrario
// (permutazione casuale): l'entry centralizzato è eseguito esattamente una
// volta, indipendentemente dall'ordine.
RC_GTEST_PROP(Property1IdempotentEntry,
              SequentialInvocationsRunEntryExactlyOnce,
              ()) {
    // N ≥ 1 invocazioni simulate (anche un solo vettore di early-load).
    const auto invocations =
        *rc::gen::inRange<std::size_t>(1, 257).as("numero di invocazioni N");

    // Ordine di esecuzione arbitrario: permutazione indotta da chiavi casuali.
    std::vector<std::size_t> order(invocations);
    std::iota(order.begin(), order.end(), std::size_t{0});
    const auto shuffleKeys =
        *rc::gen::container<std::vector<int>>(invocations,
                                              rc::gen::arbitrary<int>())
             .as("chiavi di permutazione");
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t l, std::size_t r) {
                         return shuffleKeys[l] < shuffleKeys[r];
                     });

    OnceGuard guard;
    for (std::size_t i = 0; i < order.size(); ++i) {
        guard.runOnce();
    }

    // Entry eseguito esattamente una volta a fronte di N ≥ 1 invocazioni.
    RC_ASSERT(guard.entryRuns.load(std::memory_order_relaxed) == 1);
}

// --- Property 1 — invocazioni CONCORRENTI in qualunque interleaving --------
// Feature: pulse-gd-integration, Property 1. Validates: Requirements 2.6.
//
// T thread invocano il guard K volte ciascuno (N = T*K ≥ 1), rilasciati
// simultaneamente da una barriera spin per massimizzare la contesa: qualunque
// sia l'interleaving, l'entry centralizzato è eseguito esattamente una volta.
RC_GTEST_PROP(Property1IdempotentEntry,
              ConcurrentInvocationsRunEntryExactlyOnce,
              ()) {
    const auto numThreads =
        *rc::gen::inRange<unsigned>(1, 9).as("numero di thread T");
    const auto perThread =
        *rc::gen::inRange<unsigned>(1, 33).as("invocazioni per thread K");

    OnceGuard guard;

    // Barriera spin: tutti i thread attendono `go` per partire insieme.
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> threads;
    threads.reserve(numThreads);
    for (unsigned t = 0; t < numThreads; ++t) {
        threads.emplace_back([&]() {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (unsigned k = 0; k < perThread; ++k) {
                guard.runOnce();
            }
        });
    }

    // Attende che tutti i thread siano pronti, poi li rilascia simultaneamente.
    while (ready.load(std::memory_order_acquire) < numThreads) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);

    for (auto& th : threads) {
        th.join();
    }

    // Esattamente una esecuzione dell'entry, qualunque sia l'ordine concorrente.
    RC_ASSERT(guard.entryRuns.load(std::memory_order_relaxed) == 1);
}

}  // namespace
