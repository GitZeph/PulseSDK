// tests/property28_task_cancellation_test.cpp
// Feature: pulse-sdk, Property 28 — Cancellazione dei task alla disabilitazione.
// Validates: Requirements 11.4 (Requisito 11.4)
//
// Property 28 (design.md / Req 11.4): per ogni insieme di operazioni
// asincrone in corso avviate da una mod, la disabilitazione della mod
// (cancelMod) annulla quelle operazioni e impedisce l'esecuzione delle loro
// continuazioni sul thread principale. Le continuazioni delle mod NON
// disabilitate, invece, devono essere eseguite esattamente una volta al
// pumpMainThread().
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si genera un numero modesto di Mod (1..3) e, per ciascuna, un numero di
//     task (1..8). Ogni task ha una continuazione registrata via then() che
//     incrementa un contatore per-task condiviso, così possiamo osservare
//     quante volte ciascuna continuazione è stata eseguita;
//   * un sottoinsieme casuale di Mod viene disabilitato (cancelMod). Per ogni
//     Mod disabilitata si sceglie casualmente il MOMENTO della cancellazione:
//       - "prima" del completamento (mentre i task sono ancora in volo, con il
//         lavoro bloccato da un flag atomico), oppure
//       - "dopo" il completamento ma PRIMA del pump (lavoro rilasciato e idle,
//         continuazioni accodate ma non ancora eseguite);
//     questo copre sia i task in volo sia quelli completati-ma-non-pompati;
//   * il corpo di lavoro è bloccante: resta in attesa del flag atomico
//     `release`, così possiamo controllare con precisione la tempistica della
//     cancellazione rispetto al completamento;
//   * dopo aver rilasciato il lavoro e atteso l'idle di TUTTE le Mod, si chiama
//     pumpMainThread() e si verifica l'INVARIANTE:
//       (a) NESSUNA continuazione di una Mod disabilitata viene eseguita (0);
//       (b) OGNI continuazione di una Mod NON disabilitata viene eseguita
//           esattamente una volta (1).
//   * LIFETIME: il flag `release` è dichiarato PRIMA dello scheduler, così
//     viene distrutto DOPO di esso; inoltre si rilascia il lavoro e si attende
//     l'idle di tutte le Mod prima che lo scheduler venga distrutto (stesso
//     schema dei test in tests/task_async_test.cpp e property27_*).

#include <pulse/async.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

using pulse::Task;
using pulse::TaskResult;
using pulse::TaskScheduler;
using namespace std::chrono_literals;

namespace {

// Attende (con timeout) che la Mod non abbia più task in volo: garantisce che
// i worker abbiano scaricato il loro stato prima del pump e della distruzione.
void waitUntilIdle(TaskScheduler& scheduler, const std::string& modId) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (scheduler.activeCount(modId) != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
}

}  // namespace

