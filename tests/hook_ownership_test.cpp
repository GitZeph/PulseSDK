// tests/hook_ownership_test.cpp — unit test del HookOwnershipLedger
// (External Mod Loading, task 5.5, Requisiti 5.8, 9.6).
//
// Copre:
//   * finestre di epoca: openEpoch() = count() corrente; closeEpoch(owner,
//     start) attribuisce e restituisce gli indici in [start, count());
//   * seeding della demo: seedBuiltinDemo() attribuisce [0, count()) al Mod_Id
//     riservato kBuiltinDemoModId, e una epoca successiva di una mod esterna
//     non si sovrappone;
//   * attribuzione/interrogazione/rimozione degli hook installati per owner
//     (Req 5.8, 7.3) e unione globale = mod attive (Req 9.6);
//   * casi limite: start >= count() → finestra vuota; ownerOfIndex su indice
//     non attribuito → stringa vuota.
#include "lifecycle/hook_ownership.hpp"

#include <gtest/gtest.h>

#include <pulse/hooks.hpp>

#include <string_view>
#include <vector>

namespace {

using pulse::lifecycle::HookOwnershipLedger;
using pulse::lifecycle::ModId;
using pulse::lifecycle::OwnedHook;
using pulse::lifecycle::kBuiltinDemoModId;

// Azzera il registro globale dello SDK così ogni test parte da uno stato noto:
// il registro è un singleton di processo condiviso tra le unità di traduzione.
void resetRegistry() { pulse::hooks::registry().clear(); }

// Registra `n` hook fittizi nel registro globale (detour/trampoline non usati
// dal ledger). Restituisce il count risultante.
std::size_t registerDummyHooks(std::size_t n, std::string_view symbol) {
    for (std::size_t i = 0; i < n; ++i) {
        pulse::hooks::register_hook(symbol, /*detour=*/nullptr,
                                    /*trampoline=*/nullptr);
    }
    return pulse::hooks::count();
}

OwnedHook ownedHook(const ModId& owner, std::string symbol, std::uintptr_t target,
                    std::size_t index) {
    return OwnedHook{owner, std::move(symbol), target, index};
}

// --- finestre di epoca -----------------------------------------------------

TEST(HookOwnershipLedger, OpenEpochReflectsCurrentRegistryCount) {
    resetRegistry();
    HookOwnershipLedger ledger;
    EXPECT_EQ(ledger.openEpoch(), 0u);

    registerDummyHooks(3, "A::x");
    EXPECT_EQ(ledger.openEpoch(), 3u);
}

TEST(HookOwnershipLedger, CloseEpochAttributesWindowAndReturnsIndices) {
    resetRegistry();
    HookOwnershipLedger ledger;

    // Una mod esterna apre l'epoca su registro vuoto, poi i suoi PULSE_HOOK si
    // registrano (qui simulati): la finestra [0, 3) è del mod.
    const std::size_t start = ledger.openEpoch();
    registerDummyHooks(3, "modA::sym");

    const std::vector<std::size_t> idx = ledger.closeEpoch("modA", start);
    EXPECT_EQ(idx, (std::vector<std::size_t>{0, 1, 2}));
    EXPECT_EQ(ledger.ownerOfIndex(0), "modA");
    EXPECT_EQ(ledger.ownerOfIndex(2), "modA");
}

TEST(HookOwnershipLedger, NonOverlappingWindowsForSequentialMods) {
    resetRegistry();
    HookOwnershipLedger ledger;

    const std::size_t startA = ledger.openEpoch();
    registerDummyHooks(2, "modA::sym");
    ledger.closeEpoch("modA", startA);

    const std::size_t startB = ledger.openEpoch();  // = 2
    registerDummyHooks(3, "modB::sym");
    const std::vector<std::size_t> idxB = ledger.closeEpoch("modB", startB);

    EXPECT_EQ(idxB, (std::vector<std::size_t>{2, 3, 4}));
    EXPECT_EQ(ledger.ownerOfIndex(1), "modA");
    EXPECT_EQ(ledger.ownerOfIndex(2), "modB");
    EXPECT_EQ(ledger.ownerOfIndex(4), "modB");
}

TEST(HookOwnershipLedger, CloseEpochWithStartAtOrBeyondCountIsEmpty) {
    resetRegistry();
    HookOwnershipLedger ledger;
    registerDummyHooks(2, "modA::sym");

    EXPECT_TRUE(ledger.closeEpoch("modA", 2).empty());  // start == count()
    EXPECT_TRUE(ledger.closeEpoch("modA", 5).empty());  // start > count()
}

// --- seeding della demo ----------------------------------------------------

TEST(HookOwnershipLedger, SeedBuiltinDemoAttributesPreexistingRegistrations) {
    resetRegistry();
    // La demo interna registra il proprio hook allo static-init PRIMA di
    // qualunque epoca esterna.
    registerDummyHooks(2, "MenuLayer_init");

    HookOwnershipLedger ledger;
    const std::vector<std::size_t> demo = ledger.seedBuiltinDemo();
    EXPECT_EQ(demo, (std::vector<std::size_t>{0, 1}));
    EXPECT_EQ(ledger.ownerOfIndex(0), ModId(kBuiltinDemoModId));
    EXPECT_EQ(ledger.ownerOfIndex(1), ModId(kBuiltinDemoModId));

    // Una mod esterna caricata dopo non si sovrappone alla finestra della demo.
    const std::size_t start = ledger.openEpoch();  // = 2
    registerDummyHooks(1, "modA::sym");
    const std::vector<std::size_t> idx = ledger.closeEpoch("modA", start);
    EXPECT_EQ(idx, (std::vector<std::size_t>{2}));
    EXPECT_EQ(ledger.ownerOfIndex(2), "modA");
    EXPECT_EQ(ledger.ownerOfIndex(0), ModId(kBuiltinDemoModId));
}

// --- attribuzione / interrogazione / rimozione degli hook installati -------

TEST(HookOwnershipLedger, AttributeAndQueryHooksByOwner) {
    resetRegistry();
    HookOwnershipLedger ledger;

    ledger.attribute(ownedHook("modA", "A::f", 0x1000, 0));
    ledger.attribute(ownedHook("modB", "B::g", 0x2000, 1));
    ledger.attribute(ownedHook("modA", "A::h", 0x3000, 2));

    const std::vector<OwnedHook> a = ledger.hooksOf("modA");
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(a[0].symbol, "A::f");
    EXPECT_EQ(a[1].symbol, "A::h");

    EXPECT_EQ(ledger.hooksOf("modB").size(), 1u);
    EXPECT_TRUE(ledger.hooksOf("absent").empty());
    EXPECT_EQ(ledger.installedCount(), 3u);
}

TEST(HookOwnershipLedger, ReleaseRemovesOnlyOwnersInstalledHooks) {
    resetRegistry();
    HookOwnershipLedger ledger;
    ledger.attribute(ownedHook("modA", "A::f", 0x1000, 0));
    ledger.attribute(ownedHook("modB", "B::g", 0x2000, 1));
    ledger.attribute(ownedHook("modA", "A::h", 0x3000, 2));

    ledger.release("modA");

    EXPECT_TRUE(ledger.hooksOf("modA").empty());          // Req 7.3 / 8.6
    ASSERT_EQ(ledger.hooksOf("modB").size(), 1u);          // altri intatti
    EXPECT_EQ(ledger.hooksOf("modB")[0].symbol, "B::g");
}

TEST(HookOwnershipLedger, AllInstalledIsUnionOfActiveOwners) {
    resetRegistry();
    HookOwnershipLedger ledger;
    ledger.attribute(ownedHook("modA", "A::f", 0x1000, 0));
    ledger.attribute(ownedHook("modB", "B::g", 0x2000, 1));

    EXPECT_EQ(ledger.allInstalled().size(), 2u);  // Req 9.6

    ledger.release("modA");
    const std::vector<OwnedHook> all = ledger.allInstalled();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].owner, "modB");
}

TEST(HookOwnershipLedger, OwnerOfUnattributedIndexIsEmpty) {
    resetRegistry();
    HookOwnershipLedger ledger;
    EXPECT_TRUE(ledger.ownerOfIndex(0).empty());
    EXPECT_TRUE(ledger.ownerOfIndex(42).empty());
}

}  // namespace
