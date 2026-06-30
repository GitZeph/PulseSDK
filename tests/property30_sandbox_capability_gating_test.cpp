// tests/property30_sandbox_capability_gating_test.cpp
// Feature: pulse-sdk, Property 30 — Gating delle capability sui permessi
// (ramo Sandbox NATIVO).
// Validates: Requirements 12.6, 17.3, 17.4, 19.3, 19.4
//            (Requisiti 12.6, 17.3, 17.4, 19.3, 19.4)
//
// Property 30 (design.md): "Per ogni operazione che richiede un permesso P e
// per ogni combinazione (P dichiarato, P approvato), l'operazione è consentita
// se e solo se P è dichiarato nel Manifest ed è stato approvato dall'User; in
// caso contrario è negata prima di qualsiasi effetto sullo stato del sistema,
// con registrazione della violazione. La proprietà vale identicamente per le
// mod native (Sandbox) e per le mod di scripting (Scripting_Runtime)."
//
// QUESTO test copre il ramo NATIVO (Sandbox). Il ramo Scripting_Runtime è
// coperto dal task 36.2 con la medesima proprietà.
//
// Strategia (RapidCheck, ≥100 iterazioni per default):
//   * Si genera per ciascuno dei permessi riconosciuti se è DICHIARATO nel
//     Manifest e, se dichiarato, se è CONCESSO dall'User (consenso granulare).
//   * Si installa la Mod con un'approvazione che concede esattamente il
//     sottoinsieme scelto; l'installazione riesce sempre (sezione presente,
//     token tutti riconosciuti, decisione = approva).
//   * INVARIANTE per ogni permesso interrogato: `check(mod, perm)` riesce SE E
//     SOLO SE `perm` è dichiarato E approvato; altrimenti è negato (nessun
//     valore) E viene registrata ESATTAMENTE UNA violazione per quella query
//     negata (id Mod + permesso combaciano) — dimostra deny-before-effect +
//     logging della violazione.
//   * Caso Mod NON installata: ogni `check` è sempre negato e registrato.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <array>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "sandbox/sandbox.hpp"

namespace {

using pulse::sandbox::ApprovalDecision;
using pulse::sandbox::ApprovalRequest;
using pulse::sandbox::InMemoryViolationSink;
using pulse::sandbox::InstallResult;
using pulse::sandbox::ModId;
using pulse::sandbox::Permission;
using pulse::sandbox::permissionName;
using pulse::sandbox::Sandbox;

// Insieme completo dei permessi riconosciuti dal sistema (Req 17.1).
constexpr std::array<Permission, 5> kAllPermissions = {
    Permission::Network, Permission::FileSystem, Permission::Hooking,
    Permission::UI, Permission::Events};

}  // namespace

// ===========================================================================
// Property 30 (ramo nativo) — check consente SSE dichiarato E approvato;
// altrimenti nega prima di ogni effetto e registra esattamente una violazione.
// Feature: pulse-sdk, Property 30.
// Validates: Requirements 12.6, 17.3, 17.4, 19.3, 19.4.
// ===========================================================================
RC_GTEST_PROP(Property30SandboxCapabilityGating,
              NativeCapabilityGatedByDeclaredAndApproved,
              ()) {
    // Costruisce, per ciascun permesso, la coppia (dichiarato, approvato) in
    // modo casuale: un permesso può essere concesso solo se dichiarato, così
    // si esplora l'intero spazio (non dichiarato / dichiarato-non-approvato /
    // dichiarato-e-approvato).
    std::vector<std::string> declaredTokens;
    std::vector<Permission> grantedPerms;
    std::set<Permission> declaredSet;
    std::set<Permission> approvedSet;

    for (const Permission p : kAllPermissions) {
        const bool isDeclared = *rc::gen::arbitrary<bool>().as("dichiarato");
        if (!isDeclared) {
            continue;
        }
        declaredTokens.push_back(permissionName(p));
        declaredSet.insert(p);

        const bool isGranted = *rc::gen::arbitrary<bool>().as("approvato");
        if (isGranted) {
            grantedPerms.push_back(p);
            approvedSet.insert(p);
        }
    }

    // Installa la Mod nativa concedendo esattamente il sottoinsieme scelto.
    // La sezione dei permessi è presente (anche se vuota è valida) e tutti i
    // token sono riconosciuti, quindi l'installazione riesce sempre.
    InMemoryViolationSink sink;
    Sandbox sandbox(&sink);
    const ModId modId = "mod.native";

    const InstallResult res = sandbox.installMod(
        modId,
        std::optional<std::vector<std::string>>{declaredTokens},
        [&](const ApprovalRequest&) {
            return ApprovalDecision::grantSubset(grantedPerms);
        });

    RC_ASSERT(res.ok);
    RC_ASSERT(sandbox.isInstalled(modId));

    // INVARIANTE: per ogni permesso, check riesce SSE dichiarato E approvato;
    // altrimenti è negato e registra esattamente una violazione coerente.
    for (const Permission p : kAllPermissions) {
        const bool shouldAllow =
            declaredSet.count(p) != 0 && approvedSet.count(p) != 0;

        // Sink azzerato per misurare con precisione le violazioni di QUESTA
        // singola query (deny-before-effect + logging per operazione negata).
        sink.clear();
        const auto result = sandbox.check(modId, p);

        if (shouldAllow) {
            RC_ASSERT(result.has_value());
            RC_ASSERT(static_cast<bool>(result));
            // Nessun effetto collaterale di violazione sul percorso consentito.
            RC_ASSERT(sink.count() == 0u);
        } else {
            // Negato PRIMA di qualsiasi effetto: nessun valore di consenso.
            RC_ASSERT(!result.has_value());
            RC_ASSERT(!static_cast<bool>(result));
            RC_ASSERT(result.error().modId == modId);
            RC_ASSERT(result.error().permission == p);

            // Esattamente UNA violazione registrata, con id + permesso coerenti.
            RC_ASSERT(sink.count() == 1u);
            RC_ASSERT(sink.violations().front().modId == modId);
            RC_ASSERT(sink.violations().front().permission == p);
        }
    }
}

// ===========================================================================
// Property 30 (ramo nativo) — una Mod MAI installata non ha alcun consenso:
// ogni operazione è negata prima di ogni effetto e registrata.
// Feature: pulse-sdk, Property 30.
// Validates: Requirements 17.3, 17.4, 19.3, 19.4.
// ===========================================================================
RC_GTEST_PROP(Property30SandboxCapabilityGating,
              NonInstalledModAlwaysDenied,
              ()) {
    const Permission p =
        *rc::gen::element(Permission::Network, Permission::FileSystem,
                          Permission::Hooking, Permission::UI,
                          Permission::Events)
             .as("permesso interrogato");
    const ModId modId =
        *rc::gen::nonEmpty(
             rc::gen::container<std::string>(rc::gen::inRange<char>('a', 'z')))
             .as("id Mod non installata");

    InMemoryViolationSink sink;
    Sandbox sandbox(&sink);

    // La Mod non è stata installata: nessun consenso registrato.
    RC_ASSERT(!sandbox.isInstalled(modId));

    const auto result = sandbox.check(modId, p);

    // Sempre negata prima di ogni effetto, con violazione coerente registrata.
    RC_ASSERT(!result.has_value());
    RC_ASSERT(!static_cast<bool>(result));
    RC_ASSERT(result.error().modId == modId);
    RC_ASSERT(result.error().permission == p);
    RC_ASSERT(sink.count() == 1u);
    RC_ASSERT(sink.violations().front().modId == modId);
    RC_ASSERT(sink.violations().front().permission == p);
}
