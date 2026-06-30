// tests/crash_report_test.cpp — unit test della generazione e gestione dei
// CrashReport (task 38.1, Requisiti 21.1, 21.2, 21.3, 21.4, 21.5, 21.6).
//
// Con clock, capturer, transmitter, consenso e store TUTTI iniettati, verifica
// sull'host (senza crash reale, senza rete, senza disco):
//   * Req 21.1 — il report è generato entro 10 s e contiene la traccia dello
//     stack disponibile al momento del crash;
//   * Req 21.2 — un report attribuibile include id + versione della Mod;
//   * Req 21.3 — un report non attribuibile è contrassegnato come non attribuito
//     (attributedMod == nullopt) e conservato localmente;
//   * Req 21.4 — opt-in: senza consenso non c'è trasmissione (ritenzione locale
//     >= 30 giorni); con consenso la trasmissione riesce entro 30 s;
//   * Req 21.5 — su fallimento la trasmissione viene ritentata fino a 3 volte e,
//     se persiste, il report resta conservato localmente;
//   * Req 21.6 — senza consenso il report è conservato fino a now + 30 giorni.
//
// Header-only: include "telemetry/crash_report.hpp" (radice loader/ nella
// include path del target di test).

#include <gtest/gtest.h>

#include <optional>

#include "telemetry/crash_report.hpp"

