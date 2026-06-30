// tests/hook_chain_registry_concurrency_test.cpp — coerenza della dispatch
// rispetto alle mutazioni (concorrenza modellata) del Hook_Chaining (task 5.6,
// Requisiti 9.1, 9.2, 9.3).
//
// Verifica il modello di concorrenza descritto nel design ("6. Concorrenza
// (Requisito 9)"):
//   * ogni mutazione strutturale (insert/remove/relink/teardown) è serializzata
//     dallo `std::mutex` della HookChainRegistry, così nessuna dispatch osserva
//     uno stato della catena parzialmente modificato (Req 9.3);
//   * `currentHead` e i Trampoline_Slot (`void**`) sono scritti con store
//     atomico a semantica release e letti con load acquire, così una dispatch in
//     volo non osserva mai un puntatore *lacerato* (Req 9.1);
//   * una mutazione che avviene **durante** una dispatch in corso ha effetto a
//     partire da una **invocazione successiva**, senza alterare la dispatch già
//     in corso: la testa è letta una volta all'ingresso e gli slot inoltrano
//     lungo la catena esistente (Req 9.2).
//
// La concorrenza è **modellata** (non multi-thread reale): l'assunzione di
// serializzazione single-thread di early-load è documentata in `head_thunk.hpp`
// e `hook_ownership.hpp`. La dispatch in volo è modellata da
// `ChainDispatchHarness::dispatchCoherent`, che congela la catena esistente in
// uno snapshot all'ingresso (testa letta una volta, slot attraversati una volta)
// e una mutazione strutturale è iniettata DENTRO la behavior di un anello, cioè
// mentre la dispatch è in corso. Usa il FakeBackend in-memory + la
// HookChainRegistry per produrre il cablaggio reale degli slot.

#include <gtest/gtest.h>

#include <algorithm>
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
                           "pulse_chain_concurrency_test.bin"};
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

// Conta le occorrenze di un Mod_Id nella traccia di esecuzione (per verificare
// "al più una volta", Req 9.1).
std::size_t count(const std::vector<std::string>& v, const std::string& id) {
    return static_cast<std::size_t>(std::count(v.begin(), v.end(), id));
}

