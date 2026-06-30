// tests/mod_loader_dedup_test.cpp — Unit test della dedup deterministica per
// Mod_Id (task 3.2, Requisiti 1.6, 1.7).
//
// `dedup_by_mod_id` è una funzione componibile applicata all'output di
// `discover_mods`: garantisce che ogni Mod_Id compaia al più una volta (Req 1.6)
// e, in caso di collisione, mantiene il Mod_Package il cui `entryName` precede
// in ordine lessicografico, escludendo gli altri con diagnostica che nomina il
// Mod_Id duplicato e le voci coinvolte (Req 1.7).
//
// La costruzione dei `DiscoveredMod` riusa lo stesso filesystem virtuale della
// discovery (seam `DirectoryLister`/`PackageOpener`), così il test esercita la
// pipeline discover → dedup esattamente come la userà la Fase D.
//
// Header del loader in loader/ (include relativo alla radice loader/).

#include "lifecycle/mod_loader.hpp"

#include <algorithm>
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
using pulse::lifecycle::DiscoveredMod;
using pulse::lifecycle::DiscoveredModSet;
using pulse::lifecycle::dedup_by_mod_id;
using pulse::lifecycle::discover_mods;
using pulse::manifest::EntryPoint;
using pulse::manifest::Manifest;
using pulse::manifest::ModType;
using pulse::manifest::SemVer;
using pulse::package::OpenResult;
using pulse::package::PackageArchive;
using pulse::package::PulsePackage;

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

PackageArchive makeArchive(const std::string& id) {
    PackageArchive a;
    a.addText(std::string(pulse::package::kManifestEntry),
              pulse::manifest::serialize(makeManifest(id)));
    a.addText("code/module.pulsebin", "BINARY");
    return a;
}

// Filesystem virtuale: entryName -> manifest id contenuto nel `.pulse`.
struct FakeFs {
    std::map<std::string, std::string> entries;  // entryName -> modId

    pulse::lifecycle::DirectoryLister lister() const {
        return [this](const std::filesystem::path&) {
            DirectoryListing listing;
            listing.status = DirectoryReadStatus::Ok;
            for (const auto& [name, _] : entries)
                listing.entryNames.push_back(name);
            return listing;
        };
    }

    pulse::lifecycle::PackageOpener opener() const {
        return [this](const std::filesystem::path&,
                      std::string_view entryName) -> OpenResult {
            auto it = entries.find(std::string(entryName));
            if (it == entries.end()) {
                OpenResult res;
                res.ok = false;
                res.error = pulse::package::OpenError::ManifestMissing;
                res.message = "container non apribile";
                return res;
            }
            PackageArchive archive = makeArchive(it->second);
            PulsePackage::Options opts;
            opts.verifyIntegrity = false;
            return PulsePackage::open(archive, opts);
        };
    }
};

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

// Helper: costruisce un DiscoveredMod (container aperto) per (entryName, modId).
DiscoveredMod makeDiscovered(const std::string& entryName,
                             const std::string& modId) {
    PackageArchive archive = makeArchive(modId);
    PulsePackage::Options opts;
    opts.verifyIntegrity = false;
    OpenResult res = PulsePackage::open(archive, opts);
    EXPECT_TRUE(res.ok);
    return DiscoveredMod{entryName, modId, std::move(*res.package)};
}

// --- Req 1.6: nessuna collisione → set invariato, ogni Mod_Id una volta ----

TEST(ModLoaderDedup, DistinctModIdsArePreserved) {
    DiscoveredModSet in;
    in.push_back(makeDiscovered("alpha.pulse", "com.example.alpha"));
    in.push_back(makeDiscovered("bravo.pulse", "com.example.bravo"));
    in.push_back(makeDiscovered("charlie.pulse", "com.example.charlie"));

    CapturingSink sink;
    DiscoveredModSet out = dedup_by_mod_id(std::move(in), sink.sink());

    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].entryName, "alpha.pulse");
    EXPECT_EQ(out[1].entryName, "bravo.pulse");
    EXPECT_EQ(out[2].entryName, "charlie.pulse");
    EXPECT_TRUE(sink.messages.empty());
}

// --- Req 1.7: collisione → vince l'entryName lessicografico minore ----------

TEST(ModLoaderDedup, CollisionKeepsLexicographicallySmallestEntryName) {
    DiscoveredModSet in;
    // Stesso Mod_Id "dup" da tre voci; deve vincere "aaa.pulse".
    in.push_back(makeDiscovered("ccc.pulse", "com.example.dup"));
    in.push_back(makeDiscovered("aaa.pulse", "com.example.dup"));
    in.push_back(makeDiscovered("bbb.pulse", "com.example.dup"));

    CapturingSink sink;
    DiscoveredModSet out = dedup_by_mod_id(std::move(in), sink.sink());

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].entryName, "aaa.pulse");
    EXPECT_EQ(out[0].modId, "com.example.dup");

    // Diagnostica nomina il Mod_Id duplicato e le voci coinvolte (Req 1.7).
    EXPECT_TRUE(sink.anyContains("com.example.dup"));
    EXPECT_TRUE(sink.anyContains("bbb.pulse"));
    EXPECT_TRUE(sink.anyContains("ccc.pulse"));
    // La voce mantenuta è nominata come tale nelle diagnostiche di esclusione.
    EXPECT_TRUE(sink.anyContains("aaa.pulse"));
}