namespace {

using pulse::telemetry::CrashReporter;
using pulse::telemetry::CrashReport;
using pulse::telemetry::InMemoryCrashReportStore;
using pulse::telemetry::ModId;
using pulse::telemetry::SemVer;
using pulse::telemetry::StackTrace;
using pulse::telemetry::TelemetryOutcome;
using pulse::telemetry::Timestamp;
using pulse::telemetry::kGenerationBudgetSeconds;
using pulse::telemetry::kLocalRetentionSeconds;
using pulse::telemetry::kMaxTransmissionRetries;
using pulse::telemetry::kTransmissionBudgetSeconds;

// Clock controllato: avanza solo quando un test lo richiede.
struct FakeClock {
    Timestamp value{1'000};
    [[nodiscard]] Timestamp operator()() { return value; }
};

const StackTrace kSampleStack{"GJBaseGameLayer::update+0x40",
                              "PlayLayer::update+0x18", "main+0x10"};

// --- Req 21.1: generazione entro il budget con traccia dello stack ---------
TEST(CrashReport, GeneratedWithinBudgetWithStackTrace) {
    auto clock = std::make_shared<FakeClock>();
    InMemoryCrashReportStore store;

    // Il capturer simula un costo di cattura di 3 s avanzando il clock.
    CrashReporter reporter{
        [clock] { return (*clock)(); },
        [clock] { clock->value += 3; return kSampleStack; },
        [](int) { return true; },
        [] { return false; },
        store};

    CrashReport report = reporter.generate();

    EXPECT_EQ(report.stack, kSampleStack);          // traccia presente (Req 21.1)
    EXPECT_FALSE(report.stack.empty());
    EXPECT_EQ(report.generationElapsedSeconds, 3);
    EXPECT_LE(report.generationElapsedSeconds, kGenerationBudgetSeconds);
    EXPECT_TRUE(report.withinGenerationBudget());   // <= 10 s (Req 21.1)
}

// --- Req 21.2: report attribuito include id + versione della Mod -----------
TEST(CrashReport, AttributedReportIncludesModIdAndVersion) {
    auto clock = std::make_shared<FakeClock>();
    InMemoryCrashReportStore store;
    CrashReporter reporter{[clock] { return (*clock)(); },
                           [] { return kSampleStack; }, [](int) { return true; },
                           [] { return true; }, store};

    const ModId mod = "com.example.coolmod";
    const SemVer version{2, 4, 1};
    CrashReport report = reporter.generate(mod, version);

    ASSERT_TRUE(report.isAttributed());             // Req 21.2
    EXPECT_EQ(report.attributedMod, mod);
    ASSERT_TRUE(report.modVersion.has_value());
    EXPECT_EQ(report.modVersion.value(), version);
    EXPECT_EQ(report.modVersion->toString(), "2.4.1");
}

// --- Req 21.3: report non attribuibile -> nullopt + ritenzione locale ------
TEST(CrashReport, UnattributedReportMarkedAndRetainedLocally) {
    auto clock = std::make_shared<FakeClock>();
    InMemoryCrashReportStore store;
    // Nessun consenso: il report non attribuito viene conservato localmente.
    CrashReporter reporter{[clock] { return (*clock)(); },
                           [] { return kSampleStack; }, [](int) { return true; },
                           [] { return false; }, store};

    auto [report, outcome] = reporter.generateAndProcess();  // nessuna Mod imputata

    EXPECT_FALSE(report.isAttributed());            // non attribuito (Req 21.3)
    EXPECT_FALSE(report.attributedMod.has_value());
    EXPECT_FALSE(report.modVersion.has_value());
    EXPECT_EQ(outcome, TelemetryOutcome::RetainedNoConsent);
    EXPECT_EQ(store.size(), 1u);                    // conservato localmente
}

// --- Req 21.4 + 21.6: opt-in. Senza consenso nessuna trasmissione ----------
TEST(CrashReport, NoConsentMeansNoTransmissionAndRetainedAtLeast30Days) {
    auto clock = std::make_shared<FakeClock>();
    clock->value = 5'000;
    InMemoryCrashReportStore store;

    int transmitCalls = 0;
    CrashReporter reporter{
        [clock] { return (*clock)(); },
        [] { return kSampleStack; },
        [&transmitCalls](int) { ++transmitCalls; return true; },
        [] { return false; },  // opt-in NON concesso
        store};

    auto [report, outcome] = reporter.generateAndProcess("com.example.mod",
                                                         SemVer{1, 0, 0});

    EXPECT_EQ(outcome, TelemetryOutcome::RetainedNoConsent);  // Req 21.4 (opt-in)
    EXPECT_EQ(transmitCalls, 0);                  // nessun tentativo di invio
    EXPECT_FALSE(report.transmitted);

    // Req 21.6 — conservato fino a >= now + 30 giorni.
    ASSERT_EQ(store.entries().size(), 1u);
    const Timestamp expiry = store.entries().front().expiry;
    EXPECT_EQ(expiry, report.when + kLocalRetentionSeconds);
    EXPECT_GE(expiry - report.when, kLocalRetentionSeconds);
    // Il report risulta ancora valido un istante prima della scadenza.
    EXPECT_EQ(store.retainedCountAt(expiry), 1u);
    EXPECT_EQ(store.retainedCountAt(expiry + 1), 0u);
}

// --- Req 21.4: con consenso la trasmissione riesce entro 30 s --------------
TEST(CrashReport, WithConsentTransmissionSucceedsWithinBudget) {
    auto clock = std::make_shared<FakeClock>();
    InMemoryCrashReportStore store;

    int transmitCalls = 0;
    CrashReporter reporter{
        [clock] { return (*clock)(); },
        [] { return kSampleStack; },
        // L'invio costa 5 s (avanza il clock) e riesce al primo tentativo.
        [clock, &transmitCalls](int) { ++transmitCalls; clock->value += 5; return true; },
        [] { return true; },  // opt-in concesso
        store};

    auto [report, outcome] = reporter.generateAndProcess("com.example.mod",
                                                         SemVer{1, 2, 3});

    EXPECT_EQ(outcome, TelemetryOutcome::Transmitted);  // Req 21.4
    EXPECT_TRUE(report.transmitted);
    EXPECT_EQ(transmitCalls, 1);
    EXPECT_EQ(report.retryCount, 0);
    EXPECT_EQ(report.transmissionElapsedSeconds, 5);
    EXPECT_LE(report.transmissionElapsedSeconds, kTransmissionBudgetSeconds);
    EXPECT_TRUE(report.withinTransmissionBudget());
    EXPECT_TRUE(store.empty());  // trasmesso: nessuna ritenzione necessaria
}

// --- Req 21.5: ritenta fino a 3 volte, poi riesce --------------------------
TEST(CrashReport, TransmissionRetriesThenSucceeds) {
    auto clock = std::make_shared<FakeClock>();
    InMemoryCrashReportStore store;

    // Fallisce ai tentativi 0,1,2; riesce al tentativo 3 (terzo ritentativo).
    CrashReporter reporter{
        [clock] { return (*clock)(); },
        [] { return kSampleStack; },
        [](int attempt) { return attempt >= 3; },
        [] { return true; },
        store};

    auto [report, outcome] = reporter.generateAndProcess("com.example.mod",
                                                         SemVer{1, 0, 0});

    EXPECT_EQ(outcome, TelemetryOutcome::Transmitted);
    EXPECT_TRUE(report.transmitted);
    EXPECT_EQ(report.retryCount, kMaxTransmissionRetries);  // 3 ritentativi (Req 21.5)
    EXPECT_TRUE(store.empty());
}

// --- Req 21.5: fallimento persistente -> max 3 ritentativi, poi ritenzione -
TEST(CrashReport, PersistentFailureGivesUpAfterThreeRetriesAndRetains) {
    auto clock = std::make_shared<FakeClock>();
    InMemoryCrashReportStore store;

    int transmitCalls = 0;
    CrashReporter reporter{
        [clock] { return (*clock)(); },
        [] { return kSampleStack; },
        [&transmitCalls](int) { ++transmitCalls; return false; },  // fallisce sempre
        [] { return true; },  // opt-in concesso
        store};

    auto [report, outcome] = reporter.generateAndProcess("com.example.mod",
                                                         SemVer{1, 0, 0});

    EXPECT_EQ(outcome, TelemetryOutcome::TransmissionFailedRetained);  // Req 21.5
    EXPECT_FALSE(report.transmitted);
    EXPECT_EQ(report.retryCount, kMaxTransmissionRetries);  // si arrende a 3
    // 1 invio iniziale + 3 ritentativi = 4 chiamate totali al transmitter.
    EXPECT_EQ(transmitCalls, kMaxTransmissionRetries + 1);
    // Conservato localmente dopo il fallimento (Req 21.5).
    ASSERT_EQ(store.size(), 1u);
    EXPECT_EQ(store.entries().front().expiry,
              report.when + kLocalRetentionSeconds);
}

}  // namespace
