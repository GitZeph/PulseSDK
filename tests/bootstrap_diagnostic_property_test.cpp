// tests/bootstrap_diagnostic_property_test.cpp
// Feature: pulse-gd-integration, Property 2 — Diagnostica della causa di
// fallimento del bootstrap.
// Validates: Requirements 2.7 (Requisiti 2.7)
//
// Property 2 (design.md §2.1 / Req 2.7): se il Platform_Bootstrap non riesce a
// ottenere il controllo del processo di Geometry Dash prima della scena
// iniziale, DEVE registrare un errore diagnosticamente recuperabile che
// identifica la causa. In termini osservabili sul tipo `BootstrapResult`: per
// OGNI causa di fallimento iniettata, `inject()`/la fabbrica di esito devono
// produrre `injected == false` e una `DiagnosticError` il cui `code` coincide
// con la causa iniettata e il cui `message` è NON vuoto (così il loader può
// loggarlo e lasciar partire GD senza mod — Req 2.8).
//
// Perché un modello speculare oltre al codice reale:
//   La produzione reale della diagnostica vive in due punti:
//     1. `BootstrapResult::failure(code, message)` (header platform-agnostic
//        `platform_bootstrap.hpp`) — la fabbrica che impacchetta causa+messaggio;
//     2. `MacOSBootstrap::inject()` (macos_bootstrap.cpp) che, sulle proprie
//        cause (AlreadyInjected, EntryPointHookFailed, UnsupportedHost), invoca
//        quella fabbrica con un messaggio non vuoto.
//   Il costruttore di early-load reale che *consuma* la diagnostica è compilato
//   solo sotto `PULSE_LOADER_ARTIFACT` e in namespace anonimo, quindi non
//   linkabile dall'host. Come nel test della Property 1, verifichiamo la STESSA
//   logica di produzione della diagnostica in modo host-testabile: (a) sulla
//   fabbrica reale `BootstrapResult::failure`, (b) sul `MacOSBootstrap` reale per
//   le cause che esso effettivamente emette, e (c) su un modello che inietta
//   ciascuna causa dell'enum `BootstrapErrorCode`.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * FABBRICA: per ogni causa di fallimento e un messaggio NON vuoto
//     arbitrario, `BootstrapResult::failure(cause, msg)` → injected==false,
//     error presente, code==cause, message==msg (preservato, non vuoto).
//   * MODELLO BOOTSTRAP: per ogni causa di fallimento iniettata, il modello che
//     replica la produzione della diagnostica del bootstrap → injected==false,
//     error con code==cause e messaggio non vuoto.
//   * BOOTSTRAP REALE: ripetute `MacOSBootstrap::inject()` con un entry point
//     che fallisce → esito di fallimento con messaggio non vuoto e codice
//     coerente con la piattaforma (EntryPointHookFailed su Apple,
//     UnsupportedHost altrove); la seconda inject() → AlreadyInjected.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <string>
#include <vector>

#include "bootstrap/macos_bootstrap.hpp"
#include "bootstrap/platform_bootstrap.hpp"

