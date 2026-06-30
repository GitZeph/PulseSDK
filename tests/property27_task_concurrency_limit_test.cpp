// tests/property27_task_concurrency_limit_test.cpp
// Feature: pulse-sdk, Property 27 — Limite di concorrenza dei task asincroni.
// Validates: Requirements 11.5 (Requisito 11.5)
//
// Property 27 (design.md / Req 11.5): per ogni Mod, il TaskScheduler ammette
// al massimo `kMaxConcurrentTasksPerMod` (= 64) task asincroni simultanei.
// Mantenendo i task in volo con un lavoro bloccante (rilasciato da un flag
// atomico), l'invariante da verificare è:
//   (a) per ciascuna Mod i primi fino-a-64 spawn hanno successo e OGNI spawn
//       oltre il 64° è rifiutato con TaskErrorCode::ConcurrencyLimitExceeded;
//   (b) activeCount(mod) non supera mai 64 (mentre i task sono in volo);
//   (c) il limite è applicato in modo INDIPENDENTE per ogni Mod — il fatto che
//       una Mod raggiunga il limite non riduce la capacità di un'altra Mod
//       (isolamento per-mod).
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si genera un numero modesto di Mod (≤ 4) e, per ciascuna, un numero di
//     spawn richiesti che spazia sia SOTTO sia SOPRA il limite di 64 (cap a
//     ~70 per non esaurire i thread del sistema e mantenere il test veloce);
//   * il corpo di lavoro è bloccante: resta in attesa di un flag atomico
//     `release`, così tutti i task accettati restano contemporaneamente "in
//     volo" e il conteggio attivo riflette davvero la concorrenza;
//   * si conta, per ogni Mod, il numero di spawn riusciti e si verificano gli
//     invarianti (a)/(b)/(c);
//   * LIFETIME: lo scheduler viene distrutto (join dei worker) PRIMA del flag
//     `release`, e si attende comunque l'idle prima di uscire dallo scope, per
//     evitare che un worker acceda a stato già distrutto (stesso schema dei
//     test in tests/task_async_test.cpp).

#include <pulse/async.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

using pulse::kMaxConcurrentTasksPerMod;
using pulse::Task;
using pulse::TaskErrorCode;
using pulse::TaskScheduler;
using namespace std::chrono_literals;

namespace {

// Attende (con timeout) che la Mod non abbia più task in volo: garantisce che
// i worker abbiano scaricato il loro stato prima della distruzione.
void waitUntilIdle(TaskScheduler& scheduler, const std::string& modId) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (scheduler.activeCount(modId) != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
}

}  // namespace

// Property 27: limite di concorrenza per-mod, applicato in isolamento.
RC_GTEST_PROP(Property27TaskConcurrency,
              LimitEnforcedIndependentlyPerMod,
              ()) {
    // Numero modesto di Mod (1..4) per non esaurire i thread del sistema.
    const std::size_t numMods =
        *rc::gen::inRange<std::size_t>(1, 5).as("numero di mod");

    // Per ogni Mod, un numero di spawn richiesti che attraversa il limite:
    // 1..70 (alcuni sotto 64, altri sopra). Il cap a 70 mantiene il test rapido.
    std::vector<std::size_t> requested;
    requested.reserve(numMods);
    for (std::size_t i = 0; i < numMods; ++i) {
        requested.push_back(
            *rc::gen::inRange<std::size_t>(1, 71).as("spawn richiesti per mod"));
    }

    // Flag di rilascio del lavoro bloccante: dichiarato PRIMA dello scheduler
    // così viene distrutto DOPO di esso (lo scheduler, nel suo distruttore,
    // unisce i worker che leggono `release`).
    std::atomic<bool> release{false};

    {
        TaskScheduler scheduler;  // distrutto (join dei worker) PRIMA di `release`.

        const auto blockingWork = [&release]() -> int {
            while (!release.load()) {
                std::this_thread::sleep_for(1ms);
            }
            return 1;
        };

        std::vector<std::string> modIds;
        modIds.reserve(numMods);
        std::map<std::string, std::size_t> succeeded;

        // Avvia, per ciascuna Mod, gli spawn richiesti e verifica l'esito di
        // ciascun avvio rispetto al limite per-mod.
        for (std::size_t i = 0; i < numMods; ++i) {
            const std::string modId = "mod." + std::to_string(i);
            modIds.push_back(modId);

            std::size_t ok = 0;
            for (std::size_t k = 0; k < requested[i]; ++k) {
                auto spawned = scheduler.spawn<int>(modId, blockingWork);
                if (k < kMaxConcurrentTasksPerMod) {
                    // (a) i primi fino-a-64 spawn DEVONO riuscire.
                    RC_ASSERT(spawned.isOk());
                    ++ok;
                } else {
                    // (a) ogni spawn oltre il 64° DEVE essere rifiutato con
                    //     ConcurrencyLimitExceeded.
                    RC_ASSERT(!spawned.isOk());
                    RC_ASSERT(spawned.error().code ==
                              TaskErrorCode::ConcurrencyLimitExceeded);
                }
            }
            succeeded[modId] = ok;

            // (b) il conteggio attivo non supera mai il limite.
            RC_ASSERT(scheduler.activeCount(modId) <= kMaxConcurrentTasksPerMod);
        }

        // (b)/(c) con TUTTE le Mod ancora in volo: ogni Mod ha esattamente
        // min(richiesti, 64) task attivi, indipendentemente dalle altre. Questo
        // dimostra l'isolamento: una Mod al limite non riduce la capacità di
        // un'altra.
        for (std::size_t i = 0; i < numMods; ++i) {
            const std::string& modId = modIds[i];
            const std::size_t expected =
                std::min<std::size_t>(requested[i], kMaxConcurrentTasksPerMod);
            RC_ASSERT(succeeded[modId] == expected);
            RC_ASSERT(scheduler.activeCount(modId) == expected);
            RC_ASSERT(scheduler.activeCount(modId) <= kMaxConcurrentTasksPerMod);
        }

        // Rilascia il lavoro bloccante e attende che ogni Mod torni idle, così
        // i worker completano prima che lo scheduler venga distrutto.
        release.store(true);
        for (const auto& modId : modIds) {
            waitUntilIdle(scheduler, modId);
        }
    }  // ~TaskScheduler: join dei worker (che hanno già osservato release==true).
}
