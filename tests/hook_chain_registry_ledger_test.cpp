// tests/hook_chain_registry_ledger_test.cpp — unit test del cablaggio del
// HookOwnershipLedger nella HookChainRegistry (task 5.3, Requisiti 6.1, 6.2,
// 6.3, 6.4, 6.5).
//
// Verifica l'attribuzione per Mod_Id all'inserimento e la rimozione selettiva
// per Mod_Id, sia nelle catene sia nel ledger:
//   * ogni anello inserito è attribuito al proprio Mod_Id, interrogabile via
//     `hooksOf(modId)` (Req 6.1);
//   * più Mod_Id che condividono un Target_Address sono attribuiti senza
//     ambiguità: `hooksOf` restituisce esattamente gli anelli di ciascuno
//     (Req 6.3);
//   * `removeOwner(modId)` rimuove dalle catene e rilascia dal ledger
//     ESCLUSIVAMENTE gli anelli di quel Mod_Id, preservando gli altri (Req 6.2,
//     6.4), corretto anche con Target_Address condivisi;
//   * invariante globale (Req 6.5): in ogni istante l'insieme degli anelli
//     abilitati (ledger `allInstalled`) coincide con l'unione degli anelli delle
//     mod ancora presenti, senza anelli orfani dopo le rimozioni.
//
// Usa il FakeBackend in-memory (loader/hooking/) come Hook_Engine host-testabile.

#include <gtest/gtest.h>

#include <algorithm>
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
using pulse::lifecycle::OwnedHook;
using pulse::loader::bindings::FunctionBinding;

// Detour distinti per ogni anello: nel caso host sono semplici puntatori opachi.
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
                           "pulse_chain_ledger_test.bin"};
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

// Conta gli anelli attribuiti a `owner` nell'insieme globale degli installati.
std::size_t countOwned(const HookOwnershipLedger& ledger, const std::string& owner) {
    std::size_t n = 0;
    for (const auto& h : ledger.allInstalled()) {
        if (h.owner == owner) {
            ++n;
        }
    }
    return n;
}

// --- Attribuzione all'inserimento: hooksOf restituisce gli anelli del Mod_Id -
TEST(HookChainRegistryLedger, AttributesEachLinkToItsOwner) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x4000;

    void* slotA = nullptr;  // alpha
    void* slotB = nullptr;  // beta
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", "MenuLayer::init", 900, 0,
                                 detourA(), &slotA));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.beta", "MenuLayer::init", 500, 1,
                                 detourB(), &slotB));

    // Ogni Mod_Id possiede esattamente il proprio anello (Req 6.1).
    ASSERT_EQ(fx.ledger.hooksOf("com.pulse.alpha").size(), 1u);
    ASSERT_EQ(fx.ledger.hooksOf("com.pulse.beta").size(), 1u);
    EXPECT_EQ(fx.ledger.hooksOf("com.pulse.alpha").front().symbol, "MenuLayer::init");
    EXPECT_EQ(fx.ledger.hooksOf("com.pulse.alpha").front().target, kTarget);
    // Unione globale = somma degli anelli (Req 6.5).
    EXPECT_EQ(fx.ledger.installedCount(), 2u);
}

