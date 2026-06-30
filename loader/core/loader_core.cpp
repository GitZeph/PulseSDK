// loader/core/loader_core.cpp — implementazione del Loader Core (Layer 1).
#include "core/loader_core.hpp"

#include <iostream>
#include <string>
#include <utility>

namespace pulse::loader {

DiagnosticSink default_diagnostic_sink() {
    return [](std::string_view message) {
        std::cerr << "[pulse-loader] " << message << '\n';
    };
}

LoaderCore::LoaderCore()
    : detector_(std::make_shared<DefaultVersionDetector>()),
      sink_(default_diagnostic_sink()) {}

LoaderCore::LoaderCore(std::shared_ptr<IVersionDetector> detector, DiagnosticSink sink)
    : detector_(detector ? std::move(detector)
                         : std::make_shared<DefaultVersionDetector>()),
      sink_(sink ? std::move(sink) : default_diagnostic_sink()) {}

InitOutcome LoaderCore::initialize() {
    initialized_ = false;

    // 1) Rilevamento della piattaforma corrente (compile-time host).
    const Platform platform = current_platform();
    const std::string_view platformId = platform_id(platform);

    // 2) Rilevamento di GD_Version tramite il detector (iniettabile).
    const std::optional<GdVersion> version = detector_->detect();
    if (!version.has_value()) {
        // GD_Version non rilevata: abortire il caricamento delle mod e loggare
        // un errore diagnostico (Requisito 1.7).
        sink_(std::string{"GD_Version non rilevata su piattaforma '"} +
              std::string{platformId} +
              "': caricamento delle mod abortito (avvio di Geometry Dash in "
              "modalita' senza mod).");
        return InitOutcome::VersionDetectionFailed;
    }

    // 3) Successo: popola ed espone il RuntimeContext a tutte le mod (Req 1.5).
    context_ = RuntimeContext{
        *version,
        platform,
        std::string{platformId},
    };
    initialized_ = true;
    return InitOutcome::Success;
}

}  // namespace pulse::loader
