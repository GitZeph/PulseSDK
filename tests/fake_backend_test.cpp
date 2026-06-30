// tests/fake_backend_test.cpp — unit test del FakeBackend in-memory (task 4.2).
//
// Verifica il comportamento del doppio di test usato dai property test su
// catena di hook e rollback (Req 2.2):
//   * install/remove aggiornano correttamente lo stato simulato;
//   * readOriginal è invariante rispetto a install/remove (round-trip byte);
//   * remove ripristina esattamente i byte originali (rollback);
//   * i fallimenti iniettabili forzano errori senza mutare la memoria
//     simulata (atomicità del singolo tentativo).
//
// Implementa l'interfaccia canonica `pulse::hooking::IHookBackend` (task 4.1).

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "hooking/fake_backend.hpp"

namespace {

using pulse::hooking::FakeBackend;
using pulse::hooking::HookErrorCode;

using Bytes = FakeBackend::Bytes;

constexpr std::uintptr_t kTarget = 0x1000;
int g_detour = 0;
void* const kDetour = &g_detour;

// --- install di base: trampolino valido + stato installato ----------------
TEST(FakeBackend, InstallMarksTargetInstalledAndReturnsTrampoline) {
    FakeBackend backend;
    auto result = backend.install(kTarget, kDetour);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().valid());
    EXPECT_TRUE(backend.isInstalled(kTarget));
    EXPECT_EQ(backend.installedCount(), 1u);
    EXPECT_EQ(backend.installAttempts(), 1u);
}

// --- install su indirizzo nullo -> InvalidArgument ------------------------
TEST(FakeBackend, InstallOnNullTargetFails) {
    FakeBackend backend;
    auto result = backend.install(0, kDetour);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, HookErrorCode::InvalidArgument);
    EXPECT_EQ(backend.installedCount(), 0u);
}

// --- doppio install sullo stesso bersaglio -> AlreadyHooked ---------------
TEST(FakeBackend, DoubleInstallFails) {
    FakeBackend backend;
    ASSERT_TRUE(backend.install(kTarget, kDetour).has_value());

    auto second = backend.install(kTarget, kDetour);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, HookErrorCode::AlreadyHooked);
    EXPECT_TRUE(backend.isInstalled(kTarget));
}

// --- install patcha i byte live ma NON gli originali ----------------------
TEST(FakeBackend, InstallChangesLiveBytesButNotOriginal) {
    FakeBackend backend;
    backend.seedOriginal(kTarget, Bytes{1, 2, 3, 4});

    ASSERT_TRUE(backend.install(kTarget, kDetour).has_value());

    const auto live = backend.liveBytes(kTarget);
    const auto original = backend.snapshotOriginal(kTarget);
    ASSERT_TRUE(live.has_value());
    ASSERT_TRUE(original.has_value());

    EXPECT_NE(*live, *original);                // i byte live sono patchati
    EXPECT_EQ(*original, (Bytes{1, 2, 3, 4}));  // gli originali restano intatti
}

// --- remove ripristina esattamente i byte originali (rollback) ------------
TEST(FakeBackend, RemoveRestoresOriginalBytes) {
    FakeBackend backend;
    backend.seedOriginal(kTarget, Bytes{10, 20, 30});

    ASSERT_TRUE(backend.install(kTarget, kDetour).has_value());
    auto removed = backend.remove(kTarget);

    ASSERT_TRUE(removed.has_value());
    EXPECT_FALSE(backend.isInstalled(kTarget));
    EXPECT_EQ(backend.installedCount(), 0u);

    const auto live = backend.liveBytes(kTarget);
    ASSERT_TRUE(live.has_value());
    EXPECT_EQ(*live, (Bytes{10, 20, 30}));
}

// --- remove senza install -> NotHooked ------------------------------------
TEST(FakeBackend, RemoveWithoutInstallFails) {
    FakeBackend backend;
    auto result = backend.remove(kTarget);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, HookErrorCode::NotHooked);
}

