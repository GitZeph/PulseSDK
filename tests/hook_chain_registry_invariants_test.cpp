// tests/hook_chain_registry_invariants_test.cpp — unit test degli invarianti di
// catena di HookChainRegistry (task 3.4, Requisiti 1.4, 4.5, 4.6, 5.5).
//
// Consolida e verifica gli invarianti che la Registry mantiene attraverso
// sequenze di inserimento e su più Target_Address distinti:
//   * installed == (chain non vuota) e #DobbyHook su address == (installed?1:0)
//     — una sola Underlying_Installation per target con almeno un anello, e mai
//     una seconda DobbyHook sullo stesso indirizzo (Req 1.4, 5.5);
//   * a ciascun Target_Address distinto è associato un ChainSlot distinto, senza
//     condividere alcun Hook_Link tra catene di target diversi (Req 1.5 / 1.1) —
//     catene disgiunte;
//   * l'aggiunta di un anello a una catena con install attiva NON sovrascrive i
//     byte del prologo del Target_Address (Req 4.5) né muta i record persistiti
//     nel Rollback_Store (Req 4.6), attraverso sequenze di più inserimenti e su
//     più target contemporaneamente.
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

// Pool di detour distinti: nel caso host i detour sono semplici puntatori
// opachi la cui identità conta per verificare il relinking e la disgiunzione
// delle catene. Ogni Hook_Link ottiene il proprio tag indirizzabile.
int gDetourTags[32] = {};
void* detourFor(int i) { return static_cast<void*>(&gDetourTags[i]); }

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
                           "pulse_chain_invariants_test.bin"};
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

// Inserisce un anello con il proprio slot di trampolino e ritorna l'esito.
ChainOpResult insert(HookChainRegistry& reg, std::uintptr_t target,
                     const std::string& owner, int priority, uint64_t loadOrder,
                     void* detour, void** slot) {
    LinkSpec link;
    link.owner = owner;
    link.symbol = "MenuLayer::init";
    link.priority = priority;
    link.loadOrder = loadOrder;
    link.detour = detour;
    link.slot = slot;
    return reg.insertLink(target, resolvedBinding("MenuLayer::init", target), link);
}

// ---------------------------------------------------------------------------
// Invariante: installed == (chain non vuota) e #DobbyHook == (installed?1:0).
//
// Attraverso una sequenza di inserimenti su un solo target, la Registry mantiene
// esattamente UNA Underlying_Installation finché la catena è non vuota, e il
// backend riceve esattamente UN tentativo di install (mai una seconda DobbyHook
// sullo stesso indirizzo) (Req 1.4, 5.5).
// ---------------------------------------------------------------------------
TEST(HookChainRegistryInvariants, SingleInstallAcrossInsertSequence) {
    Fixture fx;
    auto reg = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x4000;

    // Prima dell'inserimento: nessuna install, catena vuota.
    EXPECT_FALSE(reg.hasInstall(kTarget));
    EXPECT_EQ(reg.chainSize(kTarget), 0u);
    EXPECT_EQ(fx.backend.installedCount(), 0u);

    constexpr int kCount = 6;
    void* slots[kCount] = {};
    const int priorities[kCount] = {500, 900, 100, 700, 300, 500};

    for (int i = 0; i < kCount; ++i) {
        insert(reg, kTarget, "mod" + std::to_string(i), priorities[i],
               static_cast<uint64_t>(i), detourFor(i), &slots[i]);

        // Dopo OGNI inserimento: install attiva sse catena non vuota, ed esiste
        // esattamente UNA Underlying_Installation per il target.
        EXPECT_TRUE(reg.hasInstall(kTarget));
        EXPECT_EQ(reg.installCount(), 1u);
        EXPECT_EQ(reg.chainSize(kTarget), static_cast<std::size_t>(i + 1));

        // #DobbyHook sull'indirizzo == 1 (una sola install nel backend), e il
        // backend ha ricevuto un solo tentativo: nessuna seconda DobbyHook.
        EXPECT_EQ(fx.backend.installedCount(), 1u);
        EXPECT_EQ(fx.backend.installAttempts(), 1u);
        EXPECT_TRUE(fx.backend.isInstalled(kTarget));
    }
}

