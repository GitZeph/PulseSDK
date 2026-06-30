// tests/install_gate_test.cpp — Unit test del gate di installazione su firma e
// integrità (task 37.1, Req 23.3, 23.4, 23.5, 28.6, 28.7). Verifica che:
//   * firma valida + integrità valida -> l'installazione procede (file scritti
//     e codice eseguito);
//   * firma assente -> rifiuto, ZERO file installati, ZERO codice eseguito +
//     messaggio (Req 23.4);
//   * firma non valida -> stesso rifiuto, zero file, zero codice + messaggio
//     (Req 23.5);
//   * verifica della firma oltre il budget di 10 s -> rifiuto (Req 23.3);
//   * integrità (MANIFEST.sha256) mancante o non corrispondente -> rifiuto,
//     ZERO codice eseguito (Req 28.6/28.7);
//   * la firma è verificata PRIMA di qualsiasi effetto di installazione.
//
// Header-only: include "package/install_gate.hpp", "package/pulse_package.hpp"
// e "lifecycle/manifest.hpp" (radice loader/ nella include path). Usa
// PulsePackage/PackageArchive per costruire package in memoria e un
// ISignatureVerifier fittizio (nessuna crittografia reale).

#include "package/install_gate.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lifecycle/manifest.hpp"
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

// Verificatore di firma fittizio deterministico: restituisce un esito fisso e
// registra se è stato invocato (per provare l'ordine firma -> installazione).
class FakeVerifier final : public ISignatureVerifier {
public:
    explicit FakeVerifier(bool result) : result_(result) {}

    bool verify(const Bytes& payload, const Bytes& signature) const override {
        ++calls_;
        lastPayload_ = payload;
        lastSignature_ = signature;
        return result_;
    }

    [[nodiscard]] int calls() const { return calls_; }
    [[nodiscard]] const Bytes& lastPayload() const { return lastPayload_; }

private:
    bool result_;
    mutable int calls_{0};
    mutable Bytes lastPayload_;
    mutable Bytes lastSignature_;
};

// Orologio controllato: avanza di `stepMs` ad ogni lettura, così la verifica
// della firma "dura" esattamente `stepMs` millisecondi.
class FakeClock final : public IClock {
public:
    explicit FakeClock(std::uint64_t stepMs) : stepMs_(stepMs) {}

    std::uint64_t nowMs() const override {
        std::uint64_t v = current_;
        current_ += stepMs_;
        return v;
    }

private:
    std::uint64_t stepMs_;
    mutable std::uint64_t current_{0};
};

// Sink in-memory che registra file installati ed esecuzioni di codice. I test
// lo ispezionano per asserire zero effetti sui percorsi di rifiuto.
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

// Archivio base con manifest valido + entry di codice/risorse (senza firma né
// MANIFEST.sha256: aggiunti dai singoli test secondo lo scenario).
PackageArchive makeBaseArchive() {
    PackageArchive a;
    a.addText(std::string(pulse::package::kManifestEntry),
              pulse::manifest::serialize(makeValidManifest()));
    a.addText("code/mymod.bin", "BINARY-CODE-PLACEHOLDER");
    a.addText("resources/icon.png", "PNGDATA");
    return a;
}

// Aggiunge MANIFEST.sha256 coerente con il contenuto attuale dell'archivio.
void addIntegrity(PackageArchive& a) {
    a.addText(std::string(pulse::package::kIntegrityEntry),
              PulsePackage::buildIntegrityManifest(a));
}

// Aggiunge una firma fittizia (i byte non sono interpretati dal FakeVerifier).
void addSignature(PackageArchive& a) {
    a.addText(std::string(pulse::package::kSignatureEntry), "SIGNATURE-BYTES");
}

// --- Caso felice: firma valida + integrità valida -> installa --------------

TEST(InstallGate, ValidSignatureAndIntegrityInstallsAndExecutes) {
    PackageArchive a = makeBaseArchive();
    addIntegrity(a);  // integrità prima della firma (la firma copre il resto)
    addSignature(a);

    FakeVerifier verifier(true);
    FakeClock clock(5);  // 5 ms: ben entro il budget
    RecordingSink sink;
    InstallGate gate(verifier, clock, sink);

    InstallResult res = gate.install(std::move(a));

    ASSERT_TRUE(res.ok) << res.message;
    EXPECT_EQ(res.error, InstallError::None);
    EXPECT_TRUE(res.codeExecuted);
    EXPECT_GT(res.filesInstalled, 0u);
    // Gli effetti osservabili coincidono con l'esito.
    EXPECT_EQ(sink.fileCount(), res.filesInstalled);
    EXPECT_EQ(sink.executions(), 1);
    EXPECT_LE(res.signatureVerifyMs, InstallGate::kSignatureVerifyBudgetMs);
}

// --- Req 23.4: firma assente -> rifiuto, zero file, zero codice ------------

