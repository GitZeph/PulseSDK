// Pulse — Unit test del benchmark di regressione "a vuoto" (task 40.1,
// Requisiti 28.1, 28.2, 28.3).
//
// Esercita l'harness header-only `tools/regression_benchmark.hpp` con fonti di
// misura iniettate (deterministico, senza tempo reale né gioco vero):
//   • frame rate entro il 2% => criterio 28.1 passa; degrado > 2% => fallisce
//     (verificando anche il delta misurato);
//   • un evento di crash/freeze => criterio 28.2 fallisce;
//   • init <= 2000 ms passa, > 2000 ms fallisce (criterio 28.3);
//   • l'esito complessivo è pass sse TUTTI i criteri passano.

#include "tools/regression_benchmark.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

using pulse::bench::BenchmarkInputs;
using pulse::bench::BenchmarkResult;
using pulse::bench::runBenchmark;

// Helper: costruisce ingressi "tutti verdi" che si possono perturbare per test.
BenchmarkInputs makePassingInputs() {
    BenchmarkInputs in;
    // 60 FPS => 16.6667 ms per frame, identico per baseline e a vuoto.
    in.baselineFrameTimesMs = std::vector<double>(10, 1000.0 / 60.0);
    in.idleFrameTimesMs = std::vector<double>(10, 1000.0 / 60.0);
    in.crashCount = 0;
    in.freezeCount = 0;
    in.initDurationMs = 500.0;
    return in;
}

// --- Criterio 28.1 -------------------------------------------------------

TEST(RegressionBenchmark, FrameRateWithinTwoPercentPasses) {
    BenchmarkInputs in = makePassingInputs();
    // Overhead a vuoto: frame-time leggermente maggiore (~1%), entro il budget.
    in.idleFrameTimesMs = std::vector<double>(10, (1000.0 / 60.0) * 1.01);

    const BenchmarkResult r = runBenchmark(in);

    EXPECT_TRUE(r.frameRate.passed);
    // Lo scostamento misurato deve essere entro il 2%.
    EXPECT_LE(std::abs(r.frameRateDeltaRatio), pulse::bench::kMaxFrameRateDeltaRatio);
}

TEST(RegressionBenchmark, FrameRateDegradationOverTwoPercentFails) {
    BenchmarkInputs in = makePassingInputs();
    // Degrado del 10% sul frame-time => ~9% di calo del frame rate.
    in.idleFrameTimesMs = std::vector<double>(10, (1000.0 / 60.0) * 1.10);

    const BenchmarkResult r = runBenchmark(in);

    EXPECT_FALSE(r.frameRate.passed);
    // Il delta misurato deve eccedere il budget ed essere una regressione (segno -).
    EXPECT_GT(std::abs(r.frameRateDeltaRatio), pulse::bench::kMaxFrameRateDeltaRatio);
    EXPECT_LT(r.frameRateDeltaRatio, 0.0);
    EXPECT_FALSE(r.overallPassed);
}

// --- Criterio 28.2 -------------------------------------------------------

TEST(RegressionBenchmark, CrashEventFailsStabilityCriterion) {
    BenchmarkInputs in = makePassingInputs();
    in.crashCount = 1;

    const BenchmarkResult r = runBenchmark(in);

    EXPECT_FALSE(r.stability.passed);
    EXPECT_FALSE(r.overallPassed);
}

TEST(RegressionBenchmark, FreezeEventFailsStabilityCriterion) {
    BenchmarkInputs in = makePassingInputs();
    in.freezeCount = 2;

    const BenchmarkResult r = runBenchmark(in);

    EXPECT_FALSE(r.stability.passed);
    EXPECT_FALSE(r.overallPassed);
}

// --- Criterio 28.3 -------------------------------------------------------

TEST(RegressionBenchmark, InitWithinBudgetPasses) {
    BenchmarkInputs in = makePassingInputs();
    in.initDurationMs = 2000.0;  // esattamente al limite => passa.

    const BenchmarkResult r = runBenchmark(in);

    EXPECT_TRUE(r.initTime.passed);
}

TEST(RegressionBenchmark, InitOverBudgetFails) {
    BenchmarkInputs in = makePassingInputs();
    in.initDurationMs = 2001.0;  // appena oltre il budget => fallisce.

    const BenchmarkResult r = runBenchmark(in);

    EXPECT_FALSE(r.initTime.passed);
    EXPECT_FALSE(r.overallPassed);
}

// --- Esito aggregato -----------------------------------------------------

TEST(RegressionBenchmark, OverallPassRequiresAllCriteria) {
    const BenchmarkResult r = runBenchmark(makePassingInputs());

    EXPECT_TRUE(r.frameRate.passed);
    EXPECT_TRUE(r.stability.passed);
    EXPECT_TRUE(r.initTime.passed);
    EXPECT_TRUE(r.overallPassed);
}

}  // namespace
