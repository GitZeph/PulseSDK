// tests/marketplace_signer_test.cpp — Unit test della firma del Marketplace in
// pubblicazione (task 37.2, Req 23.1, 23.2). Verifica che:
//   * firma riuscita entro il budget -> SIGNATURE.sig registrato e package
//     pubblicato (reso disponibile) entro 5 s (Req 23.1);
//   * la firma prodotta è ACCETTATA da un verificatore corrispondente
//     (consistenza con install_gate.hpp: round-trip firma -> verifica);
//   * fallimento del signer -> pubblicazione rifiutata, ZERO package
//     pubblicati, messaggio per l'autore (Req 23.2);
//   * apposizione della firma oltre il budget di 5 s -> rifiuto, ZERO package
//     pubblicati (Req 23.1).
//
// Header-only: include "package/marketplace_signer.hpp" (che a sua volta
// riusa "package/install_gate.hpp" e "package/pulse_package.hpp") e
// "lifecycle/manifest.hpp" (radice loader/ nella include path). Usa
// PulsePackage/PackageArchive per costruire package in memoria e un ISigner
// fittizio (nessuna crittografia reale).

#include "package/marketplace_signer.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

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
using pulse::package::IPublishSink;
using pulse::package::ISigner;
using pulse::package::ISignatureVerifier;
using pulse::package::MarketplaceSigner;
using pulse::package::PackageArchive;
using pulse::package::PublishError;
using pulse::package::PublishResult;
using pulse::package::PulsePackage;

// --- Fake iniettabili ------------------------------------------------------

// Signer fittizio deterministico. In caso di successo produce una firma
// "verificabile": il prefisso fisso kTag seguito dai byte del payload, così un
// verificatore corrispondente (sotto) può ricostruire e confrontare. In caso
// di fallimento simula l'impossibilità di firmare (es. chiave indisponibile).
class FakeSigner final : public ISigner {
public:
    static constexpr const char* kTag = "PULSE-SIG::";

    explicit FakeSigner(bool succeed, std::string error = {})
        : succeed_(succeed), error_(std::move(error)) {}

    Result sign(const Bytes& payload) const override {
        ++calls_;
        Result r;
        if (!succeed_) {
            r.ok = false;
            r.error = error_.empty() ? "chiave di firma indisponibile" : error_;
            return r;
        }
        r.ok = true;
        // Firma = kTag || payload (schema fittizio, riproducibile dal verifier).
        std::string tag(kTag);
        r.signature.assign(tag.begin(), tag.end());
        r.signature.insert(r.signature.end(), payload.begin(), payload.end());
        return r;
    }

    [[nodiscard]] int calls() const { return calls_; }

private:
    bool succeed_;
    std::string error_;
    mutable int calls_{0};
};

// Verificatore corrispondente a FakeSigner: accetta una firma sse essa è
// esattamente kTag || payload. Implementa l'interfaccia del gate di
// installazione (install_gate.hpp) per dimostrare il round-trip.
class MatchingVerifier final : public ISignatureVerifier {
public:
    bool verify(const Bytes& payload, const Bytes& signature) const override {
        std::string tag(FakeSigner::kTag);
        Bytes expected(tag.begin(), tag.end());
        expected.insert(expected.end(), payload.begin(), payload.end());
        return signature == expected;
    }
};

// Orologio controllato: avanza di `stepMs` ad ogni lettura, così l'apposizione
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

// Registro/sink di pubblicazione ispezionabile: registra ogni package reso
// disponibile. I test asseriscono ZERO pubblicazioni sui percorsi di rifiuto.
class RecordingPublishSink final : public IPublishSink {
public:
    void publish(const PackageArchive& signedArchive) override {
        published_.push_back(signedArchive);
    }

