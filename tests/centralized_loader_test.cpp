// tests/centralized_loader_test.cpp — unit test dell'entry point centralizzato
// del runtime (task 23.4, Requisiti 1.3, 1.4, 1.6).
//
// Verifica `pulse::loader::CentralizedLoader::start()`:
//   * Req 1.3 — su iniezione di piattaforma fallita, la diagnostica è loggata e
//     il gioco parte SENZA mod (esito ModLess), senza crash/abort del processo;
//   * Req 1.4 — sul percorso felice il caricamento delle mod avviene tramite un
//     UNICO entry point (lo step di init è invocato esattamente una volta) e
//     l'esito è ModsLoaded;
//   * Req 1.6 — se l'inizializzazione supera il budget di 10 s, il watchdog
//     interrompe il caricamento, logga il timeout e il gioco parte senza mod.
//
// Il watchdog è reso deterministico iniettando un clock fittizio: lo step di
// init avanza il clock oltre il budget per forzare il timeout senza dormire.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "bootstrap/platform_bootstrap.hpp"
#include "core/centralized_loader.hpp"

namespace {

using namespace pulse::loader;
using pulse::bootstrap::BootstrapErrorCode;
using pulse::bootstrap::BootstrapResult;
using pulse::bootstrap::IPlatformBootstrap;
using pulse::bootstrap::Platform;

// --- bootstrap fittizio iniettabile: successo o fallimento configurabile ----
class FakeBootstrap final : public IPlatformBootstrap {
public:
    explicit FakeBootstrap(BootstrapResult result,
                           Platform platform = Platform::WindowsX64)
        : result_(std::move(result)), platform_(platform) {}

    BootstrapResult inject() override {
        ++injectCalls;
        return result_;
    }
    Platform platform() const override { return platform_; }

