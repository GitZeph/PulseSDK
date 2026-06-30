// tests/pulse_zip_reader_test.cpp
// Feature: external-mod-loading (Fase E) — lettore `.pulse` (ZIP STORED) e
// riconciliazione del binario nativo nel Mod_Manifest_Validator.
//
// Verifica, con ZIP costruiti a mano come buffer di byte grezzi (nessuna
// dipendenza da cargo/toolchain), che:
//   * `read_pulse_archive` ricostruisca esattamente le voci STORED (byte
//     identici);
//   * `open_pulse_file` apra un `.pulse` reale su disco e ne validi il manifest;
//   * una voce con metodo Deflate (8) faccia fallire il parsing nominando il
//     metodo (fail-open, container non leggibile);
//   * un buffer malformato/corto ritorni nullopt senza lanciare;
//   * il `ModManifestValidator` accetti una mod nativa con sola libreria
//     `code/macos-arm64.dylib` (senza `module.pulsebin`) e rifiuti una mod
//     nativa priva di qualunque binario nativo (MissingRequiredFile).

#include "lifecycle/manifest.hpp"
#include "lifecycle/mod_manifest_validator.hpp"
#include "package/pulse_package.hpp"
#include "package/pulse_zip_reader.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

using pulse::lifecycle::ModManifestValidator;
using pulse::lifecycle::ValidationCause;
using pulse::lifecycle::ValidationResult;
using pulse::manifest::EntryPoint;
using pulse::manifest::Manifest;
using pulse::manifest::ModType;
using pulse::manifest::SemVer;
using pulse::package::Bytes;
using pulse::package::OpenError;
using pulse::package::OpenResult;
using pulse::package::PackageArchive;
using pulse::package::PulsePackage;

struct ZipEntry {
    std::string name;
    Bytes data;
};

