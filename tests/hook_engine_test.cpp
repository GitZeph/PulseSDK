// tests/hook_engine_test.cpp — unit test del coordinatore di installazione e
// rimozione degli hook con retry e rollback atomico (task 7.1, Req 2.4, 2.5).
//
// Verifica la politica dell'Hooking Engine descritta nel design (Layer 3):
//   * `install` tenta `backend.install` fino a un massimo di 3 volte; al
//     fallimento definitivo annulla ogni modifica parziale (0 hook residui,
//     funzione bersaglio invariata) e restituisce un esito di errore che
//     indica la funzione e la causa (Req 2.5);
//   * `install` riesce se un tentativo successivo ha successo entro i 3
//     tentativi (Req 2.5);
//   * `installAll` è atomico: al primo fallimento definitivo annulla anche gli
//     hook già installati nel batch (rollback), lasciando lo stato invariato;
//   * `remove(owner)` rimuove i soli hook di quella mod, ripristinando il
//     codice originale solo delle funzioni la cui catena resta vuota e
//     lasciando intatti gli hook delle altre mod (Req 2.4).
//
// Usa il `FakeBackend` in-memory (loader/hooking/) con i suoi fallimenti
// iniettabili (failAllInstalls, failNextInstall, failInstallAt) per pilotare
// in modo deterministico install/retry/rollback senza patchare codice reale.

#include <gtest/gtest.h>

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

// Helper: costruisce una richiesta di hook minimale.
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

// ---------------------------------------------------------------------------
// Req 2.5 — fallimento persistente: 3 tentativi, poi rollback atomico.
// ---------------------------------------------------------------------------
TEST(HookEngine, PersistentFailureRetriesThriceThenRollsBackAtomically) {
    FakeBackend backend;
    backend.failAllInstalls(true);  // ogni install fallisce
    HookEngine engine{backend};

    const std::uintptr_t target = 0x314000;
    const auto outcome = engine.install(makeReq("modA", "MenuLayer::init", target));

    // Esito di errore con funzione + causa (Req 2.5).
    EXPECT_FALSE(outcome.installed);
    ASSERT_TRUE(outcome.error.has_value());
    EXPECT_EQ(outcome.error->function, "MenuLayer::init");
    EXPECT_FALSE(outcome.error->cause.message.empty());

    // Esattamente 3 tentativi (max), non di più (Req 2.5).
    EXPECT_EQ(outcome.attempts, static_cast<std::size_t>(kMaxInstallAttempts));
    EXPECT_EQ(backend.installAttempts(), 3u);

    // Rollback atomico: 0 hook residui, funzione bersaglio invariata.
    EXPECT_EQ(engine.totalHooks(), 0u);
    EXPECT_EQ(engine.installedTargets(), 0u);
    EXPECT_FALSE(engine.isTargetInstalled(target));
    EXPECT_EQ(backend.installedCount(), 0u);
    EXPECT_FALSE(backend.isInstalled(target));
}

// ---------------------------------------------------------------------------
// Req 2.5 — successo entro i 3 tentativi: i primi 2 falliscono, il 3° riesce.
// ---------------------------------------------------------------------------
TEST(HookEngine, SucceedsWhenALaterAttemptSucceeds) {
    FakeBackend backend;
    backend.failNextInstall();  // 1° tentativo fallisce
    backend.failNextInstall();  // 2° tentativo fallisce
    HookEngine engine{backend};

    const std::uintptr_t target = 0x401000;
    const auto outcome = engine.install(makeReq("modA", "PlayLayer::init", target));

    EXPECT_TRUE(outcome.installed);
    EXPECT_FALSE(outcome.error.has_value());
    EXPECT_EQ(outcome.attempts, 3u);          // due falliti + uno riuscito
    EXPECT_EQ(backend.installAttempts(), 3u);

    EXPECT_TRUE(engine.isTargetInstalled(target));
    EXPECT_EQ(engine.installedTargets(), 1u);
    EXPECT_EQ(engine.totalHooks(), 1u);
    EXPECT_EQ(backend.installedCount(), 1u);
}

// ---------------------------------------------------------------------------
// Più hook sulla stessa funzione: un solo install sul backend, catena di 2.
// ---------------------------------------------------------------------------
TEST(HookEngine, MultipleHooksSameFunctionInstallBackendOnce) {
    FakeBackend backend;
    HookEngine engine{backend};

    const std::uintptr_t target = 0x500000;
    EXPECT_TRUE(engine.install(makeReq("modA", "GJ::f", target, 600)).installed);
    const auto second = engine.install(makeReq("modB", "GJ::f", target, 400));

    EXPECT_TRUE(second.installed);
    EXPECT_EQ(second.attempts, 0u);  // backend già installato: nessun tentativo

    EXPECT_EQ(engine.installedTargets(), 1u);
    EXPECT_EQ(engine.totalHooks(), 2u);
    EXPECT_EQ(backend.installedCount(), 1u);
    EXPECT_EQ(backend.installAttempts(), 1u);  // un solo install sul backend
}

