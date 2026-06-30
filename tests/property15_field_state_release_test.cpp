// tests/property15_field_state_release_test.cpp
// Feature: pulse-sdk, Property 15 — Rilascio dello stato iniettato alla
// distruzione.
// Validates: Requirements 6.4 (Requisito 6.4)
//
// Property 15 (design.md / Req 6.4): per ogni insieme di istanze, la
// distruzione di un sottoinsieme di esse rilascia TUTTO lo stato iniettato
// associato a quelle istanze SENZA alterare lo stato iniettato delle istanze
// non distrutte.
//
// Modello del test (RapidCheck, ≥100 iterazioni di default):
//   * si dichiara un insieme FISSO di campi `PulseField<T, Key, P15Entity>`
//     della STESSA classe `P15Entity`, ciascuno con chiave esplicita univoca e
//     tipo distinto (int, std::string, bool, std::int64_t) — Req 6.1;
//   * si genera una popolazione randomizzata di istanze (indirizzi distinti e
//     stabili via std::unique_ptr) e, per ciascuna, un valore randomizzato per
//     OGNI campo, memorizzato anche in una struttura "oracolo" di confronto;
//   * si sceglie un sottoinsieme randomizzato di istanze da "distruggere"
//     chiamando `pulse::fields::releaseInstance<P15Entity>(x)`;
//   * si verifica che:
//       (a) ogni istanza distrutta abbia TUTTI i campi riportati al default
//           tipizzato e `has()==false` su ogni chiave (Req 6.4 + 6.3);
//       (b) `releaseInstance` riporti il numero di campi effettivamente
//           rilasciati (tutti i campi, dato che ogni istanza era stata scritta
//           su ogni chiave);
//       (c) OGNI istanza sopravvissuta conservi ESATTAMENTE i propri valori su
//           tutte le chiavi, con `has()==true` (Req 6.2/6.4).
//
// Isolamento dello storage statico: i campi usano una classe DEDICATA
// `P15Entity` (registro/cleaner per-classe propri) e si esegue `clearAll()` su
// ogni campo all'inizio di ogni iterazione per ripartire da uno stato pulito.
// NB: NON si invoca `FieldRegistry::reset()`, che azzererebbe anche i cleaner
// type-erased (registrati una sola volta per istanziazione tramite guardia
// statica) e renderebbe `releaseInstance` un no-op.

#include <pulse/fields.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

namespace {

// Classe del gioco fittizia DEDICATA a questa property: garantisce un registro
// chiavi e un insieme di cleaner isolati dagli altri test sui campi.
struct P15Entity {
    int dummy{0};
};

// --- Property 15 — rilascio dello stato alla distruzione ------------------
// Feature: pulse-sdk, Property 15. Validates: Requirements 6.4.
RC_GTEST_PROP(Property15FieldStateRelease,
              ReleaseClearsDestroyedInstancesAndPreservesSurvivors,
              ()) {
    // Insieme FISSO di campi della stessa classe, su chiavi/tipi distinti.
    pulse::PulseField<int, "p15/hp", P15Entity> hp;
    pulse::PulseField<std::string, "p15/name", P15Entity> name;
    pulse::PulseField<bool, "p15/alive", P15Entity> alive;
    pulse::PulseField<std::int64_t, "p15/score", P15Entity> score;

    // Isola lo storage statico per questa iterazione (NON resettare il
    // registro: i cleaner sono registrati una sola volta a livello di programma).
    hp.clearAll();
    name.clearAll();
    alive.clearAll();
    score.clearAll();

    // Numero di campi distinti dichiarati: ogni istanza scritta su tutte le
    // chiavi rilascerà esattamente questo numero di campi alla distruzione.
    constexpr std::size_t kFieldCount = 4;

    // Popolazione randomizzata di istanze (indirizzi distinti e stabili).
    const auto count =
        *rc::gen::inRange<std::size_t>(1, 41).as("numero di istanze");

    std::vector<std::unique_ptr<P15Entity>> instances;
    instances.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        instances.push_back(std::make_unique<P15Entity>());
    }

    // Valori randomizzati per ciascun campo, uno per istanza (oracolo).
    const auto hpVals =
        *rc::gen::container<std::vector<int>>(count, rc::gen::arbitrary<int>())
             .as("valori hp");
    const auto nameVals = *rc::gen::container<std::vector<std::string>>(
                               count, rc::gen::arbitrary<std::string>())
                              .as("valori name");
    const auto aliveVals = *rc::gen::container<std::vector<bool>>(
                               count, rc::gen::arbitrary<bool>())
                              .as("valori alive");
    const auto scoreVals = *rc::gen::container<std::vector<std::int64_t>>(
                               count, rc::gen::arbitrary<std::int64_t>())
                              .as("valori score");

    // Sottoinsieme randomizzato di istanze da "distruggere".
    const auto destroy = *rc::gen::container<std::vector<bool>>(
                             count, rc::gen::arbitrary<bool>())
                            .as("istanze da distruggere");

    // Scrive ogni campo di ogni istanza (Req 6.1/6.2).
    for (std::size_t i = 0; i < count; ++i) {
        P15Entity* inst = instances[i].get();
        hp.set(inst, hpVals[i]);
        name.set(inst, nameVals[i]);
        alive.set(inst, aliveVals[i]);
        score.set(inst, scoreVals[i]);
    }

    // Distrugge il sottoinsieme scelto in un'unica operazione per istanza.
    for (std::size_t i = 0; i < count; ++i) {
        if (destroy[i]) {
            const std::size_t released =
                pulse::fields::releaseInstance<P15Entity>(instances[i].get());
            // (b) Rilascia TUTTI i campi scritti per quell'istanza (Req 6.4).
            RC_ASSERT(released == kFieldCount);
        }
    }

    // Verifica gli invarianti su tutte le istanze.
    for (std::size_t i = 0; i < count; ++i) {
        const P15Entity* inst = instances[i].get();
        if (destroy[i]) {
            // (a) Stato completamente rilasciato: has()==false su ogni chiave e
            // letture al default tipizzato (Req 6.4 + 6.3).
            RC_ASSERT(!hp.has(inst));
            RC_ASSERT(!name.has(inst));
            RC_ASSERT(!alive.has(inst));
            RC_ASSERT(!score.has(inst));

            RC_ASSERT(hp.get(inst) == 0);
            RC_ASSERT(name.get(inst) == std::string{});
            RC_ASSERT(alive.get(inst) == false);
            RC_ASSERT(score.get(inst) == std::int64_t{0});
        } else {
            // (c) Istanza sopravvissuta: conserva ESATTAMENTE i suoi valori su
            // ogni chiave (Req 6.2/6.4).
            RC_ASSERT(hp.has(inst));
            RC_ASSERT(name.has(inst));
            RC_ASSERT(alive.has(inst));
            RC_ASSERT(score.has(inst));

            RC_ASSERT(hp.get(inst) == hpVals[i]);
            RC_ASSERT(name.get(inst) == nameVals[i]);
            RC_ASSERT(alive.get(inst) == aliveVals[i]);
            RC_ASSERT(score.get(inst) == scoreVals[i]);
        }
    }
}

}  // namespace
