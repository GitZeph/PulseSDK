// sdk/log/log.hpp — implementazione (header-only) del sistema di logging e
// degli helper popup dello SDK Pulse (Layer 5, Requisito 13).
//
// Questo header contiene il codice del modulo di logging. È esposto ai
// Developer tramite l'header pubblico <pulse/log.hpp>, che lo include con un
// percorso relativo e ne promuove i tipi nel namespace `pulse` (stesso schema
// di sdk/events/event_bus.hpp ↔ sdk/include/pulse/events.hpp).
//
// Copre il task 21.1:
//   * Level — insieme CHIUSO dei livelli di severità {Debug, Info, Warning,
//     Error} (Req 13.1).
//   * Logger::log(Level, msg) — registra un messaggio con il livello e
//     l'identità della Mod emittente, in modo recuperabile per l'intera
//     sessione (Req 13.1, 13.5).
//   * Logger::log(int levelCode, msg) — variante che accetta un codice di
//     livello grezzo: modella la possibilità di un livello NON appartenente
//     all'insieme chiuso e lo rifiuta con un'indicazione di errore senza
//     registrare nulla (Req 13.2).
//   * Logger::log su sink non disponibile — preserva l'esecuzione della Mod
//     chiamante (nessuna eccezione) e restituisce un'indicazione di errore che
//     segnala il fallimento della registrazione (Req 13.6).
//   * helper popup — titolo non vuoto e ≤ 100 caratteri, corpo ≤ 1000
//     caratteri; richieste con titolo vuoto o oltre i limiti sono rifiutate
//     senza mostrare il popup, con un'indicazione di errore (Req 13.3, 13.4).
//
// Stack: C++20/23 (Requisito 26.1). Header-only, nessuna dipendenza esterna.
#ifndef PULSE_LOG_LOG_HPP
#define PULSE_LOG_LOG_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulse::log {

// ---------------------------------------------------------------------------
// ModId — identità della Mod emittente. Modellato come stringa, coerente con
// `pulse::events::ModId`, `pulse::storage::ModId` e gli altri moduli dello SDK.
// ---------------------------------------------------------------------------
using ModId = std::string;

// ---------------------------------------------------------------------------
// Level — insieme CHIUSO dei livelli di severità (Req 13.1).
// I nomi sono in CamelCase (Debug/Info/Warning/Error) anziché in maiuscolo per
// evitare collisioni con eventuali macro di piattaforma (ERROR/DEBUG).
// I valori interi 0..3 definiscono la corrispondenza con i codici grezzi usati
// da Logger::log(int, ...) per modellare/validare un livello fuori insieme
// (Req 13.2).
// ---------------------------------------------------------------------------
enum class Level : int {
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3,
};

// Codici di livello validi: l'insieme chiuso {Debug, Info, Warning, Error}
// corrisponde ai codici interi 0..3 inclusi (Req 13.1, 13.2).
inline constexpr int kMinLevelCode = 0;
inline constexpr int kMaxLevelCode = 3;

// Nome leggibile di un Level (per le segnalazioni).
inline std::string_view to_string(Level level) noexcept {
    switch (level) {
        case Level::Debug:
            return "DEBUG";
        case Level::Info:
            return "INFO";
        case Level::Warning:
            return "WARNING";
        case Level::Error:
            return "ERROR";
    }
    return "UNKNOWN";
}

// Converte un codice di livello grezzo nel Level corrispondente, oppure
// restituisce std::nullopt se il codice NON appartiene all'insieme chiuso
// {Debug, Info, Warning, Error} (Req 13.2).
inline std::optional<Level> levelFromCode(int code) noexcept {
    if (code < kMinLevelCode || code > kMaxLevelCode) {
        return std::nullopt;
    }
    return static_cast<Level>(code);
}

// ---------------------------------------------------------------------------
// Limiti di lunghezza del contenuto dei popup (Req 13.3, 13.4).
// ---------------------------------------------------------------------------
inline constexpr std::size_t kMaxPopupTitleLength = 100;
inline constexpr std::size_t kMaxPopupBodyLength = 1000;

// ---------------------------------------------------------------------------
// LogRecord — una voce di log registrata: livello di severità, identità della
// Mod emittente e messaggio (Req 13.1, 13.5).
// ---------------------------------------------------------------------------
struct LogRecord {
    Level level{Level::Info};
    ModId mod;
    std::string message;

