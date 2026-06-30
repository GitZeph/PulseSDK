// tests/property12_mod_state_transition_validity_test.cpp
// Feature: pulse-sdk, Property 12 — Validità delle transizioni di stato della
// mod.
// Validates: Requirements 4.4, 4.5 (Requisiti 4.4, 4.5)
//
// Property 12 (design.md / Req 4.4, 4.5): per ogni sequenza randomizzata di
// richieste di transizione applicate a una mod registrata, lo state machine
// del `ModManager` deve rispettare le seguenti invarianti:
//
//   (1) APPLICATA  ⟺  isAllowed(prevState, target)  AND  newState == target;
//       cioè una transizione è applicata se e solo se è ammessa dalla state
//       machine, e quando applicata porta esattamente allo stato richiesto
//       (Req 4.4).
//   (2) RIFIUTATA  ⟹  stato INVARIATO (newState == prevState) e presenza di
//       una segnalazione (`rejection`) che identifica la mod e la transizione
//       rifiutata (from == prevState, requested == target) (Req 4.5).
//   (3) lo stato della mod è SEMPRE uno dei quattro stati validi
//       {Installed, Enabled, Disabled, Removed} e cambia SOLO tramite una
//       transizione ammessa (Req 4.4).
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si registra una mod con un entry point che ha SEMPRE successo, così la
//     transizione verso `Enabled` è applicata in modo deterministico quando
//     ammessa, e l'esito atteso dipende solo da `isAllowed(prevState, target)`
//     (questo rende il test robusto rispetto al task 12.2, che instrada a
//     `Disabled` solo i FALLIMENTI di entry point);
//   * si genera una sequenza randomizzata di stati target (dai 4 stati) e la si
//     applica in ordine, confrontando ad ogni passo lo stato precedente con
//     l'esito di `transition()` secondo le invarianti (1)-(3).

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <string>
#include <vector>

#include "lifecycle/mod_manager.hpp"

namespace {

using pulse::lifecycle::ModManager;
using pulse::lifecycle::ModState;
using pulse::lifecycle::TransitionResult;
using pulse::lifecycle::TransitionStatus;

// I quattro stati validi del ciclo di vita (Req 4.4).
constexpr ModState kAllStates[] = {
    ModState::Installed,
    ModState::Enabled,
    ModState::Disabled,
    ModState::Removed,
};

bool isValidState(ModState s) noexcept {
    for (ModState v : kAllStates) {
        if (v == s) return true;
    }
    return false;
}

// --- Property 12 — validità delle transizioni di stato --------------------
// Feature: pulse-sdk, Property 12. Validates: Requirements 4.4, 4.5.
RC_GTEST_PROP(Property12ModStateTransitionValidity,
              AppliedIffAllowedAndRejectedLeavesStateUnchanged,
              ()) {
    const std::string modId = "mod.under.test";

    ModManager manager;
    // Entry point che ha SEMPRE successo: l'enable, quando ammesso, è sempre
    // applicato (indipendente dalla logica di isolamento init-failure, 12.2).
    manager.registerMod(
        modId,
        /*entry=*/[] { return pulse::lifecycle::EntryPointOutcome::success(); },
        /*terminator=*/[] {});

    // Sequenza randomizzata di target dai 4 stati validi.
    const auto targets = *rc::gen::container<std::vector<ModState>>(
        rc::gen::element(ModState::Installed, ModState::Enabled,
                         ModState::Disabled, ModState::Removed))
                             .as("sequenza di transizioni richieste");

    for (ModState target : targets) {
        // Stato prima della transizione (la mod è registrata => sempre valido).
        const auto prevOpt = manager.stateOf(modId);
        RC_ASSERT(prevOpt.has_value());
        const ModState prev = *prevOpt;
        RC_ASSERT(isValidState(prev));

        const bool allowed = ModManager::isAllowed(prev, target);
        const TransitionResult result = manager.transition(modId, target);

        // La mod è registrata: lo status non è mai UnknownMod.
        RC_ASSERT(result.status != TransitionStatus::UnknownMod);

        // Lo stato riportato coincide con lo stato effettivo nel manager.
        const auto afterOpt = manager.stateOf(modId);
        RC_ASSERT(afterOpt.has_value());
        const ModState after = *afterOpt;
        RC_ASSERT(result.state == after);

        // (3) Lo stato è sempre uno dei quattro stati validi.
        RC_ASSERT(isValidState(after));

        if (allowed) {
            // (1) Ammessa => applicata e nuovo stato == target.
            RC_ASSERT(result.applied());
            RC_ASSERT(after == target);
            RC_ASSERT(!result.rejection.has_value());
        } else {
            // (2) Non ammessa => rifiutata, stato invariato, segnalazione con
            //     mod + transizione (from == prev, requested == target).
            RC_ASSERT(result.rejected());
            RC_ASSERT(after == prev);  // stato invariato (Req 4.5)
            RC_ASSERT(result.rejection.has_value());
            const auto& rej = *result.rejection;
            RC_ASSERT(rej.mod == modId);
            RC_ASSERT(rej.from == prev);
            RC_ASSERT(rej.requested == target);
        }

        // (3) Lo stato cambia SOLO tramite una transizione ammessa: se non era
        //     ammessa, lo stato non è cambiato.
        if (!allowed) {
            RC_ASSERT(after == prev);
        }
    }
}

}  // namespace