// ===========================================================================
// Inserimento di un nuovo Chain_Head DURANTE una dispatch in corso: la dispatch
// in volo non è alterata (esegue la catena che ha già caricato), la mutazione ha
// effetto dalla invocazione successiva (Req 9.1, 9.2).
// ===========================================================================
TEST(HookChainRegistryConcurrency, HeadInsertionDuringDispatchEffectiveNextInvocation) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0xA000;
    const std::string kSymbol = "MenuLayer::init";

    // Catena iniziale: B(500) testa, C(100) coda.
    HarnessLink b;
    HarnessLink c;
    HarnessLink a;  // nuovo head inserito a dispatch in corso (prio 900).
    b.owner = "com.pulse.beta";
    c.owner = "com.pulse.gamma";
    a.owner = "com.pulse.alpha";

    bool aExecuted = false;
    a.behavior = [&](int arg, const HarnessLink::CallOriginal& callOriginal) {
        aExecuted = true;
        return callOriginal(arg);
    };

    // La behavior della testa (B) inietta la mutazione MENTRE la dispatch è in
    // corso: inserisce A come nuovo Chain_Head, poi cede al successore.
    bool mutated = false;
    b.behavior = [&](int arg, const HarnessLink::CallOriginal& callOriginal) {
        if (!mutated) {
            mutated = true;
            insert(registry, kTarget, a, kSymbol, 900, 2);  // nuovo head
        }
        return callOriginal(arg);
    };
    // C trasparente: cede al Real_Original.

    insert(registry, kTarget, b, kSymbol, 500, 0);
    insert(registry, kTarget, c, kSymbol, 100, 1);

    ASSERT_EQ(registry.chainSize(kTarget), 2u);
    ASSERT_EQ(registry.installCount(), 1u);

    // Snapshot della testa letto UNA VOLTA all'ingresso (Req 9.2): la dispatch
    // in volo entra da qui.
    void* headAtEntry = registry.currentHead(kTarget);
    EXPECT_EQ(headAtEntry, b.detour());  // B è la testa all'inizio.

    ChainDispatchHarness harness(kTarget, registry.realTrampoline(kTarget),
                                 [](int arg) { return arg; });

    // Dispatch in corso: B esegue, inietta A, poi cede a C → Real_Original. La
    // mutazione NON altera la dispatch già in corso: A non è eseguito ora.
    DispatchTrace inFlight = harness.dispatchCoherent(headAtEntry, 7);

    EXPECT_EQ(inFlight.executed,
              (std::vector<std::string>{"com.pulse.beta", "com.pulse.gamma"}));
    EXPECT_FALSE(aExecuted);                  // mutazione non attiva sulla dispatch in corso
    EXPECT_TRUE(inFlight.reachedOriginal);
    EXPECT_FALSE(inFlight.shortCircuited);
    // Ciascun anello presente all'inizio eseguito al più una volta (Req 9.1).
    EXPECT_EQ(count(inFlight.executed, "com.pulse.beta"), 1u);
    EXPECT_EQ(count(inFlight.executed, "com.pulse.gamma"), 1u);

    // La mutazione ha avuto effetto sulla struttura: A è ora il Chain_Head.
    EXPECT_TRUE(mutated);
    EXPECT_EQ(registry.chainSize(kTarget), 3u);
    EXPECT_EQ(registry.installCount(), 1u);   // sempre una sola Underlying_Installation
    EXPECT_EQ(registry.chainOrder(kTarget),
              (std::vector<std::string>{"com.pulse.alpha", "com.pulse.beta",
                                        "com.pulse.gamma"}));
    EXPECT_EQ(registry.currentHead(kTarget), a.detour());

    // Invocazione SUCCESSIVA: legge la nuova testa all'ingresso → A esegue per
    // primo (Req 9.2).
    DispatchTrace next = harness.dispatchCoherent(registry.currentHead(kTarget), 7);
    EXPECT_EQ(next.executed,
              (std::vector<std::string>{"com.pulse.alpha", "com.pulse.beta",
                                        "com.pulse.gamma"}));
    EXPECT_TRUE(aExecuted);
    EXPECT_TRUE(next.reachedOriginal);
}

// ===========================================================================
// Rimozione di un anello DURANTE una dispatch in corso: la dispatch in volo
// vede una vista coerente e non salta alcun anello presente all'inizio (Req
// 9.1); la rimozione ha effetto dalla invocazione successiva (Req 9.2).
// ===========================================================================
TEST(HookChainRegistryConcurrency, RemovalDuringDispatchEffectiveNextInvocation) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0xB000;
    const std::string kSymbol = "MenuLayer::init";

    HarnessLink a;
    HarnessLink b;
    HarnessLink c;
    a.owner = "com.pulse.alpha";  // testa (900)
    b.owner = "com.pulse.beta";   // mezzo (500)
    c.owner = "com.pulse.gamma";  // coda (100)

    bool cExecuted = false;
    c.behavior = [&](int arg, const HarnessLink::CallOriginal& callOriginal) {
        cExecuted = true;
        return callOriginal(arg);
    };

    // La testa (A) rimuove la coda (C) MENTRE la dispatch è in corso, poi cede.
    bool mutated = false;
    a.behavior = [&](int arg, const HarnessLink::CallOriginal& callOriginal) {
        if (!mutated) {
            mutated = true;
            registry.removeOwner("com.pulse.gamma");  // rimuove la coda
        }
        return callOriginal(arg);
    };
    // B trasparente.

    insert(registry, kTarget, a, kSymbol, 900, 0);
    insert(registry, kTarget, b, kSymbol, 500, 1);
    insert(registry, kTarget, c, kSymbol, 100, 2);

    ASSERT_EQ(registry.chainSize(kTarget), 3u);

    void* headAtEntry = registry.currentHead(kTarget);

    ChainDispatchHarness harness(kTarget, registry.realTrampoline(kTarget),
                                 [](int arg) { return arg; });

    // Dispatch in corso: la catena congelata all'ingresso è [A, B, C]. Anche se
    // C viene rimosso durante l'esecuzione di A, la dispatch in volo NON lo salta
    // (era presente all'inizio): vista coerente (Req 9.1), non alterata (Req 9.2).
    DispatchTrace inFlight = harness.dispatchCoherent(headAtEntry, 3);

    EXPECT_EQ(inFlight.executed,
              (std::vector<std::string>{"com.pulse.alpha", "com.pulse.beta",
                                        "com.pulse.gamma"}));
    EXPECT_TRUE(cExecuted);                 // C presente all'inizio: non saltato (Req 9.1)
    EXPECT_TRUE(inFlight.reachedOriginal);
    EXPECT_EQ(count(inFlight.executed, "com.pulse.gamma"), 1u);

    // La rimozione ha avuto effetto sulla struttura: C non è più in catena.
    EXPECT_TRUE(mutated);
    EXPECT_EQ(registry.chainSize(kTarget), 2u);
    EXPECT_EQ(registry.installCount(), 1u);  // install mantenuta (restano anelli)
    EXPECT_EQ(registry.chainOrder(kTarget),
              (std::vector<std::string>{"com.pulse.alpha", "com.pulse.beta"}));

    // Invocazione SUCCESSIVA: C non è più eseguito (Req 9.2).
    cExecuted = false;
    DispatchTrace next = harness.dispatchCoherent(registry.currentHead(kTarget), 3);
    EXPECT_EQ(next.executed,
              (std::vector<std::string>{"com.pulse.alpha", "com.pulse.beta"}));
    EXPECT_FALSE(cExecuted);
    EXPECT_TRUE(next.reachedOriginal);
}

