// tests/property31_sandbox_permission_rejection_test.cpp
// Feature: pulse-sdk, Property 31 — Rifiuto di dichiarazioni di permesso non
// valide.
// Validates: Requirements 17.2 (Requisito 17.2)
//
// Invariante (Req 17.2): l'installazione di una Mod è RIFIUTATA se il Manifest
//   * OMETTE del tutto la sezione dei permessi (modellata da std::nullopt) ->
//     InstallError::PermissionsSectionOmitted; oppure
//   * dichiara almeno un token di permesso NON riconosciuto ->
//     InstallError::UnrecognizedPermission, con un messaggio che identifica il
//     token offensivo.
// In entrambi i casi di rifiuto la Mod NON viene installata e nessun consenso
// viene registrato. Quando invece tutti i token sono riconosciuti e l'User
// approva, l'installazione ha successo e l'insieme approvato coincide con
// l'intersezione tra i permessi dichiarati e quelli concessi.
//
// Strategia: si generano liste di dichiarazione miste (token validi + token
// sconosciuti da un pool DISGIUNTO dall'insieme riconosciuto) e talvolta la
// sezione omessa (nullopt). Il callback di approvazione concede tutto, così i
// casi di successo non sono bloccati da un rifiuto dell'User.
//
// Header-only: include direttamente "sandbox/sandbox.hpp" (radice loader/
// nella include path del target di test).

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "sandbox/sandbox.hpp"

namespace {

using pulse::sandbox::ApprovalDecision;
using pulse::sandbox::ApprovalRequest;
using pulse::sandbox::InstallError;
using pulse::sandbox::InstallResult;
using pulse::sandbox::Permission;
using pulse::sandbox::Sandbox;

// Token RICONOSCIUTI dal sistema (coincidono con parsePermission).
const std::vector<std::string>& recognizedTokens() {
    static const std::vector<std::string> kTokens = {
        "network", "filesystem", "hooking", "ui", "events"};
    return kTokens;
}

// Pool di token SCONOSCIUTI, scelto DISGIUNTO dall'insieme riconosciuto: ogni
// elemento di questo pool fa fallire parsePermission (Req 17.2).
const std::vector<std::string>& unknownTokenPool() {
    static const std::vector<std::string> kPool = {
        "teleport", "filesystemx", "net", "admin", "root",
        "gpu",      "camera",      "",    "Network" /* case-sensitive */};
    return kPool;
}

// Mappa un token riconosciuto sul Permission corrispondente (per il calcolo
// dell'insieme atteso nei casi di successo).
Permission toPermission(const std::string& token) {
    if (token == "network") return Permission::Network;
    if (token == "filesystem") return Permission::FileSystem;
    if (token == "hooking") return Permission::Hooking;
    if (token == "ui") return Permission::UI;
    return Permission::Events;  // "events"
}

// Callback di approvazione che concede SEMPRE tutto ciò che è richiesto, così i
// casi di successo non vengono bloccati da un rifiuto dell'User.
ApprovalDecision grantAll(const ApprovalRequest& req) {
    return ApprovalDecision::grantAll(req.requested);
}

}  // namespace

// ===========================================================================
// Property 31 — Rifiuto di dichiarazioni di permesso non valide (Req 17.2).
// Feature: pulse-sdk, Property 31. Validates: Requirements 17.2.
// ===========================================================================
RC_GTEST_PROP(Property31SandboxPermissionRejection,
              RejectsOmittedSectionAndUnrecognizedTokens,
              ()) {
    // (1) Decide se la sezione dei permessi è OMESSA (nullopt). La probabilità
    //     è bassa-ma-non-nulla per coprire bene anche i casi con sezione
    //     presente (validi e non).
    const bool sectionOmitted = *rc::gen::weightedElement<bool>(
                                     {{1u, true}, {4u, false}})
                                     .as("sezione [permissions] omessa");

    // Genera una lista mista di token: ogni elemento è valido o sconosciuto.
    // Una lista presente ma vuota è un caso valido legittimo.
    const auto tokenGen = rc::gen::oneOf(
        rc::gen::elementOf(recognizedTokens()),
        rc::gen::elementOf(unknownTokenPool()));
    const std::vector<std::string> tokens =
        *rc::gen::container<std::vector<std::string>>(tokenGen)
             .as("token di permesso dichiarati");

    std::optional<std::vector<std::string>> declared;
    if (!sectionOmitted) {
        declared = tokens;
    }

    Sandbox sandbox;
    const std::string modId = "mod.prop31";

    InstallResult res = sandbox.installMod(modId, declared, grantAll);

    if (sectionOmitted) {
        // (a) Sezione omessa -> rifiuto con PermissionsSectionOmitted; nessuna
        //     installazione, nessun consenso.
        RC_ASSERT(!res.ok);
        RC_ASSERT(res.error == InstallError::PermissionsSectionOmitted);
        RC_ASSERT(!sandbox.isInstalled(modId));
        return;
    }

    // Determina se almeno un token è sconosciuto.
    const auto& known = recognizedTokens();
    const std::string* offending = nullptr;
    for (const std::string& t : tokens) {
        if (std::find(known.begin(), known.end(), t) == known.end()) {
            offending = &t;
            break;
        }
    }

    if (offending != nullptr) {
        // (b) Almeno un token non riconosciuto -> rifiuto con
        //     UnrecognizedPermission; il messaggio identifica un token
        //     offensivo; nessuna installazione, nessun consenso registrato.
        RC_ASSERT(!res.ok);
        RC_ASSERT(res.error == InstallError::UnrecognizedPermission);
        RC_ASSERT(res.message.find(*offending) != std::string::npos);
        RC_ASSERT(!sandbox.isInstalled(modId));
        // Nessun consenso: ogni check per la Mod è negato.
        RC_ASSERT(!sandbox.check(modId, Permission::Network).has_value());
        return;
    }

    // (c) Tutti i token sono riconosciuti e l'User concede tutto -> successo;
    //     l'insieme approvato coincide con l'intersezione dichiarati∩concessi,
    //     cioè l'insieme (deduplicato) dei permessi dichiarati.
    RC_ASSERT(res.ok);
    RC_ASSERT(res.error == InstallError::None);
    RC_ASSERT(sandbox.isInstalled(modId));

    std::set<Permission> expected;
    for (const std::string& t : tokens) {
        expected.insert(toPermission(t));
    }
    const std::set<Permission> approved(res.approved.begin(), res.approved.end());
    RC_ASSERT(approved == expected);
}
