// tests/hook_chain_registry_remove_link_test.cpp — unit test della rimozione di
// un anello NON ultimo di HookChainRegistry::removeOwner (task 5.1, Requisiti
// 5.1, 5.2, 5.6).
//
// Verifica la rimozione di un anello da una catena con altri anelli abilitati
// (keep-install): relinking dei SOLI Trampoline_Slot dei vicini SENZA
// disinstallare la Underlying_Installation e SENZA ripristinare i byte originali.
//   * anello in mezzo rimosso → `*pred.slot = succ.detour` (Req 5.1);
//   * anello di coda rimosso → `*pred.slot = Real_Trampoline` (il predecessore
//     diventa coda, Req 5.1);
//   * Chain_Head rimosso → `currentHead = nuovo head.detour` senza ripristinare
//     i byte (Req 5.2);
//   * la Underlying_Installation è mantenuta finché resta ≥1 anello (Req 5.5):
//     nessuna chiamata di remove al backend, prologo invariato;
//   * l'ordine relativo e l'esecuzione degli anelli restanti sono invariati
//     (Req 5.6);
//   * esito ChainOpOutcome::RemovedKeepInstall e diagnostica osservabile
//     (Mod_Id + Target_Address, Req 11.2).
//
// Usa il FakeBackend in-memory (loader/hooking/) come Hook_Engine host-testabile.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "bindings/bindings.hpp"
#include "hooking/fake_backend.hpp"
#include "hooking/head_thunk.hpp"
#include "hooking/hook_chain_registry.hpp"
#include "hooking/rollback_store.hpp"
#include "lifecycle/hook_ownership.hpp"

namespace {

using pulse::hooking::ChainOpOutcome;
using pulse::hooking::ChainOpResult;
using pulse::hooking::FakeBackend;
using pulse::hooking::HookChainRegistry;
using pulse::hooking::LinkSpec;
using pulse::hooking::RollbackStore;
using pulse::lifecycle::HookOwnershipLedger;
using pulse::loader::bindings::FunctionBinding;

// Detour distinti per ogni anello: nel caso host i detour sono semplici
// puntatori opachi (l'identità conta per verificare il relinking degli slot).
int gDetourA = 0;
int gDetourB = 0;
int gDetourC = 0;
void* detourA() { return static_cast<void*>(&gDetourA); }
void* detourB() { return static_cast<void*>(&gDetourB); }
void* detourC() { return static_cast<void*>(&gDetourC); }

FunctionBinding resolvedBinding(std::string symbol, std::uintptr_t address) {
    FunctionBinding b;
    b.symbol = std::move(symbol);
    b.address = address;
    b.resolved = true;
    return b;
}

// Indirizzo del Real_Trampoline sintetico del FakeBackend per un target.
void* realTrampoline(std::uintptr_t target) {
    return reinterpret_cast<void*>(target ^ 0xA5A5A5A5ULL);
}

struct Fixture {
    FakeBackend backend;
    RollbackStore rollback{std::filesystem::temp_directory_path() /
                           "pulse_chain_remove_link_test.bin"};
    HookOwnershipLedger ledger;
    std::vector<std::string> events;

    HookChainRegistry makeRegistry() {
        return HookChainRegistry{backend, rollback, ledger,
                                 [this](std::string_view m) {
                                     events.emplace_back(m);
                                 }};
    }

    ~Fixture() {
        std::error_code ec;
        std::filesystem::remove(rollback.path(), ec);
    }
};

// Inserisce un anello (helper per costruire le catene di test).
LinkSpec makeLink(const std::string& owner, int priority, uint64_t loadOrder,
                  void* detour, void** slot) {
    LinkSpec s;
    s.owner = owner;
    s.symbol = "MenuLayer::init";
    s.priority = priority;
    s.loadOrder = loadOrder;
    s.detour = detour;
    s.slot = slot;
    return s;
}

// --- Rimozione anello in mezzo: pred.slot → succ.detour (Req 5.1) ----------
TEST(HookChainRegistryRemoveLink, RemoveMiddleRelinksPredecessorToSuccessor) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x4000;

