// tests/hook_chain_registry_teardown_test.cpp — unit test del teardown ordinato
// di HookChainRegistry::teardown (task 5.5, Requisiti 7.1, 7.2, 7.3, 7.4, 7.5,
// 7.6).
//
// Verifica lo smontaggio in ordine inverso a `LoadPlan.order` con isolamento dei
// fallimenti:
//   * le mod sono rimosse nell'ordine fornito (l'inverso di LoadPlan.order,
//     Req 7.1): l'ordine degli eventi di rimozione rispetta `reverseOrder`;
//   * al termine non resta alcuna Underlying_Installation né alcun anello
//     residuo (Req 7.2, 7.4): `installCount == 0`, nessuna catena;
//   * un install→remove dell'intera catena di N anelli riporta i byte del
//     prologo identici allo stato pre-installazione (round-trip byte-esatto,
//     Req 7.3, 7.5): i byte "live" del FakeBackend tornano agli `originalBytes`;
//   * un fallimento di rimozione è isolato: la causa è registrata con Mod_Id e
//     Target_Address e il teardown prosegue con le mod restanti (Req 7.6).
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
using pulse::hooking::FakeBackend;
using pulse::hooking::HookChainRegistry;
using pulse::hooking::LinkSpec;
using pulse::hooking::RollbackStore;
using pulse::lifecycle::HookOwnershipLedger;
using pulse::loader::bindings::FunctionBinding;

// Detour distinti per ogni anello: nel caso host i detour sono semplici
// puntatori opachi (l'identità conta per il relinking degli slot).
int gDetourA = 0;
int gDetourB = 0;
int gDetourC = 0;
int gDetourD = 0;
void* detourA() { return static_cast<void*>(&gDetourA); }
void* detourB() { return static_cast<void*>(&gDetourB); }
void* detourC() { return static_cast<void*>(&gDetourC); }
void* detourD() { return static_cast<void*>(&gDetourD); }

FunctionBinding resolvedBinding(std::string symbol, std::uintptr_t address) {
    FunctionBinding b;
    b.symbol = std::move(symbol);
    b.address = address;
    b.resolved = true;
    return b;
}

LinkSpec makeLink(const std::string& owner, const std::string& symbol,
                  int priority, uint64_t loadOrder, void* detour, void** slot) {
    LinkSpec s;
    s.owner = owner;
    s.symbol = symbol;
    s.priority = priority;
    s.loadOrder = loadOrder;
    s.detour = detour;
    s.slot = slot;
    return s;
}

struct Fixture {
    FakeBackend backend;
    RollbackStore rollback{std::filesystem::temp_directory_path() /
                           "pulse_chain_teardown_test.bin"};
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

// --- Zero residual: teardown svuota TUTTE le catene (Req 7.2, 7.4) ----------
TEST(HookChainRegistryTeardown, RemovesAllInstallsAndLinks) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTargetA = 0x1000;
    constexpr std::uintptr_t kTargetB = 0x2000;

    void* slotA = nullptr;
    void* slotB = nullptr;
    void* slotC = nullptr;
    // alpha e beta condividono kTargetA; gamma su kTargetB.
    registry.insertLink(kTargetA, resolvedBinding("MenuLayer::init", kTargetA),
                        makeLink("com.pulse.alpha", "MenuLayer::init", 900, 0,
                                 detourA(), &slotA));
    registry.insertLink(kTargetA, resolvedBinding("MenuLayer::init", kTargetA),
                        makeLink("com.pulse.beta", "MenuLayer::init", 500, 1,
                                 detourB(), &slotB));
    registry.insertLink(kTargetB, resolvedBinding("PlayLayer::init", kTargetB),
                        makeLink("com.pulse.gamma", "PlayLayer::init", 500, 2,
                                 detourC(), &slotC));

    ASSERT_EQ(registry.installCount(), 2u);

    // LoadPlan.order = [alpha, beta, gamma] → reverseOrder = [gamma, beta, alpha].
    registry.teardown({"com.pulse.gamma", "com.pulse.beta", "com.pulse.alpha"});

