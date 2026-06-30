// tests/eml_property4_discovery_first_level_isolation_test.cpp
// Feature: external-mod-loading, Property 4 — Discovery enumera solo il primo
// livello e isola le voci invalide.
// Validates: Requirements 1.1, 1.5 (Requisiti 1.1, 1.5)
//
// Property 4 (design.md): per ogni albero di Mods_Directory con voci di primo
// livello e voci nidificate, l'insieme dei candidati scoperti è ESATTAMENTE
// l'insieme delle voci di primo livello riconoscibili come Mod_Package (un
// container `.pulse` aperto con successo), ogni voce NON riconoscibile è
// ignorata con una diagnostica che la nomina, e le voci valide restanti sono
// comunque scoperte.
//
// La discovery è host-testabile su un filesystem virtuale tramite i seam
// iniettabili `DirectoryLister` (enumera le SOLE voci di primo livello, Req 1.1)
// e `PackageOpener` (apre una voce come container `.pulse`, Req 1.2). Qui:
//   * il `DirectoryLister` virtuale restituisce SOLO i nomi delle voci di primo
//     livello (mai i contenuti delle sottocartelle): modella "nessuna discesa
//     ricorsiva" (Req 1.1). Le voci nidificate generate dentro le sottocartelle
//     non sono mai enumerate;
//   * il `PackageOpener` virtuale apre con successo solo le voci `.pulse` valide
//     e fallisce su quelle non apribili.
//
// Strategia (RapidCheck, ≥100 iterazioni di default — qui forzate a ≥100):
//   * si genera un insieme di voci di primo livello con nomi DISTINTI (prefisso
//     per-kind + indice), mescolando quattro categorie:
//       - ValidPulse      → `.pulse` apribile  (deve essere scoperta)
//       - UnopenablePulse → `.pulse` non apribile (ignorata + diagnostica)
//       - NonPulseFile    → voce non `.pulse`  (ignorata + diagnostica)
//       - NestedDir       → sottocartella di primo livello (ignorata +
//                            diagnostica) che CONTIENE voci `.pulse` nidificate
//                            che NON devono mai essere scoperte (Req 1.1);
//   * l'oracolo ricalcola, indipendentemente da `discover_mods`, l'insieme atteso
//     dei candidati (= le sole voci ValidPulse di primo livello) e l'insieme
//     delle voci non riconoscibili che devono produrre diagnostica.

#include "lifecycle/mod_loader.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

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

// Archivio `.pulse` valido (pulse.toml + module; nessun MANIFEST.sha256, quindi
// l'opener apre con verifyIntegrity=false).
PackageArchive makeArchive(const std::string& id) {
    PackageArchive a;
    a.addText(std::string(pulse::package::kManifestEntry),
              pulse::manifest::serialize(makeManifest(id)));
    a.addText("code/module.pulsebin", "BINARY");
    return a;
}

// Categoria di una voce generata di primo livello.
enum class EntryKind { ValidPulse, UnopenablePulse, NonPulseFile, NestedDir };

// Filesystem virtuale:
//   * `topLevel` = nomi delle SOLE voci di primo livello restituiti dal lister
//     (Req 1.1: nessuna discesa ricorsiva);
//   * `openable` = sottoinsieme dei nomi di primo livello che l'opener apre con
//     successo (le ValidPulse), con il modId dichiarato nel container.
struct FakeFs {
    std::vector<std::string> topLevel;            // voci di primo livello
    std::map<std::string, std::string> openable;  // entryName -> modId

    pulse::lifecycle::DirectoryLister lister() const {
        return [this](const std::filesystem::path&) {
            DirectoryListing listing;
            listing.status = DirectoryReadStatus::Ok;
            listing.entryNames = topLevel;  // SOLO primo livello
            return listing;
        };
    }

