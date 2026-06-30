// tests/eml_property5_validation_rejection_test.cpp
// Feature: external-mod-loading, Property 5 — Rifiuto della validazione senza
// caricare codice.
// Validates: Requirements 2.2, 2.4, 2.6, 2.8 (Requisiti 2.2, 2.4, 2.6, 2.8)
//
// Property 5 (design.md): per ogni Mod_Package a cui manca un file richiesto,
// oppure con manifest non analizzabile, oppure con almeno una voce il cui
// digest non corrisponde a `MANIFEST.sha256`, oppure senza entry point, la
// validazione RIFIUTA la mod SENZA caricare alcun contenuto e produce una
// diagnostica con la causa; mentre le ALTRE mod proseguono.
//
// "Senza caricare codice" è reso osservabile dall'invariante strutturale del
// `ValidationResult`: su rifiuto `manifest == nullopt` (nessun manifest/codice
// esposto), `accepted == false`, `cause != Ok` ed è una causa dell'insieme
// chiuso, con un messaggio diagnostico non vuoto. I Mod_Package validi sono
// invece accettati con `manifest` valorizzato (controllo del bicondizionale).
//
// "Le altre mod proseguono" è reso osservabile validando un BATCH di package
// indipendenti nella stessa esecuzione: ogni esito dipende solo dal proprio
// package (il validator è stateless), quindi un package malformato non altera
// l'esito dei package validi/altri-malformati accanto ad esso.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si genera un batch di 1..6 package, ognuno con un difetto scelto tra
//     { Valid, MissingManifest, MissingModuleBinary, UnparsableManifest,
//       IntegrityMismatch, NoEntryPoint };
//   * ogni package è costruito in memoria come `PackageArchive`, alterato SOLO
//     nella dimensione del proprio difetto e per il resto valido;
//   * l'oracolo della causa attesa deriva direttamente dalla specifica
//     (ordine dei controlli del validator: apertura [manifest → integrità] →
//     binario nativo → Mod_Id → entry point), indipendente dall'output testato.

#include "lifecycle/mod_manifest_validator.hpp"
#include "lifecycle/manifest.hpp"
#include "package/pulse_package.hpp"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using pulse::lifecycle::ModManifestValidator;
using pulse::lifecycle::ValidationCause;
using pulse::lifecycle::ValidationResult;
using pulse::lifecycle::kModuleBinaryEntry;
using pulse::manifest::EntryPoint;
using pulse::manifest::Manifest;
using pulse::manifest::ModType;
using pulse::manifest::SemVer;
using pulse::manifest::serialize;
using pulse::package::Bytes;
using pulse::package::PackageArchive;
using pulse::package::PulsePackage;
using pulse::package::kIntegrityEntry;
using pulse::package::kManifestEntry;

// I quattro difetti coperti dalla Property 5, più i due controlli di
// bicondizionale: Valid (accettato) e l'insieme chiuso dei rifiuti.
enum class Defect {
    Valid,                // accettato (Req 2.* superati)
    MissingManifest,      // `pulse.toml` assente            → MissingRequiredFile (Req 2.2)
    MissingModuleBinary,  // mod nativa senza module.pulsebin → MissingRequiredFile (Req 2.2)
    UnparsableManifest,   // `pulse.toml` non analizzabile    → ManifestUnparsable  (Req 2.4)
    IntegrityMismatch,    // digest != MANIFEST.sha256        → IntegrityMismatch   (Req 2.6)
    NoEntryPoint,         // nessun entry point dichiarato    → NoEntryPoint        (Req 2.8)
};

Defect genDefect() {
    static constexpr std::array<Defect, 6> kAll{
        Defect::Valid,            Defect::MissingManifest,
        Defect::MissingModuleBinary, Defect::UnparsableManifest,
        Defect::IntegrityMismatch, Defect::NoEntryPoint};
    const int idx = *rc::gen::inRange(0, static_cast<int>(kAll.size()));
    return kAll[static_cast<std::size_t>(idx)];
}

// Mod_Id non vuoto (l'id vuoto è una causa distinta, fuori dallo scopo di P5).
std::string genModId(std::size_t i) {
    const auto n = static_cast<unsigned>(*rc::gen::inRange(0, 1000));
    return "mod.p5." + std::to_string(i) + "." + std::to_string(n);
}

// Manifest nativo valido con il Mod_Id dato e almeno un entry point.
Manifest makeValidManifest(const std::string& id) {
    Manifest m;
    m.schemaVersion = 1;
    m.id = id;
    m.version = SemVer{1, 0, 0};
    m.name = id;
    m.type = ModType::Native;
    m.entryPoints = {EntryPoint{"init", "mod_init"}};
    return m;
}

// Causa attesa per ciascun difetto secondo l'ordine dei controlli del validator.
ValidationCause expectedCause(Defect d) {
    switch (d) {
        case Defect::Valid:               return ValidationCause::Ok;
        case Defect::MissingManifest:     return ValidationCause::MissingRequiredFile;
        case Defect::MissingModuleBinary: return ValidationCause::MissingRequiredFile;
        case Defect::UnparsableManifest:  return ValidationCause::ManifestUnparsable;
        case Defect::IntegrityMismatch:   return ValidationCause::IntegrityMismatch;
        case Defect::NoEntryPoint:        return ValidationCause::NoEntryPoint;
    }
    return ValidationCause::Ok;
}

