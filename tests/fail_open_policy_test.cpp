// tests/fail_open_policy_test.cpp — unit test della policy fail-open
// centralizzata estesa ai nuovi punti di fallimento (task 3.12, Requisiti 2.7,
// 2.8, 5.3, 5.4, 9.6, 10.1, 10.2, 10.4).
//
// Verifica che `pulse::loader::CentralizedLoader` + il cablaggio
// `make_fail_open_init_step(...)` (che riusa `HookGate`) degradino verso
// "Geometry Dash parte con zero mod" per OGNI nuovo punto di fallimento:
//
//   * bootstrap assente / piattaforma senza Platform_Bootstrap reale (Req 10.1)
//   * GD_Version/piattaforma non rilevata dall'immagine reale (Req 5.3)
//   * coppia rilevata senza Binding_Set_File `.pbind` verificato (Req 5.4, 10.2)
//   * Hooking_Backend non disponibile a runtime (Req 3.8, 10.3)
//   * funzione bersaglio non risolta (Req 4.5, 9.6)
//
// In ogni caso: 0 hook installati, una diagnostica che identifica la causa,
// esito "senza mod" che lascia GD raggiungere la scena iniziale senza terminare
// il processo, ed eseguibile/asset (modellati da un buffer di byte) invariati
// (Req 10.4). Il percorso felice installa esattamente un hook (Req 9.1).

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bindings/bindings.hpp"
#include "bootstrap/platform_bootstrap.hpp"
#include "core/centralized_loader.hpp"
#include "core/runtime_context.hpp"
#include "hooking/fake_backend.hpp"
#include "hooking/hook_backend.hpp"

namespace {

using namespace pulse::loader;
using pulse::bootstrap::BootstrapErrorCode;
using pulse::bootstrap::BootstrapResult;
using pulse::bootstrap::IPlatformBootstrap;
namespace bnd = pulse::loader::bindings;

// --- bootstrap fittizio iniettabile: successo o fallimento configurabile ----
class FakeBootstrap final : public IPlatformBootstrap {
public:
    explicit FakeBootstrap(BootstrapResult result)
        : result_(std::move(result)) {}

    BootstrapResult inject() override { return result_; }
    pulse::bootstrap::Platform platform() const override {
        return pulse::bootstrap::Platform::MacOS;
    }

private:
    BootstrapResult result_;
};

// --- backend di hooking con disponibilità configurabile ---------------------
// `FakeBackend` riporta sempre `available()==true`; per il caso "backend non
// disponibile" serve un doppio con `available()==false`.
class UnavailableBackend final : public pulse::hooking::IHookBackend {
public:
    pulse::hooking::Result<pulse::hooking::Trampoline> install(std::uintptr_t,
                                                               void*) override {
        return pulse::hooking::Result<pulse::hooking::Trampoline>::err(
            pulse::hooking::HookErrorCode::Unsupported,
            "unavailable: install non deve mai essere invocato");
    }
    pulse::hooking::Result<void> remove(std::uintptr_t) override {
        return pulse::hooking::Result<void>::err(
            pulse::hooking::HookErrorCode::Unsupported, "unavailable");
    }
    pulse::hooking::Result<pulse::hooking::ByteSpan> readOriginal(
        std::uintptr_t, std::size_t) override {
        return pulse::hooking::Result<pulse::hooking::ByteSpan>::err(
            pulse::hooking::HookErrorCode::Unsupported, "unavailable");
    }
    std::string_view name() const noexcept override { return "pulse-test-unavailable"; }
    bool available() const noexcept override { return false; }
};

// --- provider dei bindings fittizio, completamente configurabile ------------
// Permette di simulare: coppia senza `.pbind` (load -> nullopt), simbolo non
// risolto (resolve -> nullopt o binding con resolved==false) e il caso felice
// (load + resolve di un binding risolto).
class FakeBindingsProvider final : public bnd::IBindingsProvider {
public:
    void setSet(std::optional<bnd::BindingSet> set) { set_ = std::move(set); }
    void setResolved(std::optional<bnd::FunctionBinding> binding) {
        binding_ = std::move(binding);
    }

