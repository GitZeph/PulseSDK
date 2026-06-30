// tests/hot_reload_test.cpp — unit test dell'hot-reload dev (task 32.1,
// Req 15.1, 15.2, 15.3, 15.4, 15.5; design IMP-11).
//
// Verifica l'orchestrazione lato loader del servizio `HotReload`:
//   * (Req 15.5) il reload richiesto FUORI dalla modalità sviluppo è rifiutato
//     e la mod attualmente caricata resta invariata;
//   * (Req 15.2) gli hook della versione precedente vengono rimossi PRIMA di
//     installare quelli nuovi — l'ordine è osservato tramite il FakeBackend
//     nella callback `onPreviousRemoved`;
//   * (Req 15.1, 15.3) un reload riuscito sostituisce gli hook della versione
//     precedente con quelli nuovi entro il budget di 5 s e conferma;
//   * (Req 15.4) se l'installazione della nuova versione fallisce, lo stato
//     precedente al reload viene ripristinato e il "processo di GD" resta
//     attivo (la chiamata ritorna, niente crash);
//   * (Req 15.1) un reload che eccede il budget di 5 s — pilotato da un clock
//     iniettato — è trattato come fallimento con ripristino dello stato
//     precedente.
//
// Usa il `FakeBackend` in-memory (loader/hooking/) con i suoi fallimenti
// iniettabili e un clock controllato per il budget, senza patchare codice reale.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "hooking/fake_backend.hpp"
#include "hooking/hook_engine.hpp"
#include "lifecycle/hot_reload.hpp"

namespace {

using pulse::hooking::FakeBackend;
using pulse::hooking::HookEngine;
using pulse::hooking::HookRequest;
using pulse::lifecycle::HotReload;
using pulse::lifecycle::HotReloadStatus;

int g_detour = 0;
void* const kDetour = &g_detour;

HookRequest makeReq(std::string owner, std::string fn, std::uintptr_t target,
                    int priority = 500, std::uint64_t loadOrder = 0) {
    HookRequest req;
    req.owner = std::move(owner);
    req.functionName = std::move(fn);
    req.target = target;
    req.detour = kDetour;
    req.priority = priority;
    req.loadOrder = loadOrder;
    return req;
}

// Clock controllato: avanza solo quando il test chiama advance(). Permette di
// pilotare il budget di reload (Req 15.1) in modo deterministico.
class ManualClock {
public:
    std::chrono::steady_clock::time_point now() const { return now_; }
    void advance(std::chrono::milliseconds delta) { now_ += delta; }

