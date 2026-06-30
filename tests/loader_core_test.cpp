// tests/loader_core_test.cpp — unit test del LoaderCore (Layer 1, task 2.3).
//
// Verifica il comportamento di fallimento del rilevamento di GD_Version
// (Requisito 1.7): quando il detector non riesce a identificare il binario,
// `initialize()` DEVE:
//   * interrompere il caricamento (ritornare InitOutcome::VersionDetectionFailed
//     e NON esporre il RuntimeContext);
//   * registrare un errore diagnostico che indica la mancata rilevazione.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/loader_core.hpp"
#include "core/runtime_context.hpp"
#include "core/version_detector.hpp"

namespace {

using namespace pulse::loader;

// Detector fittizio iniettabile: restituisce sempre `std::nullopt`, simulando
// l'impossibilità di identificare il binario di Geometry Dash (Req 1.7).
class FailingVersionDetector final : public IVersionDetector {
public:
    std::optional<GdVersion> detect() override { return std::nullopt; }
};

// initialize() su rilevamento fallito ritorna VersionDetectionFailed e logga.
TEST(LoaderCoreVersionFailure, ReturnsVersionDetectionFailedAndLogs) {
    std::vector<std::string> logged;
    DiagnosticSink capturing = [&logged](std::string_view message) {
        logged.emplace_back(message);
    };

    LoaderCore core(std::make_shared<FailingVersionDetector>(), capturing);

    const InitOutcome outcome = core.initialize();

    // Esito: caricamento abortito (Req 1.7).
    EXPECT_EQ(outcome, InitOutcome::VersionDetectionFailed);
    // Il contesto runtime NON deve risultare inizializzato/esposto.
    EXPECT_FALSE(core.initialized());
    // Deve essere stato registrato almeno un messaggio diagnostico.
    ASSERT_FALSE(logged.empty());
    // Il messaggio diagnostico indica la mancata rilevazione della versione.
    EXPECT_NE(logged.front().find("GD_Version"), std::string::npos);
}

}  // namespace
