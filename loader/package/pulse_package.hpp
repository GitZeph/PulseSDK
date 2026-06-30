// loader/package/pulse_package.hpp — Modello del Pulse Package `.pulse`
// (Layer 4 — Lifecycle & Manifest, task 13.3).
//
// Requisito coperto da questo modulo:
//   * Req 16.2 — WHEN il Pulse_Loader carica un Pulse_Package, THE Pulse_Loader
//     SHALL validare il Manifest contro lo schema definito *prima* del
//     caricamento di qualsiasi codice della Mod. Questo file rende l'invariante
//     STRUTTURALE: un `PulsePackage` può essere ottenuto SOLO tramite
//     `PulsePackage::open(...)`, che (1) localizza `pulse.toml`, (2) lo
//     analizza con `pulse::manifest::parse` e fallisce se assente o malformato,
//     e SOLO in caso di successo costruisce l'oggetto che espone le entry
//     `code/` e `resources/`. Finché il manifest non è stato analizzato con
//     successo NON esiste alcun oggetto da cui accedere al codice.
//
// Struttura del container `.pulse` (design, "Pulse Package (.pulse)"):
//
//   mymod.pulse                (archivio ZIP)
//   ├── pulse.toml             # Manifest (letto/validato per primo)
//   ├── code/                  # binari per piattaforma o script
//   ├── resources/             # sprite, suoni, ...
//   ├── MANIFEST.sha256        # hash di integrità (Req 28.6)
//   └── SIGNATURE.sig          # firma digitale del Marketplace (Req 23.x)
//
// Scelta tecnica (documentata): il contenitore on-disk è uno ZIP, ma per
// mantenere la build *self-contained* (nessuna dipendenza esterna né rete in
// fase di build) il modello in memoria del package è astratto su un
// `PackageArchive` — un filesystem virtuale (mappa percorso-entry -> byte) che
// rappresenta fedelmente le entry dell'archivio. La logica realmente
// verificabile (lettura del manifest *prima* del codice, calcolo/verifica di
// `MANIFEST.sha256`, enumerazione di `code/`/`resources/`, presenza di
// `SIGNATURE.sig`) è implementata e testata su questo modello. Un lettore ZIP
// reale (es. miniz via FetchContent) potrà popolare un `PackageArchive`
// additivamente senza cambiare l'API di `PulsePackage`.
//
// SHA-256 self-contained: implementato qui (RFC 6234 / FIPS 180-4) per
// calcolare e verificare `MANIFEST.sha256` senza dipendere da OpenSSL.
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).
#ifndef PULSE_LOADER_PACKAGE_PULSE_PACKAGE_HPP
#define PULSE_LOADER_PACKAGE_PULSE_PACKAGE_HPP

#include <array>
#include <cstdint>
#include <cstddef>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "lifecycle/manifest.hpp"

namespace pulse::package {

// Tipo dei contenuti di una entry dell'archivio.
using Bytes = std::vector<std::uint8_t>;

// ---------------------------------------------------------------------------
// Nomi/percorsi canonici delle entry del package (`.pulse`).
// ---------------------------------------------------------------------------
inline constexpr std::string_view kManifestEntry = "pulse.toml";
inline constexpr std::string_view kIntegrityEntry = "MANIFEST.sha256";
inline constexpr std::string_view kSignatureEntry = "SIGNATURE.sig";
inline constexpr std::string_view kCodePrefix = "code/";
inline constexpr std::string_view kResourcesPrefix = "resources/";

// ===========================================================================
// SHA-256 self-contained (FIPS 180-4). Sufficiente per `MANIFEST.sha256`.
// ===========================================================================
namespace detail {

class Sha256 {
public:
    Sha256() { reset(); }

    void reset() {
        state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                  0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        bitlen_ = 0;
        buflen_ = 0;
    }

    void update(const std::uint8_t* data, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            buffer_[buflen_++] = data[i];
            if (buflen_ == 64) {
                processBlock(buffer_.data());
                bitlen_ += 512;
                buflen_ = 0;
            }
        }
    }

