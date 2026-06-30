// tests/property35_manifest_validation_soundness_test.cpp
// Feature: pulse-sdk, Property 35 — Soundness della validazione del Manifest.
// Validates: Requirements 16.2, 16.4 (Requisiti 16.2, 16.4)
//
// Componente sotto test: `pulse::manifest::validate(const Manifest&)`
// (loader/lifecycle/manifest_validation.hpp), funzione pura che valida un
// Manifest contro lo schema definito PRIMA di caricare codice della Mod
// (Req 16.2) e, in caso di non conformità, restituisce l'ELENCO COMPLETO dei
// campi non conformi (Req 16.4), non solo il primo.
//
// Property 35 (RapidCheck, ≥100 iterazioni di default), due aspetti:
//
//   (1) Soundness/completeness sui Manifest validi — un generatore che produce
//       Manifest che soddisfano TUTTE le regole di schema deve dare
//       `validate(m).valid == true` con ZERO violazioni.
//
//   (2) Ogni violazione iniettata è rilevata — partendo da un Manifest valido
//       si iniettano una o più violazioni di regola (id vuoto / id > 256 /
//       schema_version < 1 / zero entry point / entry point con kind o symbol
//       vuoto / id di dipendenza vuoto / permesso vuoto o non riconosciuto /
//       setting con nome vuoto, tipo errato o default incoerente). `validate`
//       deve dare `valid == false` E l'insieme dei percorsi di campo riportati
//       deve COPRIRE tutte le violazioni iniettate (Req 16.4: elenco completo,
//       non solo la prima). Poiché ogni mutazione produce esattamente il
//       percorso atteso, l'insieme riportato coincide con quello atteso; un
//       Manifest senza mutazioni resta valido e non produce violazioni.
//
// Regole di schema (manifest_validation.hpp): schema_version >= 1; mod.id non
// vuoto e <= 256; >=1 entry point ciascuno con kind+symbol non vuoti; id di
// dipendenza non vuoti; ogni permesso non vuoto e in {network, filesystem,
// hooking, ui, events}; ogni setting con nome non vuoto, tipo in
// {int, float, bool, string} e default coerente col tipo.
//
// Il test CONSUMA soltanto manifest.hpp / manifest_validation.hpp (header-only)
// e non li modifica.
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "lifecycle/manifest.hpp"
#include "lifecycle/manifest_validation.hpp"

