// Pulse — Benchmark di regressione "a vuoto" (Requisiti 28.1, 28.2, 28.3)
//
// Harness automatizzato, header-only, che verifica il budget di overhead del
// Pulse_Loader quando nessuna Mod è abilitata ("a vuoto"):
//
//   • 28.1 — il frame rate medio resta entro il 2% rispetto al valore misurato
//            in assenza del loader (baseline);
//   • 28.2 — assenza di crash/freeze/regressioni funzionali sull'intera corsa;
//   • 28.3 — l'inizializzazione del loader si completa entro 2000 ms.
//
// L'harness è DETERMINISTICO sull'host: invece di misurare tempo reale, thread
// o processi di gioco, riceve in INIEZIONE le fonti di misura (campioni di
// frame-time della baseline e con loader, durata di init, eventi di crash/
// freeze). Questo consente di esercitare ogni criterio in test unitari ripetibili.
//
// L'esito è OSSERVABILE: per ogni criterio espone pass/fail + i valori misurati,
// più un esito complessivo (pass sse tutti i criteri passano).

#ifndef PULSE_TOOLS_REGRESSION_BENCHMARK_HPP
#define PULSE_TOOLS_REGRESSION_BENCHMARK_HPP

#include <cstddef>
#include <numeric>
#include <string>
#include <vector>

namespace pulse::bench {

// ---------------------------------------------------------------------------
// Costanti di budget (dai requisiti).
// ---------------------------------------------------------------------------

// Tolleranza massima di scostamento del frame rate medio rispetto alla
// baseline (Req 28.1): 2%.
inline constexpr double kMaxFrameRateDeltaRatio = 0.02;

// Budget massimo di inizializzazione del loader (Req 28.3): 2000 ms.
inline constexpr double kMaxInitMillis = 2000.0;

// ---------------------------------------------------------------------------
// Ingressi iniettabili.
// ---------------------------------------------------------------------------

// Fonti di misura iniettate dall'esterno: rendono l'harness deterministico
// senza dipendere da tempo reale, thread o da un gioco vero.
struct BenchmarkInputs {
    // Campioni di tempo-per-frame (in millisecondi) misurati SENZA il loader.
    std::vector<double> baselineFrameTimesMs;
    // Campioni di tempo-per-frame (in millisecondi) misurati con il loader
    // attivo ma a vuoto (nessuna Mod abilitata).
    std::vector<double> idleFrameTimesMs;
    // Numero di crash osservati durante la corsa (Req 28.2): pass sse 0.
    std::size_t crashCount = 0;
    // Numero di freeze/stalli osservati durante la corsa (Req 28.2).
    std::size_t freezeCount = 0;
    // Durata misurata dell'inizializzazione del loader, in millisecondi (Req 28.3).
    double initDurationMs = 0.0;
};

// ---------------------------------------------------------------------------
// Esito osservabile.
// ---------------------------------------------------------------------------

// Esito di un singolo criterio: pass/fail più i valori misurati che lo motivano.
struct CriterionResult {
    bool passed = false;
    std::string detail;  // descrizione leggibile (commenti/diagnostica)
};

// Esito complessivo del benchmark: un risultato per criterio + i valori misurati
// derivati, più l'esito aggregato.
struct BenchmarkResult {
    // Criterio 28.1 — overhead del frame rate entro il 2%.
    CriterionResult frameRate;
    // Frame rate medio (fotogrammi al secondo) della baseline e a vuoto.
    double baselineAvgFps = 0.0;
    double idleAvgFps = 0.0;
    // Scostamento relativo del frame rate a vuoto rispetto alla baseline
    // (valore con segno: negativo = regressione). Il criterio confronta il
    // valore assoluto contro kMaxFrameRateDeltaRatio.
    double frameRateDeltaRatio = 0.0;

    // Criterio 28.2 — assenza di crash/freeze.
    CriterionResult stability;

    // Criterio 28.3 — init entro il budget.
    CriterionResult initTime;
    double initDurationMs = 0.0;

    // Esito aggregato: vero sse TUTTI i criteri passano.
    bool overallPassed = false;
};

// ---------------------------------------------------------------------------
// Funzioni di supporto.
// ---------------------------------------------------------------------------

// Frame rate medio (FPS) dato un insieme di tempi-per-frame in millisecondi.
// FPS = 1000 / (tempo medio per frame). Restituisce 0 se non ci sono campioni.
inline double averageFps(const std::vector<double>& frameTimesMs) {
    if (frameTimesMs.empty()) {
        return 0.0;
    }
    const double sum =
        std::accumulate(frameTimesMs.begin(), frameTimesMs.end(), 0.0);
    const double meanFrameMs = sum / static_cast<double>(frameTimesMs.size());
    if (meanFrameMs <= 0.0) {
        return 0.0;
    }
    return 1000.0 / meanFrameMs;
}

// ---------------------------------------------------------------------------
// Harness.
// ---------------------------------------------------------------------------

// Esegue la valutazione dei tre criteri sugli ingressi iniettati e restituisce
// un esito completamente osservabile.
inline BenchmarkResult runBenchmark(const BenchmarkInputs& in) {
    BenchmarkResult out;

    // --- Criterio 28.1: overhead del frame rate medio entro il 2%. ---------
    out.baselineAvgFps = averageFps(in.baselineFrameTimesMs);
    out.idleAvgFps = averageFps(in.idleFrameTimesMs);

    if (out.baselineAvgFps <= 0.0) {
        // Senza una baseline valida non possiamo confrontare: criterio fallito.
        out.frameRateDeltaRatio = 0.0;
        out.frameRate.passed = false;
        out.frameRate.detail =
            "baseline frame rate non disponibile o non valida";
    } else {
        // Scostamento relativo con segno rispetto alla baseline.
        out.frameRateDeltaRatio =
            (out.idleAvgFps - out.baselineAvgFps) / out.baselineAvgFps;
        const double absDelta = out.frameRateDeltaRatio < 0.0
                                    ? -out.frameRateDeltaRatio
                                    : out.frameRateDeltaRatio;
        out.frameRate.passed = absDelta <= kMaxFrameRateDeltaRatio;
        out.frameRate.detail =
            "scostamento frame rate = " + std::to_string(absDelta * 100.0) +
            "% (budget " + std::to_string(kMaxFrameRateDeltaRatio * 100.0) + "%)";
    }

    // --- Criterio 28.2: nessun crash, nessun freeze. -----------------------
    out.stability.passed = (in.crashCount == 0) && (in.freezeCount == 0);
    out.stability.detail = "crash = " + std::to_string(in.crashCount) +
                           ", freeze = " + std::to_string(in.freezeCount);

    // --- Criterio 28.3: init entro il budget di 2000 ms. -------------------
    out.initDurationMs = in.initDurationMs;
    out.initTime.passed = in.initDurationMs <= kMaxInitMillis;
    out.initTime.detail = "init = " + std::to_string(in.initDurationMs) +
                          " ms (budget " + std::to_string(kMaxInitMillis) + " ms)";

    // --- Esito aggregato: pass sse tutti i criteri passano. ----------------
    out.overallPassed =
        out.frameRate.passed && out.stability.passed && out.initTime.passed;

    return out;
}

}  // namespace pulse::bench

#endif  // PULSE_TOOLS_REGRESSION_BENCHMARK_HPP