// ---------------------------------------------------------------------------
// Invariante: una catena per target, catene disgiunte tra target distinti.
//
// Tre Target_Address distinti, ciascuno con più anelli: ogni anello appartiene
// alla SOLA catena del proprio target; le catene non condividono alcun
// Hook_Link; ogni target ha la propria singola install (Req 1.5).
// ---------------------------------------------------------------------------
TEST(HookChainRegistryInvariants, DistinctTargetsHaveDisjointChains) {
    Fixture fx;
    auto reg = fx.makeRegistry();

    const std::uintptr_t targets[3] = {0x5000, 0x6000, 0x7000};
    void* slots[3][3] = {};
    int detourIdx = 0;

    // Popola ogni target con 3 anelli di priorità diverse.
    for (int t = 0; t < 3; ++t) {
        for (int k = 0; k < 3; ++k) {
            insert(reg, targets[t], "mod_t" + std::to_string(t) + "_k" + std::to_string(k),
                   100 * (k + 1), static_cast<uint64_t>(k), detourFor(detourIdx++),
                   &slots[t][k]);
        }
    }

    // Una install per target distinto: 3 Underlying_Installation totali.
    EXPECT_EQ(reg.installCount(), 3u);
    EXPECT_EQ(fx.backend.installedCount(), 3u);
    EXPECT_EQ(fx.backend.installAttempts(), 3u);  // una sola DobbyHook per target

    // Ogni catena contiene esattamente i propri 3 anelli e solo quelli: i Mod_Id
    // del Chain_Order di un target non compaiono in nessun altro target.
    for (int t = 0; t < 3; ++t) {
        EXPECT_EQ(reg.chainSize(targets[t]), 3u);
        const auto order = reg.chainOrder(targets[t]);
        ASSERT_EQ(order.size(), 3u);
        for (const auto& owner : order) {
            const std::string expectedPrefix = "mod_t" + std::to_string(t) + "_";
            EXPECT_EQ(owner.rfind(expectedPrefix, 0), 0u)
                << "owner '" << owner << "' nel target " << t << " non gli appartiene";
        }
    }

    // I detour (identità degli anelli) non sono condivisi tra catene: l'insieme
    // dei currentHead dei tre target è fatto di puntatori distinti.
    void* heads[3] = {reg.currentHead(targets[0]), reg.currentHead(targets[1]),
                      reg.currentHead(targets[2])};
    EXPECT_NE(heads[0], heads[1]);
    EXPECT_NE(heads[1], heads[2]);
    EXPECT_NE(heads[0], heads[2]);
}

// ---------------------------------------------------------------------------
// Invariante: aggiungere anelli a una catena con install attiva NON tocca i
// byte del prologo (Req 4.5) né muta i record del Rollback_Store (Req 4.6).
//
// Verificato su una sequenza di più inserimenti su un solo target.
// ---------------------------------------------------------------------------
TEST(HookChainRegistryInvariants, SubsequentInsertsLeavePrologueAndRollbackIntact) {
    Fixture fx;
    auto reg = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0x8000;

    void* slot0 = nullptr;
    insert(reg, kTarget, "mod0", 500, 0, detourFor(0), &slot0);

    // Snapshot dei byte "live" del prologo e dei record di rollback dopo 0→1.
    const auto liveAfterInstall = fx.backend.liveBytes(kTarget);
    const auto recordsAfterInstall = fx.rollback.records();
    ASSERT_EQ(recordsAfterInstall.size(), 1u);

    // Aggiunge 5 anelli successivi (varie priorità: testa, mezzo, coda).
    const int priorities[5] = {900, 200, 600, 100, 700};
    void* slots[5] = {};
    for (int i = 0; i < 5; ++i) {
        insert(reg, kTarget, "mod" + std::to_string(i + 1), priorities[i],
               static_cast<uint64_t>(i + 1), detourFor(i + 1), &slots[i]);

        // Dopo OGNI inserimento successivo: prologo invariato (Req 4.5)…
        EXPECT_EQ(fx.backend.liveBytes(kTarget), liveAfterInstall);
        // …e Rollback_Store invariato byte-per-byte (Req 4.6): stesso numero di
        // record e stesso contenuto del record originale.
        const auto records = fx.rollback.records();
        ASSERT_EQ(records.size(), 1u);
        EXPECT_EQ(records.front().address, recordsAfterInstall.front().address);
        EXPECT_EQ(records.front().owner, recordsAfterInstall.front().owner);
        EXPECT_EQ(records.front().originalBytes,
                  recordsAfterInstall.front().originalBytes);
    }
}

