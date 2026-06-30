// loader/package/marketplace_signer.hpp — Firma del Marketplace in
// pubblicazione (Layer 4 — Lifecycle & Manifest, task 37.2).
//
// Requisiti coperti da questo modulo:
//   * Req 23.1 — WHEN un Pulse_Package viene pubblicato nel Marketplace, THE
//     Marketplace SHALL apporre e registrare una firma digitale associata al
//     Pulse_Package, completando l'operazione entro 5 secondi dalla richiesta
//     di pubblicazione. Qui il budget temporale è modellato tramite un
//     orologio iniettabile (`IClock`, lo stesso usato dal gate di
//     installazione): se l'apposizione della firma eccede il budget la
//     pubblicazione è rifiutata.
//   * Req 23.2 — IF l'apposizione della firma digitale a un Pulse_Package
//     fallisce durante la pubblicazione, THEN THE Marketplace SHALL rifiutare
//     la pubblicazione, NON rendere disponibile il Pulse_Package per il
//     download, e mostrare all'autore un messaggio di errore che indica il
//     fallimento della firma.
//
// CONTROPARTE DEL GATE DI INSTALLAZIONE (install_gate.hpp, task 37.1): mentre
// il gate VERIFICA `SIGNATURE.sig` prima dell'installazione, questo modulo lo
// PRODUCE e lo REGISTRA in fase di pubblicazione. Per garantire il round-trip
// concettuale (una firma prodotta qui è accettata da un verificatore
// corrispondente), il payload firmato è ESATTAMENTE la stessa rappresentazione
// canonica che il gate verifica: i byte del manifest di integrità
// (`PulsePackage::buildIntegrityManifest`, che copre ogni entry tranne
// `SIGNATURE.sig` e `MANIFEST.sha256`). Aggiungere `SIGNATURE.sig` dopo aver
// calcolato il payload non altera il payload, perché quella entry è esclusa
// dal manifest di integrità.
//
// INVARIANTE CHIAVE: il Pulse_Package è reso disponibile (pubblicato sul
// registro/sink) SOLO quando la firma è stata apposta e registrata con
// successo entro il budget. In caso di fallimento della firma (errore del
// signer oppure budget superato) NIENTE viene pubblicato (nessuna
// disponibilità per il download) e all'autore è restituito un messaggio di
// errore.
//
// Testabilità host (senza crittografia né rete reale): il signer, l'orologio e
// il sink di pubblicazione sono iniettati. I test costruiscono package in
// memoria tramite PackageArchive/PulsePackage, iniettano un signer fittizio
// (nessuna crittografia) e un sink ispezionabile per asserire che, sui
// percorsi di rifiuto, ZERO package siano stati pubblicati.
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).
#ifndef PULSE_LOADER_PACKAGE_MARKETPLACE_SIGNER_HPP
#define PULSE_LOADER_PACKAGE_MARKETPLACE_SIGNER_HPP

#include <cstdint>
#include <optional>
#include <string>

#include "package/install_gate.hpp"  // riusa IClock (stesso orologio del gate)
#include "package/pulse_package.hpp"

namespace pulse::package {

// ---------------------------------------------------------------------------
// ISigner — produttore iniettabile della firma digitale su un payload.
// Disaccoppia la crittografia dalla logica di pubblicazione: in test si
// inietta un signer deterministico (nessuna crittografia reale). Modella sia
// il successo (firma prodotta) sia il fallimento (es. chiave indisponibile,
// servizio di firma non raggiungibile).
// ---------------------------------------------------------------------------
class ISigner {
public:
    virtual ~ISigner() = default;

    // Esito dell'apposizione della firma.
    struct Result {
        bool ok{false};        // true se la firma è stata prodotta
        Bytes signature;       // byte della firma (valorizzato sse ok)
        std::string error;     // descrizione del fallimento (sse !ok)
    };

    // Firma `payload` (rappresentazione canonica del contenuto del package).
    [[nodiscard]] virtual Result sign(const Bytes& payload) const = 0;
};

// ---------------------------------------------------------------------------
// IPublishSink — destinazione/registro iniettabile della pubblicazione.
// Modella il "rendere disponibile per il download" il Pulse_Package firmato
// senza toccare rete/filesystem reali. I test ne ispezionano lo stato per
// asserire ZERO package pubblicati sui percorsi di rifiuto (Req 23.2).
// ---------------------------------------------------------------------------
class IPublishSink {
public:
    virtual ~IPublishSink() = default;