namespace {

using pulse::bootstrap::BootstrapErrorCode;
using pulse::bootstrap::BootstrapResult;
using pulse::bootstrap::DiagnosticError;
using pulse::bootstrap::MacOSBootstrap;

// Tutte le cause di FALLIMENTO dell'enum `BootstrapErrorCode`. `None` è escluso
// perché modella l'assenza di errore (esito di successo), non una causa di
// fallimento del bootstrap.
const std::vector<BootstrapErrorCode>& failure_causes() {
    static const std::vector<BootstrapErrorCode> kCauses = {
        BootstrapErrorCode::ProxyChainLoadFailed,
        BootstrapErrorCode::EntryPointHookFailed,
        BootstrapErrorCode::AlreadyInjected,
        BootstrapErrorCode::UnsupportedHost,
        BootstrapErrorCode::Unknown,
    };
    return kCauses;
}

// Messaggio diagnostico canonico, NON vuoto, associato a ciascuna causa
// iniettata. Replica fedelmente l'intento dei messaggi prodotti dal bootstrap
// reale (vedi macos_bootstrap.cpp): ogni causa identifica se stessa con testo
// leggibile per il log (Req 2.7).
std::string diagnostic_message_for(BootstrapErrorCode cause) {
    switch (cause) {
        case BootstrapErrorCode::ProxyChainLoadFailed:
            return "Bootstrap: impossibile caricare/forwardare la DLL di sistema";
        case BootstrapErrorCode::EntryPointHookFailed:
            return "Bootstrap: impossibile agganciare il punto pre-scena";
        case BootstrapErrorCode::AlreadyInjected:
            return "Bootstrap: runtime gia iniettato in questo processo";
        case BootstrapErrorCode::UnsupportedHost:
            return "Bootstrap: implementazione non operativa sull'host corrente";
        case BootstrapErrorCode::Unknown:
            return "Bootstrap: causa di fallimento sconosciuta";
        case BootstrapErrorCode::None:
            return "";  // non una causa di fallimento
    }
    return "";
}

// Modella la produzione della diagnostica del bootstrap per una causa di
// fallimento iniettata: impacchetta causa + messaggio non vuoto attraverso la
// stessa fabbrica reale `BootstrapResult::failure`.
BootstrapResult simulate_injected_failure(BootstrapErrorCode cause) {
    return BootstrapResult::failure(cause, diagnostic_message_for(cause));
}

// --- Property 2 — la fabbrica di esito impacchetta causa + messaggio --------
// Feature: pulse-gd-integration, Property 2. Validates: Requirements 2.7.
//
// Per ogni causa di fallimento e QUALUNQUE messaggio non vuoto,
// `BootstrapResult::failure(cause, msg)` produce un esito non iniettato con una
// `DiagnosticError` che riporta esattamente la causa e il messaggio (preservato
// verbatim e non vuoto).
RC_GTEST_PROP(Property2BootstrapDiagnostic,
              FailureFactoryCarriesCauseAndNonEmptyMessage,
              ()) {
    const auto cause = *rc::gen::elementOf(failure_causes()).as("causa iniettata");
    // Messaggio non vuoto arbitrario: anteponiamo un carattere fisso così che
    // qualunque suffisso generato (anche vuoto) resti non vuoto.
    const auto suffix = *rc::gen::arbitrary<std::string>().as("suffisso messaggio");
    const std::string message = "x" + suffix;

    const BootstrapResult result = BootstrapResult::failure(cause, message);

    RC_ASSERT(!result.injected);                 // esito di fallimento
    RC_ASSERT(result.error.has_value());         // diagnostica recuperabile
    RC_ASSERT(result.error->code == cause);      // codice corrispondente alla causa
    RC_ASSERT(result.error->message == message); // messaggio preservato
    RC_ASSERT(!result.error->message.empty());   // messaggio NON vuoto (Req 2.7)
}

// --- Property 2 — modello del bootstrap su ogni causa iniettata -------------
// Feature: pulse-gd-integration, Property 2. Validates: Requirements 2.7.
//
// Per ogni causa di fallimento iniettata, il modello che replica la produzione
// della diagnostica del bootstrap restituisce injected==false e una
// `DiagnosticError` con il codice corrispondente e un messaggio NON vuoto.
RC_GTEST_PROP(Property2BootstrapDiagnostic,
              EveryInjectedCauseYieldsMatchingNonEmptyDiagnostic,
              ()) {
    const auto cause = *rc::gen::elementOf(failure_causes()).as("causa iniettata");

    const BootstrapResult result = simulate_injected_failure(cause);

    RC_ASSERT(!result.injected);                 // injected == false
    RC_ASSERT(result.error.has_value());         // DiagnosticError presente
    RC_ASSERT(result.error->code == cause);      // codice corrispondente
    RC_ASSERT(!result.error->message.empty());   // messaggio NON vuoto (Req 2.7)
}

// --- Property 2 — bootstrap REALE: fallimento dell'entry point --------------
// Feature: pulse-gd-integration, Property 2. Validates: Requirements 2.7.
//
// Ancora la proprietà al codice reale: con un entry point del runtime che
// segnala un errore di avvio, `MacOSBootstrap::inject()` produce SEMPRE un esito
// di fallimento con diagnostica e messaggio non vuoto, qualunque sia il numero
// di bootstrap costruiti. Il codice atteso dipende dalla piattaforma di build:
// EntryPointHookFailed su Apple (dove l'iniezione è operativa), UnsupportedHost
// altrove (Req 2.7 con build cross-platform compilabile).
RC_GTEST_PROP(Property2BootstrapDiagnostic,
              RealBootstrapEntryFailureIsDiagnosed,
              ()) {
    // N >= 1 bootstrap freschi, ciascuno con entry point che fallisce.
    const auto count = *rc::gen::inRange<int>(1, 65).as("numero di bootstrap");

    for (int i = 0; i < count; ++i) {
        MacOSBootstrap bootstrap([]() { return false; });
        const BootstrapResult result = bootstrap.inject();

        RC_ASSERT(!result.injected);
        RC_ASSERT(result.error.has_value());
        RC_ASSERT(!result.error->message.empty());  // messaggio NON vuoto (Req 2.7)
#if defined(__APPLE__)
        RC_ASSERT(result.error->code == BootstrapErrorCode::EntryPointHookFailed);
#else
        RC_ASSERT(result.error->code == BootstrapErrorCode::UnsupportedHost);
#endif
    }
}

// --- Property 2 — bootstrap REALE: seconda inject() = AlreadyInjected -------
// Feature: pulse-gd-integration, Property 2. Validates: Requirements 2.7.
//
// Solo su Apple, dove la prima inject() ha successo: ogni successiva inject()
// sullo stesso bootstrap è una causa di fallimento `AlreadyInjected` con
// messaggio non vuoto. Su host non-Apple inject() fallisce con UnsupportedHost
// (l'iniezione non è operativa) e la proprietà di "diagnostica non vuota" resta
// comunque verificata.
RC_GTEST_PROP(Property2BootstrapDiagnostic,
              RealBootstrapRepeatedInjectIsDiagnosed,
              ()) {
    const auto extra = *rc::gen::inRange<int>(1, 33).as("inject() aggiuntive");

    MacOSBootstrap bootstrap([]() { return true; });
    const BootstrapResult first = bootstrap.inject();

#if defined(__APPLE__)
    // La prima ha successo; ogni successiva è AlreadyInjected.
    RC_ASSERT(first.injected);
    for (int i = 0; i < extra; ++i) {
        const BootstrapResult again = bootstrap.inject();
        RC_ASSERT(!again.injected);
        RC_ASSERT(again.error.has_value());
        RC_ASSERT(again.error->code == BootstrapErrorCode::AlreadyInjected);
        RC_ASSERT(!again.error->message.empty());  // messaggio NON vuoto (Req 2.7)
    }
#else
    // Host non-Apple: l'iniezione non è operativa, ma ogni esito di fallimento
    // porta comunque una diagnostica con messaggio non vuoto.
    RC_ASSERT(!first.injected);
    RC_ASSERT(first.error.has_value());
    RC_ASSERT(first.error->code == BootstrapErrorCode::UnsupportedHost);
    RC_ASSERT(!first.error->message.empty());
    for (int i = 0; i < extra; ++i) {
        const BootstrapResult again = bootstrap.inject();
        RC_ASSERT(!again.injected);
        RC_ASSERT(again.error.has_value());
        RC_ASSERT(!again.error->message.empty());  // messaggio NON vuoto (Req 2.7)
    }
#endif
}

}  // namespace
