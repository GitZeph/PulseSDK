// tests/manifest_validation_test.cpp — Unit test della validazione del
// `Manifest` contro lo schema (task 13.2, Req 16.2, 16.3, 16.4).
//
// Copertura:
//   * Manifest valido → accettato senza violazioni (Req 16.2);
//   * ogni singola regola di schema → violazione segnalata sul campo corretto
//     (Req 16.4): id vuoto / troppo lungo, schema_version < 1, nessun entry
//     point, entry point con kind/symbol vuoti, dipendenza con id vuoto,
//     permesso vuoto, permesso non riconosciuto, setting con nome vuoto /
//     tipo non riconosciuto / default incoerente;
//   * violazioni MULTIPLE simultanee → tutte elencate, non solo la prima
//     (Req 16.4);
//   * Manifest assente (std::optional vuoto e ParseResult fallito) → rifiuto
//     esplicito (Req 16.3).
//
// Header del manifest+validazione in loader/lifecycle/ (include relativo alla
// radice loader/, aggiunta alla include path dal target di test).

#include "lifecycle/manifest_validation.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "lifecycle/manifest.hpp"

namespace {

using pulse::manifest::Dependency;
using pulse::manifest::EntryPoint;
using pulse::manifest::Manifest;
using pulse::manifest::ModType;
using pulse::manifest::SemVer;
using pulse::manifest::SettingDecl;
using pulse::manifest::ValidationResult;
using pulse::manifest::VersionConstraint;

// Manifest conforme allo schema, base per le mutazioni dei test.
Manifest makeValidManifest() {
    Manifest m;
    m.schemaVersion = 1;
    m.id = "com.example.mymod";
    m.version = SemVer{1, 2, 0};
    m.name = "My Mod";
    m.type = ModType::Native;
    m.entryPoints = {EntryPoint{"init", "mymod_init"}};
    m.dependencies = {
        Dependency{"com.pulse.core",
                   VersionConstraint::range(SemVer{1, 0, 0}, SemVer{2, 0, 0})}};
    m.permissions = {"hooking", "ui", "network"};
    m.settings = {SettingDecl{"jumpBoost", "int", std::int64_t{0}}};
    return m;
}

// Vero se esiste una violazione il cui campo è esattamente `field`.
bool hasField(const ValidationResult& r, std::string_view field) {
    return std::any_of(r.violations.begin(), r.violations.end(),
                       [&](const auto& v) { return v.field == field; });
}

// --- Manifest valido (Req 16.2) --------------------------------------------

TEST(ManifestValidation, ValidManifestPasses) {
    ValidationResult r = pulse::manifest::validate(makeValidManifest());
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.violations.empty());
}

TEST(ManifestValidation, MinimalValidManifestPasses) {
    // Permessi/dipendenze/settings vuoti sono ammessi dallo schema del Manifest.
    Manifest m;
    m.schemaVersion = 1;
    m.id = "a";
    m.version = SemVer{0, 0, 1};
    m.entryPoints = {EntryPoint{"init", "main"}};
    ValidationResult r = pulse::manifest::validate(m);
    EXPECT_TRUE(r.ok()) << (r.violations.empty() ? "" : r.violations.front().message);
}

// --- Regole individuali (Req 16.4) -----------------------------------------

TEST(ManifestValidation, RejectsEmptyId) {
    Manifest m = makeValidManifest();
    m.id.clear();
    ValidationResult r = pulse::manifest::validate(m);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(hasField(r, "mod.id"));
}

TEST(ManifestValidation, RejectsIdLongerThan256) {
    Manifest m = makeValidManifest();
    m.id = std::string(257, 'x');
    ValidationResult r = pulse::manifest::validate(m);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(hasField(r, "mod.id"));
}

TEST(ManifestValidation, AcceptsIdExactly256) {
    Manifest m = makeValidManifest();
    m.id = std::string(256, 'x');
    ValidationResult r = pulse::manifest::validate(m);
    EXPECT_TRUE(r.ok());
}

TEST(ManifestValidation, RejectsSchemaVersionBelowOne) {
    Manifest m = makeValidManifest();
    m.schemaVersion = 0;
    ValidationResult r = pulse::manifest::validate(m);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(hasField(r, "schema_version"));
}

TEST(ManifestValidation, RejectsNoEntryPoints) {
    Manifest m = makeValidManifest();
    m.entryPoints.clear();
    ValidationResult r = pulse::manifest::validate(m);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(hasField(r, "entry_points"));
}

TEST(ManifestValidation, RejectsEntryPointWithEmptyKindAndSymbol) {
    Manifest m = makeValidManifest();
    m.entryPoints = {EntryPoint{"", ""}};
    ValidationResult r = pulse::manifest::validate(m);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(hasField(r, "entry_points[0].kind"));
    EXPECT_TRUE(hasField(r, "entry_points[0].symbol"));
}

TEST(ManifestValidation, RejectsDependencyWithEmptyId) {
    Manifest m = makeValidManifest();
    m.dependencies = {Dependency{"", VersionConstraint::any()}};
    ValidationResult r = pulse::manifest::validate(m);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(hasField(r, "dependencies[0].id"));
}