// --- Req 1.7: il vincitore non dipende dall'ordine d'ingresso ---------------

TEST(ModLoaderDedup, WinnerIsIndependentOfInputOrder) {
    DiscoveredModSet ascending;
    ascending.push_back(makeDiscovered("aaa.pulse", "com.example.dup"));
    ascending.push_back(makeDiscovered("zzz.pulse", "com.example.dup"));

    DiscoveredModSet descending;
    descending.push_back(makeDiscovered("zzz.pulse", "com.example.dup"));
    descending.push_back(makeDiscovered("aaa.pulse", "com.example.dup"));

    CapturingSink s1, s2;
    DiscoveredModSet o1 = dedup_by_mod_id(std::move(ascending), s1.sink());
    DiscoveredModSet o2 = dedup_by_mod_id(std::move(descending), s2.sink());

    ASSERT_EQ(o1.size(), 1u);
    ASSERT_EQ(o2.size(), 1u);
    EXPECT_EQ(o1[0].entryName, "aaa.pulse");
    EXPECT_EQ(o2[0].entryName, "aaa.pulse");
}

// --- Req 1.6: mix di collisioni e id distinti, output ordinato per entryName -

TEST(ModLoaderDedup, MixedCollisionsAndDistinctIdsRemainSorted) {
    DiscoveredModSet in;
    in.push_back(makeDiscovered("d.pulse", "id.b"));
    in.push_back(makeDiscovered("b.pulse", "id.a"));  // winner per id.a
    in.push_back(makeDiscovered("e.pulse", "id.a"));  // escluso
    in.push_back(makeDiscovered("a.pulse", "id.b"));  // winner per id.b
    in.push_back(makeDiscovered("c.pulse", "id.c"));

    CapturingSink sink;
    DiscoveredModSet out = dedup_by_mod_id(std::move(in), sink.sink());

    // Ogni Mod_Id al più una volta (Req 1.6).
    ASSERT_EQ(out.size(), 3u);
    std::vector<std::string> ids;
    std::vector<std::string> entries;
    for (const auto& m : out) {
        ids.push_back(m.modId);
        entries.push_back(m.entryName);
    }
    // Output ordinato per entryName (determinismo).
    EXPECT_TRUE(std::is_sorted(entries.begin(), entries.end()));
    // Vincitori: id.b -> a.pulse, id.a -> b.pulse, id.c -> c.pulse.
    EXPECT_EQ(entries[0], "a.pulse");
    EXPECT_EQ(entries[1], "b.pulse");
    EXPECT_EQ(entries[2], "c.pulse");
    EXPECT_EQ(out[0].modId, "id.b");
    EXPECT_EQ(out[1].modId, "id.a");
    EXPECT_EQ(out[2].modId, "id.c");
    // Le voci escluse (d.pulse per id.b, e.pulse per id.a) sono diagnosticate.
    EXPECT_TRUE(sink.anyContains("d.pulse"));
    EXPECT_TRUE(sink.anyContains("e.pulse"));
}

// --- Empty set → empty set --------------------------------------------------

TEST(ModLoaderDedup, EmptySetYieldsEmptySet) {
    CapturingSink sink;
    DiscoveredModSet out = dedup_by_mod_id(DiscoveredModSet{}, sink.sink());
    EXPECT_TRUE(out.empty());
    EXPECT_TRUE(sink.messages.empty());
}

// --- Pipeline discover → dedup: collisione risolta end-to-end ---------------

TEST(ModLoaderDedup, DiscoverThenDedupResolvesCollision) {
    FakeFs fs;
    // Due voci dichiarano lo stesso Mod_Id; vince "alpha.pulse".
    fs.entries["alpha.pulse"] = "com.example.shared";
    fs.entries["omega.pulse"] = "com.example.shared";
    fs.entries["unique.pulse"] = "com.example.unique";

    CapturingSink sink;
    DiscoveredModSet discovered =
        discover_mods("/mods", fs.lister(), fs.opener(), sink.sink());
    ASSERT_EQ(discovered.size(), 3u);  // discovery non deduplica (task 3.1)

    DiscoveredModSet out = dedup_by_mod_id(std::move(discovered), sink.sink());

    ASSERT_EQ(out.size(), 2u);
    // Ogni Mod_Id una sola volta (Req 1.6).
    EXPECT_EQ(out[0].entryName, "alpha.pulse");
    EXPECT_EQ(out[0].modId, "com.example.shared");
    EXPECT_EQ(out[1].entryName, "unique.pulse");
    EXPECT_EQ(out[1].modId, "com.example.unique");
    // La voce in conflitto "omega.pulse" è esclusa con diagnostica (Req 1.7).
    EXPECT_TRUE(sink.anyContains("com.example.shared"));
    EXPECT_TRUE(sink.anyContains("omega.pulse"));
}

}  // namespace