// --- Target_Address condiviso: attribuzione senza ambiguità (Req 6.3) ------
TEST(HookChainRegistryLedger, SharedTargetAttributesEachOwnerUnambiguously) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kShared = 0x5000;

    void* slotA = nullptr;
    void* slotB = nullptr;
    void* slotC = nullptr;
    // Tre Mod_Id distinti sullo STESSO Target_Address.
    registry.insertLink(kShared, resolvedBinding("MenuLayer::init", kShared),
                        makeLink("com.pulse.alpha", "MenuLayer::init", 900, 0,
                                 detourA(), &slotA));
    registry.insertLink(kShared, resolvedBinding("MenuLayer::init", kShared),
                        makeLink("com.pulse.beta", "MenuLayer::init", 500, 1,
                                 detourB(), &slotB));
    registry.insertLink(kShared, resolvedBinding("MenuLayer::init", kShared),
                        makeLink("com.pulse.gamma", "MenuLayer::init", 100, 2,
                                 detourC(), &slotC));

    // Una sola Underlying_Installation, ma tre attribuzioni distinte (Req 6.3).
    EXPECT_EQ(registry.installCount(), 1u);
    EXPECT_EQ(countOwned(fx.ledger, "com.pulse.alpha"), 1u);
    EXPECT_EQ(countOwned(fx.ledger, "com.pulse.beta"), 1u);
    EXPECT_EQ(countOwned(fx.ledger, "com.pulse.gamma"), 1u);
    // Ogni hooksOf è esattamente il proprio anello sul target condiviso.
    EXPECT_EQ(fx.ledger.hooksOf("com.pulse.beta").front().target, kShared);
}

// --- Rimozione selettiva: release del solo Mod_Id, gli altri invariati -----
TEST(HookChainRegistryLedger, RemoveOwnerReleasesOnlyThatModFromLedger) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kShared = 0x6000;

    void* slotA = nullptr;
    void* slotB = nullptr;
    void* slotC = nullptr;
    registry.insertLink(kShared, resolvedBinding("MenuLayer::init", kShared),
                        makeLink("com.pulse.alpha", "MenuLayer::init", 900, 0,
                                 detourA(), &slotA));
    registry.insertLink(kShared, resolvedBinding("MenuLayer::init", kShared),
                        makeLink("com.pulse.beta", "MenuLayer::init", 500, 1,
                                 detourB(), &slotB));
    registry.insertLink(kShared, resolvedBinding("MenuLayer::init", kShared),
                        makeLink("com.pulse.gamma", "MenuLayer::init", 100, 2,
                                 detourC(), &slotC));

    ASSERT_EQ(fx.ledger.installedCount(), 3u);

    // Rimuovi l'owner "beta" (in mezzo): keep-install.
    auto results = registry.removeOwner("com.pulse.beta");
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().outcome, ChainOpOutcome::RemovedKeepInstall);

    // Dal ledger sono rilasciati ESCLUSIVAMENTE gli anelli di beta (Req 6.4);
    // alpha e gamma restano attribuiti (Req 6.2).
    EXPECT_TRUE(fx.ledger.hooksOf("com.pulse.beta").empty());
    EXPECT_EQ(countOwned(fx.ledger, "com.pulse.alpha"), 1u);
    EXPECT_EQ(countOwned(fx.ledger, "com.pulse.gamma"), 1u);
    EXPECT_EQ(fx.ledger.installedCount(), 2u);

    // La catena riflette la stessa rimozione selettiva (Req 6.2).
    EXPECT_EQ(registry.chainOrder(kShared),
              (std::vector<std::string>{"com.pulse.alpha", "com.pulse.gamma"}));
}

