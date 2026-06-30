// tests/hook_gate_test.cpp — unit test del gating degli hook su
// `binding.resolved` (task 9.1, Requisiti 20.3, 20.4).
//
// Verifica la politica di gating dell'Hooking Engine (design → "Layer 2 —
// Bindings System"): l'engine controlla `FunctionBinding::resolved` PRIMA di
// invocare il backend; su binding assente o non risolto NON installa alcun
// hook (mantiene 0 hook su indirizzi non risolti, Req 20.4) e fa emergere un
// errore di incompatibilità (Req 20.3). Un binding risolto, invece, viene
// installato regolarmente.
//
// Il test usa il `FakeBackend` in-memory (loader/hooking/) per osservare in
// modo deterministico se e quante install raggiungono il backend.

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bindings/bindings.hpp"
#include "hooking/fake_backend.hpp"
#include "hooking/hook_gate.hpp"

namespace {

using pulse::hooking::FakeBackend;
using pulse::hooking::GateOutcome;
using pulse::hooking::HookGate;
using pulse::hooking::binding_is_installable;
using pulse::hooking::gate_install;
using pulse::loader::bindings::FunctionBinding;
using pulse::loader::bindings::Signature;

int g_detour = 0;
void* const kDetour = &g_detour;

// Backend di test minimale con disponibilità commutabile. Implementa
// direttamente IHookBackend (FakeBackend è `final` e non derivabile) e traccia
// i tentativi di install per verificare che il gate di disponibilità non
// raggiunga mai il backend (Req 3.8/10.3). Espone un `name()` personalizzato
// per verificare che la diagnostica nomini il backend.
class ToggleableBackend final : public pulse::hooking::IHookBackend {
public:
    using Trampoline = pulse::hooking::Trampoline;
    using ByteSpan = pulse::hooking::ByteSpan;
    using HookErrorCode = pulse::hooking::HookErrorCode;
    template <class T>
    using Result = pulse::hooking::Result<T>;

    explicit ToggleableBackend(bool available = true,
                               std::string name = "toggle-backend")
        : available_(available), name_(std::move(name)) {}

    void setAvailable(bool available) noexcept { available_ = available; }

    [[nodiscard]] std::size_t installAttempts() const noexcept {
        return installAttempts_;
    }
    [[nodiscard]] std::size_t installedCount() const noexcept {
        return installedCount_;
    }