    [[nodiscard]] std::size_t count() const { return published_.size(); }
    [[nodiscard]] const PackageArchive& last() const { return published_.back(); }

private:
    std::vector<PackageArchive> published_;
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

// Archivio pronto per la pubblicazione: manifest valido + codice/risorse +
// MANIFEST.sha256 (l'autore lo include; la firma è ciò che il Marketplace
// appone). Nessuna SIGNATURE.sig: la produce questo modulo.
PackageArchive makePublishableArchive() {
    PackageArchive a;
    a.addText(std::string(pulse::package::kManifestEntry),
              pulse::manifest::serialize(makeValidManifest()));
    a.addText("code/mymod.bin", "BINARY-CODE-PLACEHOLDER");
    a.addText("resources/icon.png", "PNGDATA");
    a.addText(std::string(pulse::package::kIntegrityEntry),
              PulsePackage::buildIntegrityManifest(a));
    return a;
}

// --- Req 23.1: firma riuscita -> registra SIGNATURE.sig e pubblica ---------

TEST(MarketplaceSigner, SuccessfulSigningRegistersSignatureAndPublishes) {
    PackageArchive a = makePublishableArchive();

    FakeSigner signer(true);
    FakeClock clock(5);  // 5 ms: ben entro il budget di 5 s
    RecordingPublishSink sink;
    MarketplaceSigner marketplace(signer, clock, sink);

    PublishResult res = marketplace.publish(std::move(a));

    ASSERT_TRUE(res.ok) << res.message;
    EXPECT_EQ(res.error, PublishError::None);
    EXPECT_TRUE(res.published);
    ASSERT_TRUE(res.signature.has_value());
    EXPECT_LE(res.signMs, MarketplaceSigner::kSignBudgetMs);
    EXPECT_EQ(signer.calls(), 1);

    // Il package è stato reso disponibile esattamente una volta...
    ASSERT_EQ(sink.count(), 1u);
    // ...con SIGNATURE.sig registrato nell'archivio pubblicato.
    const PackageArchive& publishedArchive = sink.last();
    ASSERT_TRUE(publishedArchive.contains(pulse::package::kSignatureEntry));
    const Bytes* sig = publishedArchive.find(pulse::package::kSignatureEntry);
    ASSERT_NE(sig, nullptr);
    EXPECT_EQ(*sig, *res.signature);
}

// --- Consistenza con install_gate: la firma prodotta è accettata -----------

TEST(MarketplaceSigner, ProducedSignatureAcceptedByMatchingVerifier) {
    PackageArchive a = makePublishableArchive();

    FakeSigner signer(true);
    FakeClock clock(1);
    RecordingPublishSink sink;
    MarketplaceSigner marketplace(signer, clock, sink);

    PublishResult res = marketplace.publish(std::move(a));
    ASSERT_TRUE(res.ok) << res.message;
    ASSERT_EQ(sink.count(), 1u);

    // Round-trip: il payload canonico verificato dal gate è il manifest di
    // integrità dell'archivio pubblicato (SIGNATURE.sig escluso). Un
    // verificatore corrispondente DEVE accettare la firma registrata.
    const PackageArchive& publishedArchive = sink.last();
    const std::string payloadText =
        PulsePackage::buildIntegrityManifest(publishedArchive);
    const Bytes payload(
        reinterpret_cast<const std::uint8_t*>(payloadText.data()),
        reinterpret_cast<const std::uint8_t*>(payloadText.data()) +
            payloadText.size());
    const Bytes* sig = publishedArchive.find(pulse::package::kSignatureEntry);
    ASSERT_NE(sig, nullptr);

    MatchingVerifier verifier;
    EXPECT_TRUE(verifier.verify(payload, *sig));
    // Sanity: una firma su un payload diverso NON è accettata.
    Bytes tampered = payload;
    tampered.push_back('X');
    EXPECT_FALSE(verifier.verify(tampered, *sig));
}

// --- Req 23.2: fallimento della firma -> rifiuto, niente pubblicazione -----

TEST(MarketplaceSigner, SigningFailureRejectsPublicationNothingAvailable) {
    PackageArchive a = makePublishableArchive();

    FakeSigner signer(false, "modulo HSM offline");
    FakeClock clock(1);
    RecordingPublishSink sink;
    MarketplaceSigner marketplace(signer, clock, sink);

    PublishResult res = marketplace.publish(std::move(a));

    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error, PublishError::SigningFailed);
    EXPECT_FALSE(res.published);
    EXPECT_FALSE(res.signature.has_value());
    // Nessun package reso disponibile per il download.
    EXPECT_EQ(sink.count(), 0u);
    // Messaggio per l'autore che indica il fallimento della firma (Req 23.2).
    EXPECT_FALSE(res.message.empty());
    EXPECT_NE(res.message.find("modulo HSM offline"), std::string::npos);
}

// --- Req 23.1: apposizione oltre il budget di 5 s -> rifiuto ---------------

TEST(MarketplaceSigner, SigningOverBudgetRejectsPublication) {
    PackageArchive a = makePublishableArchive();

    FakeSigner signer(true);  // produrrebbe una firma valida...
    // ...ma l'apposizione "dura" oltre il budget di 5 s.
    FakeClock clock(MarketplaceSigner::kSignBudgetMs + 1);
    RecordingPublishSink sink;
    MarketplaceSigner marketplace(signer, clock, sink);

    PublishResult res = marketplace.publish(std::move(a));

    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error, PublishError::SigningTimeout);
    EXPECT_FALSE(res.published);
    EXPECT_FALSE(res.signature.has_value());
    EXPECT_GT(res.signMs, MarketplaceSigner::kSignBudgetMs);
    // Nessun package reso disponibile per il download.
    EXPECT_EQ(sink.count(), 0u);
    EXPECT_FALSE(res.message.empty());
}

// --- Package invalido (manifest assente) -> rifiuto, niente pubblicazione --

TEST(MarketplaceSigner, InvalidPackageRejectedNothingPublished) {
    PackageArchive a;  // nessun pulse.toml
    a.addText("code/mymod.bin", "BINARY");

    FakeSigner signer(true);
    FakeClock clock(1);
    RecordingPublishSink sink;
    MarketplaceSigner marketplace(signer, clock, sink);

    PublishResult res = marketplace.publish(std::move(a));

    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error, PublishError::PackageInvalid);
    EXPECT_FALSE(res.published);
    EXPECT_EQ(sink.count(), 0u);
    // Il signer non viene nemmeno invocato su un package invalido.
    EXPECT_EQ(signer.calls(), 0);
}

}  // namespace