// --- Scrittori little-endian -------------------------------------------------
void putU16(Bytes& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void putU32(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

void putBytes(Bytes& out, const Bytes& b) {
    out.insert(out.end(), b.begin(), b.end());
}

void putStr(Bytes& out, const std::string& s) {
    out.insert(out.end(), s.begin(), s.end());
}

Bytes textBytes(std::string_view s) {
    return Bytes(reinterpret_cast<const std::uint8_t*>(s.data()),
                 reinterpret_cast<const std::uint8_t*>(s.data()) + s.size());
}

// Assembla uno ZIP a mano. `method` è scritto sia nell'header locale sia nel
// record di central directory (i dati restano sempre non compressi: per
// method != 0 si simula un container "che dichiara" un metodo non supportato).
Bytes makeZip(const std::vector<ZipEntry>& entries, std::uint16_t method = 0) {
    Bytes out;
    std::vector<std::uint32_t> localOffsets;
    localOffsets.reserve(entries.size());

    // Header locali + dati.
    for (const ZipEntry& e : entries) {
        localOffsets.push_back(static_cast<std::uint32_t>(out.size()));
        const std::uint32_t size = static_cast<std::uint32_t>(e.data.size());

        putU32(out, 0x04034b50u);                 // local file header signature
        putU16(out, 20);                           // version needed
        putU16(out, 0);                            // general purpose flag
        putU16(out, method);                       // compression method
        putU16(out, 0);                            // mod time
        putU16(out, 0);                            // mod date
        putU32(out, 0);                            // crc-32 (non verificato)
        putU32(out, size);                         // compressed size
        putU32(out, size);                         // uncompressed size
        putU16(out, static_cast<std::uint16_t>(e.name.size()));  // name length
        putU16(out, 0);                            // extra length
        putStr(out, e.name);
        putBytes(out, e.data);
    }

    // Central directory.
    const std::uint32_t centralStart = static_cast<std::uint32_t>(out.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const ZipEntry& e = entries[i];
        const std::uint32_t size = static_cast<std::uint32_t>(e.data.size());

        putU32(out, 0x02014b50u);                 // central dir signature
        putU16(out, 20);                           // version made by
        putU16(out, 20);                           // version needed
        putU16(out, 0);                            // general purpose flag
        putU16(out, method);                       // compression method
        putU16(out, 0);                            // mod time
        putU16(out, 0);                            // mod date
        putU32(out, 0);                            // crc-32
        putU32(out, size);                         // compressed size
        putU32(out, size);                         // uncompressed size
        putU16(out, static_cast<std::uint16_t>(e.name.size()));  // name length
        putU16(out, 0);                            // extra length
        putU16(out, 0);                            // comment length
        putU16(out, 0);                            // disk number start
        putU16(out, 0);                            // internal attrs
        putU32(out, 0);                            // external attrs
        putU32(out, localOffsets[i]);              // local header offset
        putStr(out, e.name);
    }
    const std::uint32_t centralSize =
        static_cast<std::uint32_t>(out.size()) - centralStart;

    // End Of Central Directory.
    putU32(out, 0x06054b50u);                      // EOCD signature
    putU16(out, 0);                                // disk number
    putU16(out, 0);                                // disk with central dir
    putU16(out, static_cast<std::uint16_t>(entries.size()));  // entries on disk
    putU16(out, static_cast<std::uint16_t>(entries.size()));  // total entries
    putU32(out, centralSize);                      // central dir size
    putU32(out, centralStart);                     // central dir offset
    putU16(out, 0);                                // comment length
    return out;
}

// Manifest nativo valido come testo `pulse.toml`.
std::string validManifestText() {
    Manifest m;
    m.schemaVersion = 1;
    m.id = "com.example.disktest";
    m.version = SemVer{0, 1, 0};
    m.name = "Disk ZIP fixture";
    m.type = ModType::Native;
    m.entryPoints.push_back(EntryPoint{"init", "pulse_mod_init"});
    return pulse::manifest::serialize(m);
}

// Costruisce le voci di un `.pulse` valido (manifest + libreria nativa +
// MANIFEST.sha256 coerente).
std::vector<ZipEntry> validEntries() {
    const std::string manifestText = validManifestText();
    const Bytes moduleBytes = textBytes("\x7f" "ELF-fake-native-binary-bytes");

    // Calcola MANIFEST.sha256 sulle voci coperte via l'helper del package.
    PackageArchive a;
    a.addText(std::string(pulse::package::kManifestEntry), manifestText);
    a.add("code/macos-arm64.dylib", moduleBytes);
    const std::string integrity = PulsePackage::buildIntegrityManifest(a);

    return {
        ZipEntry{std::string(pulse::package::kManifestEntry), textBytes(manifestText)},
        ZipEntry{"code/macos-arm64.dylib", moduleBytes},
        ZipEntry{std::string(pulse::package::kIntegrityEntry), textBytes(integrity)},
    };
}

// --- read_pulse_archive: ricostruzione esatta -------------------------------
TEST(PulseZipReader, ReconstructsStoredEntriesExactly) {
    const std::vector<ZipEntry> entries = {
        ZipEntry{"pulse.toml", textBytes("schema_version = 1\n")},
        ZipEntry{"code/macos-arm64.dylib", Bytes{0x00, 0x01, 0x02, 'n', 'a', 't', 0xff}},
        ZipEntry{"MANIFEST.sha256", textBytes("deadbeef  pulse.toml\n")},
    };
    const Bytes zip = makeZip(entries);

    std::string error;
    auto archive = pulse::package::read_pulse_archive(zip, error);
    ASSERT_TRUE(archive.has_value()) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(archive->size(), 3u);

    for (const ZipEntry& e : entries) {
        const Bytes* got = archive->find(e.name);
        ASSERT_NE(got, nullptr) << "voce mancante: " << e.name;
        EXPECT_EQ(*got, e.data) << "byte differenti per: " << e.name;
    }
}

// Voce di directory ("code/") ignorata; le altre ricostruite.
TEST(PulseZipReader, SkipsDirectoryEntries) {
    const std::vector<ZipEntry> entries = {
        ZipEntry{"code/", Bytes{}},
        ZipEntry{"pulse.toml", textBytes("schema_version = 1\n")},
    };
    const Bytes zip = makeZip(entries);

    std::string error;
    auto archive = pulse::package::read_pulse_archive(zip, error);
    ASSERT_TRUE(archive.has_value()) << error;
    EXPECT_EQ(archive->size(), 1u);
    EXPECT_TRUE(archive->contains("pulse.toml"));
    EXPECT_FALSE(archive->contains("code/"));
}

// --- open_pulse_file: file reale su disco -----------------------------------
TEST(PulseZipReader, OpenPulseFileFromDiskValidatesManifest) {
    const Bytes zip = makeZip(validEntries());

    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() /
        ("pulse-zip-reader-" + std::to_string(std::random_device{}()) + ".pulse");
    {
        std::ofstream out(tmp, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out.write(reinterpret_cast<const char*>(zip.data()),
                  static_cast<std::streamsize>(zip.size()));
    }

    PulsePackage::Options opts;
    opts.verifyIntegrity = true;
    opts.requireIntegrityFile = false;
    const OpenResult res = pulse::package::open_pulse_file(tmp, opts);

    EXPECT_TRUE(res.ok) << res.message;
    ASSERT_TRUE(res.package.has_value());
    EXPECT_EQ(res.package->manifest().id, "com.example.disktest");
    EXPECT_TRUE(res.package->archive().contains("code/macos-arm64.dylib"));
    EXPECT_NE(res.package->codeEntry("macos-arm64.dylib"), nullptr);

    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}

TEST(PulseZipReader, OpenPulseFileMissingPathFailsCleanly) {
    const std::filesystem::path missing =
        std::filesystem::temp_directory_path() / "pulse-does-not-exist.pulse";
    std::error_code ec;
    std::filesystem::remove(missing, ec);

    PulsePackage::Options opts;
    const OpenResult res = pulse::package::open_pulse_file(missing, opts);
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.error, OpenError::ManifestMissing);
    EXPECT_NE(res.message.find("pulse-does-not-exist.pulse"), std::string::npos);
}

// --- Metodo Deflate (8): fail-open con messaggio che nomina il metodo --------
TEST(PulseZipReader, DeflateMethodIsRejectedNamingMethod) {
    const std::vector<ZipEntry> entries = {
        ZipEntry{"pulse.toml", textBytes("schema_version = 1\n")},
    };
    const Bytes zip = makeZip(entries, /*method=*/8);

    std::string error;
    auto archive = pulse::package::read_pulse_archive(zip, error);
    EXPECT_FALSE(archive.has_value());
    EXPECT_FALSE(error.empty());
    EXPECT_NE(error.find('8'), std::string::npos) << error;
    EXPECT_NE(error.find("pulse.toml"), std::string::npos) << error;
}

// --- Buffer malformato/corto: nullopt, nessuna eccezione ---------------------
TEST(PulseZipReader, ShortBufferReturnsNulloptNoThrow) {
    const Bytes tooShort = {0x50, 0x4b, 0x05};  // troncato, niente EOCD valido
    std::string error;
    std::optional<PackageArchive> archive;
    EXPECT_NO_THROW({ archive = pulse::package::read_pulse_archive(tooShort, error); });
    EXPECT_FALSE(archive.has_value());
    EXPECT_FALSE(error.empty());
}

TEST(PulseZipReader, EmptyBufferReturnsNulloptNoThrow) {
    const Bytes empty;
    std::string error;
    std::optional<PackageArchive> archive;
    EXPECT_NO_THROW({ archive = pulse::package::read_pulse_archive(empty, error); });
    EXPECT_FALSE(archive.has_value());
}

// --- Validator: riconciliazione del binario nativo --------------------------

PackageArchive nativeArchiveWith(const std::string& codeEntryName,
                                 bool includeCode) {
    Manifest m;
    m.schemaVersion = 1;
    m.id = "com.example.native";
    m.version = SemVer{1, 0, 0};
    m.name = "Native fixture";
    m.type = ModType::Native;
    m.entryPoints.push_back(EntryPoint{"init", "pulse_mod_init"});

    PackageArchive a;
    a.addText(std::string(pulse::package::kManifestEntry),
              pulse::manifest::serialize(m));
    if (includeCode) {
        a.add(codeEntryName, textBytes("native-binary"));
    }
    return a;
}

TEST(PulseZipReader, ValidatorAcceptsNativeWithPlatformDylibOnly) {
    // Nessun code/module.pulsebin, solo code/macos-arm64.dylib.
    PackageArchive a = nativeArchiveWith("code/macos-arm64.dylib", true);

    const ValidationResult res = ModManifestValidator{}.validate(std::move(a));
    EXPECT_TRUE(res.accepted) << res.message;
    EXPECT_EQ(res.cause, ValidationCause::Ok);
    ASSERT_TRUE(res.manifest.has_value());
    EXPECT_EQ(res.manifest->id, "com.example.native");
}

TEST(PulseZipReader, ValidatorRejectsNativeWithNoBinary) {
    // Mod nativa senza alcun binario sotto code/.
    PackageArchive a = nativeArchiveWith("code/macos-arm64.dylib", false);

    const ValidationResult res = ModManifestValidator{}.validate(std::move(a));
    EXPECT_FALSE(res.accepted);
    EXPECT_EQ(res.cause, ValidationCause::MissingRequiredFile);
    EXPECT_FALSE(res.manifest.has_value());
}

}  // namespace
