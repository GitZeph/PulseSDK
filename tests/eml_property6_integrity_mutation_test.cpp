// tests/eml_property6_integrity_mutation_test.cpp
// Feature: external-mod-loading, Property 6 — Integrità sensibile alla mutazione
// (proprietà METAMORFICA).
// Validates: Requirements 2.5, 2.6 (Requisiti 2.5, 2.6)
//
// Property 6 (design.md): per ogni Mod_Package valido, mutare un qualsiasi byte
// di una voce coperta da `MANIFEST.sha256` fa fallire la verifica di integrità e
// provoca il rifiuto della mod.
//
// Relazione metamorfica verificata:
//   * BASELINE — un Mod_Package valido (manifest analizzabile, mod nativa con
//     `code/module.pulsebin` presente, integrità coerente) è ACCETTATO dal
//     `ModManifestValidator`.
//   * MUTANTE  — lo STESSO package con UN SOLO byte mutato in una QUALSIASI voce
//     coperta da `MANIFEST.sha256` (`pulse.toml`, `code/module.pulsebin`,
//     `resources/...`) è RIFIUTATO.
//
// Causa esatta del rifiuto (oracolo preciso, coerente con l'ordine garantito da
// `PulsePackage::open`: manifest → integrità):
//   * se la voce mutata è il manifest e i byte mutati NON sono più analizzabili
//     → il rifiuto avviene al passo manifest con causa `ManifestUnparsable`;
//   * in ogni altro caso (voce binaria/risorsa mutata, oppure manifest mutato
//     ma ancora analizzabile) il manifest è valido ma il digest della voce
//     differisce da quello dichiarato in `MANIFEST.sha256` → il rifiuto avviene
//     al passo integrità con causa `IntegrityMismatch` (Req 2.5/2.6).
// In entrambi i casi la mod è RIFIUTATA (`accepted == false`) e nessun manifest
// è esposto.
//
// La mutazione di un byte è garantita "reale": si fa lo XOR del byte scelto con
// un delta in [1, 255], quindi il valore cambia sempre e il digest SHA-256 della
// voce non può coincidere con quello registrato.
//
// Strategia (RapidCheck, ≥100 iterazioni per default):
//   * payload binario/risorsa generati con byte arbitrari e lunghezza ≥1;
//   * manifest valido (id non vuoto, mod nativa, ≥1 entry point) serializzato in
//     forma canonica `pulse.toml`;
//   * si sceglie a caso UNA voce coperta tra {pulse.toml, code/module.pulsebin,
//     resources/data.bin}, UN indice di byte nel suo intervallo e UN delta di
//     mutazione; si ricostruisce il package mantenendo l'`MANIFEST.sha256`
//     ORIGINALE (calcolato sui byte NON mutati).
//   L'oracolo è indipendente dal validatore: ri-analizza i byte (mutati) del
//   manifest per decidere se attendersi `ManifestUnparsable` o `IntegrityMismatch`.

#include "lifecycle/manifest.hpp"
#include "lifecycle/mod_manifest_validator.hpp"
#include "package/pulse_package.hpp"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <array>
#include <cstdint>
#include <string>

namespace {

using pulse::lifecycle::ModManifestValidator;
using pulse::lifecycle::ValidationCause;
using pulse::lifecycle::ValidationResult;
using pulse::manifest::EntryPoint;
using pulse::manifest::Manifest;
using pulse::manifest::ModType;
using pulse::manifest::SemVer;
using pulse::package::Bytes;
using pulse::package::PackageArchive;
using pulse::package::PulsePackage;
namespace pkg = pulse::package;

// Percorsi delle voci coperte da MANIFEST.sha256 nel package di prova.
inline constexpr std::string_view kModuleEntry = "code/module.pulsebin";
inline constexpr std::string_view kResourceEntry = "resources/data.bin";

// --- Generatori --------------------------------------------------------------

// Byte arbitrari, lunghezza ≥1 (necessaria per avere almeno un byte mutabile).
Bytes genNonEmptyBytes() {
    const int n = *rc::gen::inRange(1, 33);  // 1..32 byte
    Bytes b;
    b.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        b.push_back(static_cast<std::uint8_t>(*rc::gen::inRange(0, 256)));
    }
    return b;
}

// Mod_Id valido non vuoto: "mod." + lettere minuscole.
std::string genModId() {
    const int n = *rc::gen::inRange(1, 7);  // 1..6 lettere
    std::string id = "mod.";
    for (int i = 0; i < n; ++i) {
        id.push_back(static_cast<char>('a' + *rc::gen::inRange(0, 26)));
    }
    return id;
}

SemVer genSmallSemVer() {
    const auto a = static_cast<std::uint32_t>(*rc::gen::inRange(0, 5));
    const auto b = static_cast<std::uint32_t>(*rc::gen::inRange(0, 5));
    const auto c = static_cast<std::uint32_t>(*rc::gen::inRange(0, 5));
    return SemVer{a, b, c};
}

// Manifest valido per il validatore: mod nativa, id non vuoto, ≥1 entry point.
Manifest genValidManifest() {
    Manifest m;
    m.schemaVersion = 1;
    m.id = genModId();
    m.version = genSmallSemVer();
    m.name = "EML Property 6 fixture";
    m.type = ModType::Native;
    m.entryPoints.push_back(EntryPoint{"init", "pulse_mod_init"});
    return m;
}

