// loader/lifecycle/hook_ownership.cpp — implementazione del HookOwnershipLedger
// (External Mod Loading, task 5.5). Vedi hook_ownership.hpp per il razionale
// delle finestre di epoca e dell'attribuzione per Mod_Id.

#include "lifecycle/hook_ownership.hpp"

#include <algorithm>

namespace pulse::lifecycle {

std::vector<std::size_t> HookOwnershipLedger::closeEpoch(const ModId& owner,
                                                         std::size_t start) {
    const std::size_t end = pulse::hooks::count();

    std::vector<std::size_t> indices;
    // `start` oltre la fine del registro → finestra vuota (fail-soft): nessuna
    // attribuzione, nessun indice restituito.
    if (start >= end) {
        return indices;
    }

    indices.reserve(end - start);
    for (std::size_t i = start; i < end; ++i) {
        registryOwner_[i] = owner;  // l'ultima finestra che copre `i` vince
        indices.push_back(i);
    }
    return indices;
}

void HookOwnershipLedger::attribute(OwnedHook hook) {
    installed_.push_back(std::move(hook));
}

std::vector<OwnedHook> HookOwnershipLedger::hooksOf(const ModId& owner) const {
    std::vector<OwnedHook> result;
    for (const auto& hook : installed_) {
        if (hook.owner == owner) {
            result.push_back(hook);
        }
    }
    return result;
}

void HookOwnershipLedger::release(const ModId& owner) {
    installed_.erase(
        std::remove_if(installed_.begin(), installed_.end(),
                       [&owner](const OwnedHook& hook) { return hook.owner == owner; }),
        installed_.end());
}

std::vector<OwnedHook> HookOwnershipLedger::allInstalled() const {
    return installed_;
}

ModId HookOwnershipLedger::ownerOfIndex(std::size_t index) const {
    const auto it = registryOwner_.find(index);
    return it == registryOwner_.end() ? ModId{} : it->second;
}

}  // namespace pulse::lifecycle
