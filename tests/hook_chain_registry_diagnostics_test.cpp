// tests/hook_chain_registry_diagnostics_test.cpp — unit test della diagnostica
// osservabile e della vista del Chain_Order di HookChainRegistry (task 7.3,
// Requisiti 11.1, 11.2, 11.3, 11.4).
//
// Consolida, per catene a più anelli, le garanzie di osservabilità di OGNI
// operazione di catena:
//   * aggiunta di un anello → evento con Mod_Id + Target_Address + posizione nel
//     Chain_Order (Req 11.1);
//   * rimozione di un anello → evento con Mod_Id + Target_Address (Req 11.2);
//   * creazione/rimozione dell'unica Underlying_Installation → evento con
//     Target_Address + operazione (install/remove, Req 11.3);
//   * `chainOrder(target)` espone il Chain_Order corrente come `ChainOrderView`
//     (sequenza di Mod_Id dal Chain_Head alla coda, Req 11.4).
//
// Usa il FakeBackend in-memory (loader/hooking/) come Hook_Engine host-testabile.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "bindings/bindings.hpp"
#include "hooking/fake_backend.hpp"
#include "hooking/hook_chain_registry.hpp"
#include "hooking/rollback_store.hpp"
#include "lifecycle/hook_ownership.hpp"

namespace {

using pulse::hooking::ChainOpOutcome;
using pulse::hooking::ChainOrderView;
using pulse::hooking::FakeBackend;
using pulse::hooking::HookChainRegistry;
using pulse::hooking::LinkSpec;
using pulse::hooking::RollbackStore;
using pulse::lifecycle::HookOwnershipLedger;
using pulse::loader::bindings::FunctionBinding;

// Detour opachi distinti per ogni anello (l'identità conta per la vista).
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

// Rende il Target_Address come la diagnostica della Registry (`0x` + hex).
std::string formatAddress(std::uintptr_t address) {
    std::ostringstream os;
    os << "0x" << std::hex << address;
    return os.str();
}

struct Fixture {
    FakeBackend backend;
    RollbackStore rollback{std::filesystem::temp_directory_path() /
                           "pulse_chain_diagnostics_test.bin"};
    HookOwnershipLedger ledger;
    std::vector<std::string> events;

    HookChainRegistry makeRegistry() {
        return HookChainRegistry{backend, rollback, ledger,
                                 [this](std::string_view m) {
                                     events.emplace_back(m);
                                 }};
    }

    // Vero sse almeno un evento contiene TUTTI i frammenti richiesti.
    bool eventContainsAll(std::initializer_list<std::string_view> fragments) const {
        for (const auto& e : events) {
            bool all = true;
            for (const auto& f : fragments) {
                if (e.find(f) == std::string::npos) {
                    all = false;
                    break;
                }
            }
            if (all) {
                return true;
            }
        }
        return false;
    }

    ~Fixture() { std::error_code ec; std::filesystem::remove(rollback.path(), ec); }
};

LinkSpec makeLink(std::string owner, int priority, std::uint64_t loadOrder,
                  void* detour, void** slot) {
    LinkSpec l;
    l.owner = std::move(owner);
    l.symbol = "MenuLayer::init";
    l.priority = priority;
    l.loadOrder = loadOrder;
    l.detour = detour;
    l.slot = slot;
    return l;
}

// --- Req 11.1: aggiunta → Mod_Id + Target_Address + posizione --------------
TEST(HookChainRegistryDiagnostics, AddEmitsModIdTargetAndPosition) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0xA000;
    const std::string addr = formatAddress(kTarget);

    void* slotA = nullptr;
    void* slotB = nullptr;
    void* slotC = nullptr;

    // Chain_Head (posizione 0) — primo anello, transizione 0→1.
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 900, 0, detourA(), &slotA));
    // Coda (posizione 1).
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.beta", 500, 1, detourB(), &slotB));
    // In mezzo (posizione 1, spinge beta a posizione 2).
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.gamma", 700, 2, detourC(), &slotC));

    // alpha è il Chain_Head: evento con Mod_Id + Target_Address + posizione 0.
    EXPECT_TRUE(fx.eventContainsAll({"com.pulse.alpha", addr, "posizione 0"}));
    // beta inserito in coda: posizione 1 al momento dell'inserimento.
    EXPECT_TRUE(fx.eventContainsAll({"com.pulse.beta", addr, "posizione 1"}));
    // gamma inserito in mezzo (dopo alpha): posizione 1 al momento dell'inserimento.
    EXPECT_TRUE(fx.eventContainsAll({"com.pulse.gamma", addr, "posizione 1"}));
}

