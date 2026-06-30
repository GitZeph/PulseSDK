// tests/property24_storage_read_after_write_test.cpp
// Feature: pulse-sdk, Property 24 — Round-trip read-after-write dello storage.
// Validates: Requirements 10.2 (Requisito 10.2)
//
// Property 24 (design.md / Req 10.2): per ogni sequenza di scritture su uno
// spazio di archiviazione e per ogni chiave, una lettura successiva della
// chiave restituisce il valore scritto PIÙ DI RECENTE per quella chiave.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si genera una sequenza randomizzata di operazioni `set`, ciascuna con
//     una chiave estratta da un piccolo pool fisso (così le sovrascritture
//     della stessa chiave sono frequenti) e un valore binary-safe arbitrario
//     che può contenere byte NUL incorporati;
//   * ogni `set` è applicato sia a `ModStorage` (costruito con una capacità
//     molto ampia affinché le scritture abbiano sempre successo) sia a un
//     modello di riferimento `std::map<string,string>`;
//   * dopo OGNI operazione si verifica, per OGNI chiave del pool, che
//     `storage.get(key)` coincida con il valore del modello (oppure
//     `std::nullopt` se la chiave non è mai stata scritta).
//
// Il pool di chiavi è piccolo e include una chiave con byte NUL incorporato
// per esercitare anche la binary-safety lato chiave; i valori coprono il caso
// vuoto, NUL incorporati e byte arbitrari.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "pulse/storage.hpp"

namespace {

using pulse::storage::ModStorage;
using pulse::storage::Value;

// Pool fisso di chiavi binary-safe (l'ultima contiene un byte NUL incorporato).
const std::vector<std::string>& keyPool() {
    static const std::vector<std::string> pool = {
        std::string("k0"),
        std::string("k1"),
        std::string("k2"),
        std::string(std::string("k\0nul", 5)),  // NUL incorporato nella chiave.
    };
    return pool;
}

// Capacità molto ampia: garantisce che ogni `set` abbia successo (Req 10.2 è
// definito sulle scritture valide accettate; il rifiuto per capacità superata
// è coperto dalla Property 26).
constexpr std::size_t kLargeCapacity = std::size_t{1} << 30;  // 1 GiB.

// Un'operazione di scrittura: indice di chiave nel pool + valore da scrivere.
struct SetOp {
    std::size_t keyIndex;
    Value value;
};

// --- Property 24 — read-after-write restituisce sempre l'ultimo scritto ----
// Feature: pulse-sdk, Property 24. Validates: Requirements 10.2.
RC_GTEST_PROP(Property24StorageReadAfterWrite,
              GetReturnsMostRecentlyWrittenValue,
              ()) {
    const auto& keys = keyPool();

    // Sequenza randomizzata di operazioni set: chiave dal pool + valore
    // binary-safe arbitrario (può essere vuoto o contenere NUL incorporati).
    const auto ops = *rc::gen::container<std::vector<SetOp>>(
        rc::gen::construct<SetOp>(
            rc::gen::inRange<std::size_t>(0, keys.size()),
            rc::gen::arbitrary<std::string>()))
                          .as("sequenza di set");

    ModStorage storage("mod.test.property24", kLargeCapacity);
    std::map<std::string, std::string> model;  // riferimento.

    for (const SetOp& op : ops) {
        const std::string& key = keys[op.keyIndex];

        // Applica la scrittura a entrambi: con capacità ampia deve riuscire.
        const auto result = storage.set(key, op.value);
        RC_ASSERT(result.isOk());
        model[key] = op.value;

        // Invariante read-after-write: per OGNI chiave del pool, get() deve
        // riflettere l'ultimo valore scritto (o nullopt se mai scritta).
        for (const std::string& k : keys) {
            const std::optional<Value> got = storage.get(k);
            const auto it = model.find(k);
            if (it == model.end()) {
                RC_ASSERT(!got.has_value());
            } else {
                RC_ASSERT(got.has_value());
                RC_ASSERT(*got == it->second);
            }
        }
    }
}

}  // namespace
