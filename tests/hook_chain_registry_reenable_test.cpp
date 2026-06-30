// tests/hook_chain_registry_reenable_test.cpp — unit test del round-trip di
// re-enable di HookChainRegistry (task 5.4, Requisito 5.7).
//
// Verifica che una mod precedentemente disabilitata e nuovamente abilitata su un
// Target_Address reinserisca l'anello nella posizione del Chain_Order determinata
// dalla sua Hook_Priority e dal suo load order, producendo LO STESSO Chain_Order
// (e lo stesso cablaggio degli slot/currentHead) del solo enable. La proprietà è
// verificata in due regimi:
//   (a) re-enable su una catena RIMASTA VIVA durante la disabilitazione (altri
//       anelli sono rimasti abilitati: keep-install, Req 5.1/5.5);
//   (b) re-enable su un Target_Address la cui catena è stata smontata a 0
//       (transizione 1→0: install rimossa + byte ripristinati, Req 5.3/5.4) e poi
//       ricostruita (transizione 0→1: nuova install, Req 1.6).
//
// In entrambi i casi il Chain_Order risultante dal round-trip enable → disable →
// enable deve coincidere con quello prodotto da un solo enable della medesima
// composizione, perché `insertLink` posiziona deterministicamente per
// (priority DESC, loadOrder ASC) e `removeOwner` non altera l'ordinamento dei
// restanti né le finestre di load order delle mod riabilitabili.
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

// Indirizzo del Real_Trampoline sintetico del FakeBackend per un target
// (deterministico per indirizzo: re-install sullo stesso target lo riproduce).
void* realTrampoline(std::uintptr_t target) {
    return reinterpret_cast<void*>(target ^ 0xA5A5A5A5ULL);
}

struct Fixture {
    FakeBackend backend;
    RollbackStore rollback{std::filesystem::temp_directory_path() /
                           "pulse_chain_reenable_test.bin"};
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

// Catena di riferimento prodotta dal SOLO enable della composizione [A,B,C], così
// il round-trip può essere confrontato con il "ground truth" di un singolo enable
// (Req 5.7). Calcolata in un fixture/registry separato per non interferire.
std::vector<std::string> singleEnableOrderABC() {
    Fixture ref;
    auto registry = ref.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x1000;
    void* sA = nullptr;
    void* sB = nullptr;
    void* sC = nullptr;
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 900, 0, detourA(), &sA));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.beta", 500, 1, detourB(), &sB));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.gamma", 100, 2, detourC(), &sC));
    return registry.chainOrder(kTarget);
}

// ===========================================================================
// (a) Re-enable su una catena RIMASTA VIVA (keep-install).
// ===========================================================================

// Re-enable di un anello in mezzo: disabilitato beta (catena resta viva con
// alpha+gamma), poi riabilitato con la stessa priority/loadOrder → il Chain_Order
// torna [A,B,C], identico al solo enable (Req 5.7).
TEST(HookChainRegistryReEnable, ReEnableMiddleOnLiveChainRestoresSameOrder) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x4000;

    void* slotA = nullptr;  // 900 (testa)
    void* slotB = nullptr;  // 500 (in mezzo)
    void* slotC = nullptr;  // 100 (coda)

    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 900, 0, detourA(), &slotA));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.beta", 500, 1, detourB(), &slotB));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.gamma", 100, 2, detourC(), &slotC));

    // Snapshot del cablaggio prodotto dal solo enable [A,B,C].
    const auto orderEnable = registry.chainOrder(kTarget);
    const void* headEnable = registry.currentHead(kTarget);
    const void* slotAEnable = slotA;
    const void* slotBEnable = slotB;
    const void* slotCEnable = slotC;
    ASSERT_EQ(orderEnable,
              (std::vector<std::string>{"com.pulse.alpha", "com.pulse.beta",
                                        "com.pulse.gamma"}));

    // Disabilita beta: la catena resta viva (alpha+gamma), install mantenuta.
    auto removed = registry.removeOwner("com.pulse.beta");
    ASSERT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed.front().outcome, ChainOpOutcome::RemovedKeepInstall);
    EXPECT_EQ(registry.chainOrder(kTarget),
              (std::vector<std::string>{"com.pulse.alpha", "com.pulse.gamma"}));
    EXPECT_EQ(registry.installCount(), 1u);  // catena viva: nessuna re-install

    // Riabilita beta con la STESSA priority/loadOrder: reinserimento via
    // insertSubsequent (nessuna chiamata al backend, catena già installata).
    auto reins = registry.insertLink(
        kTarget, resolvedBinding("MenuLayer::init", kTarget),
        makeLink("com.pulse.beta", 500, 1, detourB(), &slotB));
    EXPECT_EQ(reins.outcome, ChainOpOutcome::InsertedMiddle);

    // Il Chain_Order torna identico al solo enable (Req 5.7).
    EXPECT_EQ(registry.chainOrder(kTarget), orderEnable);
    // Il cablaggio (currentHead + slot dei vicini) è identico al solo enable.
    EXPECT_EQ(registry.currentHead(kTarget), headEnable);
    EXPECT_EQ(slotA, slotAEnable);  // A → B
    EXPECT_EQ(slotB, slotBEnable);  // B → C
    EXPECT_EQ(slotC, slotCEnable);  // C → Real_Trampoline
    EXPECT_EQ(slotA, detourB());
    EXPECT_EQ(slotB, detourC());
    EXPECT_EQ(slotC, realTrampoline(kTarget));

    // L'unica Underlying_Installation non è mai stata ricreata (catena viva).
    EXPECT_EQ(registry.installCount(), 1u);
    EXPECT_EQ(fx.backend.installedCount(), 1u);
    EXPECT_EQ(registry.chainSize(kTarget), 3u);
}

