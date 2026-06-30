// tests/hook_chain_registry_first_install_test.cpp — unit test della
// transizione 0→1 di HookChainRegistry::insertLink (task 3.2, Requisiti 1.3,
// 1.6, 1.8, 5.4).
//
// Verifica il PRIMO anello ammesso su un Target_Address (transizione da zero a
// uno):
//   * ammissione via Hook_Gate (binding risolto + backend available()) ed UNICA
//     Underlying_Installation creata (una sola DobbyHook), Req 1.3, 1.6;
//   * persistenza degli `originalBytes` del prologo nel RollbackStore con
//     owner = Mod_Id PRIMA dell'install, Req 5.4;
//   * cablaggio `currentHead = L0.detour` e `*L0.slot = Real_Trampoline`
//     (equivalenza con l'installazione diretta del caso a singolo anello);
//   * Req 1.8: se il gate nega (backend non disponibile / binding non risolto)
//     oppure l'install fallisce, NESSUNA catena, NESSUNA install parziale,
//     diagnostica con Mod_Id + Target_Address (ChainOpOutcome::Rejected).
//
// Usa il FakeBackend in-memory (loader/hooking/) come Hook_Engine host-testabile.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "bindings/bindings.hpp"
#include "hooking/fake_backend.hpp"
#include "hooking/hook_chain_registry.hpp"
#include "hooking/head_thunk.hpp"
#include "hooking/rollback_store.hpp"
#include "lifecycle/hook_ownership.hpp"

namespace {

using pulse::hooking::ChainOpOutcome;
using pulse::hooking::ChainOpResult;
using pulse::hooking::FakeBackend;
using pulse::hooking::HookChainRegistry;
using pulse::hooking::IHookBackend;
using pulse::hooking::LinkSpec;
using pulse::hooking::RollbackStore;
using pulse::lifecycle::HookOwnershipLedger;
using pulse::loader::bindings::FunctionBinding;

// Backend che riporta sempre `available() == false`: modella una build host
// senza backend di hooking reale (Req 1.8, 10.5).
class UnavailableBackend final : public IHookBackend {
public:
    pulse::hooking::Result<pulse::hooking::Trampoline> install(std::uintptr_t,
                                                               void*) override {
        return pulse::hooking::Result<pulse::hooking::Trampoline>::err(
            pulse::hooking::HookErrorCode::Unsupported, "unavailable");
    }
    pulse::hooking::Result<void> remove(std::uintptr_t) override {
        return pulse::hooking::Result<void>::err(
            pulse::hooking::HookErrorCode::Unsupported, "unavailable");
    }
    pulse::hooking::Result<pulse::hooking::ByteSpan> readOriginal(
        std::uintptr_t, std::size_t) override {
        return pulse::hooking::Result<pulse::hooking::ByteSpan>::err(
            pulse::hooking::HookErrorCode::Unsupported, "unavailable");
    }
    [[nodiscard]] std::string_view name() const noexcept override {
        return "unavailable-backend";
    }
    [[nodiscard]] bool available() const noexcept override { return false; }
};

// Detour/slot fittizi: nel caso host i detour sono semplici puntatori opachi e
// lo slot è il Trampoline_Slot `pulse_original` (void**) della registrazione.
int gDetourTag = 0;
void* fakeDetour() { return static_cast<void*>(&gDetourTag); }

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
                           "pulse_chain_first_install_test.bin"};
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

// --- 0→1: install riuscita crea l'unica Underlying_Installation -----------
TEST(HookChainRegistryFirstInstall, FirstLinkCreatesSingleInstall) {
    Fixture fx;
    auto registry = fx.makeRegistry();

    constexpr std::uintptr_t kTarget = 0x4000;
    void* slot = nullptr;  // Trampoline_Slot pulse_original
    LinkSpec link;
    link.owner = "com.pulse.alpha";
    link.symbol = "MenuLayer::init";
    link.priority = 500;
    link.loadOrder = 0;
    link.detour = fakeDetour();
    link.slot = &slot;

    ChainOpResult result = registry.insertLink(
        kTarget, resolvedBinding("MenuLayer::init", kTarget), link);

    EXPECT_EQ(result.outcome, ChainOpOutcome::CreatedInstall);
    EXPECT_EQ(result.error.code, pulse::hooking::HookErrorCode::None);
    ASSERT_EQ(result.chainOrder.size(), 1u);
    EXPECT_EQ(result.chainOrder.front(), "com.pulse.alpha");

    // Una sola Underlying_Installation, una sola DobbyHook (Req 1.3, 1.6).
    EXPECT_TRUE(registry.hasInstall(kTarget));
    EXPECT_EQ(registry.installCount(), 1u);
    EXPECT_EQ(registry.chainSize(kTarget), 1u);
    EXPECT_EQ(fx.backend.installedCount(), 1u);
    EXPECT_EQ(fx.backend.installAttempts(), 1u);

    // Lo slot dell'unico anello (testa == coda) punta al Real_Trampoline.
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot, reinterpret_cast<void*>(kTarget ^ 0xA5A5A5A5ULL));
}

