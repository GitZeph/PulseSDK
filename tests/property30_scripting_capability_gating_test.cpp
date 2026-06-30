// tests/property30_scripting_capability_gating_test.cpp
// Feature: pulse-sdk, Property 30 — Gating delle capability sui permessi
// (ramo SCRIPTING, via ScriptingRuntime).
// Validates: Requirements 12.6, 17.3, 17.4, 19.3, 19.4
//            (Requisiti 12.6, 17.3, 17.4, 19.3, 19.4)
//
// Property 30 (design.md): "Per ogni operazione che richiede un permesso P e
// per ogni combinazione (P dichiarato, P approvato), l'operazione è consentita
// se e solo se P è dichiarato nel Manifest ed è stato approvato dall'User; in
// caso contrario è negata prima di qualsiasi effetto sullo stato del sistema.
// La proprietà vale identicamente per le mod native (Sandbox) e per le mod di
// scripting (Scripting_Runtime)."
//
// QUESTO test copre il ramo SCRIPTING (Scripting_Runtime). Il ramo NATIVO
// (Sandbox) è coperto dal task 35.2
// (tests/property30_sandbox_capability_gating_test.cpp), con la medesima
// proprietà: dimostriamo che il gating delle capability dello scripting è
// IDENTICO a quello del Sandbox nativo (operazione consentita SSE permesso
// concesso, altrimenti negata prima di ogni effetto con Mod + capability).
//
// Strategia (RapidCheck, ≥100 iterazioni per default):
//   * Si genera, per ciascuna capability riconosciuta (Hooking/Events/UI), se
//     il permesso è CONCESSO alla Mod: il predicato di permesso iniettato
//     rispecchia esattamente questo sottoinsieme (come i permessi del
//     Manifest approvati dall'User).
//   * Si carica una Mod di scripting con il FakeScriptEngine in-memory (load
//     entro budget => caricata): il caricamento non altera il gating.
//   * INVARIANTE per ogni capability interrogata: useCapability(mod, cap)
//     riesce (allowed, nessun errore) SE E SOLO SE il predicato concede quella
//     capability per quella Mod; altrimenti è NEGATA prima di qualsiasi effetto
//     (PermissionDenied con Mod + capability coincidenti), senza terminare il
//     gioco — gating identico al ramo Sandbox nativo.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <array>
#include <set>
#include <string>

#include "scripting/fake_script_engine.hpp"
#include "scripting/scripting_runtime.hpp"

namespace {

using pulse::scripting::Capability;
using pulse::scripting::FakeScriptEngine;
using pulse::scripting::ModId;
using pulse::scripting::ScriptErrorKind;
using pulse::scripting::ScriptingRuntime;

// Insieme completo delle capability esposte alle Mod di scripting (Req 19.3):
// le stesse delle Mod native (hooking, eventi, UI).
constexpr std::array<Capability, 3> kAllCapabilities = {
    Capability::Hooking, Capability::Events, Capability::UI};

}  // namespace

// ===========================================================================
// Property 30 (ramo scripting) — useCapability consente SSE il permesso è
// concesso; altrimenti nega prima di ogni effetto con Mod + capability.
// Feature: pulse-sdk, Property 30.
// Validates: Requirements 12.6, 17.3, 17.4, 19.3, 19.4.
// ===========================================================================
RC_GTEST_PROP(Property30ScriptingCapabilityGating,
              ScriptingCapabilityGatedByGrantedPermission,
              ()) {
    // Per ciascuna capability, decidi casualmente se è concessa alla Mod: il
    // sottoinsieme concesso modella i permessi dichiarati nel Manifest E
    // approvati dall'User (consenso granulare). Esplora l'intero spazio
    // (nessuna concessa / alcune / tutte).
    std::set<int> grantedSet;
    for (const Capability cap : kAllCapabilities) {
        const bool isGranted =
            *rc::gen::arbitrary<bool>().as("capability concessa");
        if (isGranted) {
            grantedSet.insert(static_cast<int>(cap));
        }
    }

    const ModId modId = "mod.script";

    // Predicato di permesso iniettato: concede una capability SSE è nel
    // sottoinsieme generato (rispecchia il gating del Sandbox nativo, Req
    // 19.3/19.4) senza dipendere dal tipo Sandbox.
    auto permission = [&grantedSet, &modId](const ModId& mod,
                                            Capability cap) -> bool {
        if (mod != modId) return false;
        return grantedSet.count(static_cast<int>(cap)) > 0;
    };

    // Carica la Mod di scripting con il FakeScriptEngine in-memory (load entro
    // il budget di default => caricata). Il caricamento non incide sul gating.
    FakeScriptEngine engine;
    ScriptingRuntime runtime(engine, permission);

    const auto load = runtime.loadMod(modId, "print('ciao')");
    RC_ASSERT(load.loaded);
    RC_ASSERT(runtime.isLoaded(modId));

    // INVARIANTE: per ogni capability, useCapability riesce SSE concessa;
    // altrimenti è negata prima di ogni effetto con PermissionDenied coerente.
    for (const Capability cap : kAllCapabilities) {
        const bool shouldAllow = grantedSet.count(static_cast<int>(cap)) > 0;

        const auto result = runtime.useCapability(modId, cap);

        if (shouldAllow) {
            // Capability concessa => operazione consentita, nessun errore.
            RC_ASSERT(result.allowed);
            RC_ASSERT(result.ok());
            RC_ASSERT(result.error.kind == ScriptErrorKind::None);
        } else {
            // Negata PRIMA di qualsiasi effetto: bloccata con PermissionDenied,
            // riportando Mod + capability negata (gioco non terminato).
            RC_ASSERT(!result.allowed);
            RC_ASSERT(!result.ok());
            RC_ASSERT(result.error.kind == ScriptErrorKind::PermissionDenied);
            RC_ASSERT(result.error.mod == modId);
            RC_ASSERT(result.error.capability.has_value());
            RC_ASSERT(result.error.capability.value() == cap);
            RC_ASSERT(!result.error.message.empty());
        }
    }
}

// ===========================================================================
// Property 30 (ramo scripting) — una Mod senza ALCUN permesso concesso ha ogni
// capability negata prima di ogni effetto, identicamente al ramo nativo per le
// mod non approvate. Feature: pulse-sdk, Property 30.
// Validates: Requirements 17.3, 17.4, 19.3, 19.4.
// ===========================================================================
RC_GTEST_PROP(Property30ScriptingCapabilityGating,
              ModWithoutGrantsAlwaysDenied,
              ()) {
    const auto cap =
        *rc::gen::element(Capability::Hooking, Capability::Events,
                          Capability::UI)
             .as("capability interrogata");
    const ModId modId =
        *rc::gen::nonEmpty(
             rc::gen::container<std::string>(rc::gen::inRange<char>('a', 'z')))
             .as("id Mod senza permessi");

    // Predicato che non concede MAI alcuna capability (nessun permesso
    // approvato): caso limite del gating.
    auto denyAll = [](const ModId&, Capability) { return false; };

    FakeScriptEngine engine;
    ScriptingRuntime runtime(engine, denyAll);

    RC_ASSERT(runtime.loadMod(modId, "...").loaded);

    const auto result = runtime.useCapability(modId, cap);

    // Sempre negata prima di ogni effetto, con Mod + capability coerenti.
    RC_ASSERT(!result.allowed);
    RC_ASSERT(!result.ok());
    RC_ASSERT(result.error.kind == ScriptErrorKind::PermissionDenied);
    RC_ASSERT(result.error.mod == modId);
    RC_ASSERT(result.error.capability.has_value());
    RC_ASSERT(result.error.capability.value() == cap);
}
