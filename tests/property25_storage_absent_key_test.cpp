// tests/property25_storage_absent_key_test.cpp
// Feature: pulse-sdk, Property 25 — Chiave inesistente restituisce valore
// assente.
// Validates: Requirements 10.4 (Requisito 10.4)
//
// Property 25 (design.md / Req 10.4): per insiemi randomizzati di chiavi
// scritte e di chiavi interrogate, `ModStorage::get(key)` deve restituire
// `std::nullopt` (valore ASSENTE, senza errore bloccante) per ogni chiave mai
// scritta, e un valore PRESENTE per ogni chiave scritta. L'invariante
// osservabile è:
//
//     get(queryKey).has_value() == (queryKey è stata scritta)
//
// e `contains(queryKey)` deve concordare con `get(queryKey).has_value()`.
// L'interrogazione di una chiave assente non deve mai lanciare né bloccare.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si genera un insieme randomizzato di scritture (key, value) con chiavi
//     potenzialmente ripetute (l'ultima scrittura vince) — questo definisce
//     l'insieme delle chiavi PRESENTI;
//   * si genera un insieme randomizzato di chiavi di interrogazione che mescola
//     chiavi scritte (overlap) e chiavi arbitrarie (alcune mai scritte);
//   * per ogni chiave interrogata si verifica che
//       get(q).has_value() == writtenKeys.count(q) > 0
//     e che contains(q) concordi, senza eccezioni.
//   * un oracolo `std::set` delle chiavi scritte funge da modello di
//     riferimento per "presente vs assente".

#include <pulse/storage.hpp>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using pulse::storage::ModStorage;
using pulse::storage::Value;

// --- Property 25 — chiave inesistente restituisce valore assente -----------
// Feature: pulse-sdk, Property 25. Validates: Requirements 10.4.
//
// Capacità ampia (default ≥ 10 MB): le scritture non sono mai rifiutate, così
// l'insieme delle chiavi PRESENTI coincide esattamente con quelle scritte e
// l'invariante presente/assente è isolata dal rifiuto di capacità (Req 10.5,
// coperto separatamente dal property test 20.4).
RC_GTEST_PROP(Property25StorageAbsentKey,
              AbsentKeysReturnNulloptPresentKeysReturnValue,
              ()) {
    // Insieme randomizzato di scritture (key, value). Le chiavi possono
    // ripetersi: la scrittura più recente vince. Le chiavi vuote sono ammesse.
    const auto writes =
        *rc::gen::container<std::vector<std::pair<std::string, std::string>>>(
             rc::gen::pair(rc::gen::arbitrary<std::string>(),
                           rc::gen::arbitrary<std::string>()))
             .as("scritture (key,value)");

    ModStorage storage("mod.property25");

    // Oracolo: insieme delle chiavi effettivamente scritte (presenti).
    std::set<std::string> writtenKeys;
    for (const auto& [key, value] : writes) {
        RC_ASSERT(storage.set(key, Value(value)));  // capacità ampia: mai rifiutata
        writtenKeys.insert(key);
    }

    // Insieme randomizzato di chiavi di interrogazione: mescola chiavi
    // arbitrarie (alcune mai scritte) con chiavi sicuramente scritte (overlap).
    auto queryKeys =
        *rc::gen::container<std::vector<std::string>>(
             rc::gen::arbitrary<std::string>())
             .as("chiavi interrogate (arbitrarie)");

    // Aggiunge esplicitamente alcune chiavi scritte per garantire l'overlap.
    for (const auto& key : writtenKeys) {
        queryKeys.push_back(key);
    }

    for (const auto& q : queryKeys) {
        const bool expectedPresent = writtenKeys.count(q) > 0;

        // get() su chiave assente non lancia né blocca: restituisce nullopt.
        const std::optional<Value> read = storage.get(q);

        // Invariante centrale (Req 10.4): present <=> chiave scritta.
        RC_ASSERT(read.has_value() == expectedPresent);

        // contains() deve concordare con get().has_value().
        RC_ASSERT(storage.contains(q) == read.has_value());

        // Le chiavi assenti restituiscono SEMPRE std::nullopt.
        if (!expectedPresent) {
            RC_ASSERT(!read.has_value());
        }
    }
}

}  // namespace