    void* slotA = nullptr;  // priorità 900 (testa)
    void* slotC = nullptr;  // priorità 500 (in mezzo)
    void* slotB = nullptr;  // priorità 100 (coda)

    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 900, 0, detourA(), &slotA));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.beta", 100, 1, detourB(), &slotB));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.gamma", 500, 2, detourC(), &slotC));

    // Catena [A, C, B]: slotA → C, slotC → B, slotB → Real_Trampoline.
    ASSERT_EQ(slotA, detourC());
    ASSERT_EQ(slotC, detourB());
    ASSERT_EQ(slotB, realTrampoline(kTarget));

    // Rimuovi l'anello in mezzo (gamma).
    auto results = registry.removeOwner("com.pulse.gamma");

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().outcome, ChainOpOutcome::RemovedKeepInstall);
    ASSERT_EQ(results.front().chainOrder.size(), 2u);
    EXPECT_EQ(results.front().chainOrder[0], "com.pulse.alpha");
    EXPECT_EQ(results.front().chainOrder[1], "com.pulse.beta");

    // Il predecessore A ora cede direttamente al successore B (Req 5.1).
    EXPECT_EQ(slotA, detourB());
    // Il successore B resta coda → Real_Trampoline (invariato, Req 5.6).
    EXPECT_EQ(slotB, realTrampoline(kTarget));
    // La testa non cambia.
    EXPECT_EQ(registry.currentHead(kTarget), detourA());

    // Underlying_Installation mantenuta (Req 5.5): nessuna remove al backend.
    EXPECT_EQ(registry.installCount(), 1u);
    EXPECT_EQ(registry.chainSize(kTarget), 2u);
    EXPECT_TRUE(fx.backend.isInstalled(kTarget));
    EXPECT_EQ(fx.backend.installedCount(), 1u);
}

// --- Rimozione del Chain_Head: currentHead → nuovo head (Req 5.2) ----------
TEST(HookChainRegistryRemoveLink, RemoveHeadUpdatesCurrentHeadKeepsInstall) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x5000;

    void* slotA = nullptr;  // priorità 900 (testa)
    void* slotB = nullptr;  // priorità 500 (coda)

    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 900, 0, detourA(), &slotA));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.beta", 500, 1, detourB(), &slotB));

    // Snapshot dei byte "live" del prologo prima della rimozione.
    const auto liveBefore = fx.backend.liveBytes(kTarget);

    // Rimuovi la testa (alpha).
    auto results = registry.removeOwner("com.pulse.alpha");

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().outcome, ChainOpOutcome::RemovedKeepInstall);
    ASSERT_EQ(results.front().chainOrder.size(), 1u);
    EXPECT_EQ(results.front().chainOrder[0], "com.pulse.beta");

    // currentHead punta ora al detour del nuovo Chain_Head B (Req 5.2).
    EXPECT_EQ(registry.currentHead(kTarget), detourB());
    // B è ora sia testa sia coda → il suo slot punta al Real_Trampoline.
    EXPECT_EQ(slotB, realTrampoline(kTarget));

    // I byte originali NON sono ripristinati (Req 5.2): prologo invariato e
    // install mantenuta (Req 5.5).
    EXPECT_EQ(fx.backend.liveBytes(kTarget), liveBefore);
    EXPECT_TRUE(fx.backend.isInstalled(kTarget));
    EXPECT_EQ(registry.installCount(), 1u);
    EXPECT_EQ(registry.chainSize(kTarget), 1u);
}

// --- Rimozione della coda: pred.slot → Real_Trampoline (Req 5.1) -----------
TEST(HookChainRegistryRemoveLink, RemoveTailRelinksPredecessorToTrampoline) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x6000;

    void* slotA = nullptr;  // priorità 900 (testa)
    void* slotB = nullptr;  // priorità 500 (coda)

    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 900, 0, detourA(), &slotA));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.beta", 500, 1, detourB(), &slotB));

    // Catena [A, B]: slotA → B, slotB → Real_Trampoline.
    ASSERT_EQ(slotA, detourB());
    ASSERT_EQ(slotB, realTrampoline(kTarget));

    // Rimuovi la coda (beta).
    auto results = registry.removeOwner("com.pulse.beta");

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().outcome, ChainOpOutcome::RemovedKeepInstall);
    ASSERT_EQ(results.front().chainOrder.size(), 1u);
    EXPECT_EQ(results.front().chainOrder[0], "com.pulse.alpha");

    // A diventa coda: il suo slot punta ora al Real_Trampoline (Req 5.1).
    EXPECT_EQ(slotA, realTrampoline(kTarget));
    // La testa non cambia.
    EXPECT_EQ(registry.currentHead(kTarget), detourA());

    // Install mantenuta (Req 5.5).
    EXPECT_EQ(registry.installCount(), 1u);
    EXPECT_TRUE(fx.backend.isInstalled(kTarget));
    EXPECT_EQ(registry.chainSize(kTarget), 1u);
}