TEST(ManifestValidation, RejectsEmptyPermissionString) {
    Manifest m = makeValidManifest();
    m.permissions = {""};
    ValidationResult r = pulse::manifest::validate(m);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(hasField(r, "permissions.required[0]"));
}

TEST(ManifestValidation, RejectsUnrecognizedPermission) {
    Manifest m = makeValidManifest();
    m.permissions = {"hooking", "telepathy"};
    ValidationResult r = pulse::manifest::validate(m);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(hasField(r, "permissions.required[1]"));
}

TEST(ManifestValidation, AcceptsAllRecognizedPermissions) {
    Manifest m = makeValidManifest();
    m.permissions = {"network", "filesystem", "hooking", "ui", "events"};
    ValidationResult r = pulse::manifest::validate(m);
    EXPECT_TRUE(r.ok());
}

TEST(ManifestValidation, RejectsSettingWithEmptyName) {
    Manifest m = makeValidManifest();
    m.settings = {SettingDecl{"", "int", std::int64_t{0}}};
    ValidationResult r = pulse::manifest::validate(m);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(hasField(r, "settings[0].name"));
}

TEST(ManifestValidation, RejectsSettingWithUnrecognizedType) {
    Manifest m = makeValidManifest();
    m.settings = {SettingDecl{"s", "blob", std::string{"x"}}};
    ValidationResult r = pulse::manifest::validate(m);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(hasField(r, "settings[0].type"));
}

TEST(ManifestValidation, RejectsSettingWithDefaultMismatchingType) {
    Manifest m = makeValidManifest();
    // type "int" ma default è una stringa.
    m.settings = {SettingDecl{"s", "int", std::string{"not an int"}}};
    ValidationResult r = pulse::manifest::validate(m);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(hasField(r, "settings[0].default"));
}

// --- Violazioni multiple simultanee tutte elencate (Req 16.4) --------------

TEST(ManifestValidation, ReportsAllViolationsNotJustFirst) {
    Manifest m;
    m.schemaVersion = 0;                 // viola schema_version
    m.id = "";                           // viola mod.id
    m.version = SemVer{1, 0, 0};
    m.entryPoints = {EntryPoint{"", ""}};  // viola kind e symbol
    m.dependencies = {Dependency{"", VersionConstraint::any()}};  // viola dep id
    m.permissions = {"bogus"};           // permesso non riconosciuto
    m.settings = {SettingDecl{"", "int", std::string{"x"}}};  // nome + default

    ValidationResult r = pulse::manifest::validate(m);
    EXPECT_FALSE(r.ok());

    // Tutte le non conformità devono comparire contemporaneamente.
    EXPECT_TRUE(hasField(r, "schema_version"));
    EXPECT_TRUE(hasField(r, "mod.id"));
    EXPECT_TRUE(hasField(r, "entry_points[0].kind"));
    EXPECT_TRUE(hasField(r, "entry_points[0].symbol"));
    EXPECT_TRUE(hasField(r, "dependencies[0].id"));
    EXPECT_TRUE(hasField(r, "permissions.required[0]"));
    EXPECT_TRUE(hasField(r, "settings[0].name"));
    EXPECT_TRUE(hasField(r, "settings[0].default"));

    // Almeno 8 violazioni distinte: la validazione non si ferma alla prima.
    EXPECT_GE(r.violations.size(), 8u);
}

TEST(ManifestValidation, MultipleEntryPointViolationsIndexedSeparately) {
    Manifest m = makeValidManifest();
    m.entryPoints = {EntryPoint{"init", "ok"}, EntryPoint{"", "sym"},
                     EntryPoint{"kind", ""}};
    ValidationResult r = pulse::manifest::validate(m);
    EXPECT_FALSE(r.ok());
    EXPECT_FALSE(hasField(r, "entry_points[0].kind"));
    EXPECT_FALSE(hasField(r, "entry_points[0].symbol"));
    EXPECT_TRUE(hasField(r, "entry_points[1].kind"));
    EXPECT_TRUE(hasField(r, "entry_points[2].symbol"));
}

// --- Manifest assente → rifiuto (Req 16.3) ---------------------------------

TEST(ManifestValidation, AbsentManifestOptionalRejected) {
    std::optional<Manifest> absent;  // nullopt
    ValidationResult r = pulse::manifest::validate(absent);
    EXPECT_FALSE(r.ok());
    ASSERT_FALSE(r.violations.empty());
    EXPECT_TRUE(hasField(r, "manifest"));
}

TEST(ManifestValidation, PresentOptionalDelegatesToFieldValidation) {
    std::optional<Manifest> present = makeValidManifest();
    ValidationResult r = pulse::manifest::validate(present);
    EXPECT_TRUE(r.ok());
}

TEST(ManifestValidation, FailedParseResultRejectedAsAbsent) {
    pulse::manifest::ParseResult parsed;
    parsed.ok = false;
    parsed.error = "Manifest assente";
    ValidationResult r = pulse::manifest::validatePresence(parsed);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(hasField(r, "manifest"));
}

TEST(ManifestValidation, SuccessfulParseResultValidated) {
    pulse::manifest::ParseResult parsed;
    parsed.ok = true;
    parsed.manifest = makeValidManifest();
    ValidationResult r = pulse::manifest::validatePresence(parsed);
    EXPECT_TRUE(r.ok());
}

}  // namespace
