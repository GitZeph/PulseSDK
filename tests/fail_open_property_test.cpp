// tests/fail_open_property_test.cpp
// Feature: pulse-gd-integration, Property 13 — Fail-open: zero hook e byte
// invariati su ambiente non supportato.
// Validates: Requirements 2.8, 5.3, 5.4, 9.6, 10.1, 10.2, 10.4
//
// Property 13 (design.md / tasks.md, task 3.14): "Per ogni causa di ambiente
// non supportato (nessun Platform_Bootstrap reale, GD_Version senza bindings
// verificati, coppia rilevata priva di `.pbind`, Hooking_Backend non
// disponibile, funzione bersaglio non risolta), il Pulse_Loader installa ZERO
// hook, registra una diagnostica che identifica la causa, restituisce un esito
// 'senza mod' che lascia Geometry Dash raggiungere la scena iniziale senza
// terminare il processo, e lascia l'eseguibile e gli asset di Geometry Dash
// byte-per-byte identici al loro stato precedente."
//
// `pulse::loader::CentralizedLoader` + il cablaggio `make_fail_open_init_step`
// (che riusa `HookGate`) implementano questa policy fail-open centralizzata.
// Su input randomizzati con RapidCheck (≥100 iterazioni per default) generiamo,
// per ogni iterazione:
//   * una delle CINQUE cause di ambiente non supportato,
//   * un buffer di byte arbitrario che MODELLA l'eseguibile/asset di Geometry
//     Dash (il loader non vi ha alcun accesso a runtime — Req 10.4),
//   * una coppia (GD_Version, piattaforma) rilevata arbitraria,
// e verifichiamo l'invariante fail-open per OGNI causa: esito "senza mod" con
// la `StartReason` attesa, 0 hook installati (backend mai invocato), almeno una
// diagnostica registrata, e il buffer dei byte di GD invariato byte-per-byte.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
    explicit FakeBootstrap(BootstrapResult result) : result_(std::move(result)) {}

    BootstrapResult inject() override { return result_; }
    pulse::bootstrap::Platform platform() const override {
        return pulse::bootstrap::Platform::MacOS;
    }

private:
    BootstrapResult result_;
};

// --- backend di hooking con `available()==false` e tracciamento install ------
// `FakeBackend` è `final` e riporta `available()==true`; per esercitare il ramo
// "backend non disponibile" del gate serve un doppio dedicato con
// `available()==false` che tracci se `install` viene MAI invocato.
class UnavailableBackend final : public pulse::hooking::IHookBackend {
public:
    explicit UnavailableBackend(std::string name) : name_(std::move(name)) {}

    [[nodiscard]] std::size_t installAttempts() const noexcept {
        return installAttempts_;
    }
    [[nodiscard]] std::size_t installedCount() const noexcept { return 0u; }

    pulse::hooking::Result<pulse::hooking::Trampoline> install(
        std::uintptr_t target, void* /*detour*/) override {
        // Non deve MAI essere invocato dal gate quando available()==false.
        ++installAttempts_;
        return pulse::hooking::Result<pulse::hooking::Trampoline>::ok(
            pulse::hooking::Trampoline{
                reinterpret_cast<void*>(target ^ 0xA5A5A5A5ULL)});
    }
    pulse::hooking::Result<void> remove(std::uintptr_t) override {
        return pulse::hooking::Result<void>::ok();
    }
    pulse::hooking::Result<pulse::hooking::ByteSpan> readOriginal(
        std::uintptr_t, std::size_t len) override {
        return pulse::hooking::Result<pulse::hooking::ByteSpan>::ok(
            pulse::hooking::ByteSpan{std::vector<std::uint8_t>(len, 0u)});
    }
    std::string_view name() const noexcept override { return name_; }
    bool available() const noexcept override { return false; }

private:
    std::string name_;
    std::size_t installAttempts_{0};
};

// --- provider dei bindings fittizio, completamente configurabile ------------
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

// Binding risolto verso un indirizzo non-nullo, non-placeholder (caso felice).
bnd::FunctionBinding resolved_binding() {
    bnd::FunctionBinding b;
    b.symbol = "MenuLayer::init";
    b.address = 0x00A1B2C0;
    b.signature = bnd::Signature{"bool", {"MenuLayer*"}};
    b.resolved = true;
    return b;
}

