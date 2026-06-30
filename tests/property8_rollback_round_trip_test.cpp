// tests/property8_rollback_round_trip_test.cpp
// Feature: pulse-sdk, Property 8 — Round-trip del rollback degli hook.
// Validates: Requirements 18.1, 18.4 (Requisiti 18.1, 18.4)
//
// Property 8 (design.md / Req 18.1, 18.4): per insiemi randomizzati di
// RollbackRecord (owner/symbol/address/originalBytes — inclusi byte arbitrari
// 0x00..0xFF — version e platformId), valgono due round-trip:
//
//   (1) Round-trip di PERSISTENZA (Req 18.1): serializzare e poi
//       deserializzare i record (e, in parallelo, persistere su un file
//       temporaneo e ricaricarlo) restituisce record identici agli originali.
//       Modella il fatto che le informazioni di rollback (indirizzo + byte
//       originali sovrascritti) sopravvivono al riavvio del gioco.
//
//   (2) Round-trip di RIPRISTINO (Req 18.4): dopo che un FakeBackend semina i
//       byte originali e `install()` patcha i byte "live" (live != original),
//       ripristinare tramite il RollbackStore (`restoreAll` con una write
//       callback che riscrive nel backend) riporta i byte live ESATTAMENTE
//       uguali ai byte originali catturati al momento dell'installazione, per
//       ogni funzione bersaglio.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * la proprietà (1) genera vettori di record con campi completamente
//     arbitrari (originalBytes copre l'intero dominio 0x00..0xFF, incluso il
//     byte nullo) e verifica l'uguaglianza byte-perfetta dopo
//     serialize/deserialize e dopo persist→load da un file temporaneo;
//   * la proprietà (2) genera vettori di record con byte originali non vuoti e
//     indirizzi DISTINTI (uno per indice), semina il FakeBackend, persiste i
//     record PRIMA di install (write-through, Req 18.1), installa (live !=
//     original), ricarica lo store dal disco ("riavvio") e infine ripristina:
//     ogni bersaglio torna ai byte originali.
//
// I file temporanei sono creati in temp_directory_path con nome univoco e
// rimossi al termine di ogni iterazione (nessun residuo su disco).

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "hooking/fake_backend.hpp"
#include "hooking/rollback_store.hpp"

namespace {

using pulse::hooking::FakeBackend;
using pulse::hooking::RollbackRecord;
using pulse::hooking::RollbackStore;
using GdVersion = pulse::loader::bindings::GdVersion;

int g_detour = 0;
void* const kDetour = &g_detour;

// Indirizzo bersaglio univoco e non nullo derivato dall'indice, così ogni
// record agisce su una funzione distinta (regioni separate nel FakeBackend).
std::uintptr_t targetForIndex(std::size_t index) noexcept {
    return static_cast<std::uintptr_t>(0x10000 + (index + 1) * 0x1000);
}

// RAII su un percorso di file temporaneo univoco per lo store di rollback.
// Rimuove sia il file che l'eventuale temporaneo di scrittura atomica (.tmp).
struct TempStoreFile {
    std::filesystem::path path;

    TempStoreFile() {
        static std::atomic<std::uint64_t> counter{0};
        const auto n = counter.fetch_add(1, std::memory_order_relaxed);
        path = std::filesystem::temp_directory_path() /
               ("pulse_p8_rollback_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "_" +
                std::to_string(n) + ".pbk");
        remove();
    }

    ~TempStoreFile() { remove(); }

