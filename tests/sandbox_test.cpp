// tests/sandbox_test.cpp — unit test del Sandbox a permessi (task 35.1).
//
// Verifica le regole osservabili del confinamento a permessi (Req 17.1–17.6):
//   * `check` consente SOLO se il permesso è dichiarato nel Manifest ED
//     approvato dall'User (Req 17.3);
//   * `check` nega prima di qualsiasi effetto e registra la violazione (id Mod
//     + permesso) sul sink iniettato quando il permesso non è dichiarato o non
//     approvato (Req 17.4);
//   * l'installazione è rifiutata su permesso non riconosciuto (Req 17.2);
//   * l'installazione è rifiutata quando la sezione dei permessi è omessa
//     (Req 17.2);
//   * il flusso di approvazione/rifiuto è onorato: il rifiuto non abilita la
//     Mod e non registra alcun consenso (Req 17.5/17.6); l'approvazione
//     granulare concede solo il sottoinsieme approvato (Req 17.3).
//
// Header-only: include direttamente "sandbox/sandbox.hpp" (radice loader/ nella
// include path del target di test).

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "sandbox/sandbox.hpp"

namespace {

using pulse::sandbox::ApprovalDecision;
using pulse::sandbox::ApprovalRequest;
using pulse::sandbox::InMemoryViolationSink;
using pulse::sandbox::InstallError;
using pulse::sandbox::InstallResult;
using pulse::sandbox::Permission;
using pulse::sandbox::Sandbox;

// Manifest che dichiara la sezione [permissions] con i token forniti.
std::optional<std::vector<std::string>> declared(
    std::vector<std::string> perms) {
    return std::optional<std::vector<std::string>>{std::move(perms)};
}

// Manifest che OMETTE del tutto la sezione [permissions].
std::optional<std::vector<std::string>> omitted() { return std::nullopt; }

// ---------------------------------------------------------------------------
// Req 17.3 — `check` consente quando il permesso è dichiarato E approvato.
// ---------------------------------------------------------------------------
TEST(SandboxTest, AllowsWhenDeclaredAndApproved) {
    InMemoryViolationSink sink;
    Sandbox sandbox(&sink);

    InstallResult res = sandbox.installMod(
        "mod.alpha", declared({"network", "hooking"}),
        [](const ApprovalRequest& req) {
            return ApprovalDecision::grantAll(req.requested);
        });

    ASSERT_TRUE(res.ok);
    EXPECT_EQ(res.error, InstallError::None);

    EXPECT_TRUE(sandbox.check("mod.alpha", Permission::Network));
    EXPECT_TRUE(sandbox.check("mod.alpha", Permission::Hooking));
    EXPECT_EQ(sink.count(), 0u);  // nessuna violazione sul percorso consentito
}

// ---------------------------------------------------------------------------
// Req 17.4 — permesso NON dichiarato: negato prima di ogni effetto + log.
// ---------------------------------------------------------------------------
TEST(SandboxTest, DeniesAndLogsWhenNotDeclared) {
    InMemoryViolationSink sink;
    Sandbox sandbox(&sink);

    sandbox.installMod("mod.beta", declared({"network"}),
                       [](const ApprovalRequest& req) {
                           return ApprovalDecision::grantAll(req.requested);
                       });

    // FileSystem non è dichiarato dal Manifest -> negato e registrato.
    auto result = sandbox.check("mod.beta", Permission::FileSystem);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().modId, "mod.beta");
    EXPECT_EQ(result.error().permission, Permission::FileSystem);

    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(sink.violations().front().modId, "mod.beta");
    EXPECT_EQ(sink.violations().front().permission, Permission::FileSystem);
}

// ---------------------------------------------------------------------------
// Req 17.3/17.4 — permesso dichiarato ma NON approvato (consenso granulare):
// negato prima di ogni effetto + log; il permesso approvato resta consentito.
// ---------------------------------------------------------------------------
TEST(SandboxTest, DeniesAndLogsWhenDeclaredButNotApproved) {
    InMemoryViolationSink sink;
    Sandbox sandbox(&sink);

    // L'User concede solo Network tra {Network, FileSystem}.
    InstallResult res = sandbox.installMod(
        "mod.gamma", declared({"network", "filesystem"}),
        [](const ApprovalRequest&) {
            return ApprovalDecision::grantSubset({Permission::Network});
        });
    ASSERT_TRUE(res.ok);

    EXPECT_TRUE(sandbox.check("mod.gamma", Permission::Network));

    auto denied = sandbox.check("mod.gamma", Permission::FileSystem);
    EXPECT_FALSE(denied.has_value());
    ASSERT_EQ(sink.count(), 1u);
    EXPECT_EQ(sink.violations().front().permission, Permission::FileSystem);
}

