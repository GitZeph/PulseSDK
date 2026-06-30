// tests/hook_chain_registry_dispatch_test.cpp — unit test dell'arnia di
// dispatch simulata del Hook_Chaining (task 3.5, Requisiti 3.1, 3.2, 3.3, 3.4,
// 3.5, 3.6, 3.8, 3.9, 4.4).
//
// Verifica il **veicolo di test host** (`ChainDispatchHarness`) che emula il
// percorso reale costruito dalla HookChainRegistry ri-cablando i Trampoline_Slot
// (task 3.1-3.4): invocazione → `currentHead` → detour della testa →
// `callOriginal` legge `*slot` → detour del successivo → … → ultimo →
// Real_Trampoline → Real_Original (simulato). L'arnia segue il **cablaggio
// effettivo dei puntatori** prodotto dalla Registry (currentHead + slot reali),
// così questi test verificano il relinking della Registry, non un modello a
// parte.
//
// Casi coperti:
//   * attraversamento nel Chain_Order, testa per prima (Req 3.1, 4.4);
//   * inoltro dei medesimi parametri al successivo e propagazione a monte del
//     medesimo valore di ritorno prodotto a valle (Req 3.2, 3.3);
//   * l'ultimo anello raggiunge il Real_Original via trampolino (Req 3.4);
//   * ciascun anello eseguito esattamente una volta, senza salti (Req 3.5, 3.6);
//   * equivalenza del caso a singolo anello con l'installazione diretta (Req 3.7);
//   * short-circuit: un anello che ritorna senza `callOriginal` non esegue i
//     successivi né il Real_Original, il chiamante riceve il valore dell'anello
//     interruttore e si registra un evento con Mod_Id + Target_Address (Req 3.8,
//     3.9).
//
// Usa il FakeBackend in-memory (loader/hooking/) come Hook_Engine host-testabile
// e la HookChainRegistry per produrre il cablaggio reale degli slot.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "bindings/bindings.hpp"
#include "hooking/chain_dispatch_harness.hpp"
#include "hooking/fake_backend.hpp"
#include "hooking/hook_chain_registry.hpp"
#include "hooking/rollback_store.hpp"
#include "lifecycle/hook_ownership.hpp"

namespace {

using pulse::hooking::ChainOpOutcome;
using pulse::hooking::FakeBackend;
using pulse::hooking::HookChainRegistry;
using pulse::hooking::LinkSpec;
using pulse::hooking::RollbackStore;
using pulse::hooking::test::ChainDispatchHarness;
using pulse::hooking::test::DispatchTrace;
using pulse::hooking::test::HarnessLink;
using pulse::lifecycle::HookOwnershipLedger;
using pulse::loader::bindings::FunctionBinding;

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
                           "pulse_chain_dispatch_test.bin"};
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

// Inserisce un anello nella Registry collegandolo al suo HarnessLink (detour =
// &link, slot = &link.nextSlot), così la Registry cabla `currentHead`/slot sui
// puntatori dell'arnia.
void insert(HookChainRegistry& registry, std::uintptr_t target, HarnessLink& link,
            const std::string& symbol, int priority, std::uint64_t loadOrder) {
    LinkSpec spec;
    spec.owner = link.owner;
    spec.symbol = symbol;
    spec.priority = priority;
    spec.loadOrder = loadOrder;
    spec.detour = link.detour();
    spec.slot = link.slot();
    registry.insertLink(target, resolvedBinding(symbol, target), spec);
}