    void remove() const {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::path tmp = path;
        tmp += ".tmp";
        std::filesystem::remove(tmp, ec);
    }
};

// Descrittore randomizzato di un record per la proprietà di persistenza.
// Campi completamente arbitrari (address può essere 0, byte coprono 0x00..0xFF).
struct RecordSpec {
    std::string owner;
    std::string symbol;
    std::uint64_t address;
    std::vector<std::uint8_t> originalBytes;
    int versionMajor;
    int versionMinor;
    std::string platformId;
};

// Descrittore per la proprietà di ripristino: byte originali NON vuoti (così
// install produce byte live diversi dagli originali); l'indirizzo è assegnato
// per indice per garantirne l'unicità.
struct RestoreSpec {
    std::string owner;
    std::string symbol;
    std::vector<std::uint8_t> originalBytes;
    int versionMajor;
    int versionMinor;
    std::string platformId;
};

RollbackRecord toRecord(const RecordSpec& s) {
    RollbackRecord r;
    r.owner = s.owner;
    r.symbol = s.symbol;
    r.address = static_cast<std::uintptr_t>(s.address);
    r.originalBytes = s.originalBytes;
    r.version = GdVersion{s.versionMajor, s.versionMinor};
    r.platformId = s.platformId;
    return r;
}

// --- Property 8 (1) — round-trip di persistenza -----------------------------
// Feature: pulse-sdk, Property 8. Validates: Requirements 18.1, 18.4.
//
// serialize→deserialize e persist→load restituiscono record identici agli
// originali, byte-perfetti anche per originalBytes arbitrari (incluso 0x00).
RC_GTEST_PROP(Property8RollbackRoundTrip,
              PersistenceRoundTripPreservesAllRecords,
              ()) {
    const auto specs = *rc::gen::container<std::vector<RecordSpec>>(
        rc::gen::construct<RecordSpec>(
            rc::gen::arbitrary<std::string>(),
            rc::gen::arbitrary<std::string>(),
            rc::gen::arbitrary<std::uint64_t>(),
            rc::gen::arbitrary<std::vector<std::uint8_t>>(),
            rc::gen::arbitrary<int>(),
            rc::gen::arbitrary<int>(),
            rc::gen::arbitrary<std::string>()))
                           .as("record di rollback");

    TempStoreFile tmp;
    RollbackStore store{tmp.path};
    for (const auto& s : specs) {
        // add() persiste in write-through su disco (Req 18.1).
        RC_ASSERT(static_cast<bool>(store.add(toRecord(s))));
    }

    // (a) round-trip in-memory: serialize -> deserialize.
    const std::vector<std::uint8_t> bytes = store.serialize();
    std::vector<RollbackRecord> roundTripped;
    const auto deser = RollbackStore::deserialize(bytes, roundTripped);
    RC_ASSERT(static_cast<bool>(deser));
    RC_ASSERT(roundTripped == store.records());

    // (b) round-trip su disco: persist (via add) -> load da una nuova istanza
    // (modella il riavvio del gioco: i record sopravvivono).
    RollbackStore reloaded{tmp.path};
    const auto loaded = RollbackStore::load(tmp.path, reloaded);
    RC_ASSERT(static_cast<bool>(loaded));
    RC_ASSERT(reloaded.records() == store.records());
}

// --- Property 8 (2) — round-trip di ripristino ------------------------------
// Feature: pulse-sdk, Property 8. Validates: Requirements 18.1, 18.4.
//
// Dopo seed+install (live != original), ricaricare lo store dal disco e
// ripristinare riporta i byte live esattamente agli originali per ogni target.
RC_GTEST_PROP(Property8RollbackRoundTrip,
              RestoreRoundTripRewritesOriginalBytesForEveryTarget,
              ()) {
    const auto specs = *rc::gen::container<std::vector<RestoreSpec>>(
        rc::gen::construct<RestoreSpec>(
            rc::gen::arbitrary<std::string>(),
            rc::gen::arbitrary<std::string>(),
            // Byte originali non vuoti: garantisce live != original dopo install
            // (lo stub di detour del FakeBackend inverte ogni byte).
            rc::gen::nonEmpty<std::vector<std::uint8_t>>(),
            rc::gen::arbitrary<int>(),
            rc::gen::arbitrary<int>(),
            rc::gen::arbitrary<std::string>()))
                           .as("record di rollback");

    TempStoreFile tmp;
    FakeBackend backend;
    RollbackStore store{tmp.path};

    std::vector<std::uintptr_t> targets;
    targets.reserve(specs.size());
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const std::uintptr_t target = targetForIndex(i);
        targets.push_back(target);

        // Il backend semina i byte originali del bersaglio.
        backend.seedOriginal(target, specs[i].originalBytes);

        // Lo store conserva i byte originali PRIMA di install (Req 18.1).
        RollbackRecord r;
        r.owner = specs[i].owner;
        r.symbol = specs[i].symbol;
        r.address = target;
        r.originalBytes = specs[i].originalBytes;
        r.version = GdVersion{specs[i].versionMajor, specs[i].versionMinor};
        r.platformId = specs[i].platformId;
        RC_ASSERT(static_cast<bool>(store.add(r)));
    }

    // install patcha i byte live: devono divergere dagli originali.
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const auto inst = backend.install(targets[i], kDetour);
        RC_ASSERT(inst.has_value());
        const auto live = backend.liveBytes(targets[i]);
        RC_ASSERT(live.has_value());
        RC_ASSERT(live.value() != specs[i].originalBytes);
    }

    // Ricarica lo store dal disco (modella il "riavvio") e ripristina (Req 18.4).
    RollbackStore reloaded{tmp.path};
    RC_ASSERT(static_cast<bool>(RollbackStore::load(tmp.path, reloaded)));

    // La write callback riscrive i byte originali nel backend.
    const auto writeFn = [&](std::uintptr_t addr,
                             const std::vector<std::uint8_t>& original) {
        backend.seedOriginal(addr, original);
        return true;
    };
    const auto outcome = reloaded.restoreAll(writeFn);
    RC_ASSERT(outcome.ok());
    RC_ASSERT(outcome.restored == specs.size());

    // Ogni bersaglio è tornato esattamente ai byte originali.
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const auto live = backend.liveBytes(targets[i]);
        RC_ASSERT(live.has_value());
        RC_ASSERT(live.value() == specs[i].originalBytes);
    }
}

}  // namespace
