// tests/property26_storage_capacity_rejection_test.cpp
// Feature: pulse-sdk, Property 26 — Rifiuto su capacità superata preservando i dati.
// Validates: Requirements 10.5 (Requisito 10.5)
//
// Property 26 (design.md / Req 10.5): per ogni sequenza randomizzata di
// operazioni `set` su uno spazio di archiviazione con capacità PICCOLA, ogni
// scrittura la cui occupazione PROIETTATA supererebbe la capacità è RIFIUTATA
// con `StorageErrorCode::CapacityExceeded` e lascia INVARIATI tutti i dati già
// presenti (e `usedBytes()` immutato), mentre le scritture che rientrano nella
// capacità hanno successo e aggiornano i dati.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si inietta una capacità PICCOLA (32..128 byte) così che, con chiavi e
//     valori corti, sia il ramo "accettazione" sia il ramo "rifiuto" siano
//     entrambi esercitati frequentemente;
//   * si genera una sequenza di `set` con chiavi da un piccolo pool fisso
//     (sovrascritture frequenti) e valori binary-safe corti (possono contenere
//     byte NUL incorporati ed essere vuoti);
//   * si mantiene un MODELLO di riferimento (`std::map<string,string>` + un
//     contatore di byte occupati) che rispecchia il modello di costo
//     documentato: costo voce = key.size() + value.size(); occupazione
//     proiettata = used - previousEntryCost + newEntryCost;
//   * dopo OGNI `set` si verifica:
//       - result.isOk() == (projected <= capacity);
//       - in caso di rifiuto, i contenuti dello storage (per OGNI chiave del
//         pool) e `usedBytes()` sono ESATTAMENTE quelli precedenti all'op;
//       - in caso di successo, i contenuti e `usedBytes()` coincidono col
//         modello aggiornato; inoltre il codice di errore del rifiuto è
//         `CapacityExceeded`.

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
using pulse::storage::StorageErrorCode;
using pulse::storage::Value;

// Pool fisso di chiavi binary-safe e corte (l'ultima contiene un byte NUL
// incorporato). Le chiavi corte mantengono basso il costo così che la capacità
// piccola consenta sia accettazioni sia rifiuti.
const std::vector<std::string>& keyPool() {
    static const std::vector<std::string> pool = {
        std::string("a"),
        std::string("bb"),
        std::string("ccc"),
        std::string(std::string("d\0x", 3)),  // NUL incorporato nella chiave.
    };
    return pool;
}

// Costo documentato di una voce: chiave + valore.
std::size_t entryCost(const std::string& key, const std::string& value) {
    return key.size() + value.size();
}

// Un'operazione di scrittura: indice di chiave nel pool + valore da scrivere.
struct SetOp {
    std::size_t keyIndex;
    Value value;
};

// --- Property 26 — il rifiuto per capacità superata preserva i dati ---------
// Feature: pulse-sdk, Property 26. Validates: Requirements 10.5.
RC_GTEST_PROP(Property26StorageCapacityRejection,
              RejectionOnCapacityExceededPreservesData,
              ()) {
    const auto& keys = keyPool();

    // Capacità PICCOLA e iniettabile: garantisce che alcune scritture entrino e
    // altre vengano rifiutate (entrambi i rami esercitati).
    const std::size_t capacity =
        *rc::gen::inRange<std::size_t>(32, 129).as("capacità (byte)");

    // Sequenza randomizzata di set: chiave dal pool + valore binary-safe CORTO
    // (0..16 byte, può essere vuoto o contenere NUL incorporati).
    const auto ops = *rc::gen::container<std::vector<SetOp>>(
        rc::gen::construct<SetOp>(
            rc::gen::inRange<std::size_t>(0, keys.size()),
            rc::gen::resize(16, rc::gen::arbitrary<std::string>())))
                          .as("sequenza di set");

    ModStorage storage("mod.test.property26", capacity);

    // Modello di riferimento + contabilità dei byte occupati.
    std::map<std::string, std::string> model;
    std::size_t modelUsed = 0;

    for (const SetOp& op : ops) {
        const std::string& key = keys[op.keyIndex];

        // Costo della voce precedente per questa chiave (0 se assente).
        std::size_t previousCost = 0;
        const auto prev = model.find(key);
        if (prev != model.end()) {
            previousCost = entryCost(key, prev->second);
        }
        const std::size_t newCost = entryCost(key, op.value);
        const std::size_t projected = modelUsed - previousCost + newCost;
        const bool shouldFit = projected <= capacity;

        // Snapshot dello stato osservabile PRIMA dell'operazione, per poter
        // verificare la preservazione dei dati in caso di rifiuto.
        const std::size_t usedBefore = storage.usedBytes();
        std::vector<std::optional<Value>> snapshot;
        snapshot.reserve(keys.size());
        for (const std::string& k : keys) {
            snapshot.push_back(storage.get(k));
        }

        const auto result = storage.set(key, op.value);

        // (1) L'esito deve coincidere col predicato di capacità del modello.
        RC_ASSERT(result.isOk() == shouldFit);

        if (!result.isOk()) {
            // (2) Rifiuto: codice di errore corretto.
            RC_ASSERT(result.error().code ==
                      StorageErrorCode::CapacityExceeded);

            // (3) Rifiuto: dati e usedBytes ESATTAMENTE come prima dell'op.
            RC_ASSERT(storage.usedBytes() == usedBefore);
            for (std::size_t i = 0; i < keys.size(); ++i) {
                RC_ASSERT(storage.get(keys[i]) == snapshot[i]);
            }
            // Il modello non viene aggiornato: l'op è stata rifiutata.
        } else {
            // (4) Successo: aggiorna il modello e verifica la coincidenza.
            model[key] = op.value;
            modelUsed = projected;

            RC_ASSERT(storage.usedBytes() == modelUsed);
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
}

}  // namespace
