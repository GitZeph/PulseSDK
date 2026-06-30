// tests/property14_field_instance_isolation_test.cpp
// Feature: pulse-sdk, Property 14 — isolamento per-istanza dei campi iniettati.
// Validates: Requirements 6.1, 6.2, 6.3 (Requisiti 6.1, 6.2, 6.3)
//
// Property 14 (design.md / Req 6.1, 6.2, 6.3): per sequenze randomizzate di
// operazioni `set` distribuite su più istanze distinte di una classe del gioco
// e su una o più chiavi `PulseField`, deve valere che:
//   (6.2) `get(instance)` per un'istanza restituisce ESATTAMENTE l'ultimo
//         valore assegnato a QUELLA istanza per quella chiave;
//   (6.2) un'operazione `set` su un'istanza non altera MAI il valore di
//         un'altra istanza (né di un'altra chiave);
//   (6.3) un'istanza a cui non è stato assegnato alcun valore per una chiave
//         restituisce il DEFAULT tipizzato `T{}`.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si fissa un insieme di istanze (vettore di oggetti della classe del
//     gioco, indirizzi stabili) e un insieme di chiavi `PulseField<int,...>`;
//   * si genera una sequenza randomizzata di operazioni
//     `(fieldIndex, instanceIndex, value)`;
//   * ogni `set` è applicato sia al `PulseField` reale sia a un MODELLO di
//     riferimento (mappa `(chiave, istanza) -> valore atteso`);
//   * dopo OGNI operazione si verifica, per OGNI chiave e OGNI istanza, che
//     `get` coincida con il modello (valore esatto dell'ultima `set` su quella
//     istanza, oppure `int{}` == 0 se mai scritta). Verificare dopo ogni passo
//     rende esplicita l'invariante di isolamento (Req 6.2): la `set` appena
//     applicata a una sola istanza/chiave non deve aver perturbato le altre.
//
// Header-only: include il header pubblico dello SDK <pulse/fields.hpp> e
// l'header di integrazione RapidCheck+GoogleTest (extras/gtest).

#include <pulse/fields.hpp>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <array>
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace {

// Classe del gioco fittizia: scope dei campi iniettati. Gli oggetti vivono in
// un vettore con indirizzi stabili e fungono da istanze distinte (Req 6.2).
struct GameEntity {
    int dummy{0};
};

// Numero fisso di istanze distinte e di chiavi distinte usate dalla property.
constexpr std::size_t kInstanceCount = 8;
constexpr std::size_t kFieldCount = 3;

// Operazione randomizzata: assegna `value` alla chiave `fieldIndex` per
// l'istanza `instanceIndex`.
struct SetOp {
    int fieldIndex;     // [0, kFieldCount)
    int instanceIndex;  // [0, kInstanceCount)
    int value;
};

// --- Property 14 — isolamento per-istanza e default tipizzato --------------
// Feature: pulse-sdk, Property 14. Validates: Requirements 6.1, 6.2, 6.3.
RC_GTEST_PROP(Property14FieldInstanceIsolation,
              GetReturnsLastSetPerInstanceWithDefaultAndIsolation,
              ()) {
    // Tre chiavi distinte sulla STESSA classe (Req 6.1). int → default 0.
    pulse::PulseField<int, "p14/f0", GameEntity> f0;
    pulse::PulseField<int, "p14/f1", GameEntity> f1;
    pulse::PulseField<int, "p14/f2", GameEntity> f2;

    // Isola dallo storage statico condiviso con gli altri test/iterazioni.
    f0.clearAll();
    f1.clearAll();
    f2.clearAll();

    // Dispatcher per indice di campo (ogni PulseField è un tipo distinto).
    const auto doSet = [&](int field, const GameEntity* inst, int value) {
        switch (field) {
            case 0: f0.set(inst, value); break;
            case 1: f1.set(inst, value); break;
            default: f2.set(inst, value); break;
        }
    };
    const auto doGet = [&](int field, const GameEntity* inst) -> int {
        switch (field) {
            case 0: return f0.get(inst);
            case 1: return f1.get(inst);
            default: return f2.get(inst);
        }
    };

    // Insieme fisso di istanze distinte (indirizzi stabili nel vettore).
    std::vector<GameEntity> instances(kInstanceCount);

    // Modello di riferimento: presenza + valore atteso per (campo, istanza).
    std::array<std::unordered_map<std::size_t, int>, kFieldCount> model;

    // Sequenza randomizzata di operazioni set.
    const auto ops = *rc::gen::container<std::vector<SetOp>>(
        rc::gen::construct<SetOp>(
            rc::gen::inRange<int>(0, static_cast<int>(kFieldCount)),
            rc::gen::inRange<int>(0, static_cast<int>(kInstanceCount)),
            rc::gen::arbitrary<int>()))
                         .as("sequenza di set");

    for (const SetOp& op : ops) {
        const auto field = static_cast<std::size_t>(op.fieldIndex);
        const auto inst = static_cast<std::size_t>(op.instanceIndex);

        // Applica la set sia al campo reale sia al modello.
        doSet(op.fieldIndex, &instances[inst], op.value);
        model[field][inst] = op.value;

        // Dopo ogni set, verifica TUTTE le chiavi e TUTTE le istanze: la set
        // appena fatta non deve aver perturbato nessun'altra (Req 6.2) e i
        // valori devono combaciare esattamente con il modello; le istanze non
        // ancora scritte devono restituire il default tipizzato (Req 6.3).
        for (std::size_t fi = 0; fi < kFieldCount; ++fi) {
            for (std::size_t ii = 0; ii < kInstanceCount; ++ii) {
                const int got = doGet(static_cast<int>(fi), &instances[ii]);
                const auto it = model[fi].find(ii);
                if (it == model[fi].end()) {
                    RC_ASSERT(got == 0);  // default tipizzato int{} (Req 6.3)
                } else {
                    RC_ASSERT(got == it->second);  // ultimo valore (Req 6.2)
                }
            }
        }
    }
}

}  // namespace