    // Pubblica (rende disponibile) il package firmato. Invocata SOLO dopo che
    // la firma è stata apposta e registrata con successo entro il budget.
    virtual void publish(const PackageArchive& signedArchive) = 0;
};

// ---------------------------------------------------------------------------
// PublishError — causa di rifiuto della pubblicazione.
// ---------------------------------------------------------------------------
enum class PublishError {
    None,
    PackageInvalid,  // manifest assente/invalido: nessun package pubblicabile
    SigningFailed,   // il signer ha fallito l'apposizione della firma (Req 23.2)
    SigningTimeout,  // apposizione della firma oltre il budget di 5 s (Req 23.1)
};

[[nodiscard]] inline std::string to_string(PublishError e) {
    switch (e) {
        case PublishError::None: return "none";
        case PublishError::PackageInvalid: return "package-invalid";
        case PublishError::SigningFailed: return "signing-failed";
        case PublishError::SigningTimeout: return "signing-timeout";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// PublishResult — esito osservabile dell'operazione di pubblicazione.
// `ok == true` SOLO se la firma è stata apposta e registrata entro il budget:
// in tal caso il package è stato pubblicato e `signature` contiene la firma
// registrata. Su qualsiasi rifiuto `published == false` e nessuna firma è
// disponibile; `message` contiene il messaggio per l'autore.
// ---------------------------------------------------------------------------
struct PublishResult {
    bool ok{false};
    PublishError error{PublishError::None};
    std::string message;                 // messaggio per l'autore (Req 23.2)
    std::uint64_t signMs{0};             // durata misurata dell'apposizione firma
    bool published{false};               // se il package è stato reso disponibile
    std::optional<Bytes> signature;      // firma registrata (valorizzata sse ok)
};

// ===========================================================================
// MarketplaceSigner — orchestratore della firma in pubblicazione.
//
// Pipeline (ordine garantito):
//   (1) apertura/validazione del manifest (nessuna pubblicazione);
//   (2) calcolo del payload canonico (= manifest di integrità) e APPOSIZIONE
//       della firma tramite il signer iniettato, misurando il budget (Req 23.1);
//   (3) su FALLIMENTO della firma (errore del signer o budget superato):
//       rifiuto della pubblicazione, NESSUN package reso disponibile, messaggio
//       all'autore (Req 23.2);
//   (4) su SUCCESSO: REGISTRAZIONE di `SIGNATURE.sig` nell'archivio e
//       pubblicazione del package firmato sul sink.
// ===========================================================================
class MarketplaceSigner {
public:
    // Budget di default per l'apposizione della firma (Req 23.1: 5 secondi).
    static constexpr std::uint64_t kSignBudgetMs = 5'000;

    // Dipendenze iniettate per riferimento: devono sopravvivere al signer.
    MarketplaceSigner(ISigner& signer, IClock& clock, IPublishSink& sink,
                      std::uint64_t signBudgetMs = kSignBudgetMs)
        : signer_(signer),
          clock_(clock),
          sink_(sink),
          signBudgetMs_(signBudgetMs) {}

    // Pubblica `archive` (consumato per move). Vedi pipeline sopra.
    [[nodiscard]] PublishResult publish(PackageArchive archive) {
        PublishResult res;

        // (1) Manifest valido PRIMA di tutto (nessuna pubblicazione): apre
        // senza verificare l'integrità (la firma è ciò che produciamo qui).
        PulsePackage::Options openOpts;
        openOpts.verifyIntegrity = false;
        OpenResult opened = PulsePackage::open(std::move(archive), openOpts);
        if (!opened.ok || !opened.package.has_value()) {
            res.ok = false;
            res.error = PublishError::PackageInvalid;
            res.message =
                "Pulse Package non valido: " + opened.message +
                " — pubblicazione rifiutata, package non reso disponibile.";
            return res;  // niente firma, niente pubblicazione
        }

        // L'archivio (validato) torna in nostro possesso: ricostruiamo una
        // copia mutabile su cui registrare la firma in caso di successo.
        PackageArchive signedArchive = opened.package->archive();

        // (2) Payload canonico = stessa rappresentazione verificata dal gate.
        const Bytes payload = canonicalSignedPayload(signedArchive);

        const std::uint64_t t0 = clock_.nowMs();
        const ISigner::Result signRes = signer_.sign(payload);
        const std::uint64_t t1 = clock_.nowMs();
        res.signMs = (t1 >= t0) ? (t1 - t0) : 0;

        // (3a) Fallimento del signer -> rifiuto (Req 23.2).
        if (!signRes.ok) {
            res.ok = false;
            res.error = PublishError::SigningFailed;
            res.message =
                "Apposizione della firma digitale fallita: " +
                (signRes.error.empty() ? std::string("errore del firmatario")
                                       : signRes.error) +
                " — pubblicazione rifiutata, package non reso disponibile per "
                "il download.";
            return res;  // nessuna pubblicazione (sink intatto)
        }

        // (3b) Budget di apposizione (≤5 s) superato -> rifiuto (Req 23.1).
        if (res.signMs > signBudgetMs_) {
            res.ok = false;
            res.error = PublishError::SigningTimeout;
            res.message =
                "Apposizione della firma oltre il budget di " +
                std::to_string(signBudgetMs_) + " ms (" +
                std::to_string(res.signMs) +
                " ms): pubblicazione rifiutata, package non reso disponibile "
                "per il download.";
            return res;  // nessuna pubblicazione (sink intatto)
        }

        // (4) Successo: REGISTRA SIGNATURE.sig e pubblica il package firmato.
        signedArchive.add(std::string(kSignatureEntry), signRes.signature);
        sink_.publish(signedArchive);

        res.ok = true;
        res.error = PublishError::None;
        res.published = true;
        res.signature = signRes.signature;
        res.message =
            "Pubblicazione completata: firma digitale apposta e registrata.";
        return res;
    }

private:
    // Rappresentazione canonica deterministica del contenuto del package usata
    // come payload firmato. IDENTICA a quella verificata dal gate di
    // installazione (riusa il manifest di integrità: hash di ogni entry tranne
    // firma/integrità) per garantire il round-trip firma/verifica.
    [[nodiscard]] static Bytes canonicalSignedPayload(
        const PackageArchive& archive) {
        const std::string text = PulsePackage::buildIntegrityManifest(archive);
        return Bytes(
            reinterpret_cast<const std::uint8_t*>(text.data()),
            reinterpret_cast<const std::uint8_t*>(text.data()) + text.size());
    }

    ISigner& signer_;
    IClock& clock_;
    IPublishSink& sink_;
    std::uint64_t signBudgetMs_;
};

}  // namespace pulse::package

#endif  // PULSE_LOADER_PACKAGE_MARKETPLACE_SIGNER_HPP