    Result<Trampoline> install(std::uintptr_t target, void* /*detour*/) override {
        ++installAttempts_;
        ++installedCount_;
        return Result<Trampoline>::ok(
            Trampoline{reinterpret_cast<void*>(target ^ 0xA5A5A5A5ULL)});
    }
    Result<void> remove(std::uintptr_t /*target*/) override {
        return Result<void>::ok();
    }
    Result<ByteSpan> readOriginal(std::uintptr_t /*target*/,
                                  std::size_t len) override {
        return Result<ByteSpan>::ok(
            ByteSpan{std::vector<std::uint8_t>(len, 0u)});
    }
    [[nodiscard]] std::string_view name() const noexcept override {
        return name_;
    }
    [[nodiscard]] bool available() const noexcept override { return available_; }

private:
    bool available_;
    std::string name_;
    std::size_t installAttempts_{0};
    std::size_t installedCount_{0};
};

// Costruisce un binding di prova con stato `resolved` e indirizzo dati.
FunctionBinding makeBinding(std::string symbol, std::uintptr_t address,
                            bool resolved) {
    FunctionBinding b;
    b.symbol = std::move(symbol);
    b.address = address;
    b.signature = Signature{"bool", {"MenuLayer*"}};
    b.resolved = resolved;
    return b;
}

// --- predicato puro: installabile sse presente E risolto -------------------
TEST(HookGate, PredicateRejectsUnresolvedAndMissing) {
    EXPECT_TRUE(binding_is_installable(makeBinding("A::f", 0x1000, true)));
    EXPECT_FALSE(binding_is_installable(makeBinding("A::f", 0x1000, false)));

    std::optional<FunctionBinding> missing;
    EXPECT_FALSE(binding_is_installable(missing));

    std::optional<FunctionBinding> resolved = makeBinding("A::f", 0x1000, true);
    EXPECT_TRUE(binding_is_installable(resolved));
}

// --- binding NON risolto: nessun install + errore di incompatibilità -------
TEST(HookGate, UnresolvedBindingBlocksInstallAndReportsIncompatibility) {
    FakeBackend backend;
    HookGate gate{backend};

    const auto binding = makeBinding("MenuLayer::init", 0x314000, /*resolved=*/false);
    const auto result = gate.install(binding, kDetour);

    // Esito: incompatibilità, nessun hook installato.
    EXPECT_EQ(result.outcome, GateOutcome::IncompatibleUnresolved);
    EXPECT_TRUE(result.incompatible());
    EXPECT_FALSE(result.installed());

    // Errore di incompatibilità riportato (Req 20.3) e riferisce il simbolo.
    EXPECT_FALSE(result.error.message.empty());
    EXPECT_NE(result.error.message.find("MenuLayer::init"), std::string::npos);

    // INVARIANTE Req 20.4: 0 hook su indirizzi non risolti. Il backend non è
    // stato nemmeno sfiorato (nessun tentativo di install).
    EXPECT_EQ(backend.installedCount(), 0u);
    EXPECT_EQ(backend.installAttempts(), 0u);
    EXPECT_FALSE(backend.isInstalled(0x314000));
    EXPECT_EQ(gate.installedCount(), 0u);
    EXPECT_EQ(gate.blockedUnresolvedCount(), 1u);
    EXPECT_EQ(gate.hooksOnUnresolvedAddresses(), 0u);
}

// --- binding assente (nessuna corrispondenza esatta): stesso gating --------
TEST(HookGate, MissingBindingBlocksInstall) {
    FakeBackend backend;
    HookGate gate{backend};

    const std::optional<FunctionBinding> missing;  // nessuna corrispondenza
    const auto result = gate.install(missing, kDetour);

    EXPECT_TRUE(result.incompatible());
    EXPECT_FALSE(result.error.message.empty());
    EXPECT_EQ(backend.installedCount(), 0u);
    EXPECT_EQ(backend.installAttempts(), 0u);
    EXPECT_EQ(gate.hooksOnUnresolvedAddresses(), 0u);
}

// --- più binding non risolti: l'invariante resta 0 hook --------------------
TEST(HookGate, MultipleUnresolvedBindingsKeepZeroHooks) {
    FakeBackend backend;
    HookGate gate{backend};

    gate.install(makeBinding("A::f", 0x1000, false), kDetour);
    gate.install(makeBinding("B::g", 0x2000, false), kDetour);
    gate.install(std::optional<FunctionBinding>{}, kDetour);

    EXPECT_EQ(backend.installedCount(), 0u);
    EXPECT_EQ(backend.installAttempts(), 0u);
    EXPECT_EQ(gate.installedCount(), 0u);
    EXPECT_EQ(gate.blockedUnresolvedCount(), 3u);
    EXPECT_EQ(gate.hooksOnUnresolvedAddresses(), 0u);
}

// --- controllo positivo: binding risolto -> install eseguito ---------------
TEST(HookGate, ResolvedBindingInstallsHook) {
    FakeBackend backend;
    HookGate gate{backend};

    const auto binding = makeBinding("MenuLayer::init", 0x314000, /*resolved=*/true);
    const auto result = gate.install(binding, kDetour);

    EXPECT_EQ(result.outcome, GateOutcome::Installed);
    EXPECT_TRUE(result.installed());
    EXPECT_TRUE(result.trampoline.valid());

    EXPECT_TRUE(backend.isInstalled(0x314000));
    EXPECT_EQ(backend.installedCount(), 1u);
    EXPECT_EQ(gate.installedCount(), 1u);
    EXPECT_EQ(gate.hooksOnUnresolvedAddresses(), 0u);
}

// --- gate superato ma backend fallisce: errore propagato, 0 hook -----------
TEST(HookGate, ResolvedBindingPropagatesBackendInstallFailure) {
    FakeBackend backend;
    backend.failAllInstalls(true);
    HookGate gate{backend};

    const auto binding = makeBinding("MenuLayer::init", 0x314000, /*resolved=*/true);
    const auto result = gate.install(binding, kDetour);

    EXPECT_EQ(result.outcome, GateOutcome::InstallFailed);
    EXPECT_FALSE(result.installed());
    EXPECT_FALSE(result.error.message.empty());
    EXPECT_EQ(backend.installedCount(), 0u);
    EXPECT_EQ(gate.installedCount(), 0u);
}

// --- funzione libera di convenienza: stesso gating senza istanziare HookGate
TEST(HookGate, FreeFunctionGateInstallRespectsResolution) {
    FakeBackend backend;

    const auto blocked =
        gate_install(backend, makeBinding("X::y", 0x4000, false), kDetour);
    EXPECT_TRUE(blocked.incompatible());
    EXPECT_EQ(backend.installedCount(), 0u);
    EXPECT_EQ(backend.installAttempts(), 0u);

    const auto installed =
        gate_install(backend, makeBinding("X::y", 0x4000, true), kDetour);
    EXPECT_TRUE(installed.installed());
    EXPECT_EQ(backend.installedCount(), 1u);
}

// ===========================================================================
// Gate di disponibilità del backend (Req 3.8, 10.3) — feature
// pulse-gd-integration, task 3.3. Quando `IHookBackend::available()` riporta
// false al runtime, il gate blocca OGNI install, mantiene 0 hook e registra
// una diagnostica che NOMINA il backend via `name()`.
// ===========================================================================

// --- backend non disponibile: install bloccato anche con binding RISOLTO ---
TEST(HookGate, UnavailableBackendBlocksInstallEvenWhenResolved) {
    ToggleableBackend backend{/*available=*/false, /*name=*/"pulse-dobby"};
    std::vector<std::string> diagnostics;
    HookGate gate{backend, [&diagnostics](std::string_view m) {
                      diagnostics.emplace_back(m);
                  }};

    // Binding perfettamente risolto: l'unica causa del blocco è il backend.
    const auto binding = makeBinding("MenuLayer::init", 0x314000, /*resolved=*/true);
    const auto result = gate.install(binding, kDetour);

    // Esito: backend non disponibile, nessun hook installato.
    EXPECT_EQ(result.outcome, GateOutcome::BackendUnavailable);
    EXPECT_TRUE(result.backendUnavailable());
    EXPECT_FALSE(result.installed());

    // Diagnostica che NOMINA il backend (Req 3.8).
    EXPECT_FALSE(result.error.message.empty());
    EXPECT_NE(result.error.message.find("pulse-dobby"), std::string::npos);

    // La diagnostica è stata REGISTRATA sul sink (Req 3.8/10.3).
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_NE(diagnostics.front().find("pulse-dobby"), std::string::npos);

    // INVARIANTE: 0 hook. Il backend non è stato nemmeno sfiorato.
    EXPECT_EQ(backend.installedCount(), 0u);
    EXPECT_EQ(backend.installAttempts(), 0u);
    EXPECT_EQ(gate.installedCount(), 0u);
    EXPECT_EQ(gate.blockedUnavailableCount(), 1u);
    EXPECT_EQ(gate.hooksOnUnresolvedAddresses(), 0u);
}

// --- backend non disponibile + binding assente/opzionale: stesso gate -------
TEST(HookGate, UnavailableBackendBlocksInstallForMissingBinding) {
    ToggleableBackend backend{/*available=*/false, /*name=*/"pulse-shadowhook"};
    HookGate gate{backend};

    const std::optional<FunctionBinding> missing;
    const auto result = gate.install(missing, kDetour);

    // Il gate di disponibilità precede la corrispondenza del binding.
    EXPECT_EQ(result.outcome, GateOutcome::BackendUnavailable);
    EXPECT_TRUE(result.backendUnavailable());
    EXPECT_NE(result.error.message.find("pulse-shadowhook"), std::string::npos);
    EXPECT_EQ(backend.installAttempts(), 0u);
    EXPECT_EQ(gate.blockedUnavailableCount(), 1u);
}

// --- più richieste con backend non disponibile: l'invariante resta 0 hook ---
TEST(HookGate, UnavailableBackendKeepsZeroHooksAcrossManyRequests) {
    ToggleableBackend backend{/*available=*/false};
    HookGate gate{backend};

    gate.install(makeBinding("A::f", 0x1000, true), kDetour);
    gate.install(makeBinding("B::g", 0x2000, true), kDetour);
    gate.install(std::optional<FunctionBinding>{}, kDetour);

    EXPECT_EQ(backend.installedCount(), 0u);
    EXPECT_EQ(backend.installAttempts(), 0u);
    EXPECT_EQ(gate.installedCount(), 0u);
    EXPECT_EQ(gate.blockedUnavailableCount(), 3u);
    EXPECT_EQ(gate.hooksOnUnresolvedAddresses(), 0u);
}

// --- controllo positivo: backend disponibile -> il gate non blocca ----------
TEST(HookGate, AvailableBackendDoesNotBlockOnAvailability) {
    ToggleableBackend backend{/*available=*/true};
    HookGate gate{backend};

    const auto binding = makeBinding("MenuLayer::init", 0x314000, /*resolved=*/true);
    const auto result = gate.install(binding, kDetour);

    EXPECT_EQ(result.outcome, GateOutcome::Installed);
    EXPECT_TRUE(result.installed());
    EXPECT_EQ(backend.installedCount(), 1u);
    EXPECT_EQ(gate.blockedUnavailableCount(), 0u);
}

// --- diventato disponibile a runtime: gli install riprendono ----------------
TEST(HookGate, BackendBecomingAvailableResumesInstall) {
    ToggleableBackend backend{/*available=*/false};
    HookGate gate{backend};

    const auto binding = makeBinding("MenuLayer::init", 0x314000, /*resolved=*/true);
    const auto blocked = gate.install(binding, kDetour);
    EXPECT_TRUE(blocked.backendUnavailable());
    EXPECT_EQ(backend.installedCount(), 0u);

    backend.setAvailable(true);
    const auto installed = gate.install(binding, kDetour);
    EXPECT_TRUE(installed.installed());
    EXPECT_EQ(backend.installedCount(), 1u);
    EXPECT_EQ(gate.blockedUnavailableCount(), 1u);
}

// --- funzione libera: il gate di disponibilità vale anche senza HookGate -----
TEST(HookGate, FreeFunctionGateRespectsBackendAvailability) {
    ToggleableBackend backend{/*available=*/false, /*name=*/"pulse-minhook"};
    std::vector<std::string> diagnostics;

    const auto blocked = gate_install(
        backend, makeBinding("X::y", 0x4000, true), kDetour,
        [&diagnostics](std::string_view m) { diagnostics.emplace_back(m); });

    EXPECT_TRUE(blocked.backendUnavailable());
    EXPECT_NE(blocked.error.message.find("pulse-minhook"), std::string::npos);
    EXPECT_EQ(backend.installAttempts(), 0u);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_NE(diagnostics.front().find("pulse-minhook"), std::string::npos);
}

}  // namespace
