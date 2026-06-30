// tests/property37_install_gate_test.cpp
// Feature: pulse-sdk, Property 37 — Gate di installazione su firma e integrità.
// Validates: Requirements 23.3, 23.4, 23.5, 28.6, 28.7 (Requisiti 23.3–23.5,
// 28.6, 28.7)
//
// Property 37 (design.md): "Per ogni Pulse_Package, l'installazione e
// l'esecuzione del codice procedono se e solo se il package contiene una firma
// digitale valida e la verifica di integrità ha successo; in caso di firma
// assente/non valida o integrità fallita nessun file è installato e nessun
// codice è eseguito."
//
// Strategia (RapidCheck, ≥100 iterazioni per default): si genera un package in
// memoria variando IN MODO INDIPENDENTE tre assi booleani:
//   * firma presente / assente (entry SIGNATURE.sig presente o no);
//   * firma valida / non valida (pilota il verificatore fittizio iniettato);
//   * integrità corretta / corrotta (entry manomessa DOPO il calcolo del
//     MANIFEST.sha256, così l'hash non corrisponde più).
//
// INVARIANTE asserita:
//   (A) install ha successo (filesInstalled>0 AND codeExecuted==true) SE E SOLO
//       SE firma presente AND firma valida AND integrità corretta;
//   (B) in OGNI caso di fallimento filesInstalled==0 e codeExecuted==false
//       (nessuna installazione parziale, nessun codice eseguito) — verificato
//       anche tramite un RecordingSink che registra ZERO effetti;
//   (C) l'errore riflette il PRIMO gate fallito (la firma è verificata prima
//       dell'integrità: assente -> SignatureMissing; presente ma non valida ->
//       SignatureInvalid; firma OK ma integrità corrotta -> IntegrityFailed).

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "lifecycle/manifest.hpp"
#include "package/install_gate.hpp"
#include "package/pulse_package.hpp"

namespace {

using pulse::manifest::Dependency;
using pulse::manifest::EntryPoint;
using pulse::manifest::Manifest;
using pulse::manifest::ModType;
using pulse::manifest::SemVer;
using pulse::manifest::VersionConstraint;
using pulse::package::Bytes;
using pulse::package::IClock;
using pulse::package::IInstallSink;
using pulse::package::InstallError;
using pulse::package::InstallGate;
using pulse::package::InstallResult;
using pulse::package::ISignatureVerifier;
using pulse::package::PackageArchive;
using pulse::package::PulsePackage;

// --- Fake iniettabili ------------------------------------------------------

// Verificatore di firma fittizio: restituisce un esito fisso pilotato dal test
// (asse "firma valida / non valida"), senza crittografia reale.
class FakeVerifier final : public ISignatureVerifier {
public:
    explicit FakeVerifier(bool result) : result_(result) {}

    bool verify(const Bytes& /*payload*/,
                const Bytes& /*signature*/) const override {
        return result_;
    }

private:
    bool result_;
};

// Orologio deterministico entro il budget: la verifica della firma "dura"
// pochi ms, così l'asse temporale (Req 23.3) non interferisce con i tre assi
// sotto test (firma/integrità).
class FakeClock final : public IClock {
public:
    std::uint64_t nowMs() const override {
        std::uint64_t v = current_;
        current_ += 1;  // 1 ms per lettura: ben entro i 10 s di budget
        return v;
    }

private:
    mutable std::uint64_t current_{0};
};

// Sink in-memory che registra OGNI effetto: file installati ed esecuzioni di
// codice. Usato per asserire ZERO effetti collaterali sui percorsi di rifiuto.
class RecordingSink final : public IInstallSink {
public:
    void installFile(const std::string& path, const Bytes& data) override {
        files_.emplace_back(path, data);
    }

    void executeCode(const PulsePackage&) override { ++executions_; }

