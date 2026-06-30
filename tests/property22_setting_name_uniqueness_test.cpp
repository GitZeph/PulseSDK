// tests/property22_setting_name_uniqueness_test.cpp
// Feature: pulse-sdk, Property 22 — Unicità dei nomi delle impostazioni.
// Validates: Requirements 9.6 (Requisito 9.6)
//
// Property 22 (design.md / Req 9.6): per ogni insieme di dichiarazioni di
// impostazioni di una Mod, se due o più impostazioni hanno lo stesso nome la
// dichiarazione duplicata è rifiutata con una segnalazione che indica il nome
// in conflitto, e il registro conserva ESATTAMENTE la prima dichiarazione per
// quel nome.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si genera una SEQUENZA randomizzata di chiamate declare() i cui nomi sono
//     estratti da un PICCOLO insieme (pool) di nomi validi (lunghezza 1..64),
//     così le collisioni di nome si verificano spesso;
//   * ogni dichiarazione usa tipo + valore predefinito SEMPRE conformi (così
//     l'unica causa di rifiuto sotto test è la duplicazione, Req 9.6) e nomi
//     sempre nell'intervallo 1..64;
//   * un MODELLO di riferimento tiene traccia dell'insieme dei nomi già
//     accettati e della PRIMA dichiarazione vista per ciascun nome;
//   * dopo ogni declare si verifica che:
//       - ok == (nome non già dichiarato)               [il nome è valido e il
//         default conforme per costruzione]
//       - in caso di rifiuto, report contiene il nome in conflitto;
//       - declarations().size() == numero di dichiarazioni accettate;
//       - il registro conserva la PRIMA dichiarazione (find() invariato).
//
// Il componente sotto test è pulse::SettingsRegistry (header pubblico dello SDK
// <pulse/settings.hpp>).

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pulse/settings.hpp"

namespace {

using pulse::conforms;
using pulse::SettingDecl;
using pulse::SettingsRegistry;
using pulse::SettingType;
using pulse::SettingValue;

// Piccolo insieme di nomi VALIDI (lunghezza 1..64) da cui estrarre i nomi delle
// dichiarazioni: tenerlo piccolo garantisce collisioni frequenti, così la causa
// di rifiuto per duplicazione (Req 9.6) viene esercitata molte volte.
const std::vector<std::string>& namePool() {
    static const std::vector<std::string> pool = {
        "a", "bb", "volume", "speed", "enabled", "setting_one",
    };
    return pool;
}

// Etichette ammesse per le dichiarazioni di tipo Enum (default sempre incluso).
const std::vector<std::string>& enumLabels() {
    static const std::vector<std::string> labels = {"low", "mid", "high"};
    return labels;
}

// Descrittore di una singola chiamata declare() generata. I valori sono scelti
// in modo che il tipo e il valore predefinito siano SEMPRE conformi: l'unica
// causa di rifiuto possibile resta la duplicazione del nome.
struct DeclOp {
    std::size_t nameIndex;  // indice nel namePool()
    int kind;               // 0=Bool 1=Int 2=Float 3=String 4=Enum
    bool boolVal;
    std::int64_t intVal;
    double floatVal;
    std::string strVal;
    int enumPick;           // indice (modulo) nelle etichette enum
};

// Costruisce una SettingDecl valida e conforme a partire dal descrittore.
SettingDecl makeDecl(const DeclOp& op) {
    SettingDecl decl;
    decl.name = namePool()[op.nameIndex % namePool().size()];
    switch (op.kind % 5) {
        case 0:
            decl.type = SettingType::Bool;
            decl.defaultValue = SettingValue::Bool(op.boolVal);
            break;
        case 1:
            decl.type = SettingType::Int;
            decl.defaultValue = SettingValue::Int(op.intVal);
            break;
        case 2:
            decl.type = SettingType::Float;
            decl.defaultValue = SettingValue::Float(op.floatVal);
            break;
        case 3:
            decl.type = SettingType::String;
            decl.defaultValue = SettingValue::String(op.strVal);
            break;
        default: {
            decl.type = SettingType::Enum;
            decl.enumValues = enumLabels();
            const std::size_t pick =
                static_cast<std::size_t>(op.enumPick % enumLabels().size());
            decl.defaultValue = SettingValue::Enum(enumLabels()[pick]);
            break;
        }
    }
    return decl;
}

// Generatore di un descrittore di declare().
rc::Gen<DeclOp> genDeclOp() {
    return rc::gen::construct<DeclOp>(
        rc::gen::inRange<std::size_t>(0, namePool().size()),
        rc::gen::inRange(0, 5),
        rc::gen::arbitrary<bool>(),
        rc::gen::arbitrary<std::int64_t>(),
        rc::gen::arbitrary<double>(),
        rc::gen::arbitrary<std::string>(),
        rc::gen::inRange(0, 1000));
}

// --- Property 22 — unicità dei nomi delle impostazioni --------------------
// Feature: pulse-sdk, Property 22. Validates: Requirements 9.6.
RC_GTEST_PROP(Property22SettingNameUniqueness,
              DuplicateNamesRejectedFirstDeclarationKept,
              ()) {
    const auto ops =
        *rc::gen::container<std::vector<DeclOp>>(genDeclOp())
             .as("sequenza di declare()");

    SettingsRegistry registry;

    // Modello di riferimento: nomi già accettati + prima dichiarazione vista.
    std::unordered_set<std::string> declaredNames;
    std::unordered_map<std::string, SettingDecl> firstDecl;
    std::size_t accepted = 0;

    for (const auto& op : ops) {
        const SettingDecl decl = makeDecl(op);

        // Invarianti di costruzione: nome valido (1..64) e default conforme,
        // così l'unica causa di rifiuto sotto test è la duplicazione.
        RC_ASSERT(decl.name.size() >= pulse::kSettingNameMinLength);
        RC_ASSERT(decl.name.size() <= pulse::kSettingNameMaxLength);
        RC_ASSERT(conforms(decl, decl.defaultValue));

        const bool alreadyDeclared = declaredNames.count(decl.name) > 0;

        const pulse::DeclareResult result = registry.declare(decl);

        // (1) Esito: accettata se e solo se il nome NON era già stato dichiarato.
        RC_ASSERT(result.ok == !alreadyDeclared);

        if (result.ok) {
            declaredNames.insert(decl.name);
            firstDecl.emplace(decl.name, decl);
            ++accepted;
        } else {
            // (2) Rifiuto del duplicato: la segnalazione indica il nome in
            // conflitto (Req 9.6).
            RC_ASSERT(result.report.find(decl.name) != std::string::npos);
        }

        // (3) Il registro contiene esattamente le dichiarazioni accettate.
        RC_ASSERT(registry.declarations().size() == accepted);

        // (4) Il registro conserva ESATTAMENTE la PRIMA dichiarazione per il
        // nome (nessun overwrite da parte di un duplicato).
        const SettingDecl* kept = registry.find(decl.name);
        RC_ASSERT(kept != nullptr);
        RC_ASSERT(registry.contains(decl.name));
        const SettingDecl& first = firstDecl.at(decl.name);
        RC_ASSERT(kept->name == first.name);
        RC_ASSERT(kept->type == first.type);
        RC_ASSERT(kept->defaultValue == first.defaultValue);
    }
}

}  // namespace
