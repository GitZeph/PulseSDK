// tests/hook_chain_registry_insert_link_test.cpp — unit test della transizione
// n→n+1 di HookChainRegistry::insertLink (task 3.3, Requisiti 1.2, 1.7, 4.1,
// 4.2, 4.3).
//
// Verifica l'inserimento di un anello successivo su un Target_Address che
// possiede già una Underlying_Installation attiva:
//   * relinking dei SOLI Trampoline_Slot dei vicini — `*pred.slot = nuovo.detour`,
//     `*nuovo.slot = succ.detour` (o `Real_Trampoline` se il nuovo è la coda),
//     Req 4.2;
//   * inserimento come nuovo Chain_Head → `currentHead` punta al nuovo detour,
//     senza una seconda Underlying_Installation, Req 4.3;
//   * inserimento in coda / in mezzo secondo il Chain_Order (priority DESC,
//     loadOrder ASC), Req 4.1;
//   * NESSUNA chiamata al backend: nessuna seconda DobbyHook, nessun errore
//     "indirizzo già hookato" (Req 1.2, 1.7); prologo e Rollback_Store invariati;
//   * esito ChainOpOutcome corretto (InsertedHead/InsertedMiddle/InsertedTail);
//   * attribuzione di ciascun anello al proprio Mod_Id (Req 6.1).
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

struct Fixture {
    FakeBackend backend;
    RollbackStore rollback{std::filesystem::temp_directory_path() /
                           "pulse_chain_insert_link_test.bin"};
    HookOwnershipLedger ledger;
    std::vector<std::string> events;

    HookChainRegistry makeRegistry() {
        return HookChainRegistry{backend, rollback, ledger,
                                 [this](std::string_view m) {
                                     events.emplace_back(m);
                                 }};
    }

    ~Fixture() { std::error_code ec; std::filesystem::remove(rollback.path(), ec); }
};

// Indirizzo del Real_Trampoline sintetico del FakeBackend per un target.
void* realTrampoline(std::uintptr_t target) {
    return reinterpret_cast<void*>(target ^ 0xA5A5A5A5ULL);
}

// --- n→n+1: inserimento in coda (priorità inferiore) ----------------------
TEST(HookChainRegistryInsertLink, InsertTailRelinksHeadSlot) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x4000;

    void* slotA = nullptr;
    void* slotB = nullptr;

    // Primo anello (0→1): testa == coda, slotA → Real_Trampoline.
    LinkSpec a;
    a.owner = "com.pulse.alpha";
    a.symbol = "MenuLayer::init";
    a.priority = 500;
    a.loadOrder = 0;
    a.detour = detourA();
    a.slot = &slotA;
    ASSERT_EQ(registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget), a)
                  .outcome,
              ChainOpOutcome::CreatedInstall);

    // Secondo anello con priorità inferiore → coda della catena.
    LinkSpec b;
    b.owner = "com.pulse.beta";
    b.symbol = "MenuLayer::init";
    b.priority = 400;  // più bassa ⇒ coda
    b.loadOrder = 1;
    b.detour = detourB();
    b.slot = &slotB;
    ChainOpResult r =
        registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget), b);

    EXPECT_EQ(r.outcome, ChainOpOutcome::InsertedTail);
    EXPECT_EQ(r.error.code, pulse::hooking::HookErrorCode::None);
    ASSERT_EQ(r.chainOrder.size(), 2u);
    EXPECT_EQ(r.chainOrder[0], "com.pulse.alpha");
    EXPECT_EQ(r.chainOrder[1], "com.pulse.beta");

    // Relinking dei vicini: lo slot della (vecchia) testa A ora cede a B, lo slot
    // della nuova coda B punta al Real_Trampoline (Req 4.2).
    EXPECT_EQ(slotA, detourB());
    EXPECT_EQ(slotB, realTrampoline(kTarget));

    // La testa NON cambia (A resta Chain_Head).
    EXPECT_EQ(registry.currentHead(kTarget), detourA());

    // NESSUNA seconda install: una sola DobbyHook, un solo tentativo (Req 1.2, 1.7).
    EXPECT_EQ(registry.installCount(), 1u);
    EXPECT_EQ(registry.chainSize(kTarget), 2u);
    EXPECT_EQ(fx.backend.installedCount(), 1u);
    EXPECT_EQ(fx.backend.installAttempts(), 1u);
}

