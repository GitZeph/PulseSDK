// loader/lifecycle/mod_manifest_validator.hpp — Mod_Manifest_Validator
// (External Mod Loading, task 3.5).
//
// Validatore che applica, PRIMA di caricare qualsiasi codice, le verifiche
// strutturali e di integrità su un container `.pulse` (modellato come
// `PackageArchive`). Riusa `PulsePackage::open` — che già impone l'ordine
// manifest → integrità → esposizione del codice e calcola/verifica
// `MANIFEST.sha256` — e vi aggiunge i controlli specifici della feature.
//
// Requisiti coperti (insieme dei controlli fail-soft "rifiuta e prosegui"):
//   * Req 2.1/2.2 — `pulse.toml` presente e, per le mod native,
//                   `code/module.pulsebin` presente; assenza → rifiuto con
//                   diagnostica che identifica il file richiesto mancante.
//   * Req 2.3/2.4 — parsing del Mod_Manifest via `pulse::manifest::parse`;
//                   parsing fallito → rifiuto con causa.
//   * Req 2.5/2.6 — verifica dell'integrità confrontando il digest di ciascuna
//                   voce con `MANIFEST.sha256`; mismatch → rifiuto.
//   * Req 2.7     — Mod_Id vuoto → rifiuto con causa.
//   * Req 2.8     — nessun entry point dichiarato → rifiuto con causa.
//
// Nessun codice viene caricato: la validazione opera SOLO su manifest e digest
// (i byte di `code/module.pulsebin` non vengono mai eseguiti né interpretati,
// solo controllati per presenza e integrità). Ogni rifiuto produce un esito
// con una sola causa e un messaggio leggibile che identifica il package e la
// causa/il file; il Mod_Loader traduce questo esito in un `DiagnosticEntry`.
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).
#ifndef PULSE_LOADER_LIFECYCLE_MOD_MANIFEST_VALIDATOR_HPP
#define PULSE_LOADER_LIFECYCLE_MOD_MANIFEST_VALIDATOR_HPP

#include <optional>
#include <string>

#include "lifecycle/manifest.hpp"
#include "package/pulse_package.hpp"

namespace pulse::lifecycle {

// ---------------------------------------------------------------------------
// kModuleBinaryEntry — percorso canonico del binario nativo dentro il `.pulse`.
// Per le mod native (`ModType::Native`) la sua presenza è richiesta (Req 2.1).
// ---------------------------------------------------------------------------
inline constexpr std::string_view kModuleBinaryEntry = "code/module.pulsebin";

// ---------------------------------------------------------------------------
// ValidationCause — insieme chiuso delle cause dell'esito di validazione.
//   * Ok                  — la mod ha superato tutti i controlli.
//   * MissingRequiredFile — `pulse.toml` assente, oppure mod nativa priva di
//                           `code/module.pulsebin` (Req 2.1, 2.2).
//   * ManifestUnparsable  — `pulse.toml` presente ma non analizzabile (Req 2.4).
//   * IntegrityMismatch   — almeno una voce ha un digest diverso da quello
//                           dichiarato in `MANIFEST.sha256` (Req 2.5, 2.6).
//   * EmptyModId          — il Mod_Manifest dichiara un Mod_Id vuoto (Req 2.7).
//   * NoEntryPoint        — il Mod_Manifest non dichiara alcun entry point
//                           (Req 2.8).
// ---------------------------------------------------------------------------
enum class ValidationCause {
    Ok,
    MissingRequiredFile,
    ManifestUnparsable,
    IntegrityMismatch,
    EmptyModId,
    NoEntryPoint,
};

[[nodiscard]] inline std::string to_string(ValidationCause c) {
    switch (c) {
        case ValidationCause::Ok:                  return "ok";
        case ValidationCause::MissingRequiredFile: return "missing-required-file";
        case ValidationCause::ManifestUnparsable:  return "manifest-unparsable";
        case ValidationCause::IntegrityMismatch:   return "integrity-mismatch";
        case ValidationCause::EmptyModId:          return "empty-mod-id";
        case ValidationCause::NoEntryPoint:        return "no-entry-point";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// ValidationResult — esito della validazione. `accepted == true` implica
// `cause == ValidationCause::Ok` e `manifest` valorizzato con il Mod_Manifest
// analizzato; ogni rifiuto porta esattamente una causa (≠ Ok), un messaggio
// diagnostico leggibile e `manifest == nullopt`.
// ---------------------------------------------------------------------------
struct ValidationResult {
    bool accepted{false};
    ValidationCause cause{ValidationCause::Ok};
    std::string message;
    std::optional<pulse::manifest::Manifest> manifest;  // valorizzato sse accepted
};

// ---------------------------------------------------------------------------
// ModManifestValidator — applica i controlli strutturali e di integrità su un
// container `.pulse` aperto come `PackageArchive`. Stateless: una sola istanza
// può validare più package.
// ---------------------------------------------------------------------------
class ModManifestValidator {
public:
    // Valida un container `.pulse` (modellato come `PackageArchive`, consumato
    // per move). Ordine dei controlli, tutti fail-soft ("rifiuta e prosegui"):
    //   (1) `PulsePackage::open(verifyIntegrity=true)` copre `pulse.toml`
    //       mancante (→ MissingRequiredFile), manifest non analizzabile
    //       (→ ManifestUnparsable) e mismatch di integrità (→ IntegrityMismatch);
    //   (2) per le mod native, presenza di `code/module.pulsebin`
    //       (→ MissingRequiredFile);
    //   (3) Mod_Id non vuoto (→ EmptyModId);
    //   (4) almeno un entry point (→ NoEntryPoint).
    // Nessun codice viene caricato: si opera su manifest e digest.
    [[nodiscard]] ValidationResult validate(pulse::package::PackageArchive archive) const;
};

}  // namespace pulse::lifecycle

#endif  // PULSE_LOADER_LIFECYCLE_MOD_MANIFEST_VALIDATOR_HPP