    int injectCalls{0};

private:
    BootstrapResult result_;
    Platform platform_;
};

// --- clock fittizio controllabile: `now()` restituisce un istante mutabile ---
class FakeClock {
public:
    ClockFn fn() {
        return [this]() { return now_; };
    }
    void advance(std::chrono::milliseconds delta) { now_ += delta; }

private:
    std::chrono::steady_clock::time_point now_{};
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

// === Req 1.3 — iniezione fallita: log + avvio senza mod, nessun crash =======
TEST(CentralizedLoader, InjectionFailureLogsAndStartsModLess) {
    std::vector<std::string> logs;
    auto bootstrap = std::make_shared<FakeBootstrap>(BootstrapResult::failure(
        BootstrapErrorCode::ProxyChainLoadFailed,
        "impossibile caricare la DLL proxy di sistema"));

    bool initInvoked = false;
    InitStepFn initStep = [&initInvoked](const WatchdogToken&) {
        initInvoked = true;  // NON deve essere eseguito su iniezione fallita
        return true;
    };

    CentralizedLoader loader(bootstrap, initStep, /*clock=*/nullptr,
                             capturing_sink(logs));

    const CentralizedStartResult result = loader.start();

    EXPECT_TRUE(result.modLess());
    EXPECT_EQ(result.mode, StartMode::ModLess);
    EXPECT_EQ(result.reason, StartReason::InjectionFailed);
    EXPECT_FALSE(initInvoked);  // nessun caricamento mod dopo iniezione fallita
    EXPECT_EQ(bootstrap->injectCalls, 1);
    // La diagnostica identifica la causa e l'avvio senza mod (Req 1.3).
    EXPECT_TRUE(any_contains(logs, "iniezione fallita"));
    EXPECT_TRUE(any_contains(logs, "senza mod"));
    EXPECT_TRUE(any_contains(logs, "DLL proxy"));
}

// === Req 1.4 — percorso felice: mod caricate tramite un unico entry point ===
TEST(CentralizedLoader, SuccessfulPathLoadsModsViaSingleEntryPoint) {
    std::vector<std::string> logs;
    auto bootstrap = std::make_shared<FakeBootstrap>(BootstrapResult::success());

    int initCalls = 0;
    InitStepFn initStep = [&initCalls](const WatchdogToken& token) {
        ++initCalls;
        EXPECT_FALSE(token.expired());  // entro il budget all'avvio
        return true;
    };

    FakeClock clock;  // resta entro il budget (non avanzato)
    CentralizedLoader loader(bootstrap, initStep, clock.fn(), capturing_sink(logs));

    const CentralizedStartResult result = loader.start();

    EXPECT_TRUE(result.modsLoaded());
    EXPECT_EQ(result.mode, StartMode::ModsLoaded);
    EXPECT_EQ(result.reason, StartReason::Success);
    // Un UNICO entry point: lo step di caricamento è invocato esattamente una
    // volta (Req 1.4).
    EXPECT_EQ(initCalls, 1);
    EXPECT_EQ(bootstrap->injectCalls, 1);
    EXPECT_TRUE(any_contains(logs, "mod caricate"));
}

// === Req 1.6 — init oltre 10 s: timeout diagnosticato + avvio senza mod =====
TEST(CentralizedLoader, InitExceedingBudgetTimesOutAndStartsModLess) {
    std::vector<std::string> logs;
    auto bootstrap = std::make_shared<FakeBootstrap>(BootstrapResult::success());

    FakeClock clock;
    // Lo step simula un'inizializzazione che impiega 11 s, oltre il budget di
    // 10 s: avanza il clock fittizio per forzare il timeout in modo
    // deterministico, senza dormire.
    InitStepFn initStep = [&clock](const WatchdogToken& token) {
        clock.advance(std::chrono::seconds(11));
        EXPECT_TRUE(token.expired());  // il watchdog è scaduto
        return true;                   // anche se "completa", il budget è violato
    };

    CentralizedLoader loader(bootstrap, initStep, clock.fn(), capturing_sink(logs));

    const CentralizedStartResult result = loader.start();

    EXPECT_TRUE(result.modLess());
    EXPECT_EQ(result.mode, StartMode::ModLess);
    EXPECT_EQ(result.reason, StartReason::InitTimeout);
    // La diagnostica identifica il timeout di inizializzazione e l'avvio senza
    // mod (Req 1.6).
    EXPECT_TRUE(any_contains(logs, "10 s"));
    EXPECT_TRUE(any_contains(logs, "senza mod"));
}

// === Confine del watchdog: init esattamente al budget non è timeout ==========
TEST(CentralizedLoader, InitExactlyAtBudgetIsNotTimeout) {
    std::vector<std::string> logs;
    auto bootstrap = std::make_shared<FakeBootstrap>(BootstrapResult::success());

    FakeClock clock;
    InitStepFn initStep = [&clock](const WatchdogToken&) {
        clock.advance(std::chrono::seconds(10));  // esattamente al budget
        return true;
    };

    CentralizedLoader loader(bootstrap, initStep, clock.fn(), capturing_sink(logs));

    const CentralizedStartResult result = loader.start();

    EXPECT_TRUE(result.modsLoaded());
    EXPECT_EQ(result.reason, StartReason::Success);
}

// === Init fallita entro il budget (es. Req 1.7): avvio senza mod ============
TEST(CentralizedLoader, InitFailureWithinBudgetStartsModLess) {
    std::vector<std::string> logs;
    auto bootstrap = std::make_shared<FakeBootstrap>(BootstrapResult::success());

    InitStepFn initStep = [](const WatchdogToken&) {
        return false;  // es. GD_Version non rilevata
    };

    FakeClock clock;
    CentralizedLoader loader(bootstrap, initStep, clock.fn(), capturing_sink(logs));

    const CentralizedStartResult result = loader.start();

    EXPECT_TRUE(result.modLess());
    EXPECT_EQ(result.reason, StartReason::InitFailed);
    EXPECT_TRUE(any_contains(logs, "senza mod"));
}

// === Bootstrap nullo: equivale a iniezione riuscita (scenario solo runtime) ==
TEST(CentralizedLoader, NullBootstrapIsTreatedAsInjected) {
    std::vector<std::string> logs;
    int initCalls = 0;
    InitStepFn initStep = [&initCalls](const WatchdogToken&) {
        ++initCalls;
        return true;
    };

    FakeClock clock;
    CentralizedLoader loader(/*bootstrap=*/nullptr, initStep, clock.fn(),
                             capturing_sink(logs));

    const CentralizedStartResult result = loader.start();

    EXPECT_TRUE(result.modsLoaded());
    EXPECT_EQ(initCalls, 1);
}

}  // namespace
