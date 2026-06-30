// loader/hooking/crash_sentinel.cpp — implementazione di CrashSentinel
// (Layer 3 — Hooking Engine, Requisiti 18.2, 18.3, 18.5).
//
// Formato del session marker su disco (little-endian):
//   magic        : 4 byte ASCII "PSMK" (Pulse Session MarKer)
//   formatVer    : u32  (= kMarkerFormatVersion)
//   startupTime  : i64
//   lastHeartbeat: i64
//   graceSurvived: u8
//   cleanShutdown: u8
//   hasActiveMod : u8
//   activeMod    : u32 len + bytes (presente solo se hasActiveMod != 0)
//
// Formato della lista delle mod disabilitate su disco (little-endian):
//   magic    : 4 byte ASCII "PSDM" (Pulse Sentinel Disabled Mods)
//   formatVer: u32  (= kDisabledFormatVersion)
//   count    : u32
//   modId[]  : u32 len + bytes
//
// Tutte le scritture sono atomiche (file temporaneo + rename), così un crash a
// metà persistenza non corrompe lo stato esistente. La lista disabilitate
// sopravvive al riavvio (Req 18.2: impedire il caricamento all'avvio successivo).

#include "hooking/crash_sentinel.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <ios>
#include <iterator>
#include <system_error>

namespace pulse::hooking {
namespace {

constexpr std::uint8_t kMarkerMagic[4] = {'P', 'S', 'M', 'K'};
constexpr std::uint8_t kDisabledMagic[4] = {'P', 'S', 'D', 'M'};
constexpr std::uint32_t kMarkerFormatVersion = 1;
constexpr std::uint32_t kDisabledFormatVersion = 1;

// --- Helper di serializzazione (append in coda al buffer) ------------------

void putU32(std::vector<std::uint8_t>& buf, std::uint32_t v) {
    buf.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

void putU64(std::vector<std::uint8_t>& buf, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
}

void putI64(std::vector<std::uint8_t>& buf, std::int64_t v) {
    putU64(buf, static_cast<std::uint64_t>(v));
}

void putString(std::vector<std::uint8_t>& buf, const std::string& s) {
    putU32(buf, static_cast<std::uint32_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}

// --- Helper di deserializzazione (cursore + bounds check) ------------------

struct Reader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t pos = 0;

    [[nodiscard]] bool remaining(std::size_t n) const noexcept { return pos + n <= size; }

    bool readU32(std::uint32_t& out) {
        if (!remaining(4)) return false;
        out = static_cast<std::uint32_t>(data[pos]) |
              (static_cast<std::uint32_t>(data[pos + 1]) << 8) |
              (static_cast<std::uint32_t>(data[pos + 2]) << 16) |
              (static_cast<std::uint32_t>(data[pos + 3]) << 24);
        pos += 4;
        return true;
    }

    bool readU64(std::uint64_t& out) {
        if (!remaining(8)) return false;
        out = 0;
        for (int i = 0; i < 8; ++i) {
            out |= static_cast<std::uint64_t>(data[pos + static_cast<std::size_t>(i)])
                   << (8 * i);
        }
        pos += 8;
        return true;
    }

    bool readI64(std::int64_t& out) {
        std::uint64_t u = 0;
        if (!readU64(u)) return false;
        out = static_cast<std::int64_t>(u);
        return true;
    }

    bool readU8(std::uint8_t& out) {
        if (!remaining(1)) return false;
        out = data[pos];
        pos += 1;
        return true;
    }

    bool readString(std::string& out) {
        std::uint32_t len = 0;
        if (!readU32(len)) return false;
        if (!remaining(len)) return false;
        out.assign(reinterpret_cast<const char*>(data + pos), len);
        pos += len;
        return true;
    }
};

// Scrittura atomica di un buffer su `path` (file temporaneo + rename).
StoreResult writeFileAtomic(const std::filesystem::path& path,
                            const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return StoreResult::failure(
                RollbackErrorCode::IoError,
                "crash sentinel: impossibile creare la directory di destinazione");
        }
    }

    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return StoreResult::failure(
                RollbackErrorCode::IoError,
                "crash sentinel: impossibile aprire il file temporaneo in scrittura");
        }
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            return StoreResult::failure(
                RollbackErrorCode::IoError,
                "crash sentinel: errore di scrittura sul file temporaneo");
        }
    }

    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // Alcune piattaforme non rinominano sopra un file esistente.
        std::filesystem::remove(path, ec);
        std::error_code ec2;
        std::filesystem::rename(tmp, path, ec2);
        if (ec2) {
            std::filesystem::remove(tmp, ec);
            return StoreResult::failure(
                RollbackErrorCode::IoError,
                "crash sentinel: impossibile rinominare il file temporaneo");
        }
    }
    return StoreResult::success();
}

