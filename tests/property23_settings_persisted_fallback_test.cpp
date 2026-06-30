// tests/property23_settings_persisted_fallback_test.cpp
// Feature: pulse-sdk, Property 23 — Fallback al default su impostazione
// persistita non valida.
// Validates: Requirements 9.4 (Requisito 9.4)
//
// Property 23 (design.md / Req 9.4): per ogni impostazione il cui valore
// persistito non è valido per il tipo dichiarato, la lettura
// (`SettingsRegistry::loadPersisted`) restituisce il valore di default
// dichiarato, la dichiarazione resta invariata e viene prodotta una
// segnalazione dell'incoerenza. Viceversa, quando il valore persistito è
// conforme al tipo dichiarato, la lettura restituisce esattamente quel valore
// senza ricadere sul default e senza produrre segnalazioni.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si genera una dichiarazione casuale (tipo casuale + default conforme):
//       - per Enum si genera un insieme non vuoto di etichette ammesse e il
//         default è una di esse;
//   * si genera un valore persistito che è SOMETIMES conforme e SOMETIMES non
//     conforme:
//       - variante con tipo sbagliato (un SettingType diverso da quello
//         dichiarato), oppure
//       - per Enum dello stesso tipo, un'etichetta NON presente in enumValues;
//   * l'oracolo è `pulse::conforms(decl, persisted)`:
//       - conforme  => value == persisted, !usedDefault, nessun report;
//       - non conforme => value == default dichiarato, usedDefault,
//         report presente che contiene il nome dell'impostazione;
//   * in entrambi i casi la dichiarazione nel registro resta invariata.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "pulse/settings.hpp"

