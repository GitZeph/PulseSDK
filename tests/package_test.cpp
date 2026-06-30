// tests/package_test.cpp — Unit test del Pulse Package `.pulse` (task 13.3,
// Req 16.2). Verifica che:
//   * un package con `pulse.toml` valido si apra e il manifest sia analizzato
//     PRIMA di esporre qualsiasi entry di codice (l'oggetto esiste solo se il
//     manifest è valido);
//   * `pulse.toml` assente -> open fallisce (ManifestMissing) e nessun codice
//     è accessibile (nessun PulsePackage costruito);
//   * `pulse.toml` invalido -> open fallisce (ManifestInvalid), nessun codice;
//   * presenza e verifica di integrità di `MANIFEST.sha256` (match/mismatch);
//   * le entry sotto `code/` e `resources/` sono enumerabili e la firma
//     `SIGNATURE.sig` è esposta.
//
// Header-only: include "package/pulse_package.hpp" e "lifecycle/manifest.hpp"
// (radice loader/ nella include path); la logica di parsing del manifest è in
// dependency_resolver.cpp (in pulse::loader via glob lifecycle/*.cpp).

#include "package/pulse_package.hpp"

#include <string>

#include <gtest/gtest.h>

#include "lifecycle/manifest.hpp"

namespace {

using pulse::manifest::Dependency;
using pulse::manifest::EntryPoint;
using pulse::manifest::Manifest;
using pulse::manifest::ModType;
using pulse::manifest::SemVer;
using pulse::manifest::VersionConstraint;
using pulse::package::OpenError;
using pulse::package::PackageArchive;
using pulse::package::PulsePackage;

// Manifest valido minimale (id non vuoto, SemVer, almeno un entry point).
Manifest makeValidManifest() {
    Manifest m;
    m.schemaVersion = 1;
    m.id = "com.example.mymod";
    m.version = SemVer{1, 2, 0};
    m.name = "My Mod";
    m.type = ModType::Native;
    m.entryPoints = {EntryPoint{"init", "mymod_init"}};
    m.dependencies = {
        Dependency{"com.pulse.core",
                   VersionConstraint::range(SemVer{1, 0, 0}, SemVer{2, 0, 0})}};
    m.permissions = {"hooking", "ui"};
    return m;
}

// Costruisce un archivio `.pulse` di base con un pulse.toml valido + entry.
PackageArchive makeArchiveWithValidManifest() {
    PackageArchive a;
    a.addText(std::string(pulse::package::kManifestEntry),
              pulse::manifest::serialize(makeValidManifest()));
    a.addText("code/mymod.bin", "BINARY-CODE-PLACEHOLDER");
    a.addText("code/extra.bin", "MORE-CODE");
    a.addText("resources/icon.png", "PNGDATA");
    a.addText("resources/sfx/jump.wav", "WAVDATA");
    a.addText(std::string(pulse::package::kSignatureEntry), "SIGNATURE-BYTES");
    return a;
}

// --- Apertura con manifest valido -----------------------------------------

TEST(PulsePackage, OpensWithValidManifestAndParsesManifest) {
    PackageArchive a = makeArchiveWithValidManifest();
    PulsePackage::Options opts;
    opts.verifyIntegrity = false;  // nessun MANIFEST.sha256 in questo archivio

    auto res = PulsePackage::open(std::move(a), opts);
    ASSERT_TRUE(res.ok) << res.message;
    ASSERT_TRUE(res.package.has_value());
    EXPECT_EQ(res.error, OpenError::None);

    // Il manifest è stato analizzato (campi popolati) prima dell'accesso al codice.
    const Manifest& m = res.package->manifest();
    EXPECT_EQ(m.id, "com.example.mymod");
    EXPECT_EQ(m.version, (SemVer{1, 2, 0}));
    ASSERT_EQ(m.entryPoints.size(), 1u);
    EXPECT_EQ(m.entryPoints[0].symbol, "mymod_init");
}

// --- code/ e resources/ enumerabili ----------------------------------------

TEST(PulsePackage, EnumeratesCodeAndResourceEntries) {
    PackageArchive a = makeArchiveWithValidManifest();
    PulsePackage::Options opts;
    opts.verifyIntegrity = false;

    auto res = PulsePackage::open(std::move(a), opts);
    ASSERT_TRUE(res.ok) << res.message;

    auto code = res.package->codeEntries();
    ASSERT_EQ(code.size(), 2u);
    EXPECT_EQ(code[0], "code/extra.bin");  // std::map -> ordine deterministico
    EXPECT_EQ(code[1], "code/mymod.bin");

    auto resns = res.package->resourceEntries();
    ASSERT_EQ(resns.size(), 2u);
    EXPECT_EQ(resns[0], "resources/icon.png");
    EXPECT_EQ(resns[1], "resources/sfx/jump.wav");

    // Accesso per nome relativo e per percorso completo.
    const auto* byRel = res.package->codeEntry("mymod.bin");
    const auto* byFull = res.package->codeEntry("code/mymod.bin");
    ASSERT_NE(byRel, nullptr);
    ASSERT_NE(byFull, nullptr);
    EXPECT_EQ(byRel, byFull);

    // La firma è esposta.
    EXPECT_TRUE(res.package->hasSignature());
    ASSERT_NE(res.package->signature(), nullptr);
    EXPECT_FALSE(res.package->signature()->empty());
}

// --- Manifest assente -> rifiuto, nessun codice accessibile (Req 16.3) -----

TEST(PulsePackage, FailsWhenManifestMissingNoCodeAccessible) {
    PackageArchive a;
    // Solo codice/risorse, nessun pulse.toml.
    a.addText("code/mymod.bin", "BINARY");
    a.addText("resources/icon.png", "PNG");

    auto res = PulsePackage::open(std::move(a));
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error, OpenError::ManifestMissing);
    // Nessun PulsePackage costruito: il codice è strutturalmente inaccessibile.
    EXPECT_FALSE(res.package.has_value());
}

