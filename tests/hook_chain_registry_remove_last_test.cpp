// tests/hook_chain_registry_remove_last_test.cpp — unit test della rimozione
// dell'ULTIMO anello di HookChainRegistry::removeOwner (task 5.2, transizione
// 1→0, Requisiti 5.3, 5.4).
//
// Verifica la rimozione dell'unico anello rimasto su un Target_Address:
//   * l'unica Underlying_Installation (una sola DobbyHook) viene rimossa dal
//     backend (Req 5.3): `installCount` cala, il backend non risulta più
//     installato sul target;
//   * i byte originali del prologo sono ripristinati byte-esatto tramite il
//     Rollback_Store (Req 5.4): i byte "live" del FakeBackend tornano identici
//     agli `originalBytes` persistiti alla transizione 0→1;
//   * il ChainSlot del target è dismesso: `chainSize == 0`, `hasInstall == false`,
//     `currentHead == nullptr`;
//   * l'esito è ChainOpOutcome::RemovedLastInstall con diagnostica osservabile
//     (rimozione anello + rimozione install, Req 11.2, 11.3);
//   * con più anelli dello stesso owner, la rimozione del Mod_Id svuota la
//     catena e dismette l'install (1→0);
//   * un fallimento di remove dal backend è isolato (nessuna dismissione del
//     ChainSlot), esito Rejected.
//
// Usa il FakeBackend in-memory (loader/hooking/) come Hook_Engine host-testabile:
// `remove` ripristina i byte "live" agli originali (rollback byte-esatto).

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
void* detourA() { return static_cast<void*>(&gDetourA); }
void* detourB() { return static_cast<void*>(&gDetourB); }

FunctionBinding resolvedBinding(std::string symbol, std::uintptr_t address) {
    FunctionBinding b;
    b.symbol = std::move(symbol);
    b.address = address;
    b.resolved = true;
    return b;
}

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

struct Fixture {
    FakeBackend backend;
    RollbackStore rollback{std::filesystem::temp_directory_path() /
                           "pulse_chain_remove_last_test.bin"};
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

// --- 1→0: rimozione dell'unico anello rimuove l'install e dismette lo slot --
TEST(HookChainRegistryRemoveLast, RemoveSingleLinkUninstallsAndDisposesSlot) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x4000;

    void* slotA = nullptr;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 500, 0, detourA(), &slotA));

    // Stato pre-rimozione: una sola install, una sola DobbyHook.
    ASSERT_EQ(registry.installCount(), 1u);
    ASSERT_TRUE(fx.backend.isInstalled(kTarget));
    ASSERT_EQ(fx.backend.installedCount(), 1u);

    auto results = registry.removeOwner("com.pulse.alpha");

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().outcome, ChainOpOutcome::RemovedLastInstall);
    EXPECT_TRUE(results.front().chainOrder.empty());

    // L'unica Underlying_Installation è rimossa (Req 5.3): installCount cala.
    EXPECT_EQ(registry.installCount(), 0u);
    EXPECT_FALSE(fx.backend.isInstalled(kTarget));
    EXPECT_EQ(fx.backend.installedCount(), 0u);

    // Il ChainSlot del target è dismesso.
    EXPECT_FALSE(registry.hasInstall(kTarget));
    EXPECT_EQ(registry.chainSize(kTarget), 0u);
    EXPECT_EQ(registry.currentHead(kTarget), nullptr);
}

// --- 1→0: i byte originali sono ripristinati byte-esatto (Req 5.4) ---------
TEST(HookChainRegistryRemoveLast, RestoresOriginalBytesExactly) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x5000;

    void* slotA = nullptr;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 500, 0, detourA(), &slotA));

    // Byte originali del prologo (semina deterministica del FakeBackend): la
    // regione è creata dall'install, gli originali sono preservati attraverso
    // l'install (mai mutati).
    const auto originalBytes = fx.backend.snapshotOriginal(kTarget);
    ASSERT_TRUE(originalBytes.has_value());

    // Dopo l'install i byte "live" sono patchati: != originali.
    ASSERT_NE(fx.backend.liveBytes(kTarget), originalBytes);
    // Gli `originalBytes` sono stati persistiti nel Rollback_Store a 0→1.
    ASSERT_EQ(fx.rollback.size(), 1u);

    registry.removeOwner("com.pulse.alpha");

    // I byte del prologo sono ripristinati byte-esatto agli originali (Req 5.4).
    EXPECT_EQ(fx.backend.liveBytes(kTarget), originalBytes);
}