// ---------------------------------------------------------------------------
// Req 2.5 — installAll atomico: il 3° hook fallisce, rollback completo.
// ---------------------------------------------------------------------------
TEST(HookEngine, InstallAllRollsBackEntireBatchOnPersistentFailure) {
    FakeBackend backend;
    const std::uintptr_t t1 = 0x1000, t2 = 0x2000, t3 = 0x3000;
    backend.failInstallAt(t3);  // la terza funzione fallisce sempre
    HookEngine engine{backend};

    const std::vector<HookRequest> batch = {
        makeReq("modA", "A::f", t1),
        makeReq("modA", "B::g", t2),
        makeReq("modA", "C::h", t3),
    };

    const auto outcome = engine.installAll(batch);

    // Esito di errore con la funzione fallita + causa (Req 2.5).
    EXPECT_FALSE(outcome.installed);
    ASSERT_TRUE(outcome.error.has_value());
    EXPECT_EQ(outcome.error->function, "C::h");
    EXPECT_FALSE(outcome.error->cause.message.empty());

    // Rollback atomico: nessuno dei tre hook resta installato.
    EXPECT_EQ(engine.totalHooks(), 0u);
    EXPECT_EQ(engine.installedTargets(), 0u);
    EXPECT_FALSE(engine.isTargetInstalled(t1));
    EXPECT_FALSE(engine.isTargetInstalled(t2));
    EXPECT_FALSE(engine.isTargetInstalled(t3));
    EXPECT_EQ(backend.installedCount(), 0u);

    // I primi due target sono stati installati e poi ripristinati.
    EXPECT_FALSE(backend.isInstalled(t1));
    EXPECT_FALSE(backend.isInstalled(t2));
}

// ---------------------------------------------------------------------------
// installAll: tutte le richieste valide -> tutte installate.
// ---------------------------------------------------------------------------
TEST(HookEngine, InstallAllInstallsEverythingOnSuccess) {
    FakeBackend backend;
    HookEngine engine{backend};

    const std::vector<HookRequest> batch = {
        makeReq("modA", "A::f", 0x1000),
        makeReq("modA", "B::g", 0x2000),
        makeReq("modB", "A::f", 0x1000),  // secondo hook sulla stessa funzione
    };

    const auto outcome = engine.installAll(batch);

    EXPECT_TRUE(outcome.installed);
    EXPECT_EQ(engine.totalHooks(), 3u);
    EXPECT_EQ(engine.installedTargets(), 2u);  // due funzioni distinte
    EXPECT_EQ(backend.installedCount(), 2u);
}

// ---------------------------------------------------------------------------
// Req 2.4 — rimozione selettiva: rimuove i soli hook di una mod, lasciando
// intatti quelli delle altre mod, e ripristina solo le funzioni svuotate.
// ---------------------------------------------------------------------------
TEST(HookEngine, RemoveOwnerRestoresOnlyThatModsHooks) {
    FakeBackend backend;
    HookEngine engine{backend};

    const std::uintptr_t shared = 0x1000;   // hookata da modA e modB
    const std::uintptr_t onlyA = 0x2000;    // hookata solo da modA

    ASSERT_TRUE(engine.install(makeReq("modA", "Shared::f", shared, 600)).installed);
    ASSERT_TRUE(engine.install(makeReq("modB", "Shared::f", shared, 400)).installed);
    ASSERT_TRUE(engine.install(makeReq("modA", "OnlyA::g", onlyA)).installed);

    ASSERT_EQ(engine.totalHooks(), 3u);
    ASSERT_EQ(engine.installedTargets(), 2u);

    // Rimozione selettiva di modA.
    const auto outcome = engine.remove("modA");

    EXPECT_TRUE(outcome.ok());
    EXPECT_EQ(outcome.hooksRemoved, 2u);     // i due hook di modA
    EXPECT_EQ(outcome.targetsRestored, 1u);  // solo OnlyA::g (catena svuotata)

    // modA non ha più hook; modB conserva il suo (Req 2.4).
    EXPECT_EQ(engine.hooksForOwner("modA"), 0u);
    EXPECT_EQ(engine.hooksForOwner("modB"), 1u);
    EXPECT_EQ(engine.totalHooks(), 1u);

    // La funzione condivisa resta hookata (per modB); quella solo-modA è
    // ripristinata sul backend.
    EXPECT_TRUE(engine.isTargetInstalled(shared));
    EXPECT_FALSE(engine.isTargetInstalled(onlyA));
    EXPECT_TRUE(backend.isInstalled(shared));
    EXPECT_FALSE(backend.isInstalled(onlyA));

    // I byte originali della funzione ripristinata coincidono con i live.
    const auto live = backend.liveBytes(onlyA);
    const auto orig = backend.snapshotOriginal(onlyA);
    ASSERT_TRUE(live.has_value());
    ASSERT_TRUE(orig.has_value());
    EXPECT_EQ(*live, *orig);
}

// ---------------------------------------------------------------------------
// Req 2.4 — rimuovere una mod senza hook non tocca nulla.
// ---------------------------------------------------------------------------
TEST(HookEngine, RemoveUnknownOwnerIsNoOp) {
    FakeBackend backend;
    HookEngine engine{backend};

    ASSERT_TRUE(engine.install(makeReq("modA", "A::f", 0x1000)).installed);

    const auto outcome = engine.remove("modZ");
    EXPECT_TRUE(outcome.ok());
    EXPECT_EQ(outcome.hooksRemoved, 0u);
    EXPECT_EQ(outcome.targetsRestored, 0u);
    EXPECT_EQ(engine.totalHooks(), 1u);
    EXPECT_TRUE(engine.isTargetInstalled(0x1000));
}

}  // namespace