// Lettura di un file in un buffer. `exists=false` se il file non esiste (non è
// un errore).
StoreResult readFile(const std::filesystem::path& path, std::vector<std::uint8_t>& out,
                     bool& exists) {
    out.clear();
    exists = false;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return StoreResult::success();  // assente: nessun errore
    }
    exists = true;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return StoreResult::failure(RollbackErrorCode::IoError,
                                    "crash sentinel: impossibile aprire il file in lettura");
    }
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return StoreResult::failure(RollbackErrorCode::IoError,
                                    "crash sentinel: errore di lettura del file");
    }
    return StoreResult::success();
}

}  // namespace

SentinelClock default_sentinel_clock() {
    return [] {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(now).count());
    };
}

// ---------------------------------------------------------------------------
// (De)serializzazione del marker.
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> CrashSentinel::serializeMarker(const SessionMarker& m) {
    std::vector<std::uint8_t> buf;
    buf.insert(buf.end(), kMarkerMagic, kMarkerMagic + 4);
    putU32(buf, kMarkerFormatVersion);
    putI64(buf, m.startupTime);
    putI64(buf, m.lastHeartbeat);
    buf.push_back(m.graceSurvived ? 1u : 0u);
    buf.push_back(m.cleanShutdown ? 1u : 0u);
    buf.push_back(m.activeMod.has_value() ? 1u : 0u);
    if (m.activeMod.has_value()) {
        putString(buf, *m.activeMod);
    }
    return buf;
}

StoreResult CrashSentinel::deserializeMarker(const std::vector<std::uint8_t>& bytes,
                                             SessionMarker& out) {
    out = SessionMarker{};
    Reader rd{bytes.data(), bytes.size()};

    if (!rd.remaining(4) || std::memcmp(bytes.data(), kMarkerMagic, 4) != 0) {
        return StoreResult::failure(RollbackErrorCode::CorruptData,
                                    "crash sentinel: marker magic non valido");
    }
    rd.pos += 4;

    std::uint32_t formatVer = 0;
    if (!rd.readU32(formatVer) || formatVer != kMarkerFormatVersion) {
        return StoreResult::failure(RollbackErrorCode::CorruptData,
                                    "crash sentinel: versione di formato marker non supportata");
    }

    std::uint8_t grace = 0;
    std::uint8_t clean = 0;
    std::uint8_t hasMod = 0;
    if (!rd.readI64(out.startupTime) || !rd.readI64(out.lastHeartbeat) || !rd.readU8(grace) ||
        !rd.readU8(clean) || !rd.readU8(hasMod)) {
        out = SessionMarker{};
        return StoreResult::failure(RollbackErrorCode::CorruptData,
                                    "crash sentinel: marker troncato");
    }
    out.graceSurvived = (grace != 0);
    out.cleanShutdown = (clean != 0);

    if (hasMod != 0) {
        std::string mod;
        if (!rd.readString(mod)) {
            out = SessionMarker{};
            return StoreResult::failure(RollbackErrorCode::CorruptData,
                                        "crash sentinel: activeMod troncato");
        }
        out.activeMod = std::move(mod);
    }
    return StoreResult::success();
}

// ---------------------------------------------------------------------------
// Costruzione: carica la lista disabilitate persistita (sopravvive al riavvio).
// ---------------------------------------------------------------------------

CrashSentinel::CrashSentinel(std::filesystem::path markerPath,
                             std::filesystem::path disabledPath, SentinelClock clock,
                             std::int64_t graceWindowSeconds)
    : markerPath_(std::move(markerPath)),
      disabledPath_(std::move(disabledPath)),
      clock_(clock ? std::move(clock) : default_sentinel_clock()),
      graceWindowSeconds_(graceWindowSeconds) {
    // Carica la lista delle mod disabilitate da disco (best-effort): se il file
    // è corrotto, si parte da una lista vuota per non bloccare l'avvio.
    (void)loadDisabled();
}