// Re-enable del Chain_Head su catena viva: disabilitato alpha (resta beta), poi
// riabilitato → alpha torna testa e il Chain_Order coincide col solo enable.
TEST(HookChainRegistryReEnable, ReEnableHeadOnLiveChainRestoresHead) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x5000;

    void* slotA = nullptr;  // 900 (testa)
    void* slotB = nullptr;  // 500 (coda)

    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 900, 0, detourA(), &slotA));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.beta", 500, 1, detourB(), &slotB));

    const auto orderEnable = registry.chainOrder(kTarget);

    // Disabilita la testa alpha: beta diventa testa+coda (keep-install).
    registry.removeOwner("com.pulse.alpha");
    EXPECT_EQ(registry.currentHead(kTarget), detourB());
    EXPECT_EQ(registry.installCount(), 1u);

    // Riabilita alpha con la stessa priority/loadOrder → torna Chain_Head.
    auto reins = registry.insertLink(
        kTarget, resolvedBinding("MenuLayer::init", kTarget),
        makeLink("com.pulse.alpha", 900, 0, detourA(), &slotA));
    EXPECT_EQ(reins.outcome, ChainOpOutcome::InsertedHead);

    EXPECT_EQ(registry.chainOrder(kTarget), orderEnable);
    EXPECT_EQ(registry.currentHead(kTarget), detourA());  // alpha di nuovo testa
    EXPECT_EQ(slotA, detourB());                          // A → B
    EXPECT_EQ(slotB, realTrampoline(kTarget));            // B → Real_Trampoline
    EXPECT_EQ(registry.installCount(), 1u);               // mai re-installata
}

// ===========================================================================
// (b) Re-enable su un Target_Address la cui catena è stata smontata a 0.
// ===========================================================================

// Disable dell'unico anello (transizione 1→0: install rimossa + byte
// ripristinati), poi re-enable (0→1: nuova install) → stesso Chain_Order del solo
// enable e cablaggio diretto equivalente (Req 5.7, con Req 5.3/5.4 + 1.6).
TEST(HookChainRegistryReEnable, ReEnableAfterTeardownToZeroRebuildsChain) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x6000;

    void* slotA = nullptr;

    auto first = registry.insertLink(
        kTarget, resolvedBinding("MenuLayer::init", kTarget),
        makeLink("com.pulse.alpha", 500, 0, detourA(), &slotA));
    EXPECT_EQ(first.outcome, ChainOpOutcome::CreatedInstall);

    const auto orderEnable = registry.chainOrder(kTarget);
    const auto originalBytes = fx.backend.snapshotOriginal(kTarget);
    ASSERT_TRUE(originalBytes.has_value());

    // Disabilita l'unico anello: transizione 1→0 (install rimossa, byte
    // ripristinati byte-esatto, ChainSlot dismesso).
    auto removed = registry.removeOwner("com.pulse.alpha");
    ASSERT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed.front().outcome, ChainOpOutcome::RemovedLastInstall);
    EXPECT_EQ(registry.installCount(), 0u);
    EXPECT_EQ(registry.chainSize(kTarget), 0u);
    EXPECT_FALSE(fx.backend.isInstalled(kTarget));
    // Byte del prologo ripristinati agli originali (Req 5.4).
    EXPECT_EQ(fx.backend.liveBytes(kTarget), originalBytes);

    // Riabilita la mod: la catena viene ricostruita da zero (0→1).
    void* slotAReenable = nullptr;
    auto reins = registry.insertLink(
        kTarget, resolvedBinding("MenuLayer::init", kTarget),
        makeLink("com.pulse.alpha", 500, 0, detourA(), &slotAReenable));
    EXPECT_EQ(reins.outcome, ChainOpOutcome::CreatedInstall);

    // Stesso Chain_Order del solo enable e cablaggio diretto equivalente.
    EXPECT_EQ(registry.chainOrder(kTarget), orderEnable);
    EXPECT_EQ(registry.currentHead(kTarget), detourA());
    EXPECT_EQ(slotAReenable, realTrampoline(kTarget));
    EXPECT_EQ(registry.installCount(), 1u);
    EXPECT_TRUE(fx.backend.isInstalled(kTarget));
}