// --- 0→1: byte originali persistiti PRIMA dell'install (Req 5.4) ----------
TEST(HookChainRegistryFirstInstall, PersistsOriginalBytesBeforeInstall) {
    Fixture fx;
    constexpr std::uintptr_t kTarget = 0x5000;
    const auto expectedOriginal = fx.backend.snapshotOriginal(kTarget);  // semina deterministica

    auto registry = fx.makeRegistry();
    void* slot = nullptr;
    LinkSpec link;
    link.owner = "com.pulse.alpha";
    link.symbol = "PlayLayer::init";
    link.detour = fakeDetour();
    link.slot = &slot;

    registry.insertLink(kTarget, resolvedBinding("PlayLayer::init", kTarget), link);

    ASSERT_EQ(fx.rollback.size(), 1u);
    const auto& record = fx.rollback.records().front();
    EXPECT_EQ(record.owner, "com.pulse.alpha");
    EXPECT_EQ(record.symbol, "PlayLayer::init");
    EXPECT_EQ(record.address, kTarget);
    EXPECT_FALSE(record.originalBytes.empty());

    // I byte persistiti coincidono con i byte ORIGINALI del prologo letti dal
    // backend (il rollback è byte-esatto, Req 5.4).
    const auto liveOriginal = fx.backend.snapshotOriginal(kTarget);
    ASSERT_TRUE(liveOriginal.has_value());
    EXPECT_EQ(record.originalBytes, *liveOriginal);
}

// --- 0→1: l'anello è attribuito al Mod_Id nel ledger (Req 6.1) ------------
TEST(HookChainRegistryFirstInstall, AttributesLinkToOwner) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x6000;
    void* slot = nullptr;
    LinkSpec link;
    link.owner = "com.pulse.alpha";
    link.symbol = "MenuLayer::init";
    link.detour = fakeDetour();
    link.slot = &slot;

    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget), link);

    const auto owned = fx.ledger.hooksOf("com.pulse.alpha");
    ASSERT_EQ(owned.size(), 1u);
    EXPECT_EQ(owned.front().symbol, "MenuLayer::init");
    EXPECT_EQ(owned.front().target, kTarget);
}

// --- Req 1.8: binding non risolto → nessuna catena, nessuna install -------
TEST(HookChainRegistryFirstInstall, RejectsUnresolvedBindingNoInstall) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x7000;
    void* slot = nullptr;
    LinkSpec link;
    link.owner = "com.pulse.alpha";
    link.symbol = "MenuLayer::init";
    link.detour = fakeDetour();
    link.slot = &slot;

    FunctionBinding unresolved;
    unresolved.symbol = "MenuLayer::init";
    unresolved.address = kTarget;
    unresolved.resolved = false;  // non risolto

    ChainOpResult result = registry.insertLink(kTarget, unresolved, link);

    EXPECT_EQ(result.outcome, ChainOpOutcome::Rejected);
    EXPECT_TRUE(result.chainOrder.empty());
    EXPECT_FALSE(registry.hasInstall(kTarget));
    EXPECT_EQ(registry.installCount(), 0u);
    EXPECT_EQ(registry.chainSize(kTarget), 0u);
    EXPECT_EQ(fx.backend.installedCount(), 0u);
    EXPECT_EQ(fx.backend.installAttempts(), 0u);  // il gate NON tocca il backend
    EXPECT_TRUE(fx.rollback.empty());             // nessun record persistito

    // Diagnostica con Mod_Id + simbolo (Req 1.8).
    bool diagnosed = false;
    for (const auto& e : fx.events) {
        if (e.find("com.pulse.alpha") != std::string::npos &&
            e.find("MenuLayer::init") != std::string::npos) {
            diagnosed = true;
        }
    }
    EXPECT_TRUE(diagnosed);
}

// --- Req 1.8: backend non disponibile → nessuna install, diagnostica ------
TEST(HookChainRegistryFirstInstall, RejectsWhenBackendUnavailable) {
    UnavailableBackend backend;
    RollbackStore rollback{std::filesystem::temp_directory_path() /
                           "pulse_chain_unavailable_test.bin"};
    HookOwnershipLedger ledger;
    std::vector<std::string> events;
    HookChainRegistry registry{backend, rollback, ledger,
                               [&events](std::string_view m) {
                                   events.emplace_back(m);
                               }};

    constexpr std::uintptr_t kTarget = 0x8000;
    void* slot = nullptr;
    LinkSpec link;
    link.owner = "com.pulse.alpha";
    link.symbol = "MenuLayer::init";
    link.detour = fakeDetour();
    link.slot = &slot;

    ChainOpResult result =
        registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget), link);

    EXPECT_EQ(result.outcome, ChainOpOutcome::Rejected);
    EXPECT_TRUE(result.chainOrder.empty());
    EXPECT_FALSE(registry.hasInstall(kTarget));
    EXPECT_EQ(registry.installCount(), 0u);
    EXPECT_TRUE(rollback.empty());

    // La diagnostica nomina il backend (Req 10.5).
    bool namesBackend = false;
    for (const auto& e : events) {
        if (e.find("unavailable-backend") != std::string::npos) {
            namesBackend = true;
        }
    }
    EXPECT_TRUE(namesBackend);

    std::error_code ec;
    std::filesystem::remove(rollback.path(), ec);
}

// --- Req 1.8: install fallita → nessuna catena, nessuna install parziale ---
TEST(HookChainRegistryFirstInstall, RejectsWhenInstallFails) {
    Fixture fx;
    constexpr std::uintptr_t kTarget = 0x9000;
    fx.backend.failInstallAt(kTarget);  // forza il fallimento dell'install

    auto registry = fx.makeRegistry();
    void* slot = nullptr;
    LinkSpec link;
    link.owner = "com.pulse.alpha";
    link.symbol = "MenuLayer::init";
    link.detour = fakeDetour();
    link.slot = &slot;

    ChainOpResult result =
        registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget), link);

    EXPECT_EQ(result.outcome, ChainOpOutcome::Rejected);
    EXPECT_TRUE(result.chainOrder.empty());
    EXPECT_FALSE(registry.hasInstall(kTarget));   // nessuna install parziale
    EXPECT_EQ(registry.installCount(), 0u);
    EXPECT_EQ(registry.chainSize(kTarget), 0u);   // nessuna catena
    EXPECT_EQ(fx.backend.installedCount(), 0u);
}

}  // namespace