    friend bool operator==(const LogRecord& a, const LogRecord& b) {
        return a.level == b.level && a.mod == b.mod && a.message == b.message;
    }
};

// ---------------------------------------------------------------------------
// Esito di un'operazione di log (Req 13.2, 13.6).
//   * ok        — true se il messaggio è stato registrato.
//   * code      — categoria dell'errore quando ok == false.
//   * message   — descrizione leggibile (vuota se ok == true).
// ---------------------------------------------------------------------------
enum class LogErrorCode {
    None,             // nessun errore
    InvalidLevel,     // livello di severità fuori dall'insieme chiuso (Req 13.2)
    SinkUnavailable,  // destinazione di archiviazione non disponibile (Req 13.6)
};

struct LogResult {
    bool ok{false};
    LogErrorCode code{LogErrorCode::None};
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept { return ok; }

    static LogResult success() { return LogResult{true, LogErrorCode::None, {}}; }
    static LogResult failure(LogErrorCode code, std::string message) {
        return LogResult{false, code, std::move(message)};
    }
};

// ---------------------------------------------------------------------------
// ILogSink — destinazione di archiviazione dei record di log (iniettabile).
// Astrarre il sink consente ai test di simularne l'indisponibilità (Req 13.6)
// senza dipendere da un filesystem reale.
//
// write() restituisce true se il record è stato accettato/archiviato, false se
// la destinazione non è disponibile.
// ---------------------------------------------------------------------------
class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual bool write(const LogRecord& record) = 0;
};

// ---------------------------------------------------------------------------
// SessionLogStore — sink in-memory che conserva i record di log per l'intera
// durata della sessione di esecuzione corrente, rendendoli recuperabili a fini
// diagnostici (Req 13.5).
//
// Può essere reso "non disponibile" via setAvailable(false) per modellare il
// fallimento della destinazione di archiviazione (Req 13.6): in tale stato
// write() rifiuta il record e non lo conserva.
// ---------------------------------------------------------------------------
class SessionLogStore final : public ILogSink {
public:
    bool write(const LogRecord& record) override {
        if (!available_) {
            return false;  // destinazione non disponibile (Req 13.6)
        }
        records_.push_back(record);
        return true;
    }

    // Tutti i record registrati nella sessione, in ordine di emissione
    // (Req 13.5).
    [[nodiscard]] const std::vector<LogRecord>& records() const noexcept {
        return records_;
    }

    // Record emessi da una specifica Mod (recupero diagnostico per identità).
    [[nodiscard]] std::vector<LogRecord> recordsForMod(const ModId& mod) const {
        std::vector<LogRecord> out;
        for (const auto& record : records_) {
            if (record.mod == mod) {
                out.push_back(record);
            }
        }
        return out;
    }

    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

    // Rende il sink disponibile/non disponibile (per i test, Req 13.6).
    void setAvailable(bool available) noexcept { available_ = available; }
    [[nodiscard]] bool available() const noexcept { return available_; }

private:
    std::vector<LogRecord> records_;
    bool available_{true};
};

// ---------------------------------------------------------------------------
// Logger — registra i messaggi di una specifica Mod su un sink iniettabile.
//
// Ogni Logger porta l'identità della Mod emittente (Req 13.1) e scrive su un
// `ILogSink` condiviso (tipicamente un SessionLogStore) fornito alla
// costruzione. Più Mod possono condividere lo stesso sink di sessione.
// ---------------------------------------------------------------------------
class Logger {
public:
    Logger(ModId mod, ILogSink& sink) : mod_(std::move(mod)), sink_(&sink) {}

    [[nodiscard]] const ModId& mod() const noexcept { return mod_; }

    // Registra un messaggio con un livello dell'insieme chiuso (Req 13.1,
    // 13.5). Se la destinazione non è disponibile, preserva l'esecuzione (non
    // lancia) e restituisce un errore SinkUnavailable (Req 13.6).
    LogResult log(Level level, std::string_view msg) {
        const LogRecord record{level, mod_, std::string(msg)};
        if (!sink_->write(record)) {
            // Req 13.6 — registrazione fallita: nessuna eccezione, esecuzione
            // della Mod preservata, indicazione di errore al chiamante.
            return LogResult::failure(
                LogErrorCode::SinkUnavailable,
                "registrazione del log fallita: destinazione di archiviazione "
                "non disponibile (mod '" + mod_ + "')");
        }
        return LogResult::success();
    }