// --- Invariante globale (Req 6.5): ledger == unione delle mod presenti ------
TEST(HookChainRegistryLedger, GlobalInvariantLedgerMatchesEnabledLinks) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kMenu = 0x7000;  // condiviso da alpha + beta
    constexpr std::uintptr_t kPlay = 0x7100;  // solo gamma

    void* slotA = nullptr;
    void* slotB = nullptr;
    void* slotC = nullptr;
    void* slotD = nullptr;
    registry.insertLink(kMenu, resolvedBinding("MenuLayer::init", kMenu),
                        makeLink("com.pulse.alpha", "MenuLayer::init", 900, 0,
                                 detourA(), &slotA));
    registry.insertLink(kMenu, resolvedBinding("MenuLayer::init", kMenu),
                        makeLink("com.pulse.beta", "MenuLayer::init", 500, 1,
                                 detourB(), &slotB));
    registry.insertLink(kPlay, resolvedBinding("PlayLayer::init", kPlay),
                        makeLink("com.pulse.gamma", "PlayLayer::init", 500, 2,
                                 detourC(), &slotC));
    // alpha ha un secondo anello su kPlay (stesso owner, target diverso).
    registry.insertLink(kPlay, resolvedBinding("PlayLayer::init", kPlay),
                        makeLink("com.pulse.alpha", "PlayLayer::init", 800, 3,
                                 detourD(), &slotD));

    // Helper: l'unione degli anelli "abilitati" osservata dalle catene.
    const auto enabledFromChains = [&]() {
        std::vector<std::string> owners;
        for (const auto t : {kMenu, kPlay}) {
            for (const auto& o : registry.chainOrder(t)) {
                owners.push_back(o);
            }
        }
        std::sort(owners.begin(), owners.end());
        return owners;
    };

    const auto enabledFromLedger = [&]() {
        std::vector<std::string> owners;
        for (const auto& h : fx.ledger.allInstalled()) {
            owners.push_back(h.owner);
        }
        std::sort(owners.begin(), owners.end());
        return owners;
    };

    // Stato iniziale: 4 anelli totali, ledger == catene (Req 6.5).
    ASSERT_EQ(fx.ledger.installedCount(), 4u);
    EXPECT_EQ(enabledFromLedger(), enabledFromChains());

    // Rimuovi alpha: tocca ENTRAMBI i suoi anelli (su kMenu e kPlay), preservando
    // beta e gamma. Su kMenu resta beta (keep-install); su kPlay resta gamma.
    registry.removeOwner("com.pulse.alpha");

    EXPECT_TRUE(fx.ledger.hooksOf("com.pulse.alpha").empty());
    EXPECT_EQ(countOwned(fx.ledger, "com.pulse.beta"), 1u);
    EXPECT_EQ(countOwned(fx.ledger, "com.pulse.gamma"), 1u);
    EXPECT_EQ(fx.ledger.installedCount(), 2u);
    // Invariante globale ancora valido dopo la rimozione (Req 6.5).
    EXPECT_EQ(enabledFromLedger(), enabledFromChains());

    // Entrambi i target restano installati (un anello ciascuno).
    EXPECT_TRUE(registry.hasInstall(kMenu));
    EXPECT_TRUE(registry.hasInstall(kPlay));
}

// --- Rimozione 1→0 di un owner rilascia anche l'attribuzione ----------------
TEST(HookChainRegistryLedger, RemoveLastLinkAlsoReleasesAttribution) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x8000;

    void* slotA = nullptr;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", "MenuLayer::init", 500, 0,
                                 detourA(), &slotA));

    ASSERT_EQ(fx.ledger.hooksOf("com.pulse.alpha").size(), 1u);

    auto results = registry.removeOwner("com.pulse.alpha");
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().outcome, ChainOpOutcome::RemovedLastInstall);

    // L'attribuzione è rilasciata e l'insieme installato è vuoto (Req 6.4, 6.5).
    EXPECT_TRUE(fx.ledger.hooksOf("com.pulse.alpha").empty());
    EXPECT_EQ(fx.ledger.installedCount(), 0u);
}

// --- Diagnostica osservabile del rilascio dell'attribuzione -----------------
TEST(HookChainRegistryLedger, EmitsReleaseDiagnostic) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x9000;

    void* slotA = nullptr;
    void* slotB = nullptr;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", "MenuLayer::init", 900, 0,
                                 detourA(), &slotA));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.beta", "MenuLayer::init", 500, 1,
                                 detourB(), &slotB));

    fx.events.clear();
    registry.removeOwner("com.pulse.alpha");

    bool released = false;
    for (const auto& e : fx.events) {
        if (e.find("rilasciata attribuzione ledger") != std::string::npos &&
            e.find("com.pulse.alpha") != std::string::npos) {
            released = true;
        }
    }
    EXPECT_TRUE(released);
}

}  // namespace
