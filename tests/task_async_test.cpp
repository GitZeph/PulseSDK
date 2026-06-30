// task_async_test.cpp — unit test del sistema asincrono pulse::async (task 29.1).
//
// Copre le semantiche osservabili del Requisito 11 tramite l'header pubblico
// <pulse/async.hpp> e il TaskScheduler host-testabile:
//   * spawn avvia il lavoro su un thread separato e ritorna SENZA bloccare il
//     thread principale (Req 11.1); il lavoro viene effettivamente eseguito;
//   * then() registra una continuazione eseguita sul thread principale solo al
//     "frame successivo" (pumpMainThread) sia in caso di successo (Req 11.2)
//     sia in caso di errore, ricevendo la causa del fallimento (Req 11.3);
//   * il 65° task simultaneo di una mod è rifiutato (Req 11.5);
//   * la cancellazione alla disabilitazione impedisce l'esecuzione delle
//     continuazioni pendenti (Req 11.4).
#include <pulse/async.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using pulse::Task;
using pulse::TaskErrorCode;
using pulse::TaskResult;
using pulse::TaskScheduler;
using namespace std::chrono_literals;

namespace {

// Attende (con timeout) che la mod non abbia più task in volo, così il pump
// successivo trova le continuazioni pronte in modo deterministico.
void waitUntilIdle(TaskScheduler& scheduler, const std::string& modId) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (scheduler.activeCount(modId) != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
}

}  // namespace

// Req 11.1: l'avvio non blocca il thread principale e il lavoro viene eseguito.
// Il lavoro dorme 200 ms: se spawn bloccasse, l'avvio impiegherebbe ≥ 200 ms;
// verifichiamo invece che ritorni in una frazione minima di quel tempo.
TEST(TaskAsyncTest, SpawnDoesNotBlockAndRunsWork) {
    TaskScheduler scheduler;

    const auto start = std::chrono::steady_clock::now();
    auto spawned = scheduler.spawn<int>("mod.alpha", [] {
        std::this_thread::sleep_for(200ms);
        return 42;
    });
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_TRUE(spawned.isOk());
    // L'avvio ritorna ben prima del completamento del lavoro (non bloccante).
    EXPECT_LT(elapsed, 100ms);

    int delivered = 0;
    spawned.value().then([&delivered](TaskResult<int> r) {
        ASSERT_TRUE(r.isOk());
        delivered = r.value();
    });

    waitUntilIdle(scheduler, "mod.alpha");
    scheduler.pumpMainThread();
    EXPECT_EQ(delivered, 42);
}

// Req 11.2: la continuazione viene eseguita sul thread principale SOLO al frame
// successivo (pumpMainThread), non durante l'esecuzione del lavoro.
TEST(TaskAsyncTest, ContinuationRunsOnlyOnMainThreadPump) {
    TaskScheduler scheduler;

    std::atomic<bool> ran{false};
    auto spawned = scheduler.spawn<int>("mod.alpha", [] { return 7; });
    ASSERT_TRUE(spawned.isOk());
    spawned.value().then([&ran](TaskResult<int> r) {
        ASSERT_TRUE(r.isOk());
        EXPECT_EQ(r.value(), 7);
        ran.store(true);
    });

    waitUntilIdle(scheduler, "mod.alpha");
    // Prima del pump la continuazione non è ancora stata eseguita.
    EXPECT_FALSE(ran.load());

    scheduler.pumpMainThread();
    EXPECT_TRUE(ran.load());
}

// Req 11.3: se il lavoro termina con errore, la continuazione viene comunque
// eseguita sul thread principale ricevendo un'indicazione di errore con la
// causa, senza arrestare il thread principale.
TEST(TaskAsyncTest, FailureDeliversErrorToContinuation) {
    TaskScheduler scheduler;

    bool invoked = false;
    auto spawned = scheduler.spawn<int>("mod.alpha", []() -> int {
        throw std::runtime_error("boom");
    });
    ASSERT_TRUE(spawned.isOk());
    spawned.value().then([&invoked](TaskResult<int> r) {
        invoked = true;
        EXPECT_FALSE(r.isOk());
        EXPECT_EQ(r.error().code, TaskErrorCode::WorkFailed);
        EXPECT_NE(r.error().message.find("boom"), std::string::npos);
    });

    waitUntilIdle(scheduler, "mod.alpha");
    scheduler.pumpMainThread();
    EXPECT_TRUE(invoked);
}