// Property 28: la disabilitazione di una Mod annulla i suoi task e impedisce
// l'esecuzione delle loro continuazioni sul thread principale, mentre le
// continuazioni delle Mod non disabilitate vengono eseguite esattamente una
// volta.
RC_GTEST_PROP(Property28TaskCancellation,
              DisableCancelsContinuationsPerMod,
              ()) {
    // Numero modesto di Mod (1..3) per mantenere il test veloce.
    const std::size_t numMods =
        *rc::gen::inRange<std::size_t>(1, 4).as("numero di mod");

    // Per ogni Mod: quanti task avviare (1..8), se la Mod va disabilitata e —
    // se sì — quando (prima del completamento oppure dopo, prima del pump).
    std::vector<std::size_t> tasksPerMod;
    std::vector<bool> disabled;
    std::vector<bool> cancelAfterComplete;
    tasksPerMod.reserve(numMods);
    disabled.reserve(numMods);
    cancelAfterComplete.reserve(numMods);
    for (std::size_t i = 0; i < numMods; ++i) {
        tasksPerMod.push_back(
            *rc::gen::inRange<std::size_t>(1, 9).as("task per mod"));
        disabled.push_back(*rc::gen::arbitrary<bool>().as("mod disabilitata"));
        cancelAfterComplete.push_back(
            *rc::gen::arbitrary<bool>().as("cancella dopo il completamento"));
    }

    // Flag di rilascio del lavoro bloccante: dichiarato PRIMA dello scheduler
    // così viene distrutto DOPO di esso (il distruttore dello scheduler unisce
    // i worker che leggono `release`).
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

        // Contatori di esecuzione per-task condivisi: usiamo shared_ptr così le
        // continuazioni catturano un riferimento stabile anche dopo la fine
        // dello scope di costruzione. Indice [mod][task].
        std::vector<std::vector<std::shared_ptr<std::atomic<int>>>> counters;
        counters.resize(numMods);

        // Avvia tutti i task di tutte le Mod, registrando per ciascuno una
        // continuazione che incrementa il proprio contatore.
        for (std::size_t i = 0; i < numMods; ++i) {
            const std::string modId = "mod." + std::to_string(i);
            modIds.push_back(modId);

            std::vector<Task<int>> tasks;
            tasks.reserve(tasksPerMod[i]);
            for (std::size_t k = 0; k < tasksPerMod[i]; ++k) {
                auto counter = std::make_shared<std::atomic<int>>(0);
                counters[i].push_back(counter);

                auto spawned = scheduler.spawn<int>(modId, blockingWork);
                // Con ≤ 8 task per mod restiamo ben sotto il limite di 64.
                RC_ASSERT(spawned.isOk());
                spawned.value().then(
                    [counter](TaskResult<int>) { counter->fetch_add(1); });
                tasks.push_back(std::move(spawned.value()));
            }
        }

        // FASE 1 — cancellazione "prima del completamento" (task ancora in
        // volo, lavoro bloccato): disabilita le Mod marcate che devono essere
        // cancellate mentre i task sono in volo.
        for (std::size_t i = 0; i < numMods; ++i) {
            if (disabled[i] && !cancelAfterComplete[i]) {
                scheduler.cancelMod(modIds[i]);
            }
        }

        // Rilascia il lavoro bloccante e attende che TUTTE le Mod tornino idle:
        // a questo punto i worker hanno completato e le continuazioni non
        // ancora cancellate risultano accodate ma non eseguite (manca il pump).
        release.store(true);
        for (const auto& modId : modIds) {
            waitUntilIdle(scheduler, modId);
        }

        // FASE 2 — cancellazione "dopo il completamento, prima del pump": le
        // continuazioni sono già accodate ma non eseguite; la disabilitazione
        // deve comunque impedirne l'esecuzione al pump (Req 11.4, variante
        // completato-ma-non-pompato).
        for (std::size_t i = 0; i < numMods; ++i) {
            if (disabled[i] && cancelAfterComplete[i]) {
                scheduler.cancelMod(modIds[i]);
            }
        }

        // Tick di frame sul thread principale: esegue le continuazioni pronte
        // delle sole Mod non disabilitate.
        scheduler.pumpMainThread();

        // INVARIANTE Property 28:
        //   (a) Mod disabilitata  => ogni continuazione eseguita 0 volte;
        //   (b) Mod non disabilitata => ogni continuazione eseguita 1 volta.
        for (std::size_t i = 0; i < numMods; ++i) {
            const int expected = disabled[i] ? 0 : 1;
            for (std::size_t k = 0; k < counters[i].size(); ++k) {
                RC_ASSERT(counters[i][k]->load() == expected);
            }
        }

        // Un secondo pump non deve cambiare nulla: nessuna continuazione viene
        // eseguita più di una volta e nessuna continuazione cancellata "riemerge".
        scheduler.pumpMainThread();
        for (std::size_t i = 0; i < numMods; ++i) {
            const int expected = disabled[i] ? 0 : 1;
            for (std::size_t k = 0; k < counters[i].size(); ++k) {
                RC_ASSERT(counters[i][k]->load() == expected);
            }
        }

        // Già idle e lavoro rilasciato: il distruttore dello scheduler unisce i
        // worker (che hanno osservato release==true) prima di `release`.
    }
}
