// loader/telemetry/crash_report.hpp — generazione e gestione dei CrashReport
// (Layer Telemetria, Requisito 21).
//
// Implementa il modello descritto nel design ("Crash Report — Req 21"):
//
//   struct CrashReport {
//       std::optional<ModId> attributedMod; // null se non attribuibile (Req 21.3)
//       std::optional<SemVer> modVersion;
//       StackTrace stack;                    // disponibile al momento del crash
//       Timestamp when;
//       bool transmitted;
//       int retryCount;                      // max 3 (Req 21.5)
//   };
//
// Contratto richiesto dai requisiti (Requisito 21):
//   * Req 21.1 — WHEN il processo termina per un crash con il loader attivo,
//     generare ENTRO 10 SECONDI un report contenente la traccia dello stack
//     disponibile al momento del crash.
//   * Req 21.2 — WHERE il report è attribuibile a una Mod, includere id univoco
//     e versione di quella Mod.
//   * Req 21.3 — IF il report NON è attribuibile a nessuna Mod, contrassegnarlo
//     come non attribuito (attributedMod == nullopt) e conservarlo localmente.
//   * Req 21.4 — WHERE l'User ha acconsentito alla telemetria, trasmettere il
//     report ENTRO 30 SECONDI (opt-in: nessuna trasmissione senza consenso).
//   * Req 21.5 — IF la trasmissione fallisce, conservare il report localmente e
//     ritentare fino a un massimo di 3 tentativi (retryCount <= 3).
//   * Req 21.6 — IF l'User NON ha acconsentito, conservare il report SOLO
//     localmente per ALMENO 30 giorni.
//
// Testabilità: tutte le dipendenze osservabili sono INIETTABILI, così l'intera
// logica è verificabile sull'host senza un crash reale, senza rete e senza
// disco:
//   * un clock `now()` (secondi) governa i budget di generazione/trasmissione;
//   * un "capturer" fornisce la traccia dello stack (modellata come vettore di
//     frame) e può far avanzare il clock per simulare il costo della cattura;
//   * un "transmitter" simula l'invio e può essere forzato a fallire;
//   * uno "store" (ICrashReportStore) modella la ritenzione locale ed espone la
//     scadenza (now + 30 giorni) che il test può asserire.
//
// Stack: C++20/23 (Requisito 26.1). Header-only. Non thread-safe: guidato dal
// thread di crash-handling/telemetria.
#ifndef PULSE_LOADER_TELEMETRY_CRASH_REPORT_HPP
#define PULSE_LOADER_TELEMETRY_CRASH_REPORT_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pulse::telemetry {

// ---------------------------------------------------------------------------
// Tipi di base (coerenti con il resto del progetto: ModId è std::string).
// ---------------------------------------------------------------------------

// Identità della Mod imputata (coerente con pulse::hooking::ModId).
using ModId = std::string;

// Istante in secondi (epoca arbitraria ma monotona nell'ambito della logica).
using Timestamp = std::int64_t;

// Un frame della traccia dello stack: stringa simbolica (es. "Func+0x12").
using StackFrame = std::string;

// Traccia dello stack disponibile al momento del crash (Req 21.1).
using StackTrace = std::vector<StackFrame>;

// ---------------------------------------------------------------------------
// SemVer — versione semantica minimale della Mod imputata (Req 21.2).
// Forma sufficiente a identificare la versione nel report; il confronto fine
// è gestito altrove (lifecycle).
// ---------------------------------------------------------------------------
struct SemVer {
    std::uint32_t major{0};
    std::uint32_t minor{0};
    std::uint32_t patch{0};

    [[nodiscard]] std::string toString() const {
        return std::to_string(major) + '.' + std::to_string(minor) + '.' +
               std::to_string(patch);
    }

    friend bool operator==(const SemVer&, const SemVer&) = default;
};

// ---------------------------------------------------------------------------
// Budget e limiti dai requisiti.
// ---------------------------------------------------------------------------

// Budget di generazione del report (Req 21.1): <= 10 secondi.
inline constexpr std::int64_t kGenerationBudgetSeconds = 10;

// Budget di trasmissione del report (Req 21.4): <= 30 secondi.
inline constexpr std::int64_t kTransmissionBudgetSeconds = 30;

// Numero massimo di ritentativi di trasmissione (Req 21.5).
inline constexpr int kMaxTransmissionRetries = 3;

// Ritenzione locale minima senza consenso (Req 21.6): 30 giorni in secondi.
inline constexpr std::int64_t kLocalRetentionDays = 30;
inline constexpr std::int64_t kLocalRetentionSeconds =
    kLocalRetentionDays * 24 * 60 * 60;