    // Variante che accetta un codice di livello grezzo (Req 13.2). Se il codice
    // NON appartiene all'insieme chiuso {Debug, Info, Warning, Error} (0..3),
    // rifiuta il messaggio senza registrarlo e restituisce un errore
    // InvalidLevel. Altrimenti si comporta come log(Level, msg).
    LogResult log(int levelCode, std::string_view msg) {
        const std::optional<Level> level = levelFromCode(levelCode);
        if (!level.has_value()) {
            return LogResult::failure(
                LogErrorCode::InvalidLevel,
                "livello di severità non valido: " + std::to_string(levelCode) +
                    " (atteso 0..3 = {DEBUG, INFO, WARNING, ERROR})");
        }
        return log(*level, msg);
    }

private:
    ModId mod_;
    ILogSink* sink_;
};

// ---------------------------------------------------------------------------
// Popup / avvisi (Req 13.3, 13.4).
// ---------------------------------------------------------------------------

// Contenuto validato di un popup pronto da mostrare all'User.
struct PopupContent {
    std::string title;
    std::string body;
};

// Esito della validazione/visualizzazione di un popup (Req 13.4).
enum class PopupErrorCode {
    None,          // contenuto valido
    EmptyTitle,    // titolo vuoto (Req 13.4)
    TitleTooLong,  // titolo oltre i 100 caratteri (Req 13.4)
    BodyTooLong,   // corpo oltre i 1000 caratteri (Req 13.4)
};

struct PopupResult {
    bool ok{false};
    PopupErrorCode code{PopupErrorCode::None};
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept { return ok; }

    static PopupResult success() {
        return PopupResult{true, PopupErrorCode::None, {}};
    }
    static PopupResult failure(PopupErrorCode code, std::string message) {
        return PopupResult{false, code, std::move(message)};
    }
};

// Valida il contenuto di un popup senza mostrarlo (Req 13.3, 13.4):
//   * titolo non vuoto;
//   * titolo ≤ 100 caratteri;
//   * corpo ≤ 1000 caratteri.
// Restituisce success() se valido, altrimenti l'errore specifico.
inline PopupResult validatePopup(std::string_view title, std::string_view body) {
    if (title.empty()) {
        return PopupResult::failure(PopupErrorCode::EmptyTitle,
                                    "titolo del popup vuoto");
    }
    if (title.size() > kMaxPopupTitleLength) {
        return PopupResult::failure(
            PopupErrorCode::TitleTooLong,
            "titolo del popup troppo lungo: " + std::to_string(title.size()) +
                " caratteri (massimo " + std::to_string(kMaxPopupTitleLength) +
                ")");
    }
    if (body.size() > kMaxPopupBodyLength) {
        return PopupResult::failure(
            PopupErrorCode::BodyTooLong,
            "corpo del popup troppo lungo: " + std::to_string(body.size()) +
                " caratteri (massimo " + std::to_string(kMaxPopupBodyLength) +
                ")");
    }
    return PopupResult::success();
}

// ---------------------------------------------------------------------------
// IPopupPresenter — presentazione del popup all'User (iniettabile). Astrae il
// livello UI così la validazione del contenuto è testabile senza una UI reale.
// ---------------------------------------------------------------------------
class IPopupPresenter {
public:
    virtual ~IPopupPresenter() = default;
    virtual void present(const PopupContent& content) = 0;
};

// Mostra un popup all'User dopo averne validato il contenuto (Req 13.3, 13.4).
// Se il contenuto NON è valido (titolo vuoto o oltre i limiti), rifiuta la
// richiesta SENZA invocare il presenter e restituisce l'indicazione di errore.
inline PopupResult showPopup(std::string_view title, std::string_view body,
                             IPopupPresenter& presenter) {
    const PopupResult validation = validatePopup(title, body);
    if (!validation.ok) {
        return validation;  // Req 13.4 — nessun popup mostrato.
    }
    presenter.present(PopupContent{std::string(title), std::string(body)});
    return PopupResult::success();
}

}  // namespace pulse::log

#endif  // PULSE_LOG_LOG_HPP
