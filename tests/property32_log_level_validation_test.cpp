// tests/property32_log_level_validation_test.cpp
// Feature: pulse-sdk, Property 32 — Validazione del livello di log.
// Validates: Requisiti 13.2 (Requisito 13.2)
//
// Property 32 (design.md / Req 13.2): per un codice di livello intero
// randomizzato, Logger::log(code, msg) ha successo SE E SOLO SE il codice
// appartiene all'insieme chiuso [0, 3] = {Debug, Info, Warning, Error}.
//   * Un codice FUORI insieme (negativo o > 3) è rifiutato con
//     LogErrorCode::InvalidLevel e NULLA viene registrato nel sink: la
//     dimensione dello store resta invariata.
//   * Un codice IN insieme registra ESATTAMENTE un messaggio, lo store cresce
//     di uno e il Level registrato coincide con levelFromCode(code).
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si genera un codice intero in un intervallo che copre ampiamente sia i
//     valori validi (0..3) sia quelli invalidi (negativi e > 3), così
//     entrambi i rami (accettazione / rifiuto) sono esercitati di frequente;
//   * si genera un messaggio binary-safe arbitrario (può essere vuoto o
//     contenere byte NUL incorporati);
//   * si invoca Logger::log(code, msg) su un SessionLogStore e si verificano
//     gli invarianti sopra confrontando con levelFromCode(code).

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstddef>
#include <optional>
#include <string>

#include "pulse/log.hpp"

namespace {

using pulse::Level;
using pulse::levelFromCode;
using pulse::LogErrorCode;
using pulse::Logger;
using pulse::LogResult;
using pulse::SessionLogStore;

// Intervallo di codici generati: copre abbondantemente i validi (0..3) e gli
// invalidi su entrambi i lati (negativi e > 3), così i due rami sono frequenti.
constexpr int kMinGenCode = -10;
constexpr int kMaxGenCode = 13;  // inRange è half-open: genera fino a 12 incluso.

// --- Property 32 — log(code, msg) accetta sse code ∈ [0,3] ----------------
// Feature: pulse-sdk, Property 32. Validates: Requisiti 13.2.
RC_GTEST_PROP(Property32LogLevelValidation,
              LogSucceedsIffLevelCodeInClosedSet,
              ()) {
    // Codice di livello randomizzato che copre valori validi e invalidi.
    const int code =
        *rc::gen::inRange<int>(kMinGenCode, kMaxGenCode).as("codice di livello");

    // Messaggio binary-safe arbitrario (può essere vuoto o contenere NUL).
    const std::string msg =
        *rc::gen::arbitrary<std::string>().as("messaggio di log");

    SessionLogStore store;
    Logger logger("mod.test.property32", store);

    const std::size_t sizeBefore = store.size();
    const std::optional<Level> expectedLevel = levelFromCode(code);
    const bool expectedOk = (code >= 0 && code <= 3);

    // Coerenza tra levelFromCode e l'insieme chiuso atteso.
    RC_ASSERT(expectedLevel.has_value() == expectedOk);

    const LogResult result = logger.log(code, msg);

    // L'esito ha successo SSE il codice è nell'insieme chiuso [0, 3].
    RC_ASSERT(result.ok == expectedOk);

    if (!expectedOk) {
        // Rifiuto: errore InvalidLevel e NULLA registrato (store invariato).
        RC_ASSERT(result.code == LogErrorCode::InvalidLevel);
        RC_ASSERT(store.size() == sizeBefore);
    } else {
        // Successo: esattamente un record in più, con il Level corrispondente.
        RC_ASSERT(result.code == LogErrorCode::None);
        RC_ASSERT(store.size() == sizeBefore + 1);

        const auto& records = store.records();
        RC_ASSERT(!records.empty());
        const auto& last = records.back();
        RC_ASSERT(last.level == *expectedLevel);
        RC_ASSERT(last.mod == "mod.test.property32");
        RC_ASSERT(last.message == msg);
    }
}

}  // namespace