    pulse::lifecycle::HotReloadClock fn() {
        return [this] { return now_; };
    }

private:
    std::chrono::steady_clock::time_point now_{};
};

// ---------------------------------------------------------------------------
// Req 15.5 — reload fuori dalla modalità sviluppo: rifiutato, mod invariata.
// ---------------------------------------------------------------------------
TEST(HotReload, RejectsReloadOutsideDevMode) {
    FakeBackend backend;
    HookEngine engine{backend};
    HotReload hot{engine};

    const std::uintptr_t target = 0x1000;
    ASSERT_TRUE(hot.installInitial("modA", {makeReq("modA", "A::f", target)}).ok());
    ASSERT_EQ(engine.totalHooks(), 1u);

    // Reload con devMode = false su un nuovo set di hook.
    const auto result =
        hot.reload("modA", {makeReq("modA", "A::f", target), makeReq("modA", "B::g", 0x2000)},
                   /*devMode=*/false);

    EXPECT_EQ(result.status, HotReloadStatus::RejectedNotDevMode);
    EXPECT_TRUE(result.rejected());
    EXPECT_FALSE(result.message.empty());

    // La mod corrente resta INVARIATA: nessun hook rimosso o installato.
    EXPECT_EQ(result.previousHooksRemoved, 0u);
    EXPECT_EQ(result.newHooksInstalled, 0u);
    EXPECT_EQ(engine.totalHooks(), 1u);
    EXPECT_TRUE(engine.isTargetInstalled(target));
    EXPECT_FALSE(engine.isTargetInstalled(0x2000));
    EXPECT_EQ(hot.currentHooks("modA").size(), 1u);
}

// ---------------------------------------------------------------------------
// Req 15.2 — gli hook della versione precedente sono rimossi PRIMA di
// installare quelli nuovi. L'ordine è osservato sul FakeBackend.
// ---------------------------------------------------------------------------
TEST(HotReload, RemovesPreviousHooksBeforeInstallingNew) {
    FakeBackend backend;
    HookEngine engine{backend};
    HotReload hot{engine};

    const std::uintptr_t oldTarget = 0x1000;
    const std::uintptr_t newTarget = 0x2000;

    ASSERT_TRUE(hot.installInitial("modA", {makeReq("modA", "Old::f", oldTarget)}).ok());
    ASSERT_TRUE(backend.isInstalled(oldTarget));

    bool callbackRan = false;
    bool oldRemovedBeforeInstall = false;
    bool newNotYetInstalled = false;

    const auto result = hot.reload(
        "modA", {makeReq("modA", "New::g", newTarget)}, /*devMode=*/true,
        /*onPreviousRemoved=*/[&] {
            callbackRan = true;
            // In questo istante (tra rimozione e installazione) la funzione
            // della vecchia versione NON deve più essere hookata sul backend...
            oldRemovedBeforeInstall = !backend.isInstalled(oldTarget);
            // ...e quella della nuova versione non deve ancora esserlo.
            newNotYetInstalled = !backend.isInstalled(newTarget);
        });

    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(callbackRan);
    EXPECT_TRUE(oldRemovedBeforeInstall);  // Req 15.2: rimozione prima
    EXPECT_TRUE(newNotYetInstalled);       // installazione solo dopo

    // A reload completato: vecchio target ripristinato, nuovo installato.
    EXPECT_FALSE(backend.isInstalled(oldTarget));
    EXPECT_TRUE(backend.isInstalled(newTarget));
}

// ---------------------------------------------------------------------------
// Req 15.1, 15.3 — reload riuscito: gli hook nuovi sostituiscono i precedenti
// entro il budget, con conferma.
// ---------------------------------------------------------------------------
TEST(HotReload, SuccessfulReloadSwapsHooksWithinBudget) {
    FakeBackend backend;
    HookEngine engine{backend};
    ManualClock clock;
    HotReload hot{engine, clock.fn()};

    ASSERT_TRUE(hot.installInitial(
                        "modA", {makeReq("modA", "Old::f", 0x1000),
                                 makeReq("modA", "Old::h", 0x1100)})
                    .ok());
    ASSERT_EQ(engine.totalHooks(), 2u);

    // Simula 2 s di lavoro: entro il budget di 5 s (Req 15.1).
    auto reloadWithProgress = [&](std::function<void()> mid) {
        return hot.reload("modA",
                          {makeReq("modA", "New::g", 0x2000)},
                          /*devMode=*/true, std::move(mid));
    };
    const auto result = reloadWithProgress([&] { clock.advance(std::chrono::seconds(2)); });

    EXPECT_EQ(result.status, HotReloadStatus::Reloaded);
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(result.withinBudget);
    EXPECT_EQ(result.previousHooksRemoved, 2u);
    EXPECT_EQ(result.newHooksInstalled, 1u);

    // La nuova versione è ora la corrente; la precedente è stata rimossa.
    EXPECT_EQ(engine.totalHooks(), 1u);
    EXPECT_FALSE(engine.isTargetInstalled(0x1000));
    EXPECT_FALSE(engine.isTargetInstalled(0x1100));
    EXPECT_TRUE(engine.isTargetInstalled(0x2000));
    ASSERT_EQ(hot.currentHooks("modA").size(), 1u);
    EXPECT_EQ(hot.currentHooks("modA").front().functionName, "New::g");
}

// ---------------------------------------------------------------------------
// Req 15.4 — installazione della nuova versione fallita: lo stato precedente
// è ripristinato e il processo di GD resta attivo.
// ---------------------------------------------------------------------------
TEST(HotReload, FailedInstallRestoresPreviousState) {
    FakeBackend backend;
    HookEngine engine{backend};
    HotReload hot{engine};

    const std::uintptr_t oldTarget = 0x1000;
    const std::uintptr_t newTarget = 0x2000;

    ASSERT_TRUE(hot.installInitial("modA", {makeReq("modA", "Old::f", oldTarget, 700)}).ok());
    ASSERT_TRUE(engine.isTargetInstalled(oldTarget));

    // La nuova versione fallisce sempre l'installazione sul nuovo target.
    backend.failInstallAt(newTarget);

    const auto result =
        hot.reload("modA", {makeReq("modA", "New::g", newTarget)}, /*devMode=*/true);

    // Fallimento segnalato con causa; GD attivo (la chiamata è ritornata).
    EXPECT_EQ(result.status, HotReloadStatus::Failed);
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.message.empty());
    EXPECT_TRUE(result.previousStateRestored);  // Req 15.4

    // Stato precedente ripristinato: il vecchio hook è di nuovo installato e la
    // nuova versione non è presente.
    EXPECT_TRUE(engine.isTargetInstalled(oldTarget));
    EXPECT_FALSE(engine.isTargetInstalled(newTarget));
    EXPECT_EQ(engine.totalHooks(), 1u);
    ASSERT_EQ(hot.currentHooks("modA").size(), 1u);
    EXPECT_EQ(hot.currentHooks("modA").front().functionName, "Old::f");
}

// ---------------------------------------------------------------------------
// Req 15.1 — reload che eccede il budget di 5 s: fallimento con ripristino
// dello stato precedente.
// ---------------------------------------------------------------------------
TEST(HotReload, ReloadExceedingBudgetFailsAndRestores) {
    FakeBackend backend;
    HookEngine engine{backend};
    ManualClock clock;
    HotReload hot{engine, clock.fn()};

    ASSERT_TRUE(hot.installInitial("modA", {makeReq("modA", "Old::f", 0x1000)}).ok());

    // Durante il reload il clock avanza di 6 s: oltre il budget di 5 s.
    const auto result = hot.reload(
        "modA", {makeReq("modA", "New::g", 0x2000)}, /*devMode=*/true,
        /*onPreviousRemoved=*/[&] { clock.advance(std::chrono::seconds(6)); });

    EXPECT_EQ(result.status, HotReloadStatus::Failed);
    EXPECT_FALSE(result.withinBudget);
    EXPECT_TRUE(result.previousStateRestored);

    // Stato precedente ripristinato (Req 15.4) e GD attivo.
    EXPECT_TRUE(engine.isTargetInstalled(0x1000));
    EXPECT_FALSE(engine.isTargetInstalled(0x2000));
    ASSERT_EQ(hot.currentHooks("modA").size(), 1u);
    EXPECT_EQ(hot.currentHooks("modA").front().functionName, "Old::f");
}

}  // namespace
