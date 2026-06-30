// loader/package/install_gate.hpp — Gate di installazione su firma e integrità
// (Layer 4 — Lifecycle & Manifest, task 37.1).
//
// Requisiti coperti da questo modulo:
//   * Req 23.3 — WHEN un User scarica un Pulse_Package dal Marketplace, THE
//     Pulse_Loader SHALL verificare la firma digitale del Pulse_Package PRIMA
//     dell'installazione, completando la verifica entro 10 secondi. Qui il
//     budget temporale è modellato tramite un orologio iniettabile (`IClock`):
//     se la verifica eccede il budget l'installazione è rifiutata.
//   * Req 23.4 — IF un Pulse_Package non contiene una firma digitale, THEN il
//     Pulse_Loader SHALL rifiutare l'installazione, NON installare alcun file e
//     segnalare all'User l'assenza della firma.
//   * Req 23.5 — IF la verifica della firma fallisce, THEN il Pulse_Loader
//     SHALL rifiutare l'installazione, NON installare alcun file e mostrare un
//     messaggio di errore che indica la verifica fallita.
//   * Req 28.6 — WHEN un Pulse_Package viene caricato, THE Pulse_Loader SHALL
//     completare la verifica della sua integrità (`MANIFEST.sha256`) PRIMA di
//     eseguirne il codice.
//   * Req 28.7 — IF la verifica di integrità fallisce, THEN il Pulse_Loader
//     SHALL impedire l'esecuzione del codice del package e segnalare un errore.
//
// INVARIANTE CHIAVE: l'installazione (scrittura dei file) e l'esecuzione del
// codice avvengono SOLO quando SIA la firma SIA l'integrità superano la
// verifica. In caso di fallimento di una qualsiasi verifica NON viene scritto
// alcun file e NON viene eseguito alcun codice (nessuna installazione
// parziale). La firma è sempre verificata PRIMA di qualsiasi effetto di
// installazione (Req 23.3), e l'integrità PRIMA di qualsiasi esecuzione di
// codice (Req 28.6).
//
// Testabilità host (senza filesystem né crittografia reale): il verificatore
// di firma, l'orologio e il sink "installatore/esecutore" sono iniettati. I
// test costruiscono package in memoria tramite PackageArchive/PulsePackage e
// iniettano un verificatore fittizio + un sink ispezionabile per asserire che
// zero file siano stati installati e zero codice eseguito sui percorsi di
// rifiuto.
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).
#ifndef PULSE_LOADER_PACKAGE_INSTALL_GATE_HPP
#define PULSE_LOADER_PACKAGE_INSTALL_GATE_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "package/pulse_package.hpp"

namespace pulse::package {

// ---------------------------------------------------------------------------
// IClock — orologio iniettabile per modellare il budget di verifica della
// firma (Req 23.3: ≤10 s). Espone un tempo monotono in millisecondi; i test
// iniettano un orologio controllato per simulare durate arbitrarie.
// ---------------------------------------------------------------------------
class IClock {
public:
    virtual ~IClock() = default;

    // Tempo monotono corrente in millisecondi.
    [[nodiscard]] virtual std::uint64_t nowMs() const = 0;
};

// ---------------------------------------------------------------------------
// ISignatureVerifier — verificatore della firma del package iniettabile
// (analogo a pulse::loader::bindings::ISignatureVerifier). Disaccoppia la
// crittografia dal gate: in test si inietta un verificatore deterministico.
// ---------------------------------------------------------------------------
class ISignatureVerifier {
public:
    virtual ~ISignatureVerifier() = default;

    // Verifica `signature` sul `payload` (rappresentazione canonica del
    // contenuto del package). Restituisce true se la firma è valida e
    // attendibile, false altrimenti.
    [[nodiscard]] virtual bool verify(const Bytes& payload,
                                      const Bytes& signature) const = 0;
};

// ---------------------------------------------------------------------------
// IInstallSink — destinazione iniettabile degli EFFETTI di installazione.
// Modella la scrittura dei file su "disco" e l'esecuzione del codice del
// package senza toccare il filesystem reale. I test ne ispezionano lo stato
// per asserire zero file installati / zero codice eseguito sui rifiuti.
// ---------------------------------------------------------------------------
class IInstallSink {
public:
    virtual ~IInstallSink() = default;

