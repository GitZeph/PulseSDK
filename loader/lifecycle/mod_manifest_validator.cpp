// loader/lifecycle/mod_manifest_validator.cpp — implementazione del
// Mod_Manifest_Validator (External Mod Loading, task 3.5).
//
// La validazione riusa `pulse::package::PulsePackage::open` per i controlli
// che esso già garantisce in ordine (manifest → integrità) e aggiunge i
// controlli specifici della feature (binario nativo, Mod_Id, entry point).
// Nessun byte di codice viene eseguito: si ispezionano solo manifest e digest.

#include "lifecycle/mod_manifest_validator.hpp"

namespace pulse::lifecycle {

namespace {

// Traduce l'`OpenError` di `PulsePackage::open` nella `ValidationCause` della
// feature. L'ordine garantito da `open` è manifest → integrità, quindi:
//   * ManifestMissing  → `pulse.toml` assente: file richiesto mancante (Req 2.2);
//   * ManifestInvalid  → manifest non analizzabile (Req 2.4);
//   * IntegrityMissing → `MANIFEST.sha256` richiesto ma assente: trattato come
//                        integrità non verificabile (mismatch) — non occorre con
//                        le opzioni usate qui, ma è mappato per completezza;
//   * IntegrityMismatch→ digest non conforme (Req 2.6).
[[nodiscard]] ValidationCause causeFromOpenError(pulse::package::OpenError e) {
    switch (e) {
        case pulse::package::OpenError::ManifestMissing:
            return ValidationCause::MissingRequiredFile;
        case pulse::package::OpenError::ManifestInvalid:
            return ValidationCause::ManifestUnparsable;
        case pulse::package::OpenError::IntegrityMissing:
        case pulse::package::OpenError::IntegrityMismatch:
            return ValidationCause::IntegrityMismatch;
        case pulse::package::OpenError::None:
            return ValidationCause::Ok;
    }
    return ValidationCause::MissingRequiredFile;
}

// Verifica la presenza del binario nativo nel container. Il CLI emette il
// modulo nativo come `code/<platform>.<ext>` (es. `code/macos-arm64.dylib`),
// mentre il nome canonico storico è `code/module.pulsebin`. Per riconciliare
// entrambe le convenzioni: si accetta `code/module.pulsebin` se presente,
// altrimenti una qualunque voce sotto `code/` che termini con un'estensione di
// libreria nativa (`.dylib`/`.so`/`.dll`). La scansione di `paths()` è
// deterministica (l'archivio è ordinato).
[[nodiscard]] bool has_native_module(const pulse::package::PackageArchive& archive) {
    if (archive.contains(kModuleBinaryEntry)) {
        return true;
    }
    constexpr std::string_view kCodePrefix = "code/";
    for (const std::string& path : archive.paths()) {
        if (path.size() <= kCodePrefix.size() ||
            path.compare(0, kCodePrefix.size(), kCodePrefix) != 0) {
            continue;
        }
        const auto ends_with = [&path](std::string_view ext) {
            return path.size() > ext.size() &&
                   path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
        };
        if (ends_with(".dylib") || ends_with(".so") || ends_with(".dll")) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] ValidationResult reject(ValidationCause cause, std::string message) {
    ValidationResult res;
    res.accepted = false;
    res.cause = cause;
    res.message = std::move(message);
    res.manifest = std::nullopt;  // nessun manifest esposto su rifiuto
    return res;
}

}  // namespace

ValidationResult ModManifestValidator::validate(
    pulse::package::PackageArchive archive) const {
    // (1) Apertura del container con verifica di integrità abilitata. Questo
    //     copre, in ordine: `pulse.toml` mancante (Req 2.2), manifest non
    //     analizzabile (Req 2.4) e mismatch di integrità (Req 2.5/2.6). Nessun
    //     codice viene esposto finché il manifest non è valido e integro.
    pulse::package::PulsePackage::Options opts;
    opts.requireIntegrityFile = false;  // l'assenza non è un errore di per sé
    opts.verifyIntegrity = true;        // ma se presente, il digest deve combaciare

    pulse::package::OpenResult opened =
        pulse::package::PulsePackage::open(std::move(archive), opts);
    if (!opened.ok) {
        // `open` non ha costruito alcun PulsePackage: il codice resta
        // inaccessibile. Riportiamo causa + messaggio identificativi.
        return reject(causeFromOpenError(opened.error),
                      "Mod_Package rifiutato in apertura ("
                          + pulse::package::to_string(opened.error) + "): "
                          + opened.message);
    }

    const pulse::package::PulsePackage& pkg = *opened.package;
    const pulse::manifest::Manifest& manifest = pkg.manifest();

    // (2) Per le mod native deve essere presente il binario nativo: il nome
    //     canonico `code/module.pulsebin` OPPURE una libreria nativa per
    //     piattaforma `code/*.dylib`/`code/*.so`/`code/*.dll` (Req 2.1/2.2). Le
    //     mod script non richiedono il binario nativo. Si verifica SOLO la
    //     presenza della voce nell'archivio: i byte non vengono caricati né
    //     eseguiti.
    if (manifest.type == pulse::manifest::ModType::Native &&
        !has_native_module(pkg.archive())) {
        return reject(ValidationCause::MissingRequiredFile,
                      "Mod nativa '" + manifest.id + "' priva del file richiesto '"
                          + std::string(kModuleBinaryEntry)
                          + "' (o di una libreria nativa code/*.dylib|.so|.dll): "
                            "caricamento rifiutato, nessun codice eseguito.");
    }

    // (3) Mod_Id non vuoto (Req 2.7).
    if (manifest.id.empty()) {
        return reject(ValidationCause::EmptyModId,
                      "Mod_Package con Mod_Id vuoto: caricamento rifiutato, "
                      "nessun codice eseguito.");
    }

    // (4) Almeno un entry point dichiarato (Req 2.8).
    if (manifest.entryPoints.empty()) {
        return reject(ValidationCause::NoEntryPoint,
                      "Mod '" + manifest.id
                          + "' non dichiara alcun entry point: caricamento "
                            "rifiutato, nessun codice eseguito.");
    }

    // Tutti i controlli superati: mod accettata, manifest esposto.
    ValidationResult res;
    res.accepted = true;
    res.cause = ValidationCause::Ok;
    res.message = "Mod '" + manifest.id + "' validata con successo.";
    res.manifest = manifest;
    return res;
}

}  // namespace pulse::lifecycle
