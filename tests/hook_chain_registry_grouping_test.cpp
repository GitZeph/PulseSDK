// tests/hook_chain_registry_grouping_test.cpp — unit test del raggruppamento
// del registro SDK per Target_Address risolto (task 7.2, Requisito 10.1).
//
// Verifica `pulse::hooking::group_registry_by_target`:
//   * itera l'INTERO registro (più registrazioni dello stesso simbolo/target
//     coesistono — non solo la prima come `find`);
//   * raggruppa per Target_Address risolto;
//   * ordina ciascun gruppo nel Chain_Order (priority DESC con clamp,
//     loadOrder ASC = indice di inserzione nel registro);
//   * scarta le registrazioni non risolte (nessun anello);
//   * passa `detour`/`slot` (Trampoline_Slot pulse_original) della registrazione;
//   * mappa l'owner via la callback (es. ledger ownerOfIndex);
//   * è deterministico (gruppi nell'ordine di prima apparizione del target).
//
// Logica pura host-testabile: nessun runtime dell'artefatto richiesto.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <pulse/hooks.hpp>

#include "hooking/registry_grouping.hpp"

namespace {

using pulse::hooking::group_registry_by_target;
using pulse::hooking::LinkSpec;
using pulse::hooking::ModId;
using pulse::hooking::TargetGroup;
using pulse::hooks::HookRegistration;

// Costruisce una HookRegistration risolta con un Target_Address sintetico.
HookRegistration resolved(std::string_view symbol, std::uintptr_t target,
                          int priority, void* detour, void** trampoline) {
    HookRegistration r;
    r.symbol = symbol;
    r.detour = detour;
    r.trampoline = trampoline;
    r.priority = priority;
    r.resolved = true;
    r.target = reinterpret_cast<void*>(target);
    return r;
}

HookRegistration unresolved(std::string_view symbol, int priority, void* detour,
                            void** trampoline) {
    HookRegistration r;
    r.symbol = symbol;
    r.detour = detour;
    r.trampoline = trampoline;
    r.priority = priority;
    r.resolved = false;
    r.target = nullptr;
    return r;
}

// Detour/slot opachi distinti (l'identità conta per verificare il cablaggio).
int dA = 0, dB = 0, dC = 0, dD = 0;
void* slotA = nullptr;
void* slotB = nullptr;
void* slotC = nullptr;
void* slotD = nullptr;

// --- Più registrazioni dello stesso target in un unico gruppo, ordinate -----
TEST(RegistryGrouping, GroupsMultipleRegistrationsPerTargetInChainOrder) {
    constexpr std::uintptr_t kTarget = 0x1000;
    std::vector<HookRegistration> reg;
    // Inserzione (loadOrder) 0,1,2 sullo STESSO target con priorità diverse.
    reg.push_back(resolved("MenuLayer::init", kTarget, 500, &dA, &slotA));  // idx 0
    reg.push_back(resolved("MenuLayer::init", kTarget, 900, &dB, &slotB));  // idx 1
    reg.push_back(resolved("MenuLayer::init", kTarget, 100, &dC, &slotC));  // idx 2

    const auto groups = group_registry_by_target(reg, nullptr);

    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups[0].target, kTarget);
    ASSERT_EQ(groups[0].links.size(), 3u);

    // Chain_Order: priority DESC ⇒ B(900), A(500), C(100).
    EXPECT_EQ(groups[0].links[0].detour, static_cast<void*>(&dB));
    EXPECT_EQ(groups[0].links[1].detour, static_cast<void*>(&dA));
    EXPECT_EQ(groups[0].links[2].detour, static_cast<void*>(&dC));

    // detour/slot della registrazione SDK propagati al LinkSpec.
    EXPECT_EQ(groups[0].links[0].slot, &slotB);
    EXPECT_EQ(groups[0].links[1].slot, &slotA);
    EXPECT_EQ(groups[0].links[2].slot, &slotC);
}

