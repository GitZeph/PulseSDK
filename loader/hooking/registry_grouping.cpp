// loader/hooking/registry_grouping.cpp — implementazione del raggruppamento del
// registro SDK per Target_Address risolto (Hook_Chaining, task 7.2, Req 10.1).
//
// Vedi `registry_grouping.hpp` per il contratto. La logica è deterministica e
// pura: itera l'intero registro, scarta le registrazioni non risolte, raggruppa
// per Target_Address risolto preservando l'ordine di prima apparizione e ordina
// ciascun gruppo nel Chain_Order (priority DESC con clamp, loadOrder ASC).

#include "hooking/registry_grouping.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>

namespace pulse::hooking {

std::vector<TargetGroup> group_registry_by_target(
    const std::vector<pulse::hooks::HookRegistration>& registry,
    const OwnerResolver& ownerOf) {
    std::vector<TargetGroup> groups;
    // target risolto → indice del gruppo in `groups` (preserva l'ordine di
    // prima apparizione, così l'output è deterministico e ripetibile).
    std::unordered_map<std::uintptr_t, std::size_t> indexOf;

    for (std::size_t i = 0; i < registry.size(); ++i) {
        const pulse::hooks::HookRegistration& reg = registry[i];

        // Registrazione non risolta → nessun anello (Req 10.1, 10.3): un binding
        // non risolto/non verificato non aggiunge alcun Hook_Link alla catena.
        if (!reg.resolved || reg.target == nullptr) {
            continue;
        }

        const auto target = reinterpret_cast<std::uintptr_t>(reg.target);

        LinkSpec link;
        link.owner = ownerOf ? ownerOf(i) : ModId{};
        link.symbol = std::string(reg.symbol);
        link.priority = reg.priority;                 // clamp a valle (HookChain::add)
        link.loadOrder = static_cast<std::uint64_t>(i);  // ordine di inserzione/epoca
        link.detour = reg.detour;
        link.slot = reg.trampoline;                   // Trampoline_Slot pulse_original

        auto it = indexOf.find(target);
        if (it == indexOf.end()) {
            indexOf.emplace(target, groups.size());
            TargetGroup group;
            group.target = target;
            group.links.push_back(std::move(link));
            groups.push_back(std::move(group));
        } else {
            groups[it->second].links.push_back(std::move(link));
        }
    }

    // Ordina ciascun gruppo nel Chain_Order: Hook_Priority DESC (ricondotta al
    // dominio [0,1000], coerente con HookChain), poi load order ASC come
    // tie-break stabile. `std::stable_sort` preserva l'ordine di inserzione tra
    // anelli equivalenti (stessa priority effettiva e stesso loadOrder).
    for (TargetGroup& group : groups) {
        std::stable_sort(group.links.begin(), group.links.end(),
                         [](const LinkSpec& a, const LinkSpec& b) {
                             const int pa = clampHookPriority(a.priority);
                             const int pb = clampHookPriority(b.priority);
                             if (pa != pb) {
                                 return pa > pb;  // priority DESC
                             }
                             return a.loadOrder < b.loadOrder;  // loadOrder ASC
                         });
    }

    return groups;
}

}  // namespace pulse::hooking