namespace {

using pulse::manifest::Dependency;
using pulse::manifest::EntryPoint;
using pulse::manifest::Manifest;
using pulse::manifest::ModType;
using pulse::manifest::SettingDecl;
using pulse::manifest::SettingValue;
using pulse::manifest::validate;
using pulse::manifest::ValidationResult;

// I permessi riconosciuti dallo schema (Req 17.1), forma canonica minuscola.
const std::vector<std::string>& recognizedPermissionList() {
    static const std::vector<std::string> kPerms{"network", "filesystem",
                                                 "hooking", "ui", "events"};
    return kPerms;
}

const std::vector<std::string>& recognizedSettingTypes() {
    static const std::vector<std::string> kTypes{"int", "float", "bool",
                                                 "string"};
    return kTypes;
}

// Identificatore non vuoto e <= 256 caratteri (Req 16.1): genera una stringa
// non vuota e la limita prudentemente alla lunghezza massima consentita.
std::string genValidId() {
    std::string id = *rc::gen::nonEmpty<std::string>().as("mod.id valido");
    if (id.size() > 200) id.resize(200);  // resta non vuoto e <= 256
    return id;
}

// Default coerente con il tipo dichiarato del setting.
SettingValue genMatchingDefault(const std::string& type) {
    if (type == "int") return SettingValue{*rc::gen::arbitrary<std::int64_t>()};
    if (type == "float") return SettingValue{*rc::gen::arbitrary<double>()};
    if (type == "bool") return SettingValue{*rc::gen::arbitrary<bool>()};
    return SettingValue{*rc::gen::arbitrary<std::string>()};  // string
}

// Default deliberatamente INCOERENTE col tipo dichiarato (variante diversa).
SettingValue genMismatchedDefault(const std::string& type) {
    if (type == "int") return SettingValue{std::string("non-un-intero")};
    if (type == "float") return SettingValue{std::int64_t{7}};
    if (type == "bool") return SettingValue{std::int64_t{0}};
    return SettingValue{std::int64_t{42}};  // type == "string": metti un intero
}

// Costruisce un Manifest che soddisfa TUTTE le regole di schema.
Manifest genValidManifest() {
    Manifest m;

    m.schemaVersion = *rc::gen::inRange(1, 50);  // >= 1
    m.id = genValidId();
    m.version.major = *rc::gen::inRange<std::uint32_t>(0u, 50u);
    m.version.minor = *rc::gen::inRange<std::uint32_t>(0u, 50u);
    m.version.patch = *rc::gen::inRange<std::uint32_t>(0u, 50u);
    m.name = *rc::gen::arbitrary<std::string>();
    m.type = *rc::gen::arbitrary<bool>() ? ModType::Native : ModType::Script;

    // >= 1 entry point, ciascuno con kind+symbol non vuoti.
    const int nEntries = *rc::gen::inRange(1, 5);
    for (int i = 0; i < nEntries; ++i) {
        EntryPoint ep;
        ep.kind = *rc::gen::nonEmpty<std::string>();
        ep.symbol = *rc::gen::nonEmpty<std::string>();
        m.entryPoints.push_back(std::move(ep));
    }

    // 0..4 dipendenze con id non vuoto.
    const int nDeps = *rc::gen::inRange(0, 5);
    for (int i = 0; i < nDeps; ++i) {
        Dependency d;
        d.id = *rc::gen::nonEmpty<std::string>();
        // versionConstraint default-costruito: vincolo strutturalmente valido.
        m.dependencies.push_back(std::move(d));
    }

    // 0..5 permessi, tutti riconosciuti e non vuoti.
    const int nPerms = *rc::gen::inRange(0, 6);
    for (int i = 0; i < nPerms; ++i) {
        m.permissions.push_back(
            *rc::gen::elementOf(recognizedPermissionList()));
    }

    // 0..4 setting con nome non vuoto, tipo riconosciuto, default coerente.
    const int nSettings = *rc::gen::inRange(0, 5);
    for (int i = 0; i < nSettings; ++i) {
        SettingDecl s;
        s.name = *rc::gen::nonEmpty<std::string>();
        s.type = *rc::gen::elementOf(recognizedSettingTypes());
        s.defaultValue = genMatchingDefault(s.type);
        m.settings.push_back(std::move(s));
    }

    return m;
}

std::set<std::string> reportedFieldSet(const ValidationResult& res) {
    std::set<std::string> fields;
    for (const auto& v : res.violations) fields.insert(v.field);
    return fields;
}

// --- Property 35 (1) — i Manifest validi non producono violazioni ----------
// Feature: pulse-sdk, Property 35. Validates: Requirements 16.2, 16.4.
//
// Un generatore che produce Manifest conformi a tutte le regole di schema deve
// dare `valid == true` con zero violazioni (soundness sui manifest validi).
RC_GTEST_PROP(Property35ManifestValidationSoundness,
              ValidManifestYieldsNoViolations,
              ()) {
    const Manifest m = genValidManifest();
    const ValidationResult res = validate(m);

    RC_ASSERT(res.valid);
    RC_ASSERT(res.violations.empty());
}

// --- Property 35 (2) — ogni violazione iniettata è rilevata -----------------
// Feature: pulse-sdk, Property 35. Validates: Requirements 16.2, 16.4.
//
// Partendo da un Manifest valido si iniettano violazioni casuali; `validate`
// deve segnalare `valid == false` e riportare un campo per OGNI violazione
// iniettata (Req 16.4 — elenco completo). Ogni mutazione produce esattamente
// il percorso di campo atteso, quindi l'insieme riportato coincide con quello
// atteso; senza mutazioni il Manifest resta valido senza violazioni.
RC_GTEST_PROP(Property35ManifestValidationSoundness,
              EachInjectedViolationIsReported,
              ()) {
    Manifest m = genValidManifest();
    std::set<std::string> expected;

    // --- schema_version < 1 -------------------------------------------------
    if (*rc::gen::arbitrary<bool>()) {
        m.schemaVersion = *rc::gen::inRange(-50, 1);  // [-50, 0] => < 1
        expected.insert("schema_version");
    }

    // --- mod.id: vuoto oppure > 256 caratteri -------------------------------
    if (*rc::gen::arbitrary<bool>()) {
        if (*rc::gen::arbitrary<bool>()) {
            m.id.clear();  // vuoto
        } else {
            m.id = std::string(257, 'x');  // supera i 256 caratteri
        }
        expected.insert("mod.id");
    }

    // --- entry points: o zero, oppure kind/symbol vuoti -----------------------
    {
        const int mode = *rc::gen::inRange(0, 3);  // 0 nessuna, 1 azzera, 2 corrompi
        if (mode == 1) {
            m.entryPoints.clear();
            expected.insert("entry_points");
        } else if (mode == 2) {
            for (std::size_t i = 0; i < m.entryPoints.size(); ++i) {
                const int sub = *rc::gen::inRange(0, 4);  // 0 none,1 kind,2 sym,3 both
                const std::string base =
                    "entry_points[" + std::to_string(i) + "]";
                if (sub == 1 || sub == 3) {
                    m.entryPoints[i].kind.clear();
                    expected.insert(base + ".kind");
                }
                if (sub == 2 || sub == 3) {
                    m.entryPoints[i].symbol.clear();
                    expected.insert(base + ".symbol");
                }
            }
        }
    }

    // --- dependencies: id vuoto ---------------------------------------------
    for (std::size_t i = 0; i < m.dependencies.size(); ++i) {
        if (*rc::gen::arbitrary<bool>()) {
            m.dependencies[i].id.clear();
            expected.insert("dependencies[" + std::to_string(i) + "].id");
        }
    }

    // --- permissions: vuoto oppure non riconosciuto -------------------------
    for (std::size_t i = 0; i < m.permissions.size(); ++i) {
        if (*rc::gen::arbitrary<bool>()) {
            if (*rc::gen::arbitrary<bool>()) {
                m.permissions[i].clear();  // stringa vuota
            } else {
                m.permissions[i] = "permesso_inesistente";  // non riconosciuto
            }
            expected.insert("permissions.required[" + std::to_string(i) + "]");
        }
    }

    // --- settings: nome vuoto / tipo errato / default incoerente -------------
    for (std::size_t i = 0; i < m.settings.size(); ++i) {
        const int sub = *rc::gen::inRange(0, 4);  // 0 none,1 name,2 type,3 default
        const std::string base = "settings[" + std::to_string(i) + "]";
        if (sub == 1) {
            m.settings[i].name.clear();
            expected.insert(base + ".name");
        } else if (sub == 2) {
            m.settings[i].type = "tipo_sconosciuto";  // non riconosciuto
            expected.insert(base + ".type");
        } else if (sub == 3) {
            // Mantiene il tipo valido ma rende il default di variante diversa.
            m.settings[i].defaultValue =
                genMismatchedDefault(m.settings[i].type);
            expected.insert(base + ".default");
        }
    }

    const ValidationResult res = validate(m);
    const std::set<std::string> reported = reportedFieldSet(res);

    // valid se e solo se non è stata iniettata alcuna violazione.
    RC_ASSERT(res.valid == expected.empty());

    // Ogni violazione iniettata deve comparire fra i campi riportati e, poiché
    // ogni mutazione produce esattamente il proprio percorso, gli insiemi
    // coincidono (Req 16.4 — elenco completo, non solo la prima).
    RC_ASSERT(reported == expected);
}

}  // namespace