    // Nessuna Underlying_Installation residua né alcun anello residuo (Req 7.4).
    EXPECT_EQ(registry.installCount(), 0u);
    EXPECT_EQ(registry.chainSize(kTargetA), 0u);
    EXPECT_EQ(registry.chainSize(kTargetB), 0u);
    EXPECT_FALSE(registry.hasInstall(kTargetA));
    EXPECT_FALSE(registry.hasInstall(kTargetB));
    EXPECT_FALSE(fx.backend.isInstalled(kTargetA));
    EXPECT_FALSE(fx.backend.isInstalled(kTargetB));
    EXPECT_EQ(fx.backend.installedCount(), 0u);
}

// --- Ordine inverso: gli anelli sono rimossi nell'ordine fornito (Req 7.1) --
TEST(HookChainRegistryTeardown, RemovesInReverseOrder) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    // Ogni mod su un Target_Address distinto, così l'evento di rimozione di
    // ciascuna è osservabile e attribuibile in modo univoco.
    constexpr std::uintptr_t kTargetA = 0x3000;
    constexpr std::uintptr_t kTargetB = 0x3100;
    constexpr std::uintptr_t kTargetC = 0x3200;

    void* slotA = nullptr;
    void* slotB = nullptr;
    void* slotC = nullptr;
    registry.insertLink(kTargetA, resolvedBinding("A::init", kTargetA),
                        makeLink("com.pulse.alpha", "A::init", 500, 0, detourA(),
                                 &slotA));
    registry.insertLink(kTargetB, resolvedBinding("B::init", kTargetB),
                        makeLink("com.pulse.beta", "B::init", 500, 1, detourB(),
                                 &slotB));
    registry.insertLink(kTargetC, resolvedBinding("C::init", kTargetC),
                        makeLink("com.pulse.gamma", "C::init", 500, 2, detourC(),
                                 &slotC));

    fx.events.clear();

    // reverseOrder = [gamma, beta, alpha].
    registry.teardown({"com.pulse.gamma", "com.pulse.beta", "com.pulse.alpha"});

    // Estrai l'ordine delle rimozioni anello dagli eventi diagnostici.
    std::vector<std::string> removalOrder;
    for (const auto& e : fx.events) {
        if (e.find("rimosso anello mod") == std::string::npos) {
            continue;
        }
        if (e.find("com.pulse.gamma") != std::string::npos) {
            removalOrder.push_back("com.pulse.gamma");
        } else if (e.find("com.pulse.beta") != std::string::npos) {
            removalOrder.push_back("com.pulse.beta");
        } else if (e.find("com.pulse.alpha") != std::string::npos) {
            removalOrder.push_back("com.pulse.alpha");
        }
    }

    const std::vector<std::string> expected{"com.pulse.gamma", "com.pulse.beta",
                                            "com.pulse.alpha"};
    EXPECT_EQ(removalOrder, expected);
    EXPECT_EQ(registry.installCount(), 0u);
}

// --- Round-trip byte-esatto dell'intera catena di N anelli (Req 7.3, 7.5) ---
TEST(HookChainRegistryTeardown, ByteExactRoundTripOverEntireChain) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x4000;

    // Quattro anelli (3 owner) su un unico Target_Address: la prima install
    // (0→1) persiste gli originalBytes; gli anelli successivi non li mutano.
    void* slotA = nullptr;
    void* slotB = nullptr;
    void* slotC = nullptr;
    void* slotD = nullptr;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", "MenuLayer::init", 900, 0,
                                 detourA(), &slotA));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.beta", "MenuLayer::init", 700, 1,
                                 detourB(), &slotB));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.gamma", "MenuLayer::init", 500, 2,
                                 detourC(), &slotC));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", "MenuLayer::init", 300, 3,
                                 detourD(), &slotD));

    ASSERT_EQ(registry.chainSize(kTarget), 4u);
    ASSERT_EQ(registry.installCount(), 1u);

    // Byte originali del prologo (mai mutati attraverso le install).
    const auto originalBytes = fx.backend.snapshotOriginal(kTarget);
    ASSERT_TRUE(originalBytes.has_value());
    // Dopo l'unica install i byte "live" sono patchati: != originali.
    ASSERT_NE(fx.backend.liveBytes(kTarget), originalBytes);

    // Teardown dell'intera catena (reverse di LoadPlan.order).
    registry.teardown({"com.pulse.gamma", "com.pulse.beta", "com.pulse.alpha"});

    // I byte del prologo sono identici allo stato pre-installazione (Req 7.5).
    EXPECT_EQ(fx.backend.liveBytes(kTarget), originalBytes);
    // Nessuna install né anello residuo (Req 7.4).
    EXPECT_EQ(registry.installCount(), 0u);
    EXPECT_EQ(registry.chainSize(kTarget), 0u);
    EXPECT_FALSE(fx.backend.isInstalled(kTarget));
}