    pulse::lifecycle::PackageOpener opener() const {
        return [this](const std::filesystem::path&,
                      std::string_view entryName) -> OpenResult {
            auto it = openable.find(std::string(entryName));
            if (it == openable.end()) {
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

EntryKind genKind() {
    const int k = *rc::gen::inRange(0, 4);
    switch (k) {
        case 0: return EntryKind::ValidPulse;
        case 1: return EntryKind::UnopenablePulse;
        case 2: return EntryKind::NonPulseFile;
        default: return EntryKind::NestedDir;
    }
}

// --- Property 4 — discovery solo primo livello + isolamento voci invalide ---
// Feature: external-mod-loading, Property 4.
// Validates: Requirements 1.1, 1.5.
RC_GTEST_PROP(EmlProperty4DiscoveryFirstLevelIsolation,
              CandidatesAreExactlyRecognizableTopLevelEntries,
              ()) {
    const int count = *rc::gen::inRange(0, 13);

    FakeFs fs;
    std::set<std::string> expectedValid;          // entryNames da scoprire
    std::set<std::string> expectedUnrecognized;   // entryNames con diagnostica
    std::set<std::string> nestedNames;            // mai scoperte (Req 1.1)

    for (int i = 0; i < count; ++i) {
        const EntryKind kind = genKind();
        const std::string idx = std::to_string(i);
        switch (kind) {
            case EntryKind::ValidPulse: {
                const std::string name = "v" + idx + ".pulse";
                fs.topLevel.push_back(name);
                fs.openable.emplace(name, "mod." + name);
                expectedValid.insert(name);
                break;
            }
            case EntryKind::UnopenablePulse: {
                // `.pulse` di primo livello che l'opener NON apre (Req 1.2/1.5).
                const std::string name = "u" + idx + ".pulse";
                fs.topLevel.push_back(name);
                expectedUnrecognized.insert(name);
                break;
            }
            case EntryKind::NonPulseFile: {
                // Voce non `.pulse` di primo livello (Req 1.5).
                const std::string name = "f" + idx + ".txt";
                fs.topLevel.push_back(name);
                expectedUnrecognized.insert(name);
                break;
            }
            case EntryKind::NestedDir: {
                // Sottocartella di primo livello: non `.pulse` → ignorata con
                // diagnostica (Req 1.5). Le voci `.pulse` nidificate al suo
                // interno NON sono enumerate dal lister (Req 1.1): non devono
                // mai essere scoperte.
                const std::string dir = "d" + idx;
                fs.topLevel.push_back(dir);
                expectedUnrecognized.insert(dir);
                const int nested = *rc::gen::inRange(0, 3);
                for (int j = 0; j < nested; ++j) {
                    // Nome del nidificato: NON aggiunto a `topLevel`, quindi mai
                    // enumerato. Aggiunto a `openable` per dimostrare che, anche
                    // se fosse erroneamente tentata l'apertura, resterebbe fuori
                    // dai candidati perché non è di primo livello.
                    const std::string nestedName =
                        dir + "/n" + std::to_string(j) + ".pulse";
                    fs.openable.emplace(nestedName, "mod.nested." + nestedName);
                    nestedNames.insert(nestedName);
                }
                break;
            }
        }
    }

    CapturingSink sink;
    DiscoveredModSet set =
        discover_mods("/mods", fs.lister(), fs.opener(), sink.sink());

    // (1) I candidati sono ESATTAMENTE le voci ValidPulse di primo livello
    //     (Req 1.1 + 1.2): stesso insieme di entryName, nessuna in più, nessuna
    //     in meno.
    std::set<std::string> discoveredNames;
    for (const auto& dm : set) discoveredNames.insert(dm.entryName);
    RC_ASSERT(discoveredNames == expectedValid);
    RC_ASSERT(set.size() == expectedValid.size());

    // (2) Le valide restanti sono comunque scoperte, con il modId del loro
    //     container e con suffisso `.pulse`.
    for (const auto& dm : set) {
        RC_ASSERT(expectedValid.count(dm.entryName) == 1);
        RC_ASSERT(dm.modId == "mod." + dm.entryName);
        RC_ASSERT(dm.entryName.size() > 6 &&
                  dm.entryName.substr(dm.entryName.size() - 6) == ".pulse");
    }

    // (3) Determinismo: i candidati sono ordinati per `entryName` lessicografico.
    for (std::size_t i = 1; i < set.size(); ++i)
        RC_ASSERT(set[i - 1].entryName < set[i].entryName);

    // (4) Ogni voce di primo livello NON riconoscibile è ignorata con una
    //     diagnostica che la nomina (Req 1.5).
    for (const std::string& name : expectedUnrecognized)
        RC_ASSERT(sink.anyContains(name));

    // (5) Solo primo livello (Req 1.1): nessuna voce `.pulse` nidificata
    //     compare tra i candidati.
    for (const std::string& nested : nestedNames)
        RC_ASSERT(discoveredNames.count(nested) == 0);
}

}  // namespace