// ---------------------------------------------------------------------------
// Req 17.2 — installazione rifiutata su permesso NON riconosciuto.
// ---------------------------------------------------------------------------
TEST(SandboxTest, RejectsInstallOnUnrecognizedPermission) {
    InMemoryViolationSink sink;
    Sandbox sandbox(&sink);

    bool approvalInvoked = false;
    InstallResult res = sandbox.installMod(
        "mod.delta", declared({"network", "teleport"}),
        [&](const ApprovalRequest& req) {
            approvalInvoked = true;
            return ApprovalDecision::grantAll(req.requested);
        });

    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error, InstallError::UnrecognizedPermission);
    EXPECT_NE(res.message.find("teleport"), std::string::npos);
    EXPECT_FALSE(approvalInvoked);     // rifiuto prima di coinvolgere l'User
    EXPECT_FALSE(sandbox.isInstalled("mod.delta"));

    // Nessun consenso: ogni check è negato.
    EXPECT_FALSE(sandbox.check("mod.delta", Permission::Network));
}

// ---------------------------------------------------------------------------
// Req 17.2 — installazione rifiutata quando la sezione permessi è OMESSA.
// ---------------------------------------------------------------------------
TEST(SandboxTest, RejectsInstallWhenPermissionsSectionOmitted) {
    Sandbox sandbox;

    InstallResult res = sandbox.installMod(
        "mod.epsilon", omitted(),
        [](const ApprovalRequest& req) {
            return ApprovalDecision::grantAll(req.requested);
        });

    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error, InstallError::PermissionsSectionOmitted);
    EXPECT_FALSE(sandbox.isInstalled("mod.epsilon"));
}

// ---------------------------------------------------------------------------
// Req 17.6 — rifiuto dell'User: la Mod non è abilitata e lo stato resta
// invariato (nessun consenso registrato); ogni check è negato.
// ---------------------------------------------------------------------------
TEST(SandboxTest, RefusalFlowKeepsModDisabled) {
    InMemoryViolationSink sink;
    Sandbox sandbox(&sink);

    InstallResult res = sandbox.installMod(
        "mod.zeta", declared({"network", "ui"}),
        [](const ApprovalRequest&) { return ApprovalDecision::refuse(); });

    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error, InstallError::UserRefused);
    EXPECT_FALSE(sandbox.isInstalled("mod.zeta"));

    EXPECT_FALSE(sandbox.check("mod.zeta", Permission::Network));
    EXPECT_FALSE(sandbox.check("mod.zeta", Permission::UI));
    EXPECT_EQ(sink.count(), 2u);  // entrambe le operazioni negate e registrate
}

// ---------------------------------------------------------------------------
// Req 17.5 — il flusso di approvazione riceve l'elenco COMPLETO dei permessi
// richiesti e una lista vuota valida (sezione presente ma vuota) è accettata.
// ---------------------------------------------------------------------------
TEST(SandboxTest, ApprovalReceivesFullRequestedList) {
    Sandbox sandbox;

    std::vector<Permission> presented;
    InstallResult res = sandbox.installMod(
        "mod.eta", declared({"events", "hooking", "ui"}),
        [&](const ApprovalRequest& req) {
            presented = req.requested;
            return ApprovalDecision::grantAll(req.requested);
        });

    ASSERT_TRUE(res.ok);
    EXPECT_EQ(presented.size(), 3u);

    // Sezione presente ma vuota: installazione valida, nessun permesso.
    InstallResult emptyRes = sandbox.installMod(
        "mod.theta", declared({}),
        [](const ApprovalRequest& req) {
            return ApprovalDecision::grantAll(req.requested);
        });
    EXPECT_TRUE(emptyRes.ok);
    EXPECT_TRUE(emptyRes.approved.empty());
}

}  // namespace