// --- Isolamento dei fallimenti: prosegue oltre la mod che fallisce (Req 7.6) -
TEST(HookChainRegistryTeardown, IsolatesFailureAndContinues) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTargetA = 0x5000;  // alpha
    constexpr std::uintptr_t kTargetB = 0x5100;  // beta (remove forzato a fallire)
    constexpr std::uintptr_t kTargetC = 0x5200;  // gamma

    void* slotA = nullptr;
    void* slotB = nullptr;
    void* slotC = nullptr;
    registry.insertLink(kTargetA, resolvedBinding("A::init", kTargetA),
                        makeLink("com.pulse.alpha", "A::init", 500, 0, detourA(),
                                 &slotA));
    registry.insertLink(kTargetB, resolvedBinding("B::init", kTargetB),
                        makeLink("com.pulse.beta", "B::init", 500, 1, detourB(),
                                 &slotB));
    registry.insertLink(kTargetC, resolvedBinding("C::init", kTargetC),
                        makeLink("com.pulse.gamma", "C::init", 500, 2, detourC(),
                                 &slotC));

    ASSERT_EQ(registry.installCount(), 3u);

    // La rimozione dell'unica install di beta (transizione 1→0) fallisce.
    fx.backend.failRemoveAt(kTargetB);

    fx.events.clear();
    // reverseOrder = [gamma, beta, alpha]: beta fallisce ma alpha (dopo) è
    // comunque smontata → il teardown prosegue oltre il fallimento.
    registry.teardown({"com.pulse.gamma", "com.pulse.beta", "com.pulse.alpha"});

    // alpha e gamma sono state smontate; beta resta installata (fallimento
    // isolato, ChainSlot non dismesso). Il teardown è proseguito comunque.
    EXPECT_FALSE(registry.hasInstall(kTargetA));
    EXPECT_FALSE(registry.hasInstall(kTargetC));
    EXPECT_TRUE(registry.hasInstall(kTargetB));
    EXPECT_EQ(registry.installCount(), 1u);

    // La causa del fallimento è registrata con Mod_Id e Target_Address (Req 7.6).
    bool isolatedDiag = false;
    for (const auto& e : fx.events) {
        if (e.find("fallimento isolato") != std::string::npos &&
            e.find("com.pulse.beta") != std::string::npos &&
            e.find("0x5100") != std::string::npos) {
            isolatedDiag = true;
        }
    }
    EXPECT_TRUE(isolatedDiag);
}

// --- Mod sconosciute / lista vuota: nessun crash, nessun effetto -----------
TEST(HookChainRegistryTeardown, EmptyOrUnknownOwnersAreNoOps) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x6000;

    void* slotA = nullptr;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", "MenuLayer::init", 500, 0,
                                 detourA(), &slotA));

    // Teardown con una mod non presente: nessun anello rimosso.
    registry.teardown({"com.pulse.unknown"});
    EXPECT_EQ(registry.installCount(), 1u);
    EXPECT_EQ(registry.chainSize(kTarget), 1u);

    // Teardown con lista vuota: no-op.
    registry.teardown({});
    EXPECT_EQ(registry.installCount(), 1u);

    // Teardown corretto: smonta tutto.
    registry.teardown({"com.pulse.alpha"});
    EXPECT_EQ(registry.installCount(), 0u);
    EXPECT_EQ(registry.chainSize(kTarget), 0u);
}

}  // namespace