    // Installa (scrive) una entry del package. Invocata SOLO dopo che firma e
    // integrità sono state verificate con successo.
    virtual void installFile(const std::string& path, const Bytes& data) = 0;

    // Esegue il codice del package. Invocata SOLO dopo la verifica di
    // integrità (Req 28.6) e dopo l'installazione dei file.
    virtual void executeCode(const PulsePackage& package) = 0;
};

// ---------------------------------------------------------------------------
// InstallError — causa di rifiuto dell'installazione.
// ---------------------------------------------------------------------------
enum class InstallError {
    None,
    PackageInvalid,    // manifest assente/invalido: nessun package apribile
    SignatureMissing,  // SIGNATURE.sig assente (Req 23.4)
    SignatureInvalid,  // firma presente ma non verificata (Req 23.5)
    SignatureTimeout,  // verifica oltre il budget di 10 s (Req 23.3)
    IntegrityFailed,   // MANIFEST.sha256 assente o non corrispondente (Req 28.7)
};

[[nodiscard]] inline std::string to_string(InstallError e) {
    switch (e) {
        case InstallError::None: return "none";
        case InstallError::PackageInvalid: return "package-invalid";
        case InstallError::SignatureMissing: return "signature-missing";
        case InstallError::SignatureInvalid: return "signature-invalid";
        case InstallError::SignatureTimeout: return "signature-timeout";
        case InstallError::IntegrityFailed: return "integrity-failed";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// InstallResult — esito osservabile dell'operazione di installazione.
// `ok == true` SOLO se firma e integrità sono entrambe valide: in tal caso i
// file sono stati installati e il codice eseguito. Su qualsiasi rifiuto
// `filesInstalled == 0` e `codeExecuted == false`.
// ---------------------------------------------------------------------------
struct InstallResult {
    bool ok{false};
    InstallError error{InstallError::None};
    std::string message;
    std::uint64_t signatureVerifyMs{0};  // durata misurata della verifica firma
    std::size_t filesInstalled{0};       // numero di file effettivamente scritti
    bool codeExecuted{false};            // se il codice del package è stato eseguito
};

// ===========================================================================
// InstallGate — orchestratore del gate di installazione.
//
// Pipeline (ordine garantito):
//   (1) apertura/validazione del manifest (nessun effetto di installazione);
//   (2) verifica della FIRMA — PRIMA di qualsiasi installazione (Req 23.3/.4/.5);
//   (3) verifica dell'INTEGRITÀ (MANIFEST.sha256) — PRIMA dell'esecuzione del
//       codice (Req 28.6/.7);
//   (4) SOLO se (2) e (3) passano: installazione dei file + esecuzione codice.
// ===========================================================================
class InstallGate {
public:
    // Budget di default per la verifica della firma (Req 23.3: 10 secondi).
    static constexpr std::uint64_t kSignatureVerifyBudgetMs = 10'000;

    // Dipendenze iniettate per riferimento: devono sopravvivere al gate.
    InstallGate(ISignatureVerifier& verifier, IClock& clock, IInstallSink& sink,
                std::uint64_t signatureBudgetMs = kSignatureVerifyBudgetMs)
        : verifier_(verifier),
          clock_(clock),
          sink_(sink),
          signatureBudgetMs_(signatureBudgetMs) {}

    // Esegue il gate su `archive` (consumato per move). Vedi pipeline sopra.
    [[nodiscard]] InstallResult install(PackageArchive archive) {
        InstallResult res;

        // (1) Manifest valido PRIMA di tutto (nessun effetto di installazione):
        // apre senza verificare l'integrità (la verifichiamo esplicitamente al
        // passo 3, dopo la firma, per garantire l'ordine firma -> integrità).
        PulsePackage::Options openOpts;
        openOpts.verifyIntegrity = false;
        OpenResult opened = PulsePackage::open(std::move(archive), openOpts);
        if (!opened.ok || !opened.package.has_value()) {
            res.ok = false;
            res.error = InstallError::PackageInvalid;
            res.message =
                "Pulse Package non valido: " + opened.message +
                " — installazione rifiutata, nessun file installato, nessun "
                "codice eseguito.";
            return res;  // nessun effetto: filesInstalled=0, codeExecuted=false
        }
        const PulsePackage& pkg = *opened.package;

        // (2) FIRMA prima di qualsiasi installazione (Req 23.3).
        if (!pkg.hasSignature()) {
            // Req 23.4 — firma assente: rifiuto, nessun file, nessun codice.
            res.ok = false;
            res.error = InstallError::SignatureMissing;
            res.message =
                "Pulse Package privo di firma digitale (SIGNATURE.sig): "
                "installazione rifiutata, nessun file installato.";
            return res;
        }

        // Payload firmato = rappresentazione canonica del contenuto del package
        // (il manifest di integrità copre ogni entry tranne firma/integrità).
        const Bytes canonicalPayload = canonicalSignedPayload(pkg.archive());
        const Bytes& signatureBytes = *pkg.signature();

        const std::uint64_t t0 = clock_.nowMs();
        const bool signatureValid = verifier_.verify(canonicalPayload, signatureBytes);
        const std::uint64_t t1 = clock_.nowMs();
        res.signatureVerifyMs = (t1 >= t0) ? (t1 - t0) : 0;

        if (res.signatureVerifyMs > signatureBudgetMs_) {
            // Req 23.3 — budget di verifica (≤10 s) superato: rifiuto.
            res.ok = false;
            res.error = InstallError::SignatureTimeout;
            res.message =
                "Verifica della firma oltre il budget di " +
                std::to_string(signatureBudgetMs_) + " ms (" +
                std::to_string(res.signatureVerifyMs) +
                " ms): installazione rifiutata, nessun file installato.";
            return res;
        }

        if (!signatureValid) {
            // Req 23.5 — firma non valida: rifiuto, nessun file, nessun codice.
            res.ok = false;
            res.error = InstallError::SignatureInvalid;
            res.message =
                "Verifica della firma digitale fallita: installazione "
                "rifiutata, nessun file installato.";
            return res;
        }

        // (3) INTEGRITÀ prima dell'esecuzione del codice (Req 28.6).
        if (!pkg.hasIntegrityManifest()) {
            res.ok = false;
            res.error = InstallError::IntegrityFailed;
            res.message =
                "Manifest di integrità 'MANIFEST.sha256' assente: esecuzione "
                "del codice impedita.";
            return res;
        }
        {
            const Bytes* integrity = pkg.archive().find(kIntegrityEntry);
            const std::string integrityText(
                reinterpret_cast<const char*>(integrity->data()),
                integrity->size());
            std::string mismatch;
            if (!PulsePackage::verifyIntegrityManifest(pkg.archive(),
                                                       integrityText, mismatch)) {
                // Req 28.7 — integrità fallita: nessun codice eseguito.
                res.ok = false;
                res.error = InstallError::IntegrityFailed;
                res.message =
                    "Verifica di integrità del Pulse Package fallita: " +
                    mismatch + " — esecuzione del codice impedita.";
                return res;
            }
        }

        // (4) Firma e integrità OK: installa i file ED esegui il codice.
        for (const std::string& path : pkg.archive().paths()) {
            const Bytes* data = pkg.archive().find(path);
            if (data == nullptr) continue;
            sink_.installFile(path, *data);
            ++res.filesInstalled;
        }
        sink_.executeCode(pkg);
        res.codeExecuted = true;

        res.ok = true;
        res.error = InstallError::None;
        res.message = "Installazione completata: firma e integrità verificate.";
        return res;
    }

private:
    // Rappresentazione canonica deterministica del contenuto del package usata
    // come payload firmato. Riusa il manifest di integrità (hash di ogni entry
    // tranne firma/integrità) senza reimplementare SHA-256.
    [[nodiscard]] static Bytes canonicalSignedPayload(const PackageArchive& archive) {
        const std::string text = PulsePackage::buildIntegrityManifest(archive);
        return Bytes(reinterpret_cast<const std::uint8_t*>(text.data()),
                     reinterpret_cast<const std::uint8_t*>(text.data()) + text.size());
    }

    ISignatureVerifier& verifier_;
    IClock& clock_;
    IInstallSink& sink_;
    std::uint64_t signatureBudgetMs_;
};

}  // namespace pulse::package

#endif  // PULSE_LOADER_PACKAGE_INSTALL_GATE_HPP
