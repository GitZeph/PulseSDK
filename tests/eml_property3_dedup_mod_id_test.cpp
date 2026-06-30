// tests/eml_property3_dedup_mod_id_test.cpp
// Feature: external-mod-loading, Property 3 — Dedup deterministico e unicità
// del Mod_Id scoperto.
// Validates: Requirements 1.6, 1.7 (Requisiti 1.6, 1.7)
//
// Property 3 (design.md): per ogni insieme di candidati nella Mods_Directory
// con Mod_Id eventualmente ripetuti, l'insieme scoperto contiene ogni Mod_Id
// AL PIÙ UNA VOLTA (Req 1.6) e, in caso di collisione, mantiene il Mod_Package
// il cui `entryName` è il MINIMO LESSICOGRAFICO (Req 1.7), registrando gli
// altri come esclusi con diagnostica che nomina il Mod_Id duplicato e le voci
// coinvolte (Req 1.7). L'esito è DETERMINISTICO e indipendente dall'ordine
// d'ingresso.
//
// Strategia (RapidCheck, ≥100 iterazioni di default — qui forzate a ≥100):
//   * si genera un piccolo insieme di voci con `entryName` DISTINTI (modellano
//     i nomi di voce univoci di una directory reale) e `modId` estratti da un
//     pool RISTRETTO, così le collisioni di Mod_Id sono frequenti;
//   * l'oracolo è INDIPENDENTE da `dedup_by_mod_id`: raggruppa per `modId` e
//     calcola il vincitore come il minimo lessicografico di `entryName`, poi
//     ordina i vincitori per `entryName`;
//   * la determinatezza rispetto all'ordine d'ingresso è verificata applicando
//     la dedup a una PERMUTAZIONE casuale dello stesso insieme e confrontando
//     gli output campo per campo.
//
// Header del loader in loader/lifecycle/ (include relativo alla radice loader/);
// la logica è in mod_loader.cpp (compilata in pulse::loader).

#include "lifecycle/mod_loader.hpp"

#include <algorithm>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "lifecycle/manifest.hpp"
#include "package/pulse_package.hpp"