// --- Tie-break su loadOrder ASC a parità di priorità ------------------------
TEST(RegistryGrouping, TieBreaksByLoadOrderAscending) {
    constexpr std::uintptr_t kTarget = 0x2000;
    std::vector<HookRegistration> reg;
    reg.push_back(resolved("S", kTarget, 500, &dA, &slotA));  // idx 0
    reg.push_back(resolved("S", kTarget, 500, &dB, &slotB));  // idx 1
    reg.push_back(resolved("S", kTarget, 500, &dC, &slotC));  // idx 2

    const auto groups = group_registry_by_target(reg, nullptr);

    ASSERT_EQ(groups.size(), 1u);
    ASSERT_EQ(groups[0].links.size(), 3u);
    // Stessa priorità ⇒ ordine di inserzione (loadOrder ASC): A, B, C.
    EXPECT_EQ(groups[0].links[0].loadOrder, 0u);
    EXPECT_EQ(groups[0].links[1].loadOrder, 1u);
    EXPECT_EQ(groups[0].links[2].loadOrder, 2u);
    EXPECT_EQ(groups[0].links[0].detour, static_cast<void*>(&dA));
    EXPECT_EQ(groups[0].links[1].detour, static_cast<void*>(&dB));
    EXPECT_EQ(groups[0].links[2].detour, static_cast<void*>(&dC));
}

// --- Più target distinti producono gruppi disgiunti (ordine di apparizione) -
TEST(RegistryGrouping, SeparatesDistinctTargetsByFirstAppearance) {
    constexpr std::uintptr_t kT1 = 0x3000;
    constexpr std::uintptr_t kT2 = 0x4000;
    std::vector<HookRegistration> reg;
    reg.push_back(resolved("A::f", kT1, 500, &dA, &slotA));  // idx 0 → gruppo 0 (T1)
    reg.push_back(resolved("B::g", kT2, 500, &dB, &slotB));  // idx 1 → gruppo 1 (T2)
    reg.push_back(resolved("A::f", kT1, 700, &dC, &slotC));  // idx 2 → gruppo 0 (T1)

    const auto groups = group_registry_by_target(reg, nullptr);

    ASSERT_EQ(groups.size(), 2u);
    // Ordine di prima apparizione: T1 poi T2.
    EXPECT_EQ(groups[0].target, kT1);
    EXPECT_EQ(groups[1].target, kT2);

    // Gruppo T1: due anelli, C(700) prima di A(500).
    ASSERT_EQ(groups[0].links.size(), 2u);
    EXPECT_EQ(groups[0].links[0].detour, static_cast<void*>(&dC));
    EXPECT_EQ(groups[0].links[1].detour, static_cast<void*>(&dA));

    // Gruppo T2: un solo anello B.
    ASSERT_EQ(groups[1].links.size(), 1u);
    EXPECT_EQ(groups[1].links[0].detour, static_cast<void*>(&dB));
}

// --- Le registrazioni non risolte non producono alcun anello ----------------
TEST(RegistryGrouping, SkipsUnresolvedRegistrations) {
    constexpr std::uintptr_t kTarget = 0x5000;
    std::vector<HookRegistration> reg;
    reg.push_back(resolved("S", kTarget, 500, &dA, &slotA));   // idx 0 (risolta)
    reg.push_back(unresolved("S", 900, &dB, &slotB));          // idx 1 (NON risolta)
    reg.push_back(resolved("S", kTarget, 300, &dC, &slotC));   // idx 2 (risolta)

    const auto groups = group_registry_by_target(reg, nullptr);

    ASSERT_EQ(groups.size(), 1u);
    ASSERT_EQ(groups[0].links.size(), 2u);  // solo A e C; B scartata
    // loadOrder preserva l'indice nel registro: A=0, C=2 (l'indice 1 è saltato).
    EXPECT_EQ(groups[0].links[0].detour, static_cast<void*>(&dA));
    EXPECT_EQ(groups[0].links[0].loadOrder, 0u);
    EXPECT_EQ(groups[0].links[1].detour, static_cast<void*>(&dC));
    EXPECT_EQ(groups[0].links[1].loadOrder, 2u);
}

// --- Una registrazione risolta con target nullo è scartata (difensivo) ------
TEST(RegistryGrouping, SkipsResolvedWithNullTarget) {
    std::vector<HookRegistration> reg;
    HookRegistration r;
    r.symbol = "S";
    r.detour = &dA;
    r.trampoline = &slotA;
    r.priority = 500;
    r.resolved = true;     // marcata risolta…
    r.target = nullptr;    // …ma senza indirizzo: nessun anello.
    reg.push_back(r);

    const auto groups = group_registry_by_target(reg, nullptr);
    EXPECT_TRUE(groups.empty());
}