// ===========================================================================
// Attraversamento nel Chain_Order con inoltro di parametri e ritorno (Req 3.1,
// 3.2, 3.3, 3.4, 3.6, 4.4) — Property 7.
// ===========================================================================
TEST(HookChainRegistryDispatch, TraversesChainForwardingParamsAndReturn) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x4000;
    const std::string kSymbol = "MenuLayer::init";

    // Registra l'argomento ricevuto da ogni anello (per verificare l'inoltro a
    // valle) e fa sì che ogni anello aggiunga una costante distinta al valore di
    // ritorno (per verificare la propagazione a monte).
    std::vector<int> argsSeen;
    int originalArg = -1;

    HarnessLink a;
    a.owner = "com.pulse.alpha";
    a.behavior = [&](int arg, const HarnessLink::CallOriginal& callOriginal) {
        argsSeen.push_back(arg);
        return callOriginal(arg + 1) + 10000;  // inoltra arg+1, +10000 a monte
    };

    HarnessLink b;
    b.owner = "com.pulse.beta";
    b.behavior = [&](int arg, const HarnessLink::CallOriginal& callOriginal) {
        argsSeen.push_back(arg);
        return callOriginal(arg + 2) + 20000;
    };

    HarnessLink c;
    c.owner = "com.pulse.gamma";
    c.behavior = [&](int arg, const HarnessLink::CallOriginal& callOriginal) {
        argsSeen.push_back(arg);
        return callOriginal(arg + 4) + 40000;
    };

    // priority DESC ⇒ A(900) testa, B(500), C(100) coda. Inseriti in ordine
    // arbitrario: la Registry determina il Chain_Order.
    insert(registry, kTarget, c, kSymbol, 100, 2);
    insert(registry, kTarget, a, kSymbol, 900, 0);
    insert(registry, kTarget, b, kSymbol, 500, 1);

    ASSERT_EQ(registry.chainSize(kTarget), 3u);
    ASSERT_EQ(registry.installCount(), 1u);  // una sola Underlying_Installation

    // Real_Original simulato: eco dell'argomento ricevuto dalla coda via
    // trampolino (Req 3.4).
    ChainDispatchHarness harness(
        kTarget, registry.realTrampoline(kTarget),
        [&](int arg) { originalArg = arg; return arg; });

    DispatchTrace trace = harness.dispatch(registry.currentHead(kTarget), 0);

    // Testa per prima, ordine = Chain_Order, ciascuno una volta senza salti
    // (Req 3.1, 3.6, 4.4).
    EXPECT_EQ(trace.executed,
              (std::vector<std::string>{"com.pulse.alpha", "com.pulse.beta",
                                        "com.pulse.gamma"}));
    EXPECT_EQ(trace.executed, registry.chainOrder(kTarget));

    // Inoltro dei parametri a valle (Req 3.2, 3.3): A riceve 0 → B riceve 1 → C
    // riceve 3 → Real_Original riceve 7.
    EXPECT_EQ(argsSeen, (std::vector<int>{0, 1, 3}));
    EXPECT_TRUE(trace.reachedOriginal);
    EXPECT_EQ(originalArg, 7);

    // Propagazione a monte del valore di ritorno (Req 3.3): original=7 →
    // C:7+40000 → B:+20000 → A:+10000 = 70000 + 7.
    EXPECT_EQ(trace.value, 7 + 40000 + 20000 + 10000);
    EXPECT_FALSE(trace.shortCircuited);
}

// ===========================================================================
// Equivalenza del caso a singolo anello con l'installazione diretta (Req 3.7).
// ===========================================================================
TEST(HookChainRegistryDispatch, SingleLinkReachesOriginalDirectly) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x5000;
    const std::string kSymbol = "PlayLayer::init";

    bool originalCalled = false;
    HarnessLink only;
    only.owner = "com.pulse.solo";
    only.behavior = [&](int arg, const HarnessLink::CallOriginal& callOriginal) {
        return callOriginal(arg) + 1;
    };
    insert(registry, kTarget, only, kSymbol, 500, 0);

    // currentHead == detour dell'unico anello; il suo slot == Real_Trampoline.
    EXPECT_EQ(registry.currentHead(kTarget), only.detour());
    EXPECT_EQ(only.nextSlot, registry.realTrampoline(kTarget));

    ChainDispatchHarness harness(
        kTarget, registry.realTrampoline(kTarget),
        [&](int arg) { originalCalled = true; return arg * 2; });

    DispatchTrace trace = harness.dispatch(registry.currentHead(kTarget), 21);

    EXPECT_EQ(trace.executed, (std::vector<std::string>{"com.pulse.solo"}));
    EXPECT_TRUE(trace.reachedOriginal);
    EXPECT_TRUE(originalCalled);
    EXPECT_EQ(trace.value, 21 * 2 + 1);  // l'unico anello invoca il Real_Original
    EXPECT_FALSE(trace.shortCircuited);
}