// ---------------------------------------------------------------------------
// Persistenza.
// ---------------------------------------------------------------------------

StoreResult CrashSentinel::persistMarker() const {
    if (!marker_.has_value()) {
        return StoreResult::success();
    }
    return writeFileAtomic(markerPath_, serializeMarker(*marker_));
}

StoreResult CrashSentinel::persistDisabled() const {
    std::vector<std::uint8_t> buf;
    buf.insert(buf.end(), kDisabledMagic, kDisabledMagic + 4);
    putU32(buf, kDisabledFormatVersion);
    putU32(buf, static_cast<std::uint32_t>(disabled_.size()));
    for (const ModId& m : disabled_) {
        putString(buf, m);
    }
    return writeFileAtomic(disabledPath_, buf);
}

StoreResult CrashSentinel::loadDisabled() {
    disabled_.clear();
    std::vector<std::uint8_t> bytes;
    bool exists = false;
    StoreResult r = readFile(disabledPath_, bytes, exists);
    if (!r) {
        return r;
    }
    if (!exists) {
        return StoreResult::success();  // assente: nessuna mod disabilitata
    }

    Reader rd{bytes.data(), bytes.size()};
    if (!rd.remaining(4) || std::memcmp(bytes.data(), kDisabledMagic, 4) != 0) {
        return StoreResult::failure(RollbackErrorCode::CorruptData,
                                    "crash sentinel: disabled magic non valido");
    }
    rd.pos += 4;

    std::uint32_t formatVer = 0;
    if (!rd.readU32(formatVer) || formatVer != kDisabledFormatVersion) {
        return StoreResult::failure(RollbackErrorCode::CorruptData,
                                    "crash sentinel: versione di formato disabled non supportata");
    }
    std::uint32_t count = 0;
    if (!rd.readU32(count)) {
        return StoreResult::failure(RollbackErrorCode::CorruptData,
                                    "crash sentinel: conteggio disabled troncato");
    }
    disabled_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::string mod;
        if (!rd.readString(mod)) {
            disabled_.clear();
            return StoreResult::failure(RollbackErrorCode::CorruptData,
                                        "crash sentinel: voce disabled troncata");
        }
        disabled_.push_back(std::move(mod));
    }
    return StoreResult::success();
}

void CrashSentinel::addDisabled(const ModId& mod) {
    if (std::find(disabled_.begin(), disabled_.end(), mod) == disabled_.end()) {
        disabled_.push_back(mod);
    }
}

// ---------------------------------------------------------------------------
// Gestione della sessione corrente.
// ---------------------------------------------------------------------------

StoreResult CrashSentinel::beginSession() {
    const std::int64_t t = now();
    SessionMarker m;
    m.startupTime = t;
    m.lastHeartbeat = t;
    m.graceSurvived = false;
    m.cleanShutdown = false;
    m.activeMod = std::nullopt;
    marker_ = m;
    return persistMarker();
}

StoreResult CrashSentinel::recordActiveMod(const ModId& mod) {
    if (!marker_.has_value()) {
        // Auto-begin difensivo: registra comunque la mod attiva in una sessione.
        SessionMarker m;
        m.startupTime = now();
        m.lastHeartbeat = m.startupTime;
        marker_ = m;
    }
    marker_->activeMod = mod;
    marker_->lastHeartbeat = now();
    return persistMarker();
}

StoreResult CrashSentinel::heartbeat() {
    if (!marker_.has_value()) {
        return StoreResult::success();
    }
    const std::int64_t t = now();
    marker_->lastHeartbeat = t;
    if (t - marker_->startupTime >= graceWindowSeconds_) {
        marker_->graceSurvived = true;
    }
    return persistMarker();
}

StoreResult CrashSentinel::markGraceSurvived() {
    if (!marker_.has_value()) {
        return StoreResult::success();
    }
    marker_->graceSurvived = true;
    marker_->lastHeartbeat = now();
    return persistMarker();
}

StoreResult CrashSentinel::markCleanShutdown() {
    if (!marker_.has_value()) {
        return StoreResult::success();
    }
    marker_->cleanShutdown = true;
    marker_->lastHeartbeat = now();
    return persistMarker();
}

// ---------------------------------------------------------------------------
// Lista disabilitate.
// ---------------------------------------------------------------------------