// ===========================================================================
// Nessuno stato parzialmente modificato osservabile (Req 9.3): in assenza di
// mutazioni concorrenti, la vista coerente (snapshot) e la vista live coincidono
// e rispettano gli invarianti di cablaggio; `currentHead`/slot riflettono sempre
// una catena interamente cablata dopo ogni mutazione serializzata.
// ===========================================================================
TEST(HookChainRegistryConcurrency, MutationsLeaveNoPartiallyModifiedStateObservable) {
    Fixture fx;
    auto registry = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0xC000;
    const std::string kSymbol = "MenuLayer::init";

    HarnessLink a;
    HarnessLink b;
    HarnessLink c;
    a.owner = "com.pulse.alpha";
    b.owner = "com.pulse.beta";
    c.owner = "com.pulse.gamma";

    // Inserimenti serializzati (ciascuno sotto il mutex strutturale della
    // Registry): dopo ognuno la catena è interamente cablata e coerente.
    insert(registry, kTarget, a, kSymbol, 900, 0);
    insert(registry, kTarget, c, kSymbol, 100, 1);
    insert(registry, kTarget, b, kSymbol, 500, 2);  // si inserisce nel mezzo

    // currentHead == detour del Chain_Head; ogni slot punta al successore, la
    // coda al Real_Trampoline. La vista live e quella coerente coincidono e
    // raggiungono il Real_Original passando per tutti e tre gli anelli, ciascuno
    // una sola volta (nessuno stato parziale, Req 9.3).
    ChainDispatchHarness harness(kTarget, registry.realTrampoline(kTarget),
                                 [](int arg) { return arg + 100; });

    DispatchTrace live = harness.dispatch(registry.currentHead(kTarget), 1);
    DispatchTrace coherent = harness.dispatchCoherent(registry.currentHead(kTarget), 1);

    const std::vector<std::string> expected{"com.pulse.alpha", "com.pulse.beta",
                                             "com.pulse.gamma"};
    EXPECT_EQ(live.executed, expected);
    EXPECT_EQ(coherent.executed, expected);
    EXPECT_EQ(live.value, coherent.value);
    EXPECT_TRUE(live.reachedOriginal);
    EXPECT_TRUE(coherent.reachedOriginal);
    EXPECT_EQ(registry.currentHead(kTarget), a.detour());
    // Ciascun anello eseguito esattamente una volta in entrambe le viste.
    for (const auto& id : expected) {
        EXPECT_EQ(count(live.executed, id), 1u);
        EXPECT_EQ(count(coherent.executed, id), 1u);
    }
}

}  // namespace