// Smontaggio a 0 di una catena a più anelli e ricostruzione completa: disable di
// tutte le mod (catena torn-down a 0), poi re-enable di tutte → il Chain_Order
// ricostruito coincide con quello del solo enable della stessa composizione.
TEST(HookChainRegistryReEnable, ReEnableAllAfterTeardownMatchesSingleEnableOrder) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x7000;

    void* slotA = nullptr;
    void* slotB = nullptr;
    void* slotC = nullptr;

    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 900, 0, detourA(), &slotA));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.beta", 500, 1, detourB(), &slotB));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.gamma", 100, 2, detourC(), &slotC));

    const auto orderEnable = registry.chainOrder(kTarget);
    ASSERT_EQ(orderEnable, singleEnableOrderABC());

    // Disabilita tutte le mod: gamma e beta in keep-install, alpha chiude la
    // catena (1→0). Al termine la catena è smontata a 0.
    registry.removeOwner("com.pulse.gamma");
    registry.removeOwner("com.pulse.beta");
    registry.removeOwner("com.pulse.alpha");
    ASSERT_EQ(registry.installCount(), 0u);
    ASSERT_EQ(registry.chainSize(kTarget), 0u);

    // Riabilita tutte le mod (ordine di riabilitazione diverso da quello di enable
    // originale per esercitare anche la confluenza): il Chain_Order ricostruito è
    // determinato solo da (priority, loadOrder) e coincide col solo enable.
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.gamma", 100, 2, detourC(), &slotC));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 900, 0, detourA(), &slotA));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.beta", 500, 1, detourB(), &slotB));

    EXPECT_EQ(registry.chainOrder(kTarget), orderEnable);
    EXPECT_EQ(registry.currentHead(kTarget), detourA());
    EXPECT_EQ(slotA, detourB());                // A → B
    EXPECT_EQ(slotB, detourC());                // B → C
    EXPECT_EQ(slotC, realTrampoline(kTarget));  // C → Real_Trampoline
    EXPECT_EQ(registry.installCount(), 1u);
    EXPECT_EQ(registry.chainSize(kTarget), 3u);
}

// Idempotenza del round-trip: enable → disable → enable → disable → enable
// produce sempre lo stesso Chain_Order del primo enable (Req 5.7, ripetibile).
TEST(HookChainRegistryReEnable, RepeatedReEnableCyclesAreStable) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x8000;

    void* slotA = nullptr;
    void* slotB = nullptr;

    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.alpha", 900, 0, detourA(), &slotA));
    registry.insertLink(kTarget, resolvedBinding("MenuLayer::init", kTarget),
                        makeLink("com.pulse.beta", 500, 1, detourB(), &slotB));

    const auto orderEnable = registry.chainOrder(kTarget);

    for (int i = 0; i < 3; ++i) {
        registry.removeOwner("com.pulse.beta");  // keep-install
        EXPECT_EQ(registry.chainOrder(kTarget),
                  (std::vector<std::string>{"com.pulse.alpha"}));

        auto reins = registry.insertLink(
            kTarget, resolvedBinding("MenuLayer::init", kTarget),
            makeLink("com.pulse.beta", 500, 1, detourB(), &slotB));
        EXPECT_EQ(reins.outcome, ChainOpOutcome::InsertedTail);

        // Ad ogni ciclo il Chain_Order e il cablaggio tornano identici.
        EXPECT_EQ(registry.chainOrder(kTarget), orderEnable);
        EXPECT_EQ(registry.currentHead(kTarget), detourA());
        EXPECT_EQ(slotA, detourB());
        EXPECT_EQ(slotB, realTrampoline(kTarget));
        EXPECT_EQ(registry.installCount(), 1u);
    }
}

}  // namespace