// ---------------------------------------------------------------------------
// Invariante combinato su più target: install attive multiple, ciascun
// Rollback_Store record persistito UNA sola volta al 0→1 del rispettivo target,
// e nessun inserimento successivo (su qualunque target) muta i prologhi o i
// record già persistiti (Req 4.5, 4.6, 5.5).
// ---------------------------------------------------------------------------
TEST(HookChainRegistryInvariants, MultiTargetPrologueAndRollbackStable) {
    Fixture fx;
    auto reg = fx.makeRegistry();

    const std::uintptr_t targets[2] = {0x9000, 0xA000};
    void* firstSlots[2] = {};
    int detourIdx = 0;

    // 0→1 su entrambi i target.
    insert(reg, targets[0], "a0", 500, 0, detourFor(detourIdx++), &firstSlots[0]);
    insert(reg, targets[1], "b0", 500, 0, detourFor(detourIdx++), &firstSlots[1]);

    // Un record di rollback per target (persistito una sola volta al 0→1).
    EXPECT_EQ(fx.rollback.size(), 2u);
    const auto liveT0 = fx.backend.liveBytes(targets[0]);
    const auto liveT1 = fx.backend.liveBytes(targets[1]);
    const auto recordsBaseline = fx.rollback.records();

    // Inserimenti successivi interlacciati sui due target.
    void* slots[6] = {};
    const std::uintptr_t seq[6] = {targets[0], targets[1], targets[0],
                                   targets[1], targets[0], targets[1]};
    const int prios[6] = {900, 100, 300, 700, 600, 400};
    for (int i = 0; i < 6; ++i) {
        insert(reg, seq[i], "x" + std::to_string(i), prios[i],
               static_cast<uint64_t>(i + 1), detourFor(detourIdx++), &slots[i]);
    }

    // Nessun nuovo record di rollback (resta uno per target, Req 4.6)…
    EXPECT_EQ(fx.rollback.size(), 2u);
    EXPECT_EQ(fx.rollback.records().size(), recordsBaseline.size());
    // …i byte del prologo di entrambi i target restano quelli del 0→1 (Req 4.5)…
    EXPECT_EQ(fx.backend.liveBytes(targets[0]), liveT0);
    EXPECT_EQ(fx.backend.liveBytes(targets[1]), liveT1);

    // …e ciascun target mantiene la sua singola install (Req 5.5).
    EXPECT_EQ(reg.installCount(), 2u);
    EXPECT_EQ(fx.backend.installAttempts(), 2u);  // una DobbyHook per target
    EXPECT_EQ(reg.chainSize(targets[0]), 4u);
    EXPECT_EQ(reg.chainSize(targets[1]), 4u);
}

// ---------------------------------------------------------------------------
// Invariante di cablaggio sotto inserimenti in posizioni arbitrarie: dopo una
// sequenza che inserisce testa/mezzo/coda, il currentHead punta al detour del
// Chain_Head, ogni slot non-coda punta al detour del successore e lo slot della
// coda al Real_Trampoline (Req 4.2, consolidato dalla guardia slotInvariants).
// ---------------------------------------------------------------------------
TEST(HookChainRegistryInvariants, RelinkingChainStaysWellFormed) {
    Fixture fx;
    auto reg = fx.makeRegistry();
    constexpr std::uintptr_t kTarget = 0xB000;

    constexpr int kCount = 5;
    void* slots[kCount] = {};
    const int priorities[kCount] = {500, 900, 100, 950, 250};
    for (int i = 0; i < kCount; ++i) {
        insert(reg, kTarget, "mod" + std::to_string(i), priorities[i],
               static_cast<uint64_t>(i), detourFor(i), &slots[i]);
    }

    // Ricostruisci l'ordine atteso dei detour dal Chain_Order osservabile.
    const auto order = reg.chainOrder(kTarget);
    ASSERT_EQ(order.size(), static_cast<std::size_t>(kCount));

    // Mappa Mod_Id → (detour, slot) per la verifica del cablaggio.
    auto detourOfOwner = [&](const std::string& owner) -> void* {
        const int idx = std::stoi(owner.substr(3));  // "modN" → N
        return detourFor(idx);
    };
    auto slotOfOwner = [&](const std::string& owner) -> void* {
        const int idx = std::stoi(owner.substr(3));
        return slots[idx];
    };

    // currentHead == detour del Chain_Head.
    EXPECT_EQ(reg.currentHead(kTarget), detourOfOwner(order.front()));

    // Ogni anello non-coda inoltra al successore; la coda al Real_Trampoline.
    for (std::size_t i = 0; i + 1 < order.size(); ++i) {
        EXPECT_EQ(slotOfOwner(order[i]), detourOfOwner(order[i + 1]))
            << "slot dell'anello in posizione " << i << " non inoltra al successore";
    }
    EXPECT_EQ(slotOfOwner(order.back()), realTrampoline(kTarget));
}

}  // namespace