namespace {

using pulse::lifecycle::DiscoveredMod;
using pulse::lifecycle::DiscoveredModSet;
using pulse::lifecycle::dedup_by_mod_id;
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

// Costruisce un DiscoveredMod (container aperto) per (entryName, modId).
// Senza integrità: la dedup opera solo su entryName/modId.
DiscoveredMod makeDiscovered(const std::string& entryName,
                             const std::string& modId) {
    PackageArchive archive = makeArchive(modId);
    PulsePackage::Options opts;
    opts.verifyIntegrity = false;
    OpenResult res = PulsePackage::open(archive, opts);
    RC_ASSERT(res.ok);
    return DiscoveredMod{entryName, modId, std::move(*res.package)};
}

struct CapturingSink {
    std::vector<std::string> messages;
    pulse::loader::DiagnosticSink sink() {
        return [this](std::string_view m) { messages.emplace_back(m); };
    }
    bool anyContains(const std::string& needle) const {
        for (const std::string& m : messages)
            if (m.find(needle) != std::string::npos) return true;
        return false;
    }
};

// entryName distinto: stringa di 1..3 lettere da un alfabeto ristretto.
std::string genEntryName() {
    const int len = *rc::gen::inRange(1, 4);
    std::string s;
    for (int i = 0; i < len; ++i) {
        const char c = static_cast<char>('a' + *rc::gen::inRange(0, 6));
        s.push_back(c);
    }
    return s;
}

// modId da un pool ristretto per forzare collisioni frequenti.
std::string genModId() {
    static const std::vector<std::string> kPool{"id.a", "id.b", "id.c", "id.d"};
    const int idx = *rc::gen::inRange(0, static_cast<int>(kPool.size()));
    return kPool[static_cast<std::size_t>(idx)];
}

// Costruisce una DiscoveredModSet a partire da una lista di (entryName, modId)
// nell'ordine dato dalla permutazione `order`.
DiscoveredModSet buildSet(
    const std::vector<std::pair<std::string, std::string>>& pairs,
    const std::vector<std::size_t>& order) {
    DiscoveredModSet set;
    set.reserve(order.size());
    for (std::size_t i : order)
        set.push_back(makeDiscovered(pairs[i].first, pairs[i].second));
    return set;
}

// --- Property 3 — dedup deterministico e unicità del Mod_Id ----------------
// Feature: external-mod-loading, Property 3.
// Validates: Requirements 1.6, 1.7.
RC_GTEST_PROP(EmlProperty3DedupModId,
              EachModIdAtMostOnceWinnerIsLexicographicallySmallest,
              ()) {
    // --- Genera voci con entryName DISTINTI e modId dal pool ristretto -----
    const int n = *rc::gen::inRange(0, 11);
    std::set<std::string> usedEntryNames;
    std::vector<std::pair<std::string, std::string>> pairs;  // (entryName, modId)
    for (int i = 0; i < n; ++i) {
        std::string entryName;
        // Riprova finché l'entryName è univoco (nomi di voce di directory).
        int guard = 0;
        do {
            entryName = genEntryName();
        } while (usedEntryNames.count(entryName) && ++guard < 16);
        if (usedEntryNames.count(entryName)) continue;  // raro: salta il duplicato
        usedEntryNames.insert(entryName);
        pairs.emplace_back(entryName, genModId());
    }

    // --- Oracolo INDIPENDENTE: vincitore = min entryName per ogni modId -----
    std::map<std::string, std::string> winnerEntryByMod;       // modId -> min entryName
    std::map<std::string, std::vector<std::string>> entriesByMod;  // modId -> entryNames
    for (const auto& [entryName, modId] : pairs) {
        entriesByMod[modId].push_back(entryName);
        auto it = winnerEntryByMod.find(modId);
        if (it == winnerEntryByMod.end() || entryName < it->second)
            winnerEntryByMod[modId] = entryName;
    }
    // Vincitori attesi, ordinati per entryName (determinismo dell'output).
    std::vector<std::pair<std::string, std::string>> expected;  // (entryName, modId)
    for (const auto& [modId, entryName] : winnerEntryByMod)
        expected.emplace_back(entryName, modId);
    std::sort(expected.begin(), expected.end());

    // --- Esecuzione sotto test (ordine originale) --------------------------
    std::vector<std::size_t> identity(pairs.size());
    std::iota(identity.begin(), identity.end(), 0u);

    CapturingSink sink;
    DiscoveredModSet out = dedup_by_mod_id(buildSet(pairs, identity), sink.sink());

    // (1) Ogni Mod_Id compare AL PIÙ UNA VOLTA (Req 1.6).
    std::set<std::string> seen;
    for (const auto& m : out) {
        RC_ASSERT(seen.insert(m.modId).second);  // nessun duplicato
    }

    // (2) Numero di vincitori == numero di Mod_Id distinti.
    RC_ASSERT(out.size() == expected.size());

    // (3) Output ordinato per entryName (determinismo).
    for (std::size_t i = 1; i < out.size(); ++i)
        RC_ASSERT(out[i - 1].entryName <= out[i].entryName);

    // (4) Vincitore di ogni collisione = minimo lessicografico (Req 1.7) e
    //     corrispondenza esatta con l'oracolo (entryName + modId).
    for (std::size_t i = 0; i < out.size(); ++i) {
        RC_ASSERT(out[i].entryName == expected[i].first);
        RC_ASSERT(out[i].modId == expected[i].second);
        // Coerenza con l'oracolo per-mod.
        RC_ASSERT(out[i].entryName == winnerEntryByMod[out[i].modId]);
    }

    // (5) Diagnostica delle esclusioni: per ogni Mod_Id in collisione, le voci
    //     NON vincenti sono nominate insieme al Mod_Id duplicato (Req 1.7).
    for (const auto& [modId, names] : entriesByMod) {
        if (names.size() <= 1) continue;  // nessuna collisione
        RC_ASSERT(sink.anyContains(modId));
        const std::string& winner = winnerEntryByMod[modId];
        for (const std::string& name : names) {
            if (name == winner) continue;
            RC_ASSERT(sink.anyContains(name));  // voce esclusa nominata
        }
    }

    // (6) DETERMINISMO rispetto all'ordine d'ingresso: applicando la dedup a una
    //     permutazione casuale dello stesso insieme si ottiene lo stesso output.
    std::vector<std::size_t> perm(pairs.size());
    std::iota(perm.begin(), perm.end(), 0u);
    // Permutazione pilotata da chiavi casuali (stable_sort sugli indici).
    std::vector<int> keys;
    keys.reserve(pairs.size());
    for (std::size_t i = 0; i < pairs.size(); ++i)
        keys.push_back(*rc::gen::inRange(0, 1000));
    std::stable_sort(perm.begin(), perm.end(),
                     [&](std::size_t a, std::size_t b) { return keys[a] < keys[b]; });

    CapturingSink sink2;
    DiscoveredModSet out2 = dedup_by_mod_id(buildSet(pairs, perm), sink2.sink());

    RC_ASSERT(out2.size() == out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        RC_ASSERT(out2[i].entryName == out[i].entryName);
        RC_ASSERT(out2[i].modId == out[i].modId);
    }
}

}  // namespace
