// loader/core/loader_core.hpp — Loader Core (Layer 1) del runtime Pulse.
//
// Responsabilità (Requisiti 1.5, 1.7):
//  - rilevare la piattaforma corrente e la GD_Version durante `initialize()`;
//  - su rilevamento riuscito, esporre un `RuntimeContext` a tutte le mod;
//  - su mancato rilevamento di GD_Version, abortire il caricamento e loggare
//    un errore diagnostico, restituendo `InitOutcome::VersionDetectionFailed`.
#ifndef PULSE_LOADER_CORE_LOADER_CORE_HPP
#define PULSE_LOADER_CORE_LOADER_CORE_HPP

#include <functional>
#include <memory>
#include <string_view>

#include "core/runtime_context.hpp"
#include "core/version_detector.hpp"

namespace pulse::loader {

// Esito dell'inizializzazione del Loader Core.
enum class InitOutcome {
    // Inizializzazione completata: il `RuntimeContext` è disponibile.
    Success,
    // GD_Version non rilevata: caricamento abortito (Requisito 1.7).
    VersionDetectionFailed,
};

// Sink diagnostico per gli errori del loader. Iniettabile per i test; il
// default scrive su `stderr`. Mantenuto come callback leggero per non
// dipendere dal sottosistema di logging dell'SDK (ancora non implementato).
using DiagnosticSink = std::function<void(std::string_view)>;

// Restituisce il sink diagnostico di default (scrittura su stderr).
DiagnosticSink default_diagnostic_sink();

class LoaderCore {
public:
    // Costruisce con il detector di default e il sink di default.
    LoaderCore();

    // Costruisce con un detector iniettato (testabilità host) e un sink
    // diagnostico opzionale. Se `detector` è nullo viene usato quello di
    // default; se `sink` è nullo viene usato quello di default.
    explicit LoaderCore(std::shared_ptr<IVersionDetector> detector,
                        DiagnosticSink sink = nullptr);

    // Rileva piattaforma e GD_Version.
    //  - Successo: popola il RuntimeContext e restituisce `Success`.
    //  - Fallimento del rilevamento versione: logga la diagnostica e
    //    restituisce `VersionDetectionFailed` senza esporre il contesto
    //    (il caricamento delle mod va abortito dal chiamante — Requisito 1.7).
    InitOutcome initialize();

    // Indica se l'inizializzazione è stata completata con successo.
    bool initialized() const noexcept { return initialized_; }

    // Contesto runtime esposto alle mod (Requisito 1.5).
    // Valido solo dopo un `initialize()` che ha restituito `Success`.
    const RuntimeContext& context() const noexcept { return context_; }

private:
    std::shared_ptr<IVersionDetector> detector_;
    DiagnosticSink sink_;
    RuntimeContext context_{};
    bool initialized_{false};
};

}  // namespace pulse::loader

#endif  // PULSE_LOADER_CORE_LOADER_CORE_HPP