TEST(InstallGate, MissingSignatureRejectedNoFilesNoCode) {
    PackageArchive a = makeBaseArchive();
    addIntegrity(a);
    // Nessuna firma aggiunta.

    FakeVerifier verifier(true);  // verrebbe accettata, ma manca la firma
    FakeClock clock(1);
    RecordingSink sink;
    InstallGate gate(verifier, clock, sink);

    InstallResult res = gate.install(std::move(a));

    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error, InstallError::SignatureMissing);
    EXPECT_EQ(res.filesInstalled, 0u);
    EXPECT_FALSE(res.codeExecuted);
    EXPECT_EQ(sink.fileCount(), 0u);
    EXPECT_EQ(sink.executions(), 0);
    EXPECT_FALSE(res.message.empty());  // messaggio all'User (Req 23.4)
}

// --- Req 23.5: firma non valida -> rifiuto, zero file, zero codice ---------

TEST(InstallGate, InvalidSignatureRejectedNoFilesNoCode) {
    PackageArchive a = makeBaseArchive();
    addIntegrity(a);
    addSignature(a);

    FakeVerifier verifier(false);  // verifica fallisce
    FakeClock clock(1);
    RecordingSink sink;
    InstallGate gate(verifier, clock, sink);

    InstallResult res = gate.install(std::move(a));

    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error, InstallError::SignatureInvalid);
    EXPECT_EQ(res.filesInstalled, 0u);
    EXPECT_FALSE(res.codeExecuted);
    EXPECT_EQ(sink.fileCount(), 0u);
    EXPECT_EQ(sink.executions(), 0);
    EXPECT_FALSE(res.message.empty());  // messaggio di errore (Req 23.5)
    // La firma è stata effettivamente verificata.
    EXPECT_EQ(verifier.calls(), 1);
}

// --- Req 23.3: verifica oltre il budget di 10 s -> rifiuto -----------------

TEST(InstallGate, SignatureVerificationOverBudgetRejected) {
    PackageArchive a = makeBaseArchive();
    addIntegrity(a);
    addSignature(a);

    FakeVerifier verifier(true);
    // 10001 ms tra le due letture dell'orologio: oltre il budget di 10 s.
    FakeClock clock(InstallGate::kSignatureVerifyBudgetMs + 1);
    RecordingSink sink;
    InstallGate gate(verifier, clock, sink);

    InstallResult res = gate.install(std::move(a));

    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error, InstallError::SignatureTimeout);
    EXPECT_EQ(res.filesInstalled, 0u);
    EXPECT_FALSE(res.codeExecuted);
    EXPECT_GT(res.signatureVerifyMs, InstallGate::kSignatureVerifyBudgetMs);
}

// --- Req 28.7: integrità mancante -> rifiuto, zero codice ------------------

TEST(InstallGate, MissingIntegrityRejectedNoCode) {
    PackageArchive a = makeBaseArchive();
    addSignature(a);
    // Nessun MANIFEST.sha256.

    FakeVerifier verifier(true);
    FakeClock clock(1);
    RecordingSink sink;
    InstallGate gate(verifier, clock, sink);

    InstallResult res = gate.install(std::move(a));

    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error, InstallError::IntegrityFailed);
    EXPECT_FALSE(res.codeExecuted);
    EXPECT_EQ(sink.executions(), 0);
    EXPECT_EQ(sink.fileCount(), 0u);
}

// --- Req 28.6/28.7: integrità manomessa -> rifiuto, zero codice ------------

TEST(InstallGate, IntegrityMismatchRejectedNoCode) {
    PackageArchive a = makeBaseArchive();
    addIntegrity(a);  // hash calcolati sul contenuto corrente
    addSignature(a);
    // Manomette il codice DOPO aver calcolato gli hash di integrità.
    a.addText("code/mymod.bin", "TAMPERED-DIFFERENT-BYTES");

    FakeVerifier verifier(true);  // firma OK: il rifiuto deve venire dall'integrità
    FakeClock clock(1);
    RecordingSink sink;
    InstallGate gate(verifier, clock, sink);

    InstallResult res = gate.install(std::move(a));

    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error, InstallError::IntegrityFailed);
    EXPECT_FALSE(res.codeExecuted);
    EXPECT_EQ(sink.executions(), 0);
    EXPECT_EQ(sink.fileCount(), 0u);
}

// --- Ordine: la firma è verificata PRIMA di qualsiasi effetto --------------

TEST(InstallGate, SignatureCheckedBeforeAnyInstallEffect) {
    // Firma non valida MA integrità valida: se l'ordine fosse errato si
    // potrebbe installare qualcosa. Verifichiamo che la firma blocchi tutto.
    PackageArchive a = makeBaseArchive();
    addIntegrity(a);
    addSignature(a);

    FakeVerifier verifier(false);
    FakeClock clock(1);
    RecordingSink sink;
    InstallGate gate(verifier, clock, sink);

    InstallResult res = gate.install(std::move(a));

    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error, InstallError::SignatureInvalid);
    // La firma è stata verificata...
    EXPECT_EQ(verifier.calls(), 1);
    // ...e NESSUN effetto di installazione si è prodotto.
    EXPECT_EQ(sink.fileCount(), 0u);
    EXPECT_EQ(sink.executions(), 0);
}

}  // namespace