// Costruisce un `PackageArchive` in memoria che esibisce ESATTAMENTE il difetto
// richiesto ed è per il resto valido.
PackageArchive buildArchive(Defect d, const std::string& id, bool withIntegrity) {
    Manifest m = makeValidManifest(id);
    if (d == Defect::NoEntryPoint) {
        m.entryPoints.clear();  // valido per il parse, rifiutato dal validator (Req 2.8)
    }

    PackageArchive a;

    // --- pulse.toml -------------------------------------------------------
    if (d == Defect::UnparsableManifest) {
        // Testo che il parser TOML rifiuta (riga senza '=' né header valido).
        a.addText(std::string(kManifestEntry),
                  "questo non e' un manifest TOML valido");
    } else if (d != Defect::MissingManifest) {
        a.addText(std::string(kManifestEntry), serialize(m));
    }
    // d == MissingManifest → `pulse.toml` deliberatamente assente.

    // --- code/module.pulsebin --------------------------------------------
    const Bytes original{'B', 'I', 'N', 'A', 'R', 'Y', '-', 'A'};
    if (d != Defect::MissingModuleBinary) {
        a.add(std::string(kModuleBinaryEntry), original);
    }

    // --- MANIFEST.sha256 --------------------------------------------------
    if (d == Defect::IntegrityMismatch) {
        // Manifesto di integrità COERENTE con il contenuto corrente...
        a.addText(std::string(kIntegrityEntry),
                  PulsePackage::buildIntegrityManifest(a));
        // ...poi si MUTA il binario: il digest registrato non corrisponde più
        // (Req 2.5/2.6). Contenuto differente → hash differente garantito.
        a.add(std::string(kModuleBinaryEntry),
              Bytes{'B', 'I', 'N', 'A', 'R', 'Y', '-', 'B'});
    } else if (withIntegrity && d != Defect::MissingManifest &&
               d != Defect::UnparsableManifest) {
        // Per i casi che superano l'apertura, includiamo (a volte) un manifesto
        // di integrità CORRETTO per esercitare anche il percorso con verifica.
        a.addText(std::string(kIntegrityEntry),
                  PulsePackage::buildIntegrityManifest(a));
    }

    return a;
}

// --- Property 5 — rifiuto della validazione senza caricare codice ----------
// Feature: external-mod-loading, Property 5.
// Validates: Requirements 2.2, 2.4, 2.6, 2.8.
RC_GTEST_PROP(EmlProperty5ValidationRejection,
              RejectsMalformedWithoutLoadingCodeAndOthersProceed,
              ()) {
    const ModManifestValidator validator;

    const int batchSize = *rc::gen::inRange(1, 7);  // 1..6 package indipendenti

    // Esiti attesi per ogni package del batch, calcolati dall'oracolo.
    std::vector<Defect> defects;
    std::vector<ValidationResult> results;
    defects.reserve(static_cast<std::size_t>(batchSize));
    results.reserve(static_cast<std::size_t>(batchSize));

    for (int i = 0; i < batchSize; ++i) {
        const Defect d = genDefect();
        const std::string id = genModId(static_cast<std::size_t>(i));
        const bool withIntegrity = *rc::gen::arbitrary<bool>();
        defects.push_back(d);
        results.push_back(
            validator.validate(buildArchive(d, id, withIntegrity)));
    }

    // Ogni esito dipende SOLO dal proprio package (isolamento: le altre mod
    // proseguono) e rispetta l'oracolo della causa.
    for (int i = 0; i < batchSize; ++i) {
        const Defect d = defects[static_cast<std::size_t>(i)];
        const ValidationResult& r = results[static_cast<std::size_t>(i)];
        const ValidationCause want = expectedCause(d);

        // (1) Causa esatta attesa.
        RC_ASSERT(r.cause == want);

        // (2) Bicondizionale accepted ⇔ cause==Ok.
        RC_ASSERT(r.accepted == (r.cause == ValidationCause::Ok));

        if (d == Defect::Valid) {
            // Caso valido: accettato e manifest esposto.
            RC_ASSERT(r.accepted);
            RC_ASSERT(r.manifest.has_value());
        } else {
            // Casi malformati (Req 2.2/2.4/2.6/2.8): rifiuto SENZA caricare
            // contenuto → manifest == nullopt, diagnostica con causa non vuota.
            RC_ASSERT(!r.accepted);
            RC_ASSERT(!r.manifest.has_value());        // nessun contenuto caricato
            RC_ASSERT(!r.message.empty());             // diagnostica con causa
            RC_ASSERT(r.cause == ValidationCause::MissingRequiredFile ||
                      r.cause == ValidationCause::ManifestUnparsable ||
                      r.cause == ValidationCause::IntegrityMismatch ||
                      r.cause == ValidationCause::NoEntryPoint);
        }
    }
}

}  // namespace