// ===========================================================================
// Short-circuit: un anello intermedio ritorna senza callOriginal (Req 3.8, 3.9)
// — Property 9.
// ===========================================================================
TEST(HookChainRegistryDispatch, ShortCircuitStopsChainAndReturnsBreakerValue) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x6000;
    const std::string kSymbol = "MenuLayer::init";

    bool cExecuted = false;
    bool originalCalled = false;

    // A: pass-through trasparente (restituisce esattamente il valore a valle),
    // così il valore dell'anello interruttore raggiunge il chiamante invariato
    // (Req 3.9).
    HarnessLink a;
    a.owner = "com.pulse.alpha";
    a.behavior = [&](int arg, const HarnessLink::CallOriginal& callOriginal) {
        return callOriginal(arg);
    };

    // B: interrompe la catena — ritorna SENZA invocare callOriginal (Req 3.8).
    HarnessLink b;
    b.owner = "com.pulse.beta";
    b.behavior = [&](int /*arg*/, const HarnessLink::CallOriginal& /*callOriginal*/) {
        return 777;  // valore dell'anello interruttore
    };

    // C: non deve mai essere eseguito.
    HarnessLink c;
    c.owner = "com.pulse.gamma";
    c.behavior = [&](int arg, const HarnessLink::CallOriginal& callOriginal) {
        cExecuted = true;
        return callOriginal(arg);
    };

    insert(registry, kTarget, a, kSymbol, 900, 0);
    insert(registry, kTarget, b, kSymbol, 500, 1);
    insert(registry, kTarget, c, kSymbol, 100, 2);

    ChainDispatchHarness harness(
        kTarget, registry.realTrampoline(kTarget),
        [&](int arg) { originalCalled = true; return arg; },
        [&](std::string_view m) { fx.events.emplace_back(m); });

    DispatchTrace trace = harness.dispatch(registry.currentHead(kTarget), 5);

    // Solo A e B sono eseguiti; C e il Real_Original NON sono raggiunti (Req 3.8).
    EXPECT_EQ(trace.executed,
              (std::vector<std::string>{"com.pulse.alpha", "com.pulse.beta"}));
    EXPECT_FALSE(cExecuted);
    EXPECT_FALSE(originalCalled);
    EXPECT_FALSE(trace.reachedOriginal);

    // Il chiamante riceve il valore prodotto dall'anello interruttore (Req 3.9).
    EXPECT_TRUE(trace.shortCircuited);
    EXPECT_EQ(trace.shortCircuitOwner, "com.pulse.beta");
    EXPECT_EQ(trace.value, 777);

    // Evento osservabile con Mod_Id + Target_Address (Req 3.8).
    bool diagnosed = false;
    for (const auto& e : fx.events) {
        if (e.find("com.pulse.beta") != std::string::npos &&
            e.find("0x6000") != std::string::npos) {
            diagnosed = true;
        }
    }
    EXPECT_TRUE(diagnosed);
}

// ===========================================================================
// Short-circuit alla testa: solo la testa è eseguita, nessun successivo né
// Real_Original (Req 3.8, 3.9).
// ===========================================================================
TEST(HookChainRegistryDispatch, HeadShortCircuitRunsHeadOnly) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x7000;
    const std::string kSymbol = "MenuLayer::init";

    bool tailExecuted = false;
    bool originalCalled = false;

    HarnessLink head;
    head.owner = "com.pulse.head";
    head.behavior = [&](int /*arg*/, const HarnessLink::CallOriginal& /*callOriginal*/) {
        return 99;  // interrompe subito
    };

    HarnessLink tail;
    tail.owner = "com.pulse.tail";
    tail.behavior = [&](int arg, const HarnessLink::CallOriginal& callOriginal) {
        tailExecuted = true;
        return callOriginal(arg);
    };

    insert(registry, kTarget, head, kSymbol, 900, 0);
    insert(registry, kTarget, tail, kSymbol, 100, 1);

    ChainDispatchHarness harness(
        kTarget, registry.realTrampoline(kTarget),
        [&](int arg) { originalCalled = true; return arg; });

    DispatchTrace trace = harness.dispatch(registry.currentHead(kTarget), 0);

    EXPECT_EQ(trace.executed, (std::vector<std::string>{"com.pulse.head"}));
    EXPECT_FALSE(tailExecuted);
    EXPECT_FALSE(originalCalled);
    EXPECT_TRUE(trace.shortCircuited);
    EXPECT_EQ(trace.value, 99);
}

// ===========================================================================
// Anello trasparente (nessuna behavior): cede automaticamente al successivo,
// l'intera catena è attraversata fino al Real_Original (Req 3.2, 3.6).
// ===========================================================================
TEST(HookChainRegistryDispatch, TransparentLinksTraverseToOriginal) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x8000;
    const std::string kSymbol = "MenuLayer::init";

    HarnessLink a;
    a.owner = "com.pulse.alpha";  // behavior vuota ⇒ trasparente
    HarnessLink b;
    b.owner = "com.pulse.beta";   // behavior vuota ⇒ trasparente

    insert(registry, kTarget, a, kSymbol, 900, 0);
    insert(registry, kTarget, b, kSymbol, 100, 1);

    ChainDispatchHarness harness(kTarget, registry.realTrampoline(kTarget),
                                 [&](int arg) { return arg + 1; });

    DispatchTrace trace = harness.dispatch(registry.currentHead(kTarget), 41);

    EXPECT_EQ(trace.executed,
              (std::vector<std::string>{"com.pulse.alpha", "com.pulse.beta"}));
    EXPECT_TRUE(trace.reachedOriginal);
    EXPECT_EQ(trace.value, 42);
    EXPECT_FALSE(trace.shortCircuited);
}

}  // namespace