// Costruisce un Mod_Package valido come PackageArchive, calcolando
// `MANIFEST.sha256` sulle voci coperte (manifest, binario, risorsa).
PackageArchive buildArchive(const std::string& manifestText,
                            const Bytes& moduleBytes,
                            const Bytes& resourceBytes) {
    PackageArchive a;
    a.addText(std::string(pkg::kManifestEntry), manifestText);
    a.add(std::string(kModuleEntry), moduleBytes);
    a.add(std::string(kResourceEntry), resourceBytes);
    // L'integrità copre tutte le voci tranne MANIFEST.sha256/SIGNATURE.sig.
    a.addText(std::string(pkg::kIntegrityEntry),
              PulsePackage::buildIntegrityManifest(a));
    return a;
}

Bytes textToBytes(std::string_view s) {
    return Bytes(reinterpret_cast<const std::uint8_t*>(s.data()),
                 reinterpret_cast<const std::uint8_t*>(s.data()) + s.size());
}

// --- Property 6 — integrità sensibile alla mutazione (metamorfica) -----------
// Feature: external-mod-loading, Property 6.
// Validates: Requirements 2.5, 2.6.
RC_GTEST_PROP(EmlProperty6IntegrityMutation,
              AnyMutatedCoveredByteIsRejected,
              ()) {
    const Manifest manifest = genValidManifest();
    const std::string manifestText = pulse::manifest::serialize(manifest);
    const Bytes moduleBytes = genNonEmptyBytes();
    const Bytes resourceBytes = genNonEmptyBytes();

    const Bytes manifestBytes = textToBytes(manifestText);

    // --- BASELINE: il package valido è ACCETTATO (relazione metamorfica). ---
    {
        const ValidationResult base =
            ModManifestValidator{}.validate(
                buildArchive(manifestText, moduleBytes, resourceBytes));
        RC_ASSERT(base.accepted);
        RC_ASSERT(base.cause == ValidationCause::Ok);
        RC_ASSERT(base.manifest.has_value());
    }

    // --- Scelta della voce coperta da mutare e del byte. -------------------
    // 0 = manifest (pulse.toml), 1 = code/module.pulsebin, 2 = resources/data.bin.
    const int which = *rc::gen::inRange(0, 3);
    const Bytes& target = (which == 0) ? manifestBytes
                          : (which == 1) ? moduleBytes
                                         : resourceBytes;
    const std::size_t idx = static_cast<std::size_t>(
        *rc::gen::inRange<std::size_t>(0, target.size()));
    const auto delta = static_cast<std::uint8_t>(*rc::gen::inRange(1, 256));

    // Copie mutate: si applica lo XOR (cambio di byte garantito) a UNA sola voce.
    Bytes mutManifest = manifestBytes;
    Bytes mutModule = moduleBytes;
    Bytes mutResource = resourceBytes;
    if (which == 0) {
        mutManifest[idx] = static_cast<std::uint8_t>(mutManifest[idx] ^ delta);
    } else if (which == 1) {
        mutModule[idx] = static_cast<std::uint8_t>(mutModule[idx] ^ delta);
    } else {
        mutResource[idx] = static_cast<std::uint8_t>(mutResource[idx] ^ delta);
    }

    // Ricostruzione del package: voce mutata + MANIFEST.sha256 ORIGINALE
    // (calcolato sui byte NON mutati). Così il digest registrato non combacia
    // più con i byte effettivi della voce mutata.
    PackageArchive mutated;
    mutated.add(std::string(pkg::kManifestEntry), mutManifest);
    mutated.add(std::string(kModuleEntry), mutModule);
    mutated.add(std::string(kResourceEntry), mutResource);
    mutated.addText(std::string(pkg::kIntegrityEntry),
                    PulsePackage::buildIntegrityManifest(
                        buildArchive(manifestText, moduleBytes, resourceBytes)));
    // (buildIntegrityManifest sopra ricava il testo originale: copre solo le voci
    //  coperte e ignora MANIFEST.sha256, quindi è il digest dei byte NON mutati.)

    // --- MUTANTE: il package mutato è RIFIUTATO. ---------------------------
    const ValidationResult res = ModManifestValidator{}.validate(std::move(mutated));

    RC_ASSERT(!res.accepted);                 // mod rifiutata (Req 2.6)
    RC_ASSERT(!res.manifest.has_value());     // nessun manifest esposto su rifiuto

    // --- Oracolo preciso della causa --------------------------------------
    // Ordine garantito da PulsePackage::open: manifest → integrità.
    const std::string mutManifestText(
        reinterpret_cast<const char*>(mutManifest.data()), mutManifest.size());
    const bool manifestStillParses = pulse::manifest::parse(mutManifestText).ok;

    if (which == 0 && !manifestStillParses) {
        // Il manifest mutato non è più analizzabile: rifiuto al passo manifest.
        RC_ASSERT(res.cause == ValidationCause::ManifestUnparsable);
    } else {
        // Manifest valido ma digest della voce coperta non conforme: il rifiuto
        // è dovuto alla verifica di integrità (Req 2.5/2.6).
        RC_ASSERT(res.cause == ValidationCause::IntegrityMismatch);
    }
}

}  // namespace