namespace {

using pulse::conforms;
using pulse::LoadResult;
using pulse::SettingDecl;
using pulse::SettingsRegistry;
using pulse::SettingType;
using pulse::SettingValue;

// Tutti i tipi supportati (insieme chiuso, Req 9.1).
const std::vector<SettingType>& allTypes() {
    static const std::vector<SettingType> kTypes = {
        SettingType::Bool, SettingType::Int, SettingType::Float,
        SettingType::String, SettingType::Enum};
    return kTypes;
}

// Genera un'etichetta enum non vuota (le etichette ammesse non possono essere
// vuote per una dichiarazione enum valida, Req 9.1).
rc::Gen<std::string> genEnumLabel() {
    return rc::gen::map(
        rc::gen::arbitrary<std::string>(),
        [](std::string s) {
            if (s.empty()) {
                s = "label";
            }
            return s;
        });
}

// Genera un valore conforme per il tipo `type`. Per Enum, sceglie un'etichetta
// fra quelle ammesse.
rc::Gen<SettingValue> genConformingValue(SettingType type,
                                         const std::vector<std::string>& enumValues) {
    switch (type) {
        case SettingType::Bool:
            return rc::gen::map(rc::gen::arbitrary<bool>(), SettingValue::Bool);
        case SettingType::Int:
            return rc::gen::map(rc::gen::arbitrary<std::int64_t>(),
                                SettingValue::Int);
        case SettingType::Float:
            return rc::gen::map(rc::gen::arbitrary<double>(), SettingValue::Float);
        case SettingType::String:
            return rc::gen::map(rc::gen::arbitrary<std::string>(),
                                SettingValue::String);
        case SettingType::Enum:
            return rc::gen::map(
                rc::gen::elementOf(enumValues),
                [](std::string label) { return SettingValue::Enum(std::move(label)); });
    }
    return rc::gen::just(SettingValue::Bool(false));
}

// --- Property 23 — fallback al default su persistito non valido -----------
// Feature: pulse-sdk, Property 23. Validates: Requirements 9.4.
RC_GTEST_PROP(Property23SettingsPersistedFallback,
              LoadPersistedFallsBackToDefaultOnInvalidValue,
              ()) {
    // (1) Nome valido (1..64 caratteri).
    const auto rawName = *rc::gen::arbitrary<std::string>().as("nome grezzo");
    std::string name = rawName.empty() ? std::string("s") : rawName;
    if (name.size() > pulse::kSettingNameMaxLength) {
        name.resize(pulse::kSettingNameMaxLength);
    }

    // (2) Tipo dichiarato casuale.
    const SettingType type = *rc::gen::elementOf(allTypes()).as("tipo dichiarato");

    // (3) Etichette enum (solo rilevanti per Enum): insieme non vuoto.
    std::vector<std::string> enumValues;
    if (type == SettingType::Enum) {
        enumValues = *rc::gen::nonEmpty(
                          rc::gen::container<std::vector<std::string>>(genEnumLabel()))
                          .as("etichette enum ammesse");
    }

    // (4) Default conforme al tipo dichiarato.
    const SettingValue defaultValue =
        *genConformingValue(type, enumValues).as("valore predefinito");

    SettingDecl decl;
    decl.name = name;
    decl.type = type;
    decl.defaultValue = defaultValue;
    decl.enumValues = enumValues;

    SettingsRegistry registry;
    const auto declResult = registry.declare(decl);
    // La dichiarazione costruita è sempre valida; in caso contrario scartiamo
    // il caso (non è ciò che questa property intende esercitare).
    RC_PRE(declResult.ok);

    // (5) Valore persistito: sometimes conforme, sometimes non conforme.
    // Costruiamo un generatore che produce entrambe le famiglie.

    // Variante con tipo SBAGLIATO (non conforme per mismatch di tipo).
    std::vector<SettingType> wrongTypes;
    for (SettingType t : allTypes()) {
        if (t != type) {
            wrongTypes.push_back(t);
        }
    }
    auto genWrongType = rc::gen::mapcat(
        rc::gen::elementOf(wrongTypes), [](SettingType wt) {
            // Per il tipo sbagliato un valore conforme A QUEL tipo basta: ciò
            // che conta è che il SettingType differisca da quello dichiarato.
            std::vector<std::string> labels = {"x", "y", "z"};
            return genConformingValue(wt, labels);
        });

    // Per Enum, un valore non conforme può anche essere un'etichetta NON
    // ammessa dello stesso tipo Enum.
    auto genBadLabel = rc::gen::map(
        rc::gen::suchThat(
            rc::gen::arbitrary<std::string>(),
            [enumValues](const std::string& s) {
                for (const auto& allowed : enumValues) {
                    if (allowed == s) {
                        return false;
                    }
                }
                return true;
            }),
        [](std::string s) { return SettingValue::Enum(std::move(s)); });

    const SettingValue persisted =
        (type == SettingType::Enum)
            ? *rc::gen::oneOf(genConformingValue(type, enumValues),  // conforme
                              genWrongType,        // tipo sbagliato
                              genBadLabel)         // etichetta non ammessa
                   .as("valore persistito")
            : *rc::gen::oneOf(genConformingValue(type, enumValues),  // conforme
                              genWrongType)        // tipo sbagliato
                   .as("valore persistito");

    // Oracolo: la conformità del valore persistito alla dichiarazione.
    const bool isConforming = conforms(decl, persisted);

    const LoadResult result = registry.loadPersisted(name, persisted);

    if (isConforming) {
        // Conforme => restituisce il valore persistito, niente fallback/report.
        RC_ASSERT(!result.usedDefault);
        RC_ASSERT(result.value == persisted);
        RC_ASSERT(!result.report.has_value());
    } else {
        // Non conforme => fallback al default dichiarato + report con il nome.
        RC_ASSERT(result.usedDefault);
        RC_ASSERT(result.value == decl.defaultValue);
        RC_ASSERT(result.report.has_value());
        RC_ASSERT(result.report->find(name) != std::string::npos);
    }

    // In entrambi i casi la dichiarazione resta INVARIATA nel registro.
    const SettingDecl* stored = registry.find(name);
    RC_ASSERT(stored != nullptr);
    RC_ASSERT(stored->name == decl.name);
    RC_ASSERT(stored->type == decl.type);
    RC_ASSERT(stored->defaultValue == decl.defaultValue);
    RC_ASSERT(stored->enumValues == decl.enumValues);
}

}  // namespace