// Req 11.5: oltre 64 task simultanei per la stessa mod, l'avvio aggiuntivo è
// rifiutato con un errore ConcurrencyLimitExceeded. Manteniamo 64 task in volo
// con un lavoro bloccante finché non li rilasciamo.
TEST(TaskAsyncTest, SixtyFifthConcurrentTaskRejected) {
    std::atomic<bool> release{false};
    TaskScheduler scheduler;  // distrutto (join) PRIMA di `release`.

    std::vector<Task<int>> tasks;
    tasks.reserve(64);
    for (int i = 0; i < 64; ++i) {
        auto spawned = scheduler.spawn<int>("mod.alpha", [&release]() -> int {
            while (!release.load()) {
                std::this_thread::sleep_for(1ms);
            }
            return 1;
        });
        ASSERT_TRUE(spawned.isOk()) << "il task " << i << " doveva essere avviato";
        tasks.push_back(std::move(spawned.value()));
    }

    EXPECT_EQ(scheduler.activeCount("mod.alpha"), 64u);

    // Il 65° è rifiutato.
    auto overflow = scheduler.spawn<int>("mod.alpha", [] { return 2; });
    ASSERT_FALSE(overflow.isOk());
    EXPECT_EQ(overflow.error().code, TaskErrorCode::ConcurrencyLimitExceeded);

    // Una mod diversa non è soggetta al limite dell'altra (isolamento).
    auto otherMod = scheduler.spawn<int>("mod.beta", [] { return 3; });
    EXPECT_TRUE(otherMod.isOk());

    // Rilascia i task in volo e attende che lo slot si liberi.
    release.store(true);
    waitUntilIdle(scheduler, "mod.alpha");
}

// Req 11.4: disabilitare la mod annulla i task in corso e impedisce
// l'esecuzione delle loro continuazioni sul thread principale.
TEST(TaskAsyncTest, CancellationPreventsPendingContinuation) {
    std::atomic<bool> release{false};
    TaskScheduler scheduler;  // distrutto (join) PRIMA di `release`.

    std::atomic<bool> ran{false};
    auto spawned = scheduler.spawn<int>("mod.alpha", [&release]() -> int {
        while (!release.load()) {
            std::this_thread::sleep_for(1ms);
        }
        return 99;
    });
    ASSERT_TRUE(spawned.isOk());
    spawned.value().then([&ran](TaskResult<int>) { ran.store(true); });

    // La mod viene disabilitata mentre il task è ancora in volo.
    scheduler.cancelMod("mod.alpha");

    // Lascia terminare il lavoro e prova a pompare: la continuazione NON deve
    // essere eseguita perché il task è stato annullato.
    release.store(true);
    waitUntilIdle(scheduler, "mod.alpha");
    scheduler.pumpMainThread();
    EXPECT_FALSE(ran.load());
}

// Req 11.4 (variante): un task già completato ma non ancora pompato viene
// annullato: la continuazione pendente non viene eseguita al pump successivo.
TEST(TaskAsyncTest, CancellationDropsCompletedButUnpumpedContinuation) {
    TaskScheduler scheduler;

    std::atomic<bool> ran{false};
    auto spawned = scheduler.spawn<int>("mod.alpha", [] { return 5; });
    ASSERT_TRUE(spawned.isOk());
    spawned.value().then([&ran](TaskResult<int>) { ran.store(true); });

    waitUntilIdle(scheduler, "mod.alpha");  // lavoro completato, non ancora pompato.
    scheduler.cancelMod("mod.alpha");       // disabilitazione prima del pump.

    scheduler.pumpMainThread();
    EXPECT_FALSE(ran.load());
}

// Lo scheduler globale usato da Task<T>::spawn supporta lo stesso ciclo
// spawn → then → pump (API pubblica dei Developer).
TEST(TaskAsyncTest, GlobalSchedulerSpawnAndPump) {
    int delivered = 0;
    auto spawned = Task<int>::spawn("mod.global", [] { return 21 * 2; });
    ASSERT_TRUE(spawned.isOk());
    spawned.value().then([&delivered](TaskResult<int> r) {
        ASSERT_TRUE(r.isOk());
        delivered = r.value();
    });

    waitUntilIdle(TaskScheduler::global(), "mod.global");
    TaskScheduler::global().pumpMainThread();
    EXPECT_EQ(delivered, 42);
}