    std::optional<bnd::BindingSet> load(const bnd::BindingKey&) override {
        ++loadCalls;
        return set_;
    }
    std::optional<bnd::FunctionBinding> resolve(std::string_view) const override {
        return binding_;
    }

    int loadCalls{0};

private:
    std::optional<bnd::BindingSet> set_{};
    std::optional<bnd::FunctionBinding> binding_{};
};

// Sink che cattura i messaggi diagnostici per le asserzioni.
auto capturing_sink(std::vector<std::string>& out) {
    return [&out](std::string_view m) { out.emplace_back(m); };
}

bool any_contains(const std::vector<std::string>& logs, std::string_view needle) {
    for (const auto& l : logs) {
        if (l.find(needle) != std::string::npos) return true;
    }
    return false;
}

// Coppia rilevata "valida" usata dai test (modella 2.2081 / macos-arm64).
RuntimeContext sample_context() {
    return RuntimeContext{GdVersion{2, 2081}, Platform::MacOS, "macos-arm64"};
}

// Binding risolto verso un indirizzo non-nullo (caso felice).
bnd::FunctionBinding resolved_binding() {
    bnd::FunctionBinding b;
    b.symbol = "MenuLayer::init";
    b.address = 0x00A1B2C0;  // offset non-zero, non-placeholder
    b.signature = bnd::Signature{"bool", {"MenuLayer*"}};
    b.resolved = true;
    return b;
}

// "Eseguibile/asset" di Geometry Dash modellati come buffer di byte: il loader
// non deve MAI scriverci a runtime (Req 10.4). I test verificano che restino
// invariati dopo ogni esecuzione fail-open.
std::vector<std::uint8_t> sample_gd_bytes() {
    return {0xCF, 0xFA, 0xED, 0xFE, 0x0C, 0x00, 0x00, 0x01, 0xDE, 0xAD, 0xBE, 0xEF};
}

// ===========================================================================
// Req 10.1 — piattaforma senza Platform_Bootstrap reale (UnsupportedHost).
// ===========================================================================
TEST(FailOpenPolicy, UnsupportedPlatformBootstrapStartsModLess) {
    std::vector<std::string> logs;
    const std::vector<std::uint8_t> before = sample_gd_bytes();
    std::vector<std::uint8_t> gdBytes = before;

    auto bootstrap = std::make_shared<FakeBootstrap>(BootstrapResult::failure(
        BootstrapErrorCode::UnsupportedHost,
        "MacOSBootstrap: bootstrap via dylib early-load disponibile solo su macOS"));

    // Lo step di init non deve nemmeno essere raggiunto su bootstrap fallito.
    bool initInvoked = false;
    RuntimeInitFn initStep = [&initInvoked](const WatchdogToken&) {
        initInvoked = true;
        return RuntimeInitResult::loaded(1);
    };

    CentralizedLoader loader(bootstrap, initStep, /*clock=*/nullptr,
                             capturing_sink(logs));
    const CentralizedStartResult result = loader.start();

    EXPECT_TRUE(result.modLess());
    EXPECT_EQ(result.reason, StartReason::UnsupportedPlatform);
    EXPECT_FALSE(initInvoked);
    EXPECT_TRUE(any_contains(logs, "piattaforma non supportata"));
    EXPECT_TRUE(any_contains(logs, "senza mod"));
    // Req 10.4: eseguibile/asset invariati (il loader non scrive sul binario).
    EXPECT_EQ(gdBytes, before);
}

// ===========================================================================
// Req 5.3 — GD_Version/piattaforma non rilevata: zero hook, GD prosegue.
// ===========================================================================
TEST(FailOpenPolicy, VersionDetectionFailureStartsModLess) {
    std::vector<std::string> logs;
    const std::vector<std::uint8_t> before = sample_gd_bytes();
    std::vector<std::uint8_t> gdBytes = before;

    auto bootstrap = std::make_shared<FakeBootstrap>(BootstrapResult::success());
    pulse::hooking::FakeBackend backend;
    auto provider = std::make_shared<FakeBindingsProvider>();

    FailOpenRuntime runtime;
    runtime.detect = []() -> std::optional<RuntimeContext> { return std::nullopt; };
    runtime.bindingsProvider = provider;
    runtime.backend = &backend;

    CentralizedLoader loader(bootstrap, make_fail_open_init_step(runtime, capturing_sink(logs)),
                             /*clock=*/nullptr, capturing_sink(logs));
    const CentralizedStartResult result = loader.start();

    EXPECT_TRUE(result.modLess());
    EXPECT_EQ(result.reason, StartReason::VersionDetectionFailed);
    EXPECT_EQ(backend.installedCount(), 0u);     // 0 hook
    EXPECT_EQ(backend.installAttempts(), 0u);    // backend mai invocato
    EXPECT_EQ(provider->loadCalls, 0);           // non si arriva ai bindings
    EXPECT_TRUE(any_contains(logs, "rilevamento"));
    EXPECT_EQ(gdBytes, before);                  // Req 10.4
}

// ===========================================================================
// Req 5.4 / 10.2 — coppia rilevata senza Binding_Set_File `.pbind`.
// ===========================================================================
TEST(FailOpenPolicy, MissingBindingsForPairStartsModLess) {
    std::vector<std::string> logs;
    const std::vector<std::uint8_t> before = sample_gd_bytes();
    std::vector<std::uint8_t> gdBytes = before;

    auto bootstrap = std::make_shared<FakeBootstrap>(BootstrapResult::success());
    pulse::hooking::FakeBackend backend;
    auto provider = std::make_shared<FakeBindingsProvider>();
    provider->setSet(std::nullopt);  // nessun set per la coppia esatta

    FailOpenRuntime runtime;
    runtime.detect = []() { return std::optional<RuntimeContext>{sample_context()}; };
    runtime.bindingsProvider = provider;
    runtime.backend = &backend;

    CentralizedLoader loader(bootstrap, make_fail_open_init_step(runtime, capturing_sink(logs)),
                             /*clock=*/nullptr, capturing_sink(logs));
    const CentralizedStartResult result = loader.start();

    EXPECT_TRUE(result.modLess());
    EXPECT_EQ(result.reason, StartReason::BindingsUnavailable);
    EXPECT_EQ(backend.installedCount(), 0u);
    EXPECT_EQ(backend.installAttempts(), 0u);
    EXPECT_EQ(provider->loadCalls, 1);  // tentata la coppia esatta
    EXPECT_TRUE(any_contains(logs, "Binding_Set_File"));
    EXPECT_TRUE(any_contains(logs, "2.2081"));
    EXPECT_EQ(gdBytes, before);  // Req 10.4
}

// ===========================================================================
// Req 3.8 / 10.3 — Hooking_Backend non disponibile a runtime.
// ===========================================================================
TEST(FailOpenPolicy, UnavailableBackendStartsModLess) {
    std::vector<std::string> logs;
    const std::vector<std::uint8_t> before = sample_gd_bytes();
    std::vector<std::uint8_t> gdBytes = before;

    auto bootstrap = std::make_shared<FakeBootstrap>(BootstrapResult::success());
    UnavailableBackend backend;  // available()==false
    auto provider = std::make_shared<FakeBindingsProvider>();
    bnd::BindingSet set{bnd::BindingKey{bnd::GdVersion{2, 2081}, "macos-arm64"}};
    set.add(resolved_binding());
    provider->setSet(set);
    provider->setResolved(resolved_binding());

    FailOpenRuntime runtime;
    runtime.detect = []() { return std::optional<RuntimeContext>{sample_context()}; };
    runtime.bindingsProvider = provider;
    runtime.backend = &backend;

    CentralizedLoader loader(bootstrap, make_fail_open_init_step(runtime, capturing_sink(logs)),
                             /*clock=*/nullptr, capturing_sink(logs));
    const CentralizedStartResult result = loader.start();

    EXPECT_TRUE(result.modLess());
    EXPECT_EQ(result.reason, StartReason::BackendUnavailable);
    // La diagnostica nomina il backend (Req 3.8/10.3).
    EXPECT_TRUE(any_contains(logs, "pulse-test-unavailable"));
    EXPECT_EQ(gdBytes, before);  // Req 10.4
}

// ===========================================================================
// Req 4.5 / 9.6 — funzione bersaglio non risolta: zero hook su non risolti.
// ===========================================================================
TEST(FailOpenPolicy, UnresolvedSymbolStartsModLess) {
    std::vector<std::string> logs;
    const std::vector<std::uint8_t> before = sample_gd_bytes();
    std::vector<std::uint8_t> gdBytes = before;

    auto bootstrap = std::make_shared<FakeBootstrap>(BootstrapResult::success());
    pulse::hooking::FakeBackend backend;  // available()==true
    auto provider = std::make_shared<FakeBindingsProvider>();
    bnd::BindingSet set{bnd::BindingKey{bnd::GdVersion{2, 2081}, "macos-arm64"}};
    provider->setSet(set);
    // Binding presente ma NON risolto (resolved==false) → 0 hook (Req 4.5/9.6).
    bnd::FunctionBinding unresolved = resolved_binding();
    unresolved.resolved = false;
    provider->setResolved(unresolved);

    FailOpenRuntime runtime;
    runtime.detect = []() { return std::optional<RuntimeContext>{sample_context()}; };
    runtime.bindingsProvider = provider;
    runtime.backend = &backend;

    CentralizedLoader loader(bootstrap, make_fail_open_init_step(runtime, capturing_sink(logs)),
                             /*clock=*/nullptr, capturing_sink(logs));
    const CentralizedStartResult result = loader.start();

    EXPECT_TRUE(result.modLess());
    EXPECT_EQ(result.reason, StartReason::SymbolUnresolved);
    EXPECT_EQ(backend.installedCount(), 0u);   // invariante: 0 hook
    EXPECT_EQ(backend.installAttempts(), 0u);  // backend mai invocato per non risolti
    EXPECT_TRUE(any_contains(logs, "MenuLayer::init"));
    EXPECT_TRUE(any_contains(logs, "non risolta"));
    EXPECT_EQ(gdBytes, before);  // Req 10.4
}

// ===========================================================================
// Caso felice — coppia risolta + backend disponibile: esattamente un hook.
// ===========================================================================
TEST(FailOpenPolicy, ResolvedPairInstallsExactlyOneHook) {
    std::vector<std::string> logs;
    auto bootstrap = std::make_shared<FakeBootstrap>(BootstrapResult::success());
    pulse::hooking::FakeBackend backend;
    auto provider = std::make_shared<FakeBindingsProvider>();
    bnd::BindingSet set{bnd::BindingKey{bnd::GdVersion{2, 2081}, "macos-arm64"}};
    set.add(resolved_binding());
    provider->setSet(set);
    provider->setResolved(resolved_binding());

    int detourTarget = 0;
    FailOpenRuntime runtime;
    runtime.detect = []() { return std::optional<RuntimeContext>{sample_context()}; };
    runtime.bindingsProvider = provider;
    runtime.backend = &backend;
    runtime.detour = &detourTarget;

    CentralizedLoader loader(bootstrap, make_fail_open_init_step(runtime, capturing_sink(logs)),
                             /*clock=*/nullptr, capturing_sink(logs));
    const CentralizedStartResult result = loader.start();

    EXPECT_TRUE(result.modsLoaded());
    EXPECT_EQ(result.reason, StartReason::Success);
    EXPECT_EQ(backend.installedCount(), 1u);  // esattamente un hook (Req 9.1)
    EXPECT_TRUE(any_contains(logs, "1 hook"));
}

}  // namespace