// --- n→n+1: inserimento come nuovo Chain_Head (priorità superiore) --------
TEST(HookChainRegistryInsertLink, InsertHeadUpdatesCurrentHead) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x5000;

    void* slotA = nullptr;
    void* slotB = nullptr;

    LinkSpec a;
    a.owner = "com.pulse.alpha";
    a.symbol = "MenuLayer::init";
    a.priority = 500;
    a.loadOrder = 0;
    a.detour = detourA();
    a.slot = &slotA;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget), a);

    // Secondo anello con priorità superiore → nuova testa.
    LinkSpec b;
    b.owner = "com.pulse.beta";
    b.symbol = "MenuLayer::init";
    b.priority = 900;  // più alta ⇒ testa
    b.loadOrder = 1;
    b.detour = detourB();
    b.slot = &slotB;
    ChainOpResult r =
        registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget), b);

    EXPECT_EQ(r.outcome, ChainOpOutcome::InsertedHead);
    ASSERT_EQ(r.chainOrder.size(), 2u);
    EXPECT_EQ(r.chainOrder[0], "com.pulse.beta");
    EXPECT_EQ(r.chainOrder[1], "com.pulse.alpha");

    // currentHead punta ora al detour del nuovo Chain_Head B (Req 4.3); lo slot
    // di B cede ad A; lo slot della (ora) coda A resta al Real_Trampoline.
    EXPECT_EQ(registry.currentHead(kTarget), detourB());
    EXPECT_EQ(slotB, detourA());
    EXPECT_EQ(slotA, realTrampoline(kTarget));

    // Nessuna seconda install (Req 4.3).
    EXPECT_EQ(registry.installCount(), 1u);
    EXPECT_EQ(fx.backend.installAttempts(), 1u);
}

// --- n→n+1: inserimento in mezzo tra predecessore e successore ------------
TEST(HookChainRegistryInsertLink, InsertMiddleRelinksNeighborsOnly) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x6000;

    void* slotA = nullptr;  // priorità 900 (testa)
    void* slotB = nullptr;  // priorità 100 (coda)
    void* slotC = nullptr;  // priorità 500 (in mezzo)

    LinkSpec a;
    a.owner = "com.pulse.alpha";
    a.symbol = "MenuLayer::init";
    a.priority = 900;
    a.loadOrder = 0;
    a.detour = detourA();
    a.slot = &slotA;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget), a);

    LinkSpec b;
    b.owner = "com.pulse.beta";
    b.symbol = "MenuLayer::init";
    b.priority = 100;
    b.loadOrder = 1;
    b.detour = detourB();
    b.slot = &slotB;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget), b);

    // Stato dopo [A, B]: slotA → detourB, slotB → Real_Trampoline.
    ASSERT_EQ(slotA, detourB());
    ASSERT_EQ(slotB, realTrampoline(kTarget));

    // Terzo anello con priorità intermedia → in mezzo (A, C, B).
    LinkSpec c;
    c.owner = "com.pulse.gamma";
    c.symbol = "MenuLayer::init";
    c.priority = 500;
    c.loadOrder = 2;
    c.detour = detourC();
    c.slot = &slotC;
    ChainOpResult r =
        registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget), c);

    EXPECT_EQ(r.outcome, ChainOpOutcome::InsertedMiddle);
    ASSERT_EQ(r.chainOrder.size(), 3u);
    EXPECT_EQ(r.chainOrder[0], "com.pulse.alpha");
    EXPECT_EQ(r.chainOrder[1], "com.pulse.gamma");
    EXPECT_EQ(r.chainOrder[2], "com.pulse.beta");

    // Solo i vicini sono ri-cablati: predecessore A → C, C → successore B.
    EXPECT_EQ(slotA, detourC());        // pred.slot = nuovo.detour
    EXPECT_EQ(slotC, detourB());        // nuovo.slot = succ.detour
    EXPECT_EQ(slotB, realTrampoline(kTarget));  // successore invariato (resta coda)

    // Testa invariata; nessuna seconda install.
    EXPECT_EQ(registry.currentHead(kTarget), detourA());
    EXPECT_EQ(registry.installCount(), 1u);
    EXPECT_EQ(fx.backend.installAttempts(), 1u);
}