    [[nodiscard]] std::size_t fileCount() const { return files_.size(); }
    [[nodiscard]] int executions() const { return executions_; }

private:
    std::vector<std::pair<std::string, Bytes>> files_;
    int executions_{0};
};

// --- Helper di costruzione package -----------------------------------------

// Manifest valido minimale: il gate apre sempre il package (manifest valido),
// così i rifiuti dipendono SOLO dagli assi firma/integrità sotto test.
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

// Costruisce un PackageArchive secondo i tre assi indipendenti.
//   signaturePresent: aggiunge (o no) SIGNATURE.sig;
//   integrityCorrect : se false, manomette un'entry DOPO aver calcolato gli
//                       hash, così MANIFEST.sha256 non corrisponde più.
// La firma valida/non valida è pilotata dal FakeVerifier, non dall'archivio.
PackageArchive buildArchive(bool signaturePresent, bool integrityCorrect) {
    PackageArchive a;
    a.addText(std::string(pulse::package::kManifestEntry),
              pulse::manifest::serialize(makeValidManifest()));
    a.addText("code/mymod.bin", "BINARY-CODE-PLACEHOLDER");
    a.addText("resources/icon.png", "PNGDATA");

    // MANIFEST.sha256 coerente con il contenuto corrente.
    a.addText(std::string(pulse::package::kIntegrityEntry),
              PulsePackage::buildIntegrityManifest(a));

    if (!integrityCorrect) {
        // Manomette il codice DOPO il calcolo degli hash: l'integrità non
        // corrisponde più (Req 28.7) senza toccare MANIFEST.sha256.
        a.addText("code/mymod.bin", "TAMPERED-DIFFERENT-BYTES");
    }

    if (signaturePresent) {
        a.addText(std::string(pulse::package::kSignatureEntry),
                  "SIGNATURE-BYTES");
    }
    return a;
}

// ===========================================================================
// Property 37 — install riesce IFF firma presente AND valida AND integrità
// corretta; altrimenti zero file / zero codice e l'errore riflette il primo
// gate fallito (firma prima dell'integrità).
// Feature: pulse-sdk, Property 37.
// Validates: Requirements 23.3, 23.4, 23.5, 28.6, 28.7.
// ===========================================================================
RC_GTEST_PROP(Property37InstallGate,
              InstallSucceedsIffSignedValidAndIntegrityMatches,
              (bool signaturePresent, bool signatureValid,
               bool integrityCorrect)) {
    PackageArchive archive = buildArchive(signaturePresent, integrityCorrect);

    FakeVerifier verifier(signatureValid);
    FakeClock clock;
    RecordingSink sink;
    InstallGate gate(verifier, clock, sink);

    const InstallResult res = gate.install(std::move(archive));

    // Condizione di successo attesa: tutti e tre i gate superati.
    const bool shouldSucceed =
        signaturePresent && signatureValid && integrityCorrect;

    // (A) Invariante IFF su ok e sugli effetti osservabili.
    RC_ASSERT(res.ok == shouldSucceed);

    if (shouldSucceed) {
        // Installazione completata: file scritti + codice eseguito.
        RC_ASSERT(res.error == InstallError::None);
        RC_ASSERT(res.filesInstalled > 0u);
        RC_ASSERT(res.codeExecuted);
        // Gli effetti registrati dal sink coincidono con l'esito.
        RC_ASSERT(sink.fileCount() == res.filesInstalled);
        RC_ASSERT(sink.executions() == 1);
        // Budget di verifica della firma rispettato (Req 23.3).
        RC_ASSERT(res.signatureVerifyMs <=
                  InstallGate::kSignatureVerifyBudgetMs);
    } else {
        // (B) Nessuna installazione parziale, nessun codice eseguito.
        RC_ASSERT(res.filesInstalled == 0u);
        RC_ASSERT(!res.codeExecuted);
        // Zero effetti collaterali registrati dal sink.
        RC_ASSERT(sink.fileCount() == 0u);
        RC_ASSERT(sink.executions() == 0);
        // Messaggio diagnostico sempre presente sul rifiuto.
        RC_ASSERT(!res.message.empty());

        // (C) L'errore riflette il PRIMO gate fallito: la firma è verificata
        //     PRIMA dell'integrità.
        if (!signaturePresent) {
            // Req 23.4 — firma assente.
            RC_ASSERT(res.error == InstallError::SignatureMissing);
        } else if (!signatureValid) {
            // Req 23.5 — firma presente ma non valida.
            RC_ASSERT(res.error == InstallError::SignatureInvalid);
        } else {
            // Firma OK: l'unico motivo restante è l'integrità corrotta
            // (Req 28.6/28.7).
            RC_ASSERT(!integrityCorrect);
            RC_ASSERT(res.error == InstallError::IntegrityFailed);
        }
    }
}

}  // namespace