bool CrashSentinel::isModDisabled(const ModId& mod) const {
    return std::find(disabled_.begin(), disabled_.end(), mod) != disabled_.end();
}

StoreResult CrashSentinel::enableMod(const ModId& mod) {
    auto it = std::find(disabled_.begin(), disabled_.end(), mod);
    if (it == disabled_.end()) {
        return StoreResult::success();  // non era disabilitata: no-op
    }
    disabled_.erase(it);
    return persistDisabled();
}

// ---------------------------------------------------------------------------
// Recovery all'avvio (Req 18.2, 18.3, 18.5).
// ---------------------------------------------------------------------------

RecoveryReport CrashSentinel::recoverFromPreviousSession(const RollbackStore& store,
                                                         const RestoreWriteFn& write,
                                                         const UserMessageSink& userSink) {
    RecoveryReport report;

    // Carica il marker della sessione precedente.
    std::vector<std::uint8_t> bytes;
    bool exists = false;
    StoreResult rd = readFile(markerPath_, bytes, exists);
    if (!rd || !exists) {
        report.outcome = SentinelOutcome::NoPreviousSession;
        return report;
    }

    SessionMarker prev;
    if (!CrashSentinel::deserializeMarker(bytes, prev)) {
        // Marker corrotto: trattato come nessuna sessione utilizzabile; lo
        // consuma per non rielaborarlo a ripetizione.
        std::error_code ec;
        std::filesystem::remove(markerPath_, ec);
        report.outcome = SentinelOutcome::NoPreviousSession;
        return report;
    }

    // Consuma il marker precedente in ogni caso: l'evento è elaborato una volta.
    std::error_code ec;
    std::filesystem::remove(markerPath_, ec);

    report.crashElapsedSeconds = prev.lastHeartbeat - prev.startupTime;

    // Chiusura pulita: nessun crash, nessun auto-disable.
    if (prev.cleanShutdown) {
        report.outcome = SentinelOutcome::CleanShutdown;
        return report;
    }

    // Crash dopo i primi 60 s: fuori finestra, nessun auto-disable (Req 18.2).
    if (prev.graceSurvived) {
        report.outcome = SentinelOutcome::CrashOutsideWindow;
        return report;
    }

    // Crash entro i primi 60 s ma nessuna mod attiva da imputare.
    if (!prev.activeMod.has_value()) {
        report.outcome = SentinelOutcome::NoActiveModToBlame;
        return report;
    }

    // Crash entro i primi 60 s con una mod attiva: auto-disable (Req 18.2).
    const ModId blamed = *prev.activeMod;
    report.blamedMod = blamed;

    // Registra la mod come disabilitata e impedirne il caricamento all'avvio
    // successivo (persistente, Req 18.2). Fatto PRIMA del ripristino: anche se
    // il ripristino fallisce, la mod resta disabilitata (Req 18.5).
    addDisabled(blamed);
    (void)persistDisabled();

    // Ripristina i byte originali dei SOLI hook di questa mod (Req 18.4),
    // usando i record persistiti dal RollbackStore.
    for (const RollbackRecord& rec : store.records()) {
        if (rec.owner != blamed) {
            continue;
        }
        const RestoreOutcome outcome = store.restore(rec, write);
        if (!outcome.ok()) {
            // Fallimento del ripristino: interrompe, mantiene la mod
            // disabilitata e segnala la funzione interessata (Req 18.5).
            report.outcome = SentinelOutcome::RestoreFailed;
            report.failedFunction =
                outcome.error.has_value() ? outcome.error->symbol : rec.symbol;
            report.userMessage = "Pulse: ripristino del codice originale fallito per la "
                                 "funzione '" +
                                 *report.failedFunction + "' della mod '" + blamed +
                                 "'. La mod resta disabilitata.";
            if (userSink) {
                userSink(report.userMessage);
            }
            return report;
        }
        report.restoredFunctions += outcome.restored;
    }

    // Successo: messaggio all'User col nome della mod e il motivo (Req 18.3).
    report.outcome = SentinelOutcome::AutoDisabled;
    report.userMessage = "Pulse: la mod '" + blamed +
                         "' e' stata disabilitata automaticamente perche' ha causato un "
                         "crash all'avvio (entro 60 s). Il codice originale e' stato "
                         "ripristinato.";
    if (userSink) {
        userSink(report.userMessage);
    }
    return report;
}

}  // namespace pulse::hooking
