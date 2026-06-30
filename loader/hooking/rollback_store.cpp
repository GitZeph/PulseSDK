// loader/hooking/rollback_store.cpp — implementazione di RollbackStore
// (Layer 3 — Hooking Engine, Requisiti 18.1, 18.4, 18.5).
//
// Formato su disco (little-endian):
//   magic    : 4 byte ASCII "PRBK" (Pulse RollBacK)
//   formatVer: u32  (= kFormatVersion)
//   count    : u32  (numero di record)
//   record[] :
//       owner        : u32 len + bytes
//       symbol       : u32 len + bytes
//       address      : u64
//       version.major: i32
//       version.minor: i32
//       platformId   : u32 len + bytes
//       originalBytes: u32 len + bytes
//
// Tutti i campi a lunghezza variabile sono length-prefixed, quindi byte
// arbitrari in `originalBytes` (incluso 0x00) sono preservati esattamente
// (round-trip byte-perfetto, Req 18.1/18.4).

#include "hooking/rollback_store.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <system_error>

namespace pulse::hooking {
namespace {

constexpr std::uint8_t kMagic[4] = {'P', 'R', 'B', 'K'};
constexpr std::uint32_t kFormatVersion = 1;

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

void putI32(std::vector<std::uint8_t>& buf, std::int32_t v) {
    putU32(buf, static_cast<std::uint32_t>(v));
}

void putBytes(std::vector<std::uint8_t>& buf, const std::uint8_t* data, std::size_t len) {
    putU32(buf, static_cast<std::uint32_t>(len));
    buf.insert(buf.end(), data, data + len);
}

void putString(std::vector<std::uint8_t>& buf, const std::string& s) {
    putBytes(buf, reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

// --- Helper di deserializzazione (lettura con cursore + bounds check) ------

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

    bool readI32(std::int32_t& out) {
        std::uint32_t u = 0;
        if (!readU32(u)) return false;
        out = static_cast<std::int32_t>(u);
        return true;
    }

    bool readBytes(std::vector<std::uint8_t>& out) {
        std::uint32_t len = 0;
        if (!readU32(len)) return false;
        if (!remaining(len)) return false;
        out.assign(data + pos, data + pos + len);
        pos += len;
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

}  // namespace

std::vector<std::uint8_t> RollbackStore::serialize() const {
    std::vector<std::uint8_t> buf;
    buf.insert(buf.end(), kMagic, kMagic + 4);
    putU32(buf, kFormatVersion);
    putU32(buf, static_cast<std::uint32_t>(records_.size()));

    for (const RollbackRecord& r : records_) {
        putString(buf, r.owner);
        putString(buf, r.symbol);
        putU64(buf, static_cast<std::uint64_t>(r.address));
        putI32(buf, static_cast<std::int32_t>(r.version.major));
        putI32(buf, static_cast<std::int32_t>(r.version.minor));
        putString(buf, r.platformId);
        putBytes(buf, r.originalBytes.data(), r.originalBytes.size());
    }
    return buf;
}

StoreResult RollbackStore::deserialize(const std::vector<std::uint8_t>& bytes,
                                       std::vector<RollbackRecord>& out) {
    out.clear();
    Reader rd{bytes.data(), bytes.size()};

    if (!rd.remaining(4) || std::memcmp(bytes.data(), kMagic, 4) != 0) {
        return StoreResult::failure(RollbackErrorCode::CorruptData,
                                    "rollback store: magic non valido");
    }
    rd.pos += 4;

    std::uint32_t formatVer = 0;
    if (!rd.readU32(formatVer)) {
        return StoreResult::failure(RollbackErrorCode::CorruptData,
                                    "rollback store: header troncato");
    }
    if (formatVer != kFormatVersion) {
        return StoreResult::failure(RollbackErrorCode::CorruptData,
                                    "rollback store: versione di formato non supportata");
    }

    std::uint32_t count = 0;
    if (!rd.readU32(count)) {
        return StoreResult::failure(RollbackErrorCode::CorruptData,
                                    "rollback store: conteggio record troncato");
    }

    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        RollbackRecord r;
        std::uint64_t address = 0;
        std::int32_t major = 0;
        std::int32_t minor = 0;
        if (!rd.readString(r.owner) || !rd.readString(r.symbol) || !rd.readU64(address) ||
            !rd.readI32(major) || !rd.readI32(minor) || !rd.readString(r.platformId) ||
            !rd.readBytes(r.originalBytes)) {
            out.clear();
            return StoreResult::failure(RollbackErrorCode::CorruptData,
                                        "rollback store: record troncato o malformato");
        }
        r.address = static_cast<std::uintptr_t>(address);
        r.version.major = static_cast<int>(major);
        r.version.minor = static_cast<int>(minor);
        out.push_back(std::move(r));
    }

    return StoreResult::success();
}

StoreResult RollbackStore::load(const std::filesystem::path& filePath, RollbackStore& out) {
    out = RollbackStore{filePath};

    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec) || ec) {
        // File assente: store vuoto, nessun errore (Req 18 — nessun record).
        return StoreResult::success();
    }