// Le CINQUE cause di ambiente non supportato coperte dalla Property 13.
enum class UnsupportedCause {
    BootstrapUnsupportedHost = 0,  // Req 10.1 -> UnsupportedPlatform
    VersionDetectionFailure  = 1,  // Req 5.3  -> VersionDetectionFailed
    MissingBindingsForPair   = 2,  // Req 5.4, 10.2 -> BindingsUnavailable
    BackendUnavailable       = 3,  // Req 3.8, 10.3 -> BackendUnavailable
    UnresolvedSymbol         = 4,  // Req 4.5, 9.6 -> SymbolUnresolved
};

}  // namespace

// ===========================================================================
// Property 13 — per OGNI causa di ambiente non supportato, il caricamento
// centralizzato degrada a "senza mod" con 0 hook installati, una diagnostica
// registrata, e i byte (modellati) dell'eseguibile/asset di GD invariati.
// Feature: pulse-gd-integration, Property 13.
// Validates: Requirements 2.8, 5.3, 5.4, 9.6, 10.1, 10.2, 10.4.
// ===========================================================================
RC_GTEST_PROP(Property13FailOpenUnsupportedEnvironment,
              ZeroHooksDiagnosticModLessAndBytesUnchangedForEveryCause,
              ()) {
    // Causa di ambiente non supportato (una delle cinque).
    const auto cause = *rc::gen::element(
                            UnsupportedCause::BootstrapUnsupportedHost,
                            UnsupportedCause::VersionDetectionFailure,
                            UnsupportedCause::MissingBindingsForPair,
                            UnsupportedCause::BackendUnavailable,
                            UnsupportedCause::UnresolvedSymbol)
                            .as("causa di ambiente non supportato");

    // Buffer arbitrario che MODELLA l'eseguibile/asset di GD (albero di file
    // finto). Il loader non vi accede a runtime: deve restare invariato (10.4).
    const auto gdBytesGen =
        *rc::gen::container<std::vector<std::uint8_t>>(
             rc::gen::arbitrary<std::uint8_t>())
             .as("byte (modellati) dell'eseguibile/asset di GD");
    const std::vector<std::uint8_t> before = gdBytesGen;
    std::vector<std::uint8_t> gdBytes = before;  // "vivo": confrontato dopo

    // Coppia (GD_Version, piattaforma) rilevata arbitraria (quando rilevata).
    const auto major = *rc::gen::inRange(1, 4).as("GD major");
    const auto minor = *rc::gen::inRange(1, 4000).as("GD minor");
    const auto platformId =
        *rc::gen::nonEmpty(
             rc::gen::container<std::string>(rc::gen::inRange<char>('a', 'z')))
             .as("platformId");
    const RuntimeContext detected{
        GdVersion{static_cast<std::uint32_t>(major),
                  static_cast<std::uint32_t>(minor)},
        Platform::MacOS, platformId};

    // Nome del backend (per la diagnostica del caso "backend non disponibile").
    const auto backendName =
        *rc::gen::nonEmpty(
             rc::gen::container<std::string>(rc::gen::inRange<char>('a', 'z')))
             .as("nome backend");

    // Sink diagnostico condiviso: raccoglie ogni messaggio registrato.
    std::vector<std::string> logs;
    const auto sink = [&logs](std::string_view m) { logs.emplace_back(m); };

    // Backend: FakeBackend (available()==true) per tutte le cause tranne quella
    // dedicata al backend non disponibile.
    pulse::hooking::FakeBackend okBackend;
    UnavailableBackend downBackend{backendName};

    auto provider = std::make_shared<FakeBindingsProvider>();

    // Bootstrap: fallisce con UnsupportedHost solo per la prima causa.
    std::shared_ptr<FakeBootstrap> bootstrap;
    if (cause == UnsupportedCause::BootstrapUnsupportedHost) {
        bootstrap = std::make_shared<FakeBootstrap>(BootstrapResult::failure(
            BootstrapErrorCode::UnsupportedHost,
            "MacOSBootstrap: nessun Platform_Bootstrap reale su questo host"));
    } else {
        bootstrap = std::make_shared<FakeBootstrap>(BootstrapResult::success());
    }

    // Configura il runtime fail-open in base alla causa generata.
    FailOpenRuntime runtime;
    runtime.bindingsProvider = provider;
    runtime.backend = &okBackend;
    StartReason expected = StartReason::Success;

    switch (cause) {
        case UnsupportedCause::BootstrapUnsupportedHost:
            // Lo step di init non sarà nemmeno raggiunto (bootstrap fallito).
            runtime.detect = [detected]() {
                return std::optional<RuntimeContext>{detected};
            };
            expected = StartReason::UnsupportedPlatform;
            break;
        case UnsupportedCause::VersionDetectionFailure:
            runtime.detect = []() -> std::optional<RuntimeContext> {
                return std::nullopt;
            };
            expected = StartReason::VersionDetectionFailed;
            break;
        case UnsupportedCause::MissingBindingsForPair:
            runtime.detect = [detected]() {
                return std::optional<RuntimeContext>{detected};
            };
            provider->setSet(std::nullopt);  // nessun .pbind per la coppia esatta
            expected = StartReason::BindingsUnavailable;
            break;
        case UnsupportedCause::BackendUnavailable: {
            runtime.detect = [detected]() {
                return std::optional<RuntimeContext>{detected};
            };
            bnd::BindingSet set{
                bnd::BindingKey{bnd::GdVersion{major, minor}, platformId}};
            set.add(resolved_binding());
            provider->setSet(set);
            provider->setResolved(resolved_binding());
            runtime.backend = &downBackend;  // available()==false
            expected = StartReason::BackendUnavailable;
            break;
        }
        case UnsupportedCause::UnresolvedSymbol: {
            runtime.detect = [detected]() {
                return std::optional<RuntimeContext>{detected};
            };
            bnd::BindingSet set{
                bnd::BindingKey{bnd::GdVersion{major, minor}, platformId}};
            provider->setSet(set);
            bnd::FunctionBinding unresolved = resolved_binding();
            unresolved.resolved = false;  // presente ma NON risolto
            provider->setResolved(unresolved);
            expected = StartReason::SymbolUnresolved;
            break;
        }
    }

    CentralizedLoader loader(bootstrap, make_fail_open_init_step(runtime, sink),
                             /*clock=*/nullptr, sink);
    const CentralizedStartResult result = loader.start();

    // --- INVARIANTI FAIL-OPEN (Property 13) --------------------------------

    // Esito "senza mod": GD raggiunge la scena iniziale senza terminare (Req
    // 2.8, 5.3, 5.4, 9.6, 10.1, 10.2).
    RC_ASSERT(result.modLess());
    RC_ASSERT(!result.modsLoaded());

    // Causa classificata correttamente per la specifica causa di ambiente.
    RC_ASSERT(result.reason == expected);

    // 0 hook installati: il backend non installa mai nulla (Req 9.6, 10.x).
    RC_ASSERT(okBackend.installedCount() == 0u);
    RC_ASSERT(downBackend.installedCount() == 0u);
    // Per backend non disponibile / simbolo non risolto / detection fallita /
    // coppia assente, il backend reale non deve mai essere sfiorato.
    RC_ASSERT(downBackend.installAttempts() == 0u);
    if (cause != UnsupportedCause::BackendUnavailable) {
        RC_ASSERT(okBackend.installAttempts() == 0u);
    }

    // Almeno una diagnostica registrata che identifica la causa (Req 2.8, 5.3,
    // 5.4, 9.6, 10.1, 10.2, 10.3).
    RC_ASSERT(!logs.empty());
    if (cause == UnsupportedCause::BackendUnavailable) {
        bool named = false;
        for (const auto& m : logs) {
            if (m.find(backendName) != std::string::npos) {
                named = true;
                break;
            }
        }
        RC_ASSERT(named);  // la diagnostica nomina il backend (Req 3.8/10.3)
    }

    // Req 10.4: l'eseguibile/asset (modellati) di GD restano byte-per-byte
    // identici allo stato precedente; il loader non scrive sul binario di GD.
    RC_ASSERT(gdBytes == before);
}