// --- n→n+1: prologo e Rollback_Store invariati, nessun nuovo backend call --
TEST(HookChainRegistryInsertLink, SubsequentInsertDoesNotTouchPrologueOrRollback) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x7000;

    void* slotA = nullptr;
    void* slotB = nullptr;

    LinkSpec a;
    a.owner = "com.pulse.alpha";
    a.symbol = "MenuLayer::init";
    a.detour = detourA();
    a.slot = &slotA;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget), a);

    // Snapshot dei byte "live" del prologo e dei record di rollback dopo 0→1.
    const auto liveAfterInstall = fx.backend.liveBytes(kTarget);
    const std::size_t rollbackAfterInstall = fx.rollback.size();

    LinkSpec b;
    b.owner = "com.pulse.beta";
    b.symbol = "MenuLayer::init";
    b.priority = 400;
    b.loadOrder = 1;
    b.detour = detourB();
    b.slot = &slotB;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget), b);

    // I byte del prologo non sono toccati dall'aggiunta dell'anello (Req 4.5)…
    EXPECT_EQ(fx.backend.liveBytes(kTarget), liveAfterInstall);
    // …e il Rollback_Store non è mutato (nessun nuovo record, Req 4.6).
    EXPECT_EQ(fx.rollback.size(), rollbackAfterInstall);
    EXPECT_EQ(fx.rollback.size(), 1u);
}

// --- n→n+1: binding non risolto → nessun anello aggiunto (Req 10.3) -------
TEST(HookChainRegistryInsertLink, RejectsUnresolvedSubsequentBinding) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x7500;

    void* slotA = nullptr;
    LinkSpec a;
    a.owner = "com.pulse.alpha";
    a.symbol = "MenuLayer::init";
    a.detour = detourA();
    a.slot = &slotA;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget), a);

    void* slotB = nullptr;
    LinkSpec b;
    b.owner = "com.pulse.beta";
    b.symbol = "MenuLayer::init";
    b.priority = 400;
    b.detour = detourB();
    b.slot = &slotB;

    FunctionBinding unresolved;
    unresolved.symbol = "MenuLayer::init";
    unresolved.address = kTarget;
    unresolved.resolved = false;

    ChainOpResult r = registry.insertLink(kTarget, unresolved, b);

    EXPECT_EQ(r.outcome, ChainOpOutcome::Rejected);
    EXPECT_TRUE(r.chainOrder.empty());
    // La catena resta a un solo anello; slotB intatto.
    EXPECT_EQ(registry.chainSize(kTarget), 1u);
    EXPECT_EQ(slotB, nullptr);
    EXPECT_EQ(fx.backend.installAttempts(), 1u);
}

// --- n→n+1: attribuzione di ciascun anello al proprio Mod_Id (Req 6.1) -----
TEST(HookChainRegistryInsertLink, AttributesEachLinkToOwner) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x8000;

    void* slotA = nullptr;
    void* slotB = nullptr;

    LinkSpec a;
    a.owner = "com.pulse.alpha";
    a.symbol = "MenuLayer::init";
    a.detour = detourA();
    a.slot = &slotA;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget), a);

    LinkSpec b;
    b.owner = "com.pulse.beta";
    b.symbol = "MenuLayer::init";
    b.priority = 400;
    b.loadOrder = 1;
    b.detour = detourB();
    b.slot = &slotB;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget), b);

    const auto alpha = fx.ledger.hooksOf("com.pulse.alpha");
    const auto beta = fx.ledger.hooksOf("com.pulse.beta");
    ASSERT_EQ(alpha.size(), 1u);
    ASSERT_EQ(beta.size(), 1u);
    EXPECT_EQ(alpha.front().target, kTarget);
    EXPECT_EQ(beta.front().target, kTarget);
}

// --- n→n+1: diagnostica osservabile dell'aggiunta (Req 11.1) --------------
TEST(HookChainRegistryInsertLink, EmitsAddDiagnosticWithPosition) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x9000;

    void* slotA = nullptr;
    void* slotB = nullptr;

    LinkSpec a;
    a.owner = "com.pulse.alpha";
    a.symbol = "MenuLayer::init";
    a.detour = detourA();
    a.slot = &slotA;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget), a);

    LinkSpec b;
    b.owner = "com.pulse.beta";
    b.symbol = "MenuLayer::init";
    b.priority = 400;
    b.loadOrder = 1;
    b.detour = detourB();
    b.slot = &slotB;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget), b);

    // Evento di aggiunta con Mod_Id + Target_Address + posizione nel Chain_Order
    // (posizione 1 per il secondo anello in coda).
    bool diagnosed = false;
    for (const auto& e : fx.events) {
        if (e.find("com.pulse.beta") != std::string::npos &&
            e.find("posizione 1") != std::string::npos) {
            diagnosed = true;
        }
    }
    EXPECT_TRUE(diagnosed);
}

}  // namespace