    std::ifstream in(filePath, std::ios::binary);
    if (!in) {
        return StoreResult::failure(RollbackErrorCode::IoError,
                                    "rollback store: impossibile aprire il file in lettura");
    }

    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    if (in.bad()) {
        return StoreResult::failure(RollbackErrorCode::IoError,
                                    "rollback store: errore di lettura del file");
    }

    std::vector<RollbackRecord> records;
    StoreResult r = deserialize(bytes, records);
    if (!r) {
        return r;
    }
    out.records_ = std::move(records);
    return StoreResult::success();
}

StoreResult RollbackStore::persist() const {
    std::error_code ec;
    const std::filesystem::path parent = filePath_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return StoreResult::failure(
                RollbackErrorCode::IoError,
                "rollback store: impossibile creare la directory di destinazione");
        }
    }

    // Scrittura atomica: scrive su un file temporaneo poi rinomina, così un
    // crash a metà non corrompe lo store esistente.
    std::filesystem::path tmp = filePath_;
    tmp += ".tmp";

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return StoreResult::failure(
                RollbackErrorCode::IoError,
                "rollback store: impossibile aprire il file temporaneo in scrittura");
        }
        const std::vector<std::uint8_t> bytes = serialize();
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            return StoreResult::failure(RollbackErrorCode::IoError,
                                        "rollback store: errore di scrittura sul file temporaneo");
        }
    }

    std::filesystem::rename(tmp, filePath_, ec);
    if (ec) {
        // Fallback: alcune piattaforme non rinominano sopra un file esistente.
        std::filesystem::remove(filePath_, ec);
        std::error_code ec2;
        std::filesystem::rename(tmp, filePath_, ec2);
        if (ec2) {
            std::filesystem::remove(tmp, ec);
            return StoreResult::failure(RollbackErrorCode::IoError,
                                        "rollback store: impossibile rinominare il file temporaneo");
        }
    }

    return StoreResult::success();
}

StoreResult RollbackStore::add(RollbackRecord record) {
    records_.push_back(std::move(record));
    StoreResult r = persist();
    if (!r) {
        // Persistenza fallita: annulla l'aggiunta in memoria così lo stato
        // riflette ciò che è effettivamente su disco (Req 18.1).
        records_.pop_back();
    }
    return r;
}

RestoreOutcome RollbackStore::restore(const RollbackRecord& record,
                                      const RestoreWriteFn& write) const {
    RestoreOutcome outcome;
    if (!write) {
        outcome.aborted = true;
        outcome.error = RollbackError{RollbackErrorCode::InvalidArgument,
                                      "rollback store: callback di write nulla", record.symbol};
        return outcome;
    }

    if (write(record.address, record.originalBytes)) {
        outcome.restored = 1;
    } else {
        outcome.aborted = true;
        outcome.error = RollbackError{
            RollbackErrorCode::RestoreFailed,
            "rollback store: ripristino dei byte originali fallito", record.symbol};
    }
    return outcome;
}

RestoreOutcome RollbackStore::restoreAll(const RestoreWriteFn& write) const {
    RestoreOutcome outcome;
    if (!write) {
        outcome.aborted = true;
        outcome.error = RollbackError{RollbackErrorCode::InvalidArgument,
                                      "rollback store: callback di write nulla", {}};
        return outcome;
    }

    for (const RollbackRecord& r : records_) {
        if (!write(r.address, r.originalBytes)) {
            // Interrompe il ripristino e segnala la funzione interessata (Req 18.5).
            outcome.aborted = true;
            outcome.error = RollbackError{
                RollbackErrorCode::RestoreFailed,
                "rollback store: ripristino dei byte originali fallito", r.symbol};
            return outcome;
        }
        ++outcome.restored;
    }
    return outcome;
}

StoreResult RollbackStore::clear() {
    records_.clear();
    std::error_code ec;
    std::filesystem::remove(filePath_, ec);
    if (ec) {
        return StoreResult::failure(RollbackErrorCode::IoError,
                                    "rollback store: impossibile rimuovere il file dello store");
    }
    return StoreResult::success();
}

}  // namespace pulse::hooking
