// tests/mod_loader_discovery_test.cpp — Unit test della discovery della
// Mods_Directory (task 3.1, Requisiti 1.1–1.5).
//
// La discovery è host-testabile su un filesystem virtuale tramite i seam
// iniettabili `DirectoryLister` (enumerazione delle voci di primo livello) e
// `PackageOpener` (apertura di un container `.pulse`). Qui costruiamo una
// Mods_Directory finta come mappa entryName -> PackageArchive e verifichiamo:
//   * solo le voci di primo livello sono considerate (no ricorsione, Req 1.1);
//   * un candidato è riconosciuto sse `.pulse` apribile con successo (Req 1.2);
//   * directory assente/vuota → set vuoto + esito registrato (Req 1.3);
//   * directory non leggibile → set vuoto + diagnostica con dir e causa (1.4);
//   * voce non `.pulse`/non apribile → ignorata con diagnostica, prosegue (1.5);
//   * determinismo: candidati ordinati per `entryName` lessicografico (Req 1.6).
//
// Header del loader in loader/ (include relativo alla radice loader/).

#include "lifecycle/mod_loader.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lifecycle/manifest.hpp"
#include "package/pulse_package.hpp"

namespace {

using pulse::lifecycle::DirectoryListing;
using pulse::lifecycle::DirectoryReadStatus;
using pulse::lifecycle::DiscoveredModSet;
using pulse::lifecycle::discover_mods;
using pulse::manifest::EntryPoint;
using pulse::manifest::Manifest;
using pulse::manifest::ModType;
using pulse::manifest::SemVer;
using pulse::package::OpenResult;
using pulse::package::PackageArchive;
using pulse::package::PulsePackage;

// Manifest valido minimale con il Mod_Id dato.
Manifest makeManifest(const std::string& id) {
    Manifest m;
    m.schemaVersion = 1;
    m.id = id;
    m.version = SemVer{1, 0, 0};
    m.name = id;
    m.type = ModType::Native;
    m.entryPoints = {EntryPoint{"init", "mod_init"}};
    return m;
}

// Archivio `.pulse` valido (solo pulse.toml; nessun MANIFEST.sha256, quindi
// l'opener apre con verifyIntegrity=false).
PackageArchive makeArchive(const std::string& id) {
    PackageArchive a;
    a.addText(std::string(pulse::package::kManifestEntry),
              pulse::manifest::serialize(makeManifest(id)));
    a.addText("code/module.pulsebin", "BINARY");
    return a;
}

// Filesystem virtuale: nomi di voce di primo livello → contenuto.
//   * un entry con archivio presente è un `.pulse` apribile;
//   * un entry con archivio assente (std::nullopt) è una voce non apribile
//     (es. file `.pulse` corrotto) oppure una voce non-`.pulse`.
struct FakeFs {
    DirectoryReadStatus status{DirectoryReadStatus::Ok};
    std::string error;
    std::map<std::string, std::optional<PackageArchive>> entries;

    pulse::lifecycle::DirectoryLister lister() const {
        return [this](const std::filesystem::path&) {
            DirectoryListing listing;
            listing.status = status;
            listing.error = error;
            if (status == DirectoryReadStatus::Ok) {
                for (const auto& [name, _] : entries)
                    listing.entryNames.push_back(name);
            }
            return listing;
        };
    }