// ---------------------------------------------------------------------------
// CrashReport — modello del report (vedi design, Req 21).
// ---------------------------------------------------------------------------
struct CrashReport {
    std::optional<ModId> attributedMod{};   // nullopt se non attribuibile (Req 21.3)
    std::optional<SemVer> modVersion{};      // versione della Mod imputata (Req 21.2)
    StackTrace stack{};                      // traccia al momento del crash (Req 21.1)
    Timestamp when{0};                       // istante di generazione
    bool transmitted{false};                 // trasmesso con successo (Req 21.4)
    int retryCount{0};                       // ritentativi effettuati, <= 3 (Req 21.5)

    // Secondi impiegati a generare il report (per il budget di Req 21.1).
    std::int64_t generationElapsedSeconds{0};
    // Secondi impiegati nella trasmissione (per il budget di Req 21.4).
    std::int64_t transmissionElapsedSeconds{0};

    // True se il report è stato attribuito a una Mod specifica (Req 21.2);
    // false => non attribuito (Req 21.3).
    [[nodiscard]] bool isAttributed() const noexcept {
        return attributedMod.has_value();
    }

    // True se la generazione è rientrata nel budget di 10 s (Req 21.1).
    [[nodiscard]] bool withinGenerationBudget() const noexcept {
        return generationElapsedSeconds <= kGenerationBudgetSeconds;
    }

    // True se la trasmissione è rientrata nel budget di 30 s (Req 21.4).
    [[nodiscard]] bool withinTransmissionBudget() const noexcept {
        return transmissionElapsedSeconds <= kTransmissionBudgetSeconds;
    }

    // Istante fino al quale il report va conservato localmente (Req 21.6).
    [[nodiscard]] Timestamp retainedUntil() const noexcept {
        return when + kLocalRetentionSeconds;
    }
};

// ---------------------------------------------------------------------------
// Esito dell'elaborazione di un report (consenso + trasmissione/ritenzione).
// ---------------------------------------------------------------------------
enum class TelemetryOutcome {
    Transmitted,                 // consenso + invio riuscito entro 30 s (Req 21.4)
    TransmissionFailedRetained,  // consenso ma invio fallito dopo 3 ritentativi:
                                 // report conservato localmente (Req 21.5)
    RetainedNoConsent,           // nessun consenso: solo ritenzione locale (Req 21.6)
};

// ---------------------------------------------------------------------------
// Dipendenze iniettabili.
// ---------------------------------------------------------------------------

// Clock iniettabile: ritorna "ora" in secondi. Governa i budget temporali.
using TelemetryClock = std::function<Timestamp()>;

// Cattura della traccia dello stack al momento del crash (Req 21.1). Può far
// avanzare il clock iniettato per simulare il costo della cattura.
using StackCapturer = std::function<StackTrace()>;

// Predicato di consenso (opt-in, Req 21.4): true sse l'User ha acconsentito.
using ConsentPredicate = std::function<bool()>;

// Trasmettitore iniettabile: tenta l'invio e ritorna true sse riuscito. Può
// far avanzare il clock iniettato per simulare la latenza di rete. Il parametro
// è l'indice del tentativo (0 = primo invio, 1..3 = ritentativi).
using Transmitter = std::function<bool(int attemptIndex)>;

// ---------------------------------------------------------------------------
// ICrashReportStore — ritenzione locale del report (Req 21.3, 21.5, 21.6).
// Astratto e iniettabile: i test usano un'implementazione in-memory.
// ---------------------------------------------------------------------------
class ICrashReportStore {
public:
    virtual ~ICrashReportStore() = default;

    // Conserva il report localmente fino all'istante `expiry` (incluso).
    virtual void retain(const CrashReport& report, Timestamp expiry) = 0;
};

// ---------------------------------------------------------------------------
// InMemoryCrashReportStore — store in-memory per host/test (no disco).
// ---------------------------------------------------------------------------
class InMemoryCrashReportStore final : public ICrashReportStore {
public:
    struct Entry {
        CrashReport report;
        Timestamp expiry{0};  // istante di scadenza della ritenzione (Req 21.6)
    };

    void retain(const CrashReport& report, Timestamp expiry) override {
        entries_.push_back(Entry{report, expiry});
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept {
        return entries_;
    }

    // Numero di report ancora validi all'istante `now` (non scaduti).
    [[nodiscard]] std::size_t retainedCountAt(Timestamp now) const {
        std::size_t n = 0;
        for (const auto& e : entries_) {
            if (e.expiry >= now) {
                ++n;
            }
        }
        return n;
    }

private:
    std::vector<Entry> entries_{};
};

// Clock di default: orologio di sistema in secondi.
[[nodiscard]] inline TelemetryClock default_telemetry_clock() {
    return [] {
        // Evita la dipendenza diretta da <chrono> nel default: i chiamanti
        // tipicamente iniettano il proprio clock. Qui ritorniamo 0 come base
        // neutra; il runtime reale fornisce un clock di sistema.
        return Timestamp{0};
    };
}

// ---------------------------------------------------------------------------
// CrashReporter — generazione e gestione dei CrashReport (Req 21).
//
// Uso tipico (runtime del loader, al rilevamento di un crash):
//   CrashReporter reporter{clock, capturer, transmitter, consent, store};
//   CrashReport report = reporter.generate(modId, modVersion);  // Req 21.1/21.2/21.3
//   TelemetryOutcome outcome = reporter.process(report);        // Req 21.4/21.5/21.6
// ---------------------------------------------------------------------------
class CrashReporter {
public:
    CrashReporter(TelemetryClock clock,
                  StackCapturer capturer,
                  Transmitter transmitter,
                  ConsentPredicate consent,
                  ICrashReportStore& store)
        : clock_(clock ? std::move(clock) : default_telemetry_clock()),
          capturer_(std::move(capturer)),
          transmitter_(std::move(transmitter)),
          consent_(std::move(consent)),
          store_(store) {}

