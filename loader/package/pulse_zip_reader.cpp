// loader/package/pulse_zip_reader.cpp — implementazione del lettore `.pulse`
// (ZIP STORED) self-contained. Vedi pulse_zip_reader.hpp per il contratto.

#include "package/pulse_zip_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>

namespace pulse::package {

namespace {

// Firme ZIP (little-endian) dei record rilevanti.
constexpr std::uint32_t kSigLocalHeader = 0x04034b50u;     // local file header
constexpr std::uint32_t kSigCentralDir = 0x02014b50u;      // central directory
constexpr std::uint32_t kSigEndOfCentralDir = 0x06054b50u; // EOCD

// Metodo di compressione: solo STORED (0) è supportato in questa build.
constexpr std::uint16_t kMethodStored = 0;

// Dimensioni fisse dei record (parte a lunghezza fissa, esclusi i campi
// a lunghezza variabile come nome/extra/comment).
constexpr std::size_t kEocdFixedSize = 22;           // EOCD senza comment
constexpr std::size_t kCentralRecordFixedSize = 46;  // central dir header fisso
constexpr std::size_t kLocalHeaderFixedSize = 30;    // local header fisso

// Massima lunghezza ammessa per il comment trailing dell'EOCD (64KB).
constexpr std::size_t kMaxComment = 0xFFFFu;

// --- Letture little-endian con controllo dei limiti -------------------------
// Ogni lettura verifica che [off, off+N) sia interamente dentro `data`.

[[nodiscard]] bool read_u16(const std::vector<std::uint8_t>& data,
                            std::size_t off, std::uint16_t& out) {
    if (off + 2 > data.size()) return false;
    out = static_cast<std::uint16_t>(data[off]) |
          (static_cast<std::uint16_t>(data[off + 1]) << 8);
    return true;
}

[[nodiscard]] bool read_u32(const std::vector<std::uint8_t>& data,
                            std::size_t off, std::uint32_t& out) {
    if (off + 4 > data.size()) return false;
    out = static_cast<std::uint32_t>(data[off]) |
          (static_cast<std::uint32_t>(data[off + 1]) << 8) |
          (static_cast<std::uint32_t>(data[off + 2]) << 16) |
          (static_cast<std::uint32_t>(data[off + 3]) << 24);
    return true;
}

// Localizza l'offset dell'EOCD scandendo all'indietro dalla fine del buffer.
// Ritorna true e valorizza `eocdOff` se trovato; false altrimenti.
[[nodiscard]] bool find_eocd(const std::vector<std::uint8_t>& data,
                             std::size_t& eocdOff) {
    const std::size_t n = data.size();
    if (n < kEocdFixedSize) return false;

    // L'EOCD può essere seguito da un comment di lunghezza fino a 64KB; quindi
    // l'inizio dell'EOCD non può essere prima di n - kEocdFixedSize - kMaxComment.
    const std::size_t maxBack = kEocdFixedSize + kMaxComment;
    const std::size_t lowerBound = (n > maxBack) ? (n - maxBack) : 0;

    // Scansione all'indietro: il primo (più vicino alla fine) record con firma
    // valida e lunghezza-comment coerente con la posizione vince.
    std::size_t i = n - kEocdFixedSize;
    for (;;) {
        std::uint32_t sig = 0;
        if (read_u32(data, i, sig) && sig == kSigEndOfCentralDir) {
            std::uint16_t commentLen = 0;
            if (read_u16(data, i + 20, commentLen)) {
                // Il comment dichiarato deve combaciare con i byte residui.
                if (i + kEocdFixedSize + commentLen == n) {
                    eocdOff = i;
                    return true;
                }
            }
        }
        if (i == lowerBound) break;
        --i;
    }
    return false;
}

}  // namespace

std::optional<PackageArchive> read_pulse_archive(
    const std::vector<std::uint8_t>& zipBytes, std::string& error) {
    error.clear();

    // (1) Localizza l'EOCD.
    std::size_t eocdOff = 0;
    if (!find_eocd(zipBytes, eocdOff)) {
        error = "container .pulse malformato: End Of Central Directory non "
                "trovato (non è uno ZIP valido).";
        return std::nullopt;
    }

    // (2) Dall'EOCD ricava numero di voci e offset della central directory.
    std::uint16_t totalEntries = 0;
    std::uint32_t centralDirSize = 0;
    std::uint32_t centralDirOffset = 0;
    if (!read_u16(zipBytes, eocdOff + 10, totalEntries) ||
        !read_u32(zipBytes, eocdOff + 12, centralDirSize) ||
        !read_u32(zipBytes, eocdOff + 16, centralDirOffset)) {
        error = "container .pulse malformato: EOCD troncato.";
        return std::nullopt;
    }

    if (static_cast<std::size_t>(centralDirOffset) > zipBytes.size()) {
        error = "container .pulse malformato: offset della central directory "
                "fuori dai limiti.";
        return std::nullopt;
    }

    PackageArchive archive;

    // (3) Itera i record di central directory.
    std::size_t pos = static_cast<std::size_t>(centralDirOffset);
    for (std::uint16_t entryIdx = 0; entryIdx < totalEntries; ++entryIdx) {
        std::uint32_t sig = 0;
        if (!read_u32(zipBytes, pos, sig) || sig != kSigCentralDir) {
            error = "container .pulse malformato: firma di central directory "
                    "assente o non valida.";
            return std::nullopt;
        }
        if (pos + kCentralRecordFixedSize > zipBytes.size()) {
            error = "container .pulse malformato: record di central directory "
                    "troncato.";
            return std::nullopt;
        }

        std::uint16_t method = 0;
        std::uint32_t compressedSize = 0;
        std::uint32_t uncompressedSize = 0;
        std::uint16_t nameLen = 0;
        std::uint16_t extraLen = 0;
        std::uint16_t commentLen = 0;
        std::uint32_t localHeaderOffset = 0;
        // Offset dei campi nel central directory header (ZIP spec):
        //   10: compression method (u16)
        //   20: compressed size (u32)
        //   24: uncompressed size (u32)
        //   28: file name length (u16)
        //   30: extra field length (u16)
        //   32: file comment length (u16)
        //   42: relative offset of local header (u32)
        if (!read_u16(zipBytes, pos + 10, method) ||
            !read_u32(zipBytes, pos + 20, compressedSize) ||
            !read_u32(zipBytes, pos + 24, uncompressedSize) ||
            !read_u16(zipBytes, pos + 28, nameLen) ||
            !read_u16(zipBytes, pos + 30, extraLen) ||
            !read_u16(zipBytes, pos + 32, commentLen) ||
            !read_u32(zipBytes, pos + 42, localHeaderOffset)) {
            error = "container .pulse malformato: campi del record di central "
                    "directory non leggibili.";
            return std::nullopt;
        }

        const std::size_t nameOff = pos + kCentralRecordFixedSize;
        if (nameOff + nameLen > zipBytes.size()) {
            error = "container .pulse malformato: nome di voce fuori dai limiti.";
            return std::nullopt;
        }
        std::string name(reinterpret_cast<const char*>(zipBytes.data() + nameOff),
                         nameLen);

        // Avanza alla prossima voce di central directory.
        pos = nameOff + nameLen + extraLen + commentLen;

        // Voce di directory: nessun contenuto da estrarre.
        if (!name.empty() && name.back() == '/') {
            continue;
        }

        // Solo STORED è supportato: altri metodi → fail-open (container non
        // leggibile) con messaggio che nomina la voce e il metodo.
        if (method != kMethodStored) {
            error = "container .pulse: voce '" + name +
                    "' usa il metodo di compressione " + std::to_string(method) +
                    " non supportato (solo STORED/0): container trattato come "
                    "non leggibile.";
            return std::nullopt;
        }

        // STORED: compressed == uncompressed. Difensivo: se differiscono, il
        // record è incoerente.
        if (compressedSize != uncompressedSize) {
            error = "container .pulse malformato: voce STORED '" + name +
                    "' con dimensioni compressa/non compressa discordi.";
            return std::nullopt;
        }

        // (4) Header locale → inizio dei dati.
        const std::size_t localOff = static_cast<std::size_t>(localHeaderOffset);
        std::uint32_t localSig = 0;
        if (!read_u32(zipBytes, localOff, localSig) ||
            localSig != kSigLocalHeader) {
            error = "container .pulse malformato: header locale assente per la "
                    "voce '" + name + "'.";
            return std::nullopt;
        }
        std::uint16_t localNameLen = 0;
        std::uint16_t localExtraLen = 0;
        if (!read_u16(zipBytes, localOff + 26, localNameLen) ||
            !read_u16(zipBytes, localOff + 28, localExtraLen)) {
            error = "container .pulse malformato: header locale troncato per la "
                    "voce '" + name + "'.";
            return std::nullopt;
        }

        const std::size_t dataStart =
            localOff + kLocalHeaderFixedSize + localNameLen + localExtraLen;
        const std::size_t dataEnd = dataStart + compressedSize;
        if (dataStart > zipBytes.size() || dataEnd > zipBytes.size() ||
            dataEnd < dataStart) {
            error = "container .pulse malformato: dati della voce '" + name +
                    "' fuori dai limiti.";
            return std::nullopt;
        }

        // (5) Copia diretta dei byte (STORED).
        Bytes content(zipBytes.begin() + static_cast<std::ptrdiff_t>(dataStart),
                      zipBytes.begin() + static_cast<std::ptrdiff_t>(dataEnd));
        archive.add(std::move(name), std::move(content));
    }

    return archive;
}

OpenResult open_pulse_file(const std::filesystem::path& path,
                           const PulsePackage::Options& opts) {
    OpenResult res;

    // (1) Lettura dei byte del file (binario). Su fallimento → ManifestMissing
    // (il manifest non è raggiungibile) con messaggio che nomina il path.
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        res.ok = false;
        res.error = OpenError::ManifestMissing;
        res.message = "impossibile aprire il file .pulse '" + path.string() +
                      "': container non leggibile.";
        return res;
    }

    std::vector<std::uint8_t> bytes;
    {
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size < 0) {
            res.ok = false;
            res.error = OpenError::ManifestMissing;
            res.message = "impossibile determinare la dimensione del file .pulse '" +
                          path.string() + "'.";
            return res;
        }
        in.seekg(0, std::ios::beg);
        bytes.resize(static_cast<std::size_t>(size));
        if (size > 0) {
            in.read(reinterpret_cast<char*>(bytes.data()), size);
            if (!in) {
                res.ok = false;
                res.error = OpenError::ManifestMissing;
                res.message = "lettura del file .pulse '" + path.string() +
                              "' fallita: container non leggibile.";
                return res;
            }
        }
    }

    // (2) Ricostruzione del PackageArchive.
    std::string parseError;
    std::optional<PackageArchive> archive = read_pulse_archive(bytes, parseError);
    if (!archive.has_value()) {
        res.ok = false;
        res.error = OpenError::ManifestMissing;
        res.message = "container .pulse '" + path.string() +
                      "' non leggibile: " + parseError;
        return res;
    }

    // (3) Apertura validata (manifest → integrità → esposizione del codice).
    return PulsePackage::open(std::move(*archive), opts);
}

}  // namespace pulse::package