// --- Req 11.3: creazione dell'unica Underlying_Installation ----------------
TEST(HookChainRegistryDiagnostics, InstallCreateEmitsTargetAndOperation) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0xB000;
    const std::string addr = formatAddress(kTarget);

    void* slotA = nullptr;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 500, 0, detourA(), &slotA));

    // Solo il PRIMO anello (0→1) crea l'install: evento con Target_Address +
    // operazione (install).
    EXPECT_TRUE(fx.eventContainsAll({"Underlying_Installation", addr, "install"}));

    // L'aggiunta di un anello successivo NON emette un secondo evento di creazione
    // install (l'unica Underlying_Installation è invariata).
    void* slotB = nullptr;
    fx.events.clear();
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.beta", 400, 1, detourB(), &slotB));
    for (const auto& e : fx.events) {
        EXPECT_EQ(e.find("creata Underlying_Installation"), std::string::npos);
    }
}

// --- Req 11.2: rimozione non-ultima → Mod_Id + Target_Address --------------
TEST(HookChainRegistryDiagnostics, RemoveKeepInstallEmitsModIdAndTarget) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0xC000;
    const std::string addr = formatAddress(kTarget);

    void* slotA = nullptr;
    void* slotB = nullptr;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 900, 0, detourA(), &slotA));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.beta", 400, 1, detourB(), &slotB));

    fx.events.clear();
    registry.removeOwner("com.pulse.beta");

    // Evento di rimozione con Mod_Id + Target_Address; l'install è mantenuta
    // (nessun evento di rimozione install).
    EXPECT_TRUE(fx.eventContainsAll({"com.pulse.beta", addr}));
    for (const auto& e : fx.events) {
        EXPECT_EQ(e.find("rimossa Underlying_Installation"), std::string::npos);
    }
}

// --- Req 11.2 + 11.3: rimozione ultimo anello → anello + remove install ----
TEST(HookChainRegistryDiagnostics, RemoveLastEmitsModIdTargetAndUninstall) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0xD000;
    const std::string addr = formatAddress(kTarget);

    void* slotA = nullptr;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 500, 0, detourA(), &slotA));

    fx.events.clear();
    registry.removeOwner("com.pulse.alpha");

    // Rimozione anello (Req 11.2): Mod_Id + Target_Address.
    EXPECT_TRUE(fx.eventContainsAll({"com.pulse.alpha", addr}));
    // Rimozione dell'unica Underlying_Installation (Req 11.3): Target_Address +
    // operazione (remove).
    EXPECT_TRUE(fx.eventContainsAll({"Underlying_Installation", addr, "remove"}));
}

// --- Req 11.4: chainOrder() espone il Chain_Order come ChainOrderView ------
TEST(HookChainRegistryDiagnostics, ChainOrderViewIsHeadToTail) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0xE000;

    void* slotA = nullptr;
    void* slotB = nullptr;
    void* slotC = nullptr;

    // Inseriti in ordine di abilitazione differente dal Chain_Order atteso, così
    // la vista riflette priority DESC / loadOrder ASC, NON l'ordine d'inserimento.
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.beta", 500, 0, detourB(), &slotB));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 900, 1, detourA(), &slotA));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.gamma", 100, 2, detourC(), &slotC));

    // ChainOrderView: dal Chain_Head (priorità più alta) alla coda (più bassa).
    const ChainOrderView order = registry.chainOrder(kTarget);
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], "com.pulse.alpha");   // priorità 900 → Chain_Head
    EXPECT_EQ(order[1], "com.pulse.beta");    // priorità 500 → in mezzo
    EXPECT_EQ(order[2], "com.pulse.gamma");   // priorità 100 → coda

    // La vista di un target senza catena è vuota.
    EXPECT_TRUE(registry.chainOrder(0x1).empty());
}

// --- Req 11.4: ChainOpResult.chainOrder coincide con la vista corrente ------
TEST(HookChainRegistryDiagnostics, OpResultChainOrderMatchesView) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0xF000;

    void* slotA = nullptr;
    void* slotB = nullptr;

    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 900, 0, detourA(), &slotA));
    const auto r = registry.insertLink(
        kTarget, resolvedBinding("MenuLayer::init", kTarget),
        makeLink("com.pulse.beta", 400, 1, detourB(), &slotB));

    // Il Chain_Order riportato nell'esito dell'operazione è la stessa sequenza
    // head→tail osservabile via chainOrder() (Req 11.4).
    EXPECT_EQ(r.chainOrder, registry.chainOrder(kTarget));
    ASSERT_EQ(r.chainOrder.size(), 2u);
    EXPECT_EQ(r.chainOrder.front(), "com.pulse.alpha");
    EXPECT_EQ(r.chainOrder.back(), "com.pulse.beta");
}

}  // namespace