// --- 1→0: esito RemovedLastInstall + diagnostica osservabile (Req 11.2/11.3) -
TEST(HookChainRegistryRemoveLast, EmitsRemoveAndUninstallDiagnostics) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x6000;

    void* slotA = nullptr;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 500, 0, detourA(), &slotA));

    fx.events.clear();
    registry.removeOwner("com.pulse.alpha");

    bool removedLink = false;
    bool removedInstall = false;
    for (const auto& e : fx.events) {
        if (e.find("rimosso anello") != std::string::npos &&
            e.find("com.pulse.alpha") != std::string::npos &&
            e.find("0x6000") != std::string::npos) {
            removedLink = true;
        }
        if (e.find("rimossa Underlying_Installation") != std::string::npos &&
            e.find("0x6000") != std::string::npos) {
            removedInstall = true;
        }
    }
    EXPECT_TRUE(removedLink);
    EXPECT_TRUE(removedInstall);
}

// --- 1→0: più anelli dello stesso owner svuotano la catena e dismettono ----
TEST(HookChainRegistryRemoveLast, RemovingOwnerWithAllLinksUninstalls) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x7000;

    // Stesso owner registra due anelli sullo stesso target (priorità diverse).
    void* slotA = nullptr;
    void* slotB = nullptr;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 900, 0, detourA(), &slotA));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 100, 1, detourB(), &slotB));

    ASSERT_EQ(registry.chainSize(kTarget), 2u);
    ASSERT_EQ(registry.installCount(), 1u);

    // Rimuovere l'owner toglie ENTRAMBI gli anelli → catena vuota → 1→0.
    auto results = registry.removeOwner("com.pulse.alpha");

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().outcome, ChainOpOutcome::RemovedLastInstall);
    EXPECT_EQ(registry.installCount(), 0u);
    EXPECT_FALSE(registry.hasInstall(kTarget));
    EXPECT_EQ(registry.chainSize(kTarget), 0u);
    EXPECT_FALSE(fx.backend.isInstalled(kTarget));
}

// --- Catene su target distinti: la 1→0 di uno non tocca l'altro ------------
TEST(HookChainRegistryRemoveLast, DisposesOnlyTheEmptiedTarget) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTargetA = 0x8000;
    constexpr std::uintptr_t kTargetB = 0x8100;

    void* slotA = nullptr;
    void* slotB = nullptr;
    registry.insertLink(kTargetA, resolvedBinding("MenuLayer::init", kTargetA),
                        makeLink("com.pulse.alpha", 500, 0, detourA(), &slotA));
    registry.insertLink(kTargetB, resolvedBinding("PlayLayer::init", kTargetB),
                        makeLink("com.pulse.beta", 500, 1, detourB(), &slotB));

    ASSERT_EQ(registry.installCount(), 2u);

    // Rimuovi solo alpha (unico anello di A): A va 1→0, B resta intatto.
    auto results = registry.removeOwner("com.pulse.alpha");

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().outcome, ChainOpOutcome::RemovedLastInstall);

    // Target A dismesso, target B mantenuto.
    EXPECT_FALSE(registry.hasInstall(kTargetA));
    EXPECT_EQ(registry.chainSize(kTargetA), 0u);
    EXPECT_TRUE(registry.hasInstall(kTargetB));
    EXPECT_EQ(registry.chainSize(kTargetB), 1u);
    EXPECT_EQ(registry.installCount(), 1u);
    EXPECT_TRUE(fx.backend.isInstalled(kTargetB));
}

// --- Fallimento di remove dal backend: isolato, ChainSlot non dismesso -----
TEST(HookChainRegistryRemoveLast, IsolatesBackendRemoveFailure) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x9000;

    void* slotA = nullptr;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 500, 0, detourA(), &slotA));

    // Forza il fallimento della rimozione dal backend.
    fx.backend.failRemoveAt(kTarget);

    auto results = registry.removeOwner("com.pulse.alpha");

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().outcome, ChainOpOutcome::Rejected);
    EXPECT_NE(results.front().error.code, pulse::hooking::HookErrorCode::None);

    // Il fallimento è isolato: il ChainSlot NON viene dismesso (resta nella
    // mappa), così lo stato di errore è osservabile e non si perde il target.
    EXPECT_TRUE(registry.hasInstall(kTarget));
}

}  // namespace