// --- readOriginal è invariante rispetto a install/remove ------------------
TEST(FakeBackend, ReadOriginalIsInvariantUnderInstall) {
    FakeBackend backend;
    backend.seedOriginal(kTarget, Bytes{5, 6, 7, 8, 9});

    auto before = backend.readOriginal(kTarget, 5);
    ASSERT_TRUE(before.has_value());

    ASSERT_TRUE(backend.install(kTarget, kDetour).has_value());
    auto duringInstall = backend.readOriginal(kTarget, 5);
    ASSERT_TRUE(duringInstall.has_value());

    ASSERT_TRUE(backend.remove(kTarget).has_value());
    auto afterRemove = backend.readOriginal(kTarget, 5);
    ASSERT_TRUE(afterRemove.has_value());

    EXPECT_EQ(before.value().bytes(), (std::vector<std::uint8_t>{5, 6, 7, 8, 9}));
    EXPECT_EQ(duringInstall.value().bytes(), before.value().bytes());
    EXPECT_EQ(afterRemove.value().bytes(), before.value().bytes());
}

// --- readOriginal su bersaglio non seminato è deterministico --------------
TEST(FakeBackend, ReadOriginalDeterministicForUnseededTarget) {
    FakeBackend a;
    FakeBackend b;
    auto ra = a.readOriginal(kTarget, 8);
    auto rb = b.readOriginal(kTarget, 8);

    ASSERT_TRUE(ra.has_value());
    ASSERT_TRUE(rb.has_value());
    EXPECT_EQ(ra.value().bytes(), rb.value().bytes());  // ripetibile tra istanze
    EXPECT_EQ(ra.value().size(), 8u);
}

// --- fallimento iniettato per-bersaglio: nessuna mutazione (atomicità) ----
TEST(FakeBackend, InjectedInstallFailureLeavesMemoryUnchanged) {
    FakeBackend backend;
    backend.seedOriginal(kTarget, Bytes{1, 1, 1});
    backend.failInstallAt(kTarget, HookErrorCode::BackendFailure);

    auto result = backend.install(kTarget, kDetour);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, HookErrorCode::BackendFailure);
    EXPECT_FALSE(backend.isInstalled(kTarget));
    EXPECT_EQ(backend.installedCount(), 0u);

    const auto live = backend.liveBytes(kTarget);
    ASSERT_TRUE(live.has_value());
    EXPECT_EQ(*live, (Bytes{1, 1, 1}));        // memoria invariata
    EXPECT_EQ(backend.installAttempts(), 1u);  // il tentativo è comunque contato
}

// --- failNextInstall si applica una sola volta ----------------------------
TEST(FakeBackend, FailNextInstallAppliesOnce) {
    FakeBackend backend;
    backend.failNextInstall(HookErrorCode::BackendFailure);

    auto first = backend.install(kTarget, kDetour);
    EXPECT_FALSE(first.has_value());

    auto second = backend.install(kTarget, kDetour);
    EXPECT_TRUE(second.has_value());
    EXPECT_TRUE(backend.isInstalled(kTarget));
}

// --- failAllInstalls può essere attivato e disattivato --------------------
TEST(FakeBackend, FailAllInstallsToggle) {
    FakeBackend backend;
    backend.failAllInstalls(true);

    EXPECT_FALSE(backend.install(0x1, kDetour).has_value());
    EXPECT_FALSE(backend.install(0x2, kDetour).has_value());

    backend.failAllInstalls(false);
    EXPECT_TRUE(backend.install(0x3, kDetour).has_value());
}

// --- clearInjectedFailures ripristina il comportamento normale ------------
TEST(FakeBackend, ClearInjectedFailures) {
    FakeBackend backend;
    backend.failInstallAt(kTarget);
    backend.clearInjectedFailures();

    EXPECT_TRUE(backend.install(kTarget, kDetour).has_value());
}

// --- fallimenti iniettati su remove e read --------------------------------
TEST(FakeBackend, InjectedRemoveAndReadFailures) {
    FakeBackend backend;
    ASSERT_TRUE(backend.install(kTarget, kDetour).has_value());

    backend.failRemoveAt(kTarget, HookErrorCode::BackendFailure);
    auto removed = backend.remove(kTarget);
    EXPECT_FALSE(removed.has_value());
    EXPECT_EQ(removed.error().code, HookErrorCode::BackendFailure);
    EXPECT_TRUE(backend.isInstalled(kTarget));  // remove fallito: ancora installato

    backend.failReadAt(kTarget, HookErrorCode::BackendFailure);
    auto read = backend.readOriginal(kTarget, 4);
    EXPECT_FALSE(read.has_value());
    EXPECT_EQ(read.error().code, HookErrorCode::BackendFailure);
}

// --- name()/available() del fake ------------------------------------------
TEST(FakeBackend, ReportsNameAndAvailability) {
    FakeBackend backend;
    EXPECT_EQ(backend.name(), "fake-backend");
    EXPECT_TRUE(backend.available());
}

}  // namespace