// --- La priorità fuori dominio è ricondotta per l'ordinamento (clamp) -------
TEST(RegistryGrouping, ClampsPriorityForOrdering) {
    constexpr std::uintptr_t kTarget = 0x6000;
    std::vector<HookRegistration> reg;
    // 2000 e 1000 sono equivalenti dopo il clamp a 1000 ⇒ tie-break su loadOrder.
    reg.push_back(resolved("S", kTarget, 2000, &dA, &slotA));  // idx 0 → clamp 1000
    reg.push_back(resolved("S", kTarget, 1000, &dB, &slotB));  // idx 1 → 1000
    reg.push_back(resolved("S", kTarget, -50, &dC, &slotC));   // idx 2 → clamp 0 (coda)

    const auto groups = group_registry_by_target(reg, nullptr);

    ASSERT_EQ(groups.size(), 1u);
    ASSERT_EQ(groups[0].links.size(), 3u);
    // A e B equivalenti a 1000 ⇒ A (loadOrder 0) prima di B (loadOrder 1); C ultima.
    EXPECT_EQ(groups[0].links[0].detour, static_cast<void*>(&dA));
    EXPECT_EQ(groups[0].links[1].detour, static_cast<void*>(&dB));
    EXPECT_EQ(groups[0].links[2].detour, static_cast<void*>(&dC));
}

// --- L'owner è risolto via callback (es. ledger ownerOfIndex) ---------------
TEST(RegistryGrouping, ResolvesOwnerViaCallback) {
    constexpr std::uintptr_t kTarget = 0x7000;
    std::vector<HookRegistration> reg;
    reg.push_back(resolved("S", kTarget, 900, &dA, &slotA));  // idx 0 → demo
    reg.push_back(resolved("S", kTarget, 500, &dB, &slotB));  // idx 1 → esterna

    const auto ownerOf = [](std::size_t index) -> ModId {
        return index == 0 ? ModId{"pulse.demo"} : ModId{"com.pulse.ext"};
    };

    const auto groups = group_registry_by_target(reg, ownerOf);

    ASSERT_EQ(groups.size(), 1u);
    ASSERT_EQ(groups[0].links.size(), 2u);
    // Chain_Head è A (900) di pulse.demo, poi B di com.pulse.ext.
    EXPECT_EQ(groups[0].links[0].owner, "pulse.demo");
    EXPECT_EQ(groups[0].links[0].symbol, "S");
    EXPECT_EQ(groups[0].links[1].owner, "com.pulse.ext");
}

// --- Owner di default (callback nulla) ⇒ stringa vuota -----------------------
TEST(RegistryGrouping, DefaultsOwnerToEmptyWhenNoResolver) {
    constexpr std::uintptr_t kTarget = 0x8000;
    std::vector<HookRegistration> reg;
    reg.push_back(resolved("S", kTarget, 500, &dA, &slotA));

    const auto groups = group_registry_by_target(reg, nullptr);
    ASSERT_EQ(groups.size(), 1u);
    ASSERT_EQ(groups[0].links.size(), 1u);
    EXPECT_TRUE(groups[0].links[0].owner.empty());
}

// --- Registro vuoto ⇒ nessun gruppo ----------------------------------------
TEST(RegistryGrouping, EmptyRegistryYieldsNoGroups) {
    std::vector<HookRegistration> reg;
    EXPECT_TRUE(group_registry_by_target(reg, nullptr).empty());
}

// --- Deterministico: due valutazioni dello stesso registro coincidono -------
TEST(RegistryGrouping, DeterministicAcrossEvaluations) {
    std::vector<HookRegistration> reg;
    reg.push_back(resolved("A", 0xAA00, 500, &dA, &slotA));
    reg.push_back(resolved("B", 0xBB00, 600, &dB, &slotB));
    reg.push_back(resolved("A", 0xAA00, 700, &dC, &slotC));
    reg.push_back(resolved("C", 0xCC00, 500, &dD, &slotD));

    const auto g1 = group_registry_by_target(reg, nullptr);
    const auto g2 = group_registry_by_target(reg, nullptr);

    ASSERT_EQ(g1.size(), g2.size());
    for (std::size_t i = 0; i < g1.size(); ++i) {
        EXPECT_EQ(g1[i].target, g2[i].target);
        ASSERT_EQ(g1[i].links.size(), g2[i].links.size());
        for (std::size_t j = 0; j < g1[i].links.size(); ++j) {
            EXPECT_EQ(g1[i].links[j].detour, g2[i].links[j].detour);
            EXPECT_EQ(g1[i].links[j].loadOrder, g2[i].links[j].loadOrder);
        }
    }
}

}  // namespace