    void update(std::string_view s) {
        update(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    }

    void update(const Bytes& b) { update(b.data(), b.size()); }

    // Finalizza e ritorna il digest esadecimale minuscolo (64 caratteri).
    std::string hexDigest() {
        std::array<std::uint32_t, 8> snapshot = state_;
        std::uint64_t totalBits = bitlen_ + static_cast<std::uint64_t>(buflen_) * 8;

        std::array<std::uint8_t, 64> blk = buffer_;
        std::size_t bl = buflen_;

        blk[bl++] = 0x80;
        if (bl > 56) {
            while (bl < 64) blk[bl++] = 0;
            processBlockInto(blk.data(), snapshot);
            bl = 0;
        }
        while (bl < 56) blk[bl++] = 0;
        for (int i = 7; i >= 0; --i) {
            blk[bl++] = static_cast<std::uint8_t>((totalBits >> (i * 8)) & 0xff);
        }
        processBlockInto(blk.data(), snapshot);

        static const char* hexd = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (std::uint32_t w : snapshot) {
            for (int i = 3; i >= 0; --i) {
                std::uint8_t byte = static_cast<std::uint8_t>((w >> (i * 8)) & 0xff);
                out.push_back(hexd[byte >> 4]);
                out.push_back(hexd[byte & 0xf]);
            }
        }
        return out;
    }

private:
    static std::uint32_t rotr(std::uint32_t x, std::uint32_t n) {
        return (x >> n) | (x << (32 - n));
    }

    void processBlock(const std::uint8_t* p) { processBlockInto(p, state_); }

    static void processBlockInto(const std::uint8_t* p,
                                 std::array<std::uint32_t, 8>& st) {
        static const std::uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
            0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
            0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
            0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
            0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
            0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
            0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
            0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
            0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
            0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
            0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(p[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(p[i * 4 + 2]) << 8) |
                   (static_cast<std::uint32_t>(p[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
        std::uint32_t e = st[4], f = st[5], g = st[6], h = st[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t t1 = h + S1 + ch + k[i] + w[i];
            std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t t2 = S0 + maj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        st[0] += a; st[1] += b; st[2] += c; st[3] += d;
        st[4] += e; st[5] += f; st[6] += g; st[7] += h;
    }

    std::array<std::uint32_t, 8> state_{};
    std::uint64_t bitlen_{0};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buflen_{0};
};

[[nodiscard]] inline std::string sha256Hex(const Bytes& data) {
    Sha256 h;
    h.update(data);
    return h.hexDigest();
}

[[nodiscard]] inline std::string sha256Hex(std::string_view data) {
    Sha256 h;
    h.update(data);
    return h.hexDigest();
}

}  // namespace detail

// ===========================================================================
// PackageArchive — filesystem virtuale del container `.pulse`.
// Mappa percorso-entry -> byte; modella le entry dell'archivio ZIP.
// ===========================================================================
class PackageArchive {
public:
    // Aggiunge/sovrascrive una entry. `path` è il percorso interno (es.
    // "pulse.toml", "code/mymod.dll", "resources/icon.png").
    void add(std::string path, Bytes data) {
        entries_[std::move(path)] = std::move(data);
    }

    void addText(std::string path, std::string_view text) {
        add(std::move(path),
            Bytes(reinterpret_cast<const std::uint8_t*>(text.data()),
                  reinterpret_cast<const std::uint8_t*>(text.data()) + text.size()));
    }

    [[nodiscard]] bool contains(std::string_view path) const {
        return entries_.find(std::string(path)) != entries_.end();
    }

    // Ritorna i byte della entry o nullptr se assente.
    [[nodiscard]] const Bytes* find(std::string_view path) const {
        auto it = entries_.find(std::string(path));
        return it == entries_.end() ? nullptr : &it->second;
    }

    // Tutti i percorsi delle entry, in ordine deterministico (std::map).
    [[nodiscard]] std::vector<std::string> paths() const {
        std::vector<std::string> out;
        out.reserve(entries_.size());
        for (const auto& [p, _] : entries_) out.push_back(p);
        return out;
    }

    [[nodiscard]] std::size_t size() const { return entries_.size(); }

private:
    std::map<std::string, Bytes> entries_;
};

// ---------------------------------------------------------------------------
// OpenError — causa di fallimento dell'apertura del package.
// ---------------------------------------------------------------------------
enum class OpenError {
    None,
    ManifestMissing,    // pulse.toml assente (Req 16.3 -> rifiuto)
    ManifestInvalid,    // pulse.toml presente ma non analizzabile (Req 16.4)
    IntegrityMissing,   // MANIFEST.sha256 richiesto ma assente
    IntegrityMismatch,  // hash di integrità non corrispondenti (Req 28.6)
};

[[nodiscard]] inline std::string to_string(OpenError e) {
    switch (e) {
        case OpenError::None: return "none";
        case OpenError::ManifestMissing: return "manifest-missing";
        case OpenError::ManifestInvalid: return "manifest-invalid";
        case OpenError::IntegrityMissing: return "integrity-missing";
        case OpenError::IntegrityMismatch: return "integrity-mismatch";
    }
    return "unknown";
}

class PulsePackage;  // fwd
struct OpenResult;   // fwd (definito dopo PulsePackage)

// ===========================================================================
// PulsePackage — vista validata di un container `.pulse`.
//
// INVARIANTE CHIAVE (Req 16.2): un'istanza esiste SOLO se `pulse.toml` è stato
// localizzato e analizzato con successo. Il costruttore è privato; l'unica via
// di costruzione è `open(...)`, che valida il manifest PRIMA di esporre
// qualsiasi entry `code/`. Gli accessor di codice/risorse sono quindi
// raggiungibili solo dopo la validazione del manifest.
// ===========================================================================
class PulsePackage {
public:
    // Opzioni di apertura.
    struct Options {
        // Se true e `MANIFEST.sha256` è assente -> IntegrityMissing.
        bool requireIntegrityFile{false};
        // Se true e `MANIFEST.sha256` è presente, verifica gli hash; in caso
        // di discrepanza -> IntegrityMismatch (nessun codice esposto).
        bool verifyIntegrity{true};
    };

    // Apre il package dal filesystem virtuale. Consuma `archive` (per move).
    // Ordine garantito: (1) manifest, (2) integrità, (3) esposizione codice.
    // Definita out-of-line dopo `OpenResult` (che contiene un
    // std::optional<PulsePackage> e richiede il tipo completo).
    [[nodiscard]] static OpenResult open(PackageArchive archive,
                                         const Options& opts);
    // Overload con opzioni di default (evita il default-arg `= {}` su tipo
    // annidato all'interno della definizione della classe).
    [[nodiscard]] static OpenResult open(PackageArchive archive);

    // -- Manifest (sempre disponibile: validato in open) --------------------
    [[nodiscard]] const pulse::manifest::Manifest& manifest() const {
        return manifest_;
    }

    // -- Entry di codice (code/) --------------------------------------------
    [[nodiscard]] std::vector<std::string> codeEntries() const {
        return entriesWithPrefix(kCodePrefix);
    }

    [[nodiscard]] const Bytes* codeEntry(std::string_view name) const {
        // `name` può essere relativo a code/ o il percorso completo.
        std::string full = startsWith(name, kCodePrefix)
                               ? std::string(name)
                               : std::string(kCodePrefix) + std::string(name);
        return archive_.find(full);
    }

    // -- Entry di risorse (resources/) --------------------------------------
    [[nodiscard]] std::vector<std::string> resourceEntries() const {
        return entriesWithPrefix(kResourcesPrefix);
    }

    [[nodiscard]] const Bytes* resourceEntry(std::string_view name) const {
        std::string full = startsWith(name, kResourcesPrefix)
                               ? std::string(name)
                               : std::string(kResourcesPrefix) + std::string(name);
        return archive_.find(full);
    }

    // -- Firma (SIGNATURE.sig) ----------------------------------------------
    [[nodiscard]] bool hasSignature() const {
        return archive_.contains(kSignatureEntry);
    }

    [[nodiscard]] const Bytes* signature() const {
        return archive_.find(kSignatureEntry);
    }

    [[nodiscard]] bool hasIntegrityManifest() const {
        return archive_.contains(kIntegrityEntry);
    }

    // Accesso di sola lettura all'archivio sottostante (per ispezione/test).
    [[nodiscard]] const PackageArchive& archive() const { return archive_; }

    // -----------------------------------------------------------------------
    // Helper di costruzione: genera il testo `MANIFEST.sha256` (formato stile
    // sha256sum: "<hex>  <path>") per tutte le entry tranne MANIFEST.sha256 e
    // SIGNATURE.sig. Utile alla CLI (`pulse build`) e ai test.
    // -----------------------------------------------------------------------
    [[nodiscard]] static std::string buildIntegrityManifest(
        const PackageArchive& archive) {
        std::ostringstream os;
        for (const std::string& path : archive.paths()) {
            if (path == kIntegrityEntry || path == kSignatureEntry) continue;
            const Bytes* data = archive.find(path);
            if (data == nullptr) continue;
            os << detail::sha256Hex(*data) << "  " << path << "\n";
        }
        return os.str();
    }

    // Verifica un testo `MANIFEST.sha256` contro le entry dell'archivio.
    // Ritorna true se ogni riga corrisponde e ogni entry coperta esiste con
    // hash corretto; altrimenti popola `mismatch` con la prima discrepanza.
    [[nodiscard]] static bool verifyIntegrityManifest(
        const PackageArchive& archive, std::string_view integrityText,
        std::string& mismatch) {
        std::size_t pos = 0;
        while (pos <= integrityText.size()) {
            std::size_t nl = integrityText.find('\n', pos);
            std::string_view line = (nl == std::string_view::npos)
                                        ? integrityText.substr(pos)
                                        : integrityText.substr(pos, nl - pos);
            pos = (nl == std::string_view::npos) ? integrityText.size() + 1 : nl + 1;

            // Trim CR/spazi di bordo.
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                line.remove_prefix(1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                                     line.back() == '\t'))
                line.remove_suffix(1);
            if (line.empty()) continue;

            // Formato: "<hex64><spazi><path>".
            std::size_t sp = line.find(' ');
            if (sp == std::string_view::npos) {
                mismatch = "riga di integrità malformata: " + std::string(line);
                return false;
            }
            std::string_view expectedHash = line.substr(0, sp);
            std::string_view rest = line.substr(sp);
            while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t'))
                rest.remove_prefix(1);
            std::string path(rest);

            const Bytes* data = archive.find(path);
            if (data == nullptr) {
                mismatch = "entry mancante per l'hash: " + path;
                return false;
            }
            std::string actual = detail::sha256Hex(*data);
            if (actual != expectedHash) {
                mismatch = "hash non corrispondente per " + path;
                return false;
            }
        }
        return true;
    }

private:
    PulsePackage(PackageArchive archive, pulse::manifest::Manifest manifest)
        : archive_(std::move(archive)), manifest_(std::move(manifest)) {}

    [[nodiscard]] static bool startsWith(std::string_view s, std::string_view p) {
        return s.size() >= p.size() && s.substr(0, p.size()) == p;
    }

    [[nodiscard]] std::vector<std::string> entriesWithPrefix(
        std::string_view prefix) const {
        std::vector<std::string> out;
        for (const std::string& p : archive_.paths()) {
            // Esclude la "cartella" nuda (es. "code/") senza nome file.
            if (startsWith(p, prefix) && p.size() > prefix.size()) {
                out.push_back(p);
            }
        }
        return out;
    }

    PackageArchive archive_;
    pulse::manifest::Manifest manifest_;
};

// ---------------------------------------------------------------------------
// OpenResult — esito di PulsePackage::open. `ok == true` -> `package` valido.
// Definito dopo PulsePackage perché contiene un std::optional<PulsePackage>
// (richiede il tipo completo).
// ---------------------------------------------------------------------------
struct OpenResult {
    bool ok{false};
    OpenError error{OpenError::None};
    std::string message;
    std::optional<PulsePackage> package;  // valorizzato sse ok
};

// ---------------------------------------------------------------------------
// PulsePackage::open — definita qui, dove `OpenResult` (e quindi
// std::optional<PulsePackage>) è un tipo completo.
// ---------------------------------------------------------------------------
inline OpenResult PulsePackage::open(PackageArchive archive) {
    return open(std::move(archive), Options{});
}

inline OpenResult PulsePackage::open(PackageArchive archive,
                                     const Options& opts) {
    OpenResult res;

    // (1) Manifest PRIMA di qualsiasi codice (Req 16.2/16.3).
    const Bytes* manifestBytes = archive.find(kManifestEntry);
    if (manifestBytes == nullptr) {
        res.ok = false;
        res.error = OpenError::ManifestMissing;
        res.message = "Pulse Package privo di 'pulse.toml': caricamento "
                      "rifiutato, nessun codice eseguito.";
        return res;  // nessun PulsePackage costruito -> codice inaccessibile
    }

    std::string manifestText(
        reinterpret_cast<const char*>(manifestBytes->data()),
        manifestBytes->size());

    pulse::manifest::ParseResult parsed = pulse::manifest::parse(manifestText);
    if (!parsed.ok) {
        res.ok = false;
        res.error = OpenError::ManifestInvalid;
        res.message = "Manifest 'pulse.toml' non analizzabile: " + parsed.error +
                      " — caricamento rifiutato, nessun codice eseguito.";
        return res;  // manifest non valido -> codice inaccessibile
    }

    // (2) Integrità (MANIFEST.sha256) — solo dopo un manifest valido.
    const Bytes* integrity = archive.find(kIntegrityEntry);
    if (integrity == nullptr) {
        if (opts.requireIntegrityFile) {
            res.ok = false;
            res.error = OpenError::IntegrityMissing;
            res.message = "Pulse Package privo di 'MANIFEST.sha256' richiesto.";
            return res;
        }
    } else if (opts.verifyIntegrity) {
        std::string integrityText(
            reinterpret_cast<const char*>(integrity->data()),
            integrity->size());
        std::string mismatch;
        if (!verifyIntegrityManifest(archive, integrityText, mismatch)) {
            res.ok = false;
            res.error = OpenError::IntegrityMismatch;
            res.message = "Integrità del Pulse Package fallita: " + mismatch;
            return res;
        }
    }

    // (3) SOLO ORA costruiamo l'oggetto che espone codice/risorse.
    PulsePackage pkg(std::move(archive), std::move(parsed.manifest));
    res.ok = true;
    res.error = OpenError::None;
    res.package = std::move(pkg);
    return res;
}

}  // namespace pulse::package

#endif  // PULSE_LOADER_PACKAGE_PULSE_PACKAGE_HPP