// --- Manifest invalido -> rifiuto, nessun codice (Req 16.4) ----------------

TEST(PulsePackage, FailsWhenManifestInvalidNoCodeAccessible) {
    PackageArchive a;
    a.addText(std::string(pulse::package::kManifestEntry),
              "[mod]\nid = \"x\"\nversion = \"not-a-semver\"\n");
    a.addText("code/mymod.bin", "BINARY");

    auto res = PulsePackage::open(std::move(a));
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error, OpenError::ManifestInvalid);
    EXPECT_FALSE(res.package.has_value());
    EXPECT_FALSE(res.message.empty());
}

// --- Integrità: MANIFEST.sha256 valido apre correttamente ------------------

TEST(PulsePackage, IntegrityManifestVerifiesSuccessfully) {
    PackageArchive a = makeArchiveWithValidManifest();
    // Genera l'integrity manifest sulle entry attuali e lo aggiunge.
    std::string integrity = PulsePackage::buildIntegrityManifest(a);
    a.addText(std::string(pulse::package::kIntegrityEntry), integrity);

    PulsePackage::Options opts;  // verifyIntegrity = true di default
    opts.requireIntegrityFile = true;

    auto res = PulsePackage::open(std::move(a), opts);
    ASSERT_TRUE(res.ok) << res.message;
    EXPECT_TRUE(res.package->hasIntegrityManifest());
}

// --- Integrità: contenuto manomesso -> IntegrityMismatch -------------------

TEST(PulsePackage, IntegrityMismatchOnTamperedContent) {
    PackageArchive a = makeArchiveWithValidManifest();
    std::string integrity = PulsePackage::buildIntegrityManifest(a);
    a.addText(std::string(pulse::package::kIntegrityEntry), integrity);
    // Manomette il contenuto di code/ DOPO aver calcolato gli hash.
    a.addText("code/mymod.bin", "TAMPERED-CODE-DIFFERENT-BYTES");

    auto res = PulsePackage::open(std::move(a));  // verifyIntegrity = true
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error, OpenError::IntegrityMismatch);
    EXPECT_FALSE(res.package.has_value());
}

// --- Integrità: file richiesto ma assente -> IntegrityMissing --------------

TEST(PulsePackage, IntegrityMissingWhenRequiredButAbsent) {
    PackageArchive a = makeArchiveWithValidManifest();  // nessun MANIFEST.sha256
    PulsePackage::Options opts;
    opts.requireIntegrityFile = true;

    auto res = PulsePackage::open(std::move(a), opts);
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error, OpenError::IntegrityMissing);
    EXPECT_FALSE(res.package.has_value());
}

// --- SHA-256: vettore di test noto (stringa vuota) -------------------------

TEST(PulsePackage, Sha256KnownVector) {
    // SHA-256("") = e3b0c442...855
    EXPECT_EQ(
        pulse::package::detail::sha256Hex(std::string_view("")),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // SHA-256("abc")
    EXPECT_EQ(
        pulse::package::detail::sha256Hex(std::string_view("abc")),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

}  // namespace