    // -----------------------------------------------------------------------
    // Generazione del report (Req 21.1, 21.2, 21.3).
    //
    // Cattura la traccia dello stack disponibile al momento del crash e misura
    // il tempo di generazione tramite il clock iniettato (budget <= 10 s).
    // Se `mod`/`version` sono forniti, il report è ATTRIBUITO (Req 21.2);
    // altrimenti resta NON attribuito (attributedMod == nullopt, Req 21.3).
    // -----------------------------------------------------------------------
    [[nodiscard]] CrashReport generate(std::optional<ModId> mod = std::nullopt,
                                       std::optional<SemVer> version = std::nullopt) {
        const Timestamp start = now();
        StackTrace stack = capturer_ ? capturer_() : StackTrace{};
        const Timestamp end = now();

        CrashReport report{};
        report.attributedMod = std::move(mod);
        // La versione ha senso solo per un report attribuito (Req 21.2).
        report.modVersion = report.attributedMod.has_value() ? version : std::nullopt;
        report.stack = std::move(stack);
        report.when = start;
        report.transmitted = false;
        report.retryCount = 0;
        report.generationElapsedSeconds = end - start;
        report.transmissionElapsedSeconds = 0;
        return report;
    }

    // -----------------------------------------------------------------------
    // Elaborazione del report (Req 21.4, 21.5, 21.6).
    //
    // Opt-in (Req 21.4): trasmette SOLO se l'User ha acconsentito. In caso di
    // consenso tenta l'invio entro il budget di 30 s, con al massimo 3
    // ritentativi su fallimento (Req 21.5); se la trasmissione fallisce in modo
    // persistente, il report è conservato localmente. Senza consenso, nessuna
    // trasmissione: il report è conservato localmente per >= 30 giorni
    // (Req 21.6). Aggiorna `report` (transmitted, retryCount, elapsed).
    // -----------------------------------------------------------------------
    TelemetryOutcome process(CrashReport& report) {
        const bool consented = consent_ && consent_();

        if (!consented) {
            // Req 21.6 — solo ritenzione locale per >= 30 giorni.
            report.transmitted = false;
            store_.retain(report, report.retainedUntil());
            return TelemetryOutcome::RetainedNoConsent;
        }

        // Req 21.4/21.5 — trasmissione opt-in con ritentativi.
        const Timestamp start = now();
        report.retryCount = 0;

        bool ok = attempt(/*attemptIndex=*/0);
        while (!ok && report.retryCount < kMaxTransmissionRetries) {
            ++report.retryCount;
            ok = attempt(report.retryCount);
        }

        const Timestamp end = now();
        report.transmissionElapsedSeconds = end - start;
        report.transmitted = ok;

        if (ok) {
            return TelemetryOutcome::Transmitted;  // Req 21.4
        }

        // Req 21.5 — invio fallito dopo i ritentativi: conserva localmente.
        store_.retain(report, report.retainedUntil());
        return TelemetryOutcome::TransmissionFailedRetained;
    }

    // Comodità: genera ed elabora in un unico passo, restituendo report ed esito.
    struct Result {
        CrashReport report;
        TelemetryOutcome outcome;
    };

    [[nodiscard]] Result generateAndProcess(
        std::optional<ModId> mod = std::nullopt,
        std::optional<SemVer> version = std::nullopt) {
        CrashReport report = generate(std::move(mod), std::move(version));
        TelemetryOutcome outcome = process(report);
        return Result{std::move(report), outcome};
    }

private:
    [[nodiscard]] Timestamp now() const { return clock_(); }

    [[nodiscard]] bool attempt(int attemptIndex) {
        return transmitter_ ? transmitter_(attemptIndex) : false;
    }

    TelemetryClock clock_;
    StackCapturer capturer_;
    Transmitter transmitter_;
    ConsentPredicate consent_;
    ICrashReportStore& store_;
};

}  // namespace pulse::telemetry

#endif  // PULSE_LOADER_TELEMETRY_CRASH_REPORT_HPP