    pulse::lifecycle::PackageOpener opener() const {
        return [this](const std::filesystem::path&,
                      std::string_view entryName) -> OpenResult {
            auto it = entries.find(std::string(entryName));
            if (it == entries.end() || !it->second.has_value()) {
                OpenResult res;
                res.ok = false;
                res.error = pulse::package::OpenError::ManifestMissing;
                res.message = "container non apribile";
                return res;
            }
            PulsePackage::Options opts;
            opts.verifyIntegrity = false;  // archivi senza MANIFEST.sha256
            return PulsePackage::open(*it->second, opts);
        };
    }
};

// Sink diagnostico che cattura i messaggi.
struct CapturingSink {
    std::vector<std::string> messages;
    pulse::loader::DiagnosticSink sink() {
        return [this](std::string_view m) { messages.emplace_back(m); };
    }
    bool anyContains(std::string_view needle) const {
        for (const std::string& m : messages)
            if (m.find(needle) != std::string::npos) return true;
        return false;
    }
};

// --- Candidati validi: scoperti e ordinati lessicograficamente -------------

TEST(ModLoaderDiscovery, DiscoversValidPulsePackagesSortedByEntryName) {
    FakeFs fs;
    fs.entries["bravo.pulse"] = makeArchive("com.example.bravo");
    fs.entries["alpha.pulse"] = makeArchive("com.example.alpha");
    fs.entries["charlie.pulse"] = makeArchive("com.example.charlie");

    CapturingSink sink;
    DiscoveredModSet set =
        discover_mods("/mods", fs.lister(), fs.opener(), sink.sink());

    ASSERT_EQ(set.size(), 3u);
    // Ordine deterministico per entryName lessicografico (Req 1.6).
    EXPECT_EQ(set[0].entryName, "alpha.pulse");
    EXPECT_EQ(set[1].entryName, "bravo.pulse");
    EXPECT_EQ(set[2].entryName, "charlie.pulse");
    // Mod_Id ricavato dal manifest del container aperto.
    EXPECT_EQ(set[0].modId, "com.example.alpha");
    EXPECT_EQ(set[1].modId, "com.example.bravo");
    EXPECT_EQ(set[2].modId, "com.example.charlie");
}

// --- Req 1.5: voce non-`.pulse` ignorata, le valide proseguono -------------

TEST(ModLoaderDiscovery, IgnoresNonPulseEntriesAndContinues) {
    FakeFs fs;
    fs.entries["valid.pulse"] = makeArchive("com.example.valid");
    fs.entries["README.txt"] = std::nullopt;   // non `.pulse`
    fs.entries["nested"] = std::nullopt;        // sottocartella di primo livello

    CapturingSink sink;
    DiscoveredModSet set =
        discover_mods("/mods", fs.lister(), fs.opener(), sink.sink());

    ASSERT_EQ(set.size(), 1u);
    EXPECT_EQ(set[0].entryName, "valid.pulse");
    EXPECT_TRUE(sink.anyContains("README.txt"));
    EXPECT_TRUE(sink.anyContains("nested"));
}

// --- Req 1.2/1.5: `.pulse` non apribile ignorato, le valide proseguono -----

TEST(ModLoaderDiscovery, IgnoresUnopenablePulseEntriesAndContinues) {
    FakeFs fs;
    fs.entries["good.pulse"] = makeArchive("com.example.good");
    fs.entries["broken.pulse"] = std::nullopt;  // apertura fallisce

    CapturingSink sink;
    DiscoveredModSet set =
        discover_mods("/mods", fs.lister(), fs.opener(), sink.sink());

    ASSERT_EQ(set.size(), 1u);
    EXPECT_EQ(set[0].entryName, "good.pulse");
    EXPECT_TRUE(sink.anyContains("broken.pulse"));
}

// --- Req 1.3: directory assente → set vuoto + esito registrato -------------

TEST(ModLoaderDiscovery, AbsentDirectoryYieldsEmptySet) {
    FakeFs fs;
    fs.status = DirectoryReadStatus::Absent;

    CapturingSink sink;
    DiscoveredModSet set =
        discover_mods("/mods", fs.lister(), fs.opener(), sink.sink());

    EXPECT_TRUE(set.empty());
    EXPECT_TRUE(sink.anyContains("assente"));
}

// --- Req 1.3: directory vuota → set vuoto ----------------------------------

TEST(ModLoaderDiscovery, EmptyDirectoryYieldsEmptySet) {
    FakeFs fs;  // status Ok, nessuna entry

    CapturingSink sink;
    DiscoveredModSet set =
        discover_mods("/mods", fs.lister(), fs.opener(), sink.sink());

    EXPECT_TRUE(set.empty());
    EXPECT_TRUE(sink.anyContains("priva di voci"));
}

// --- Req 1.4: directory non leggibile → set vuoto + diagnostica con causa ---

TEST(ModLoaderDiscovery, UnreadableDirectoryYieldsEmptySetWithDiagnostic) {
    FakeFs fs;
    fs.status = DirectoryReadStatus::NotReadable;
    fs.error = "permission denied";

    CapturingSink sink;
    DiscoveredModSet set =
        discover_mods("/secret-mods", fs.lister(), fs.opener(), sink.sink());

    EXPECT_TRUE(set.empty());
    EXPECT_TRUE(sink.anyContains("non leggibile"));
    EXPECT_TRUE(sink.anyContains("permission denied"));
    EXPECT_TRUE(sink.anyContains("/secret-mods"));
}

// --- default_directory_lister: directory inesistente su disco reale --------

TEST(ModLoaderDiscovery, DefaultListerReportsAbsentForMissingPath) {
    DirectoryListing listing = pulse::lifecycle::default_directory_lister(
        "/path/che/non/esiste/pulse-mods-xyz");
    EXPECT_EQ(listing.status, DirectoryReadStatus::Absent);
}

}  // namespace