// --- L'ordine e l'esecuzione degli anelli restanti sono invariati (Req 5.6) -
TEST(HookChainRegistryRemoveLink, RemainingLinksKeepRelativeOrder) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x6500;

    void* slotA = nullptr;  // 900
    void* slotB = nullptr;  // 100
    void* slotC = nullptr;  // 500

    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 900, 0, detourA(), &slotA));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.beta", 100, 1, detourB(), &slotB));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.gamma", 500, 2, detourC(), &slotC));

    // [A, C, B] prima della rimozione.
    ASSERT_EQ(registry.chainOrder(kTarget),
              (std::vector<std::string>{"com.pulse.alpha", "com.pulse.gamma",
                                        "com.pulse.beta"}));

    registry.removeOwner("com.pulse.gamma");

    // Restano A e B nell'ordine relativo originale [A, B] (Req 5.6).
    EXPECT_EQ(registry.chainOrder(kTarget),
              (std::vector<std::string>{"com.pulse.alpha", "com.pulse.beta"}));
}

// --- Diagnostica osservabile della rimozione (Req 11.2) --------------------
TEST(HookChainRegistryRemoveLink, EmitsRemoveDiagnosticWithModIdAndTarget) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x7000;

    void* slotA = nullptr;
    void* slotB = nullptr;

    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 900, 0, detourA(), &slotA));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.beta", 500, 1, detourB(), &slotB));

    fx.events.clear();
    registry.removeOwner("com.pulse.alpha");

    bool diagnosed = false;
    for (const auto& e : fx.events) {
        if (e.find("rimosso anello") != std::string::npos &&
            e.find("com.pulse.alpha") != std::string::npos &&
            e.find("0x7000") != std::string::npos) {
            diagnosed = true;
        }
    }
    EXPECT_TRUE(diagnosed);
}

// --- Rimozione selettiva: rimuovere un owner non tocca gli altri anelli -----
TEST(HookChainRegistryRemoveLink, RemovingOneOwnerKeepsOthersIntact) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x7500;

    void* slotA = nullptr;
    void* slotB = nullptr;
    void* slotC = nullptr;

    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 900, 0, detourA(), &slotA));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.beta", 500, 1, detourB(), &slotB));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.gamma", 100, 2, detourC(), &slotC));

    // [A(900), B(500), C(100)]: rimuovi B (in mezzo).
    registry.removeOwner("com.pulse.beta");

    // Restano A (testa) e C (coda): A → C, C → Real_Trampoline.
    EXPECT_EQ(registry.chainOrder(kTarget),
              (std::vector<std::string>{"com.pulse.alpha", "com.pulse.gamma"}));
    EXPECT_EQ(slotA, detourC());
    EXPECT_EQ(slotC, realTrampoline(kTarget));
    EXPECT_EQ(registry.currentHead(kTarget), detourA());
    EXPECT_EQ(registry.installCount(), 1u);
}

// --- Rimozione del Chain_Head NON ripristina i byte originali (Req 5.2) -----
TEST(HookChainRegistryRemoveLink, RemoveKeepInstallDoesNotRestoreBytes) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x8000;

    void* slotA = nullptr;
    void* slotB = nullptr;

    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 900, 0, detourA(), &slotA));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.beta", 500, 1, detourB(), &slotB));

    // I byte "live" patchati e gli originali del FakeBackend.
    const auto liveBefore = fx.backend.liveBytes(kTarget);
    const auto originalBytes = fx.backend.snapshotOriginal(kTarget);
    const std::size_t rollbackBefore = fx.rollback.size();

    registry.removeOwner("com.pulse.alpha");

    // I byte del prologo NON sono ripristinati (restano patchati, != originali)
    // e il Rollback_Store non è mutato (Req 5.2, 5.5).
    EXPECT_EQ(fx.backend.liveBytes(kTarget), liveBefore);
    EXPECT_NE(fx.backend.liveBytes(kTarget), originalBytes);
    EXPECT_EQ(fx.rollback.size(), rollbackBefore);
    EXPECT_TRUE(fx.backend.isInstalled(kTarget));
}

}  // namespace
