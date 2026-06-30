// tests/manifest_test.cpp — Unit test del modello `Manifest` e del
// parser/serializer `pulse.toml` (task 13.1, Req 16.1, 16.5).
//
// Copertura:
//   * presenza dei campi (Req 16.1): id, version SemVer, type, >=1 entry point,
//     dipendenze con vincolo di versione, permessi, settings;
//   * round-trip (Req 16.5): parse(serialize(m)) == m e
//     parse(serialize(parse(s))) == parse(s);
//   * round-trip dei vincoli di versione e dei default tipizzati dei settings;
//   * tolleranza del parser a commenti/spaziatura;
//   * riconciliazione con il DependencyResolver via toResolverManifest().
//
// Header del manifest in loader/lifecycle/ (include relativo alla radice
// loader/, aggiunta alla include path dal target di test).

#include "lifecycle/manifest.hpp"

#include <cstdint>
#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "lifecycle/dependency_resolver.hpp"

namespace {

using pulse::manifest::Dependency;
using pulse::manifest::EntryPoint;
using pulse::manifest::Manifest;
using pulse::manifest::ModType;
using pulse::manifest::SemVer;
using pulse::manifest::SettingDecl;
using pulse::manifest::VersionConstraint;

// Costruisce un Manifest valido e ricco di campi per i test di round-trip.
Manifest makeRichManifest() {
    Manifest m;
    m.schemaVersion = 1;
    m.id = "com.example.mymod";
    m.version = SemVer{1, 2, 0};
    m.name = "My Mod";
    m.type = ModType::Native;
    m.entryPoints = {EntryPoint{"init", "mymod_init"},
                     EntryPoint{"shutdown", "mymod_shutdown"}};
    m.dependencies = {
        Dependency{"com.pulse.core",
                   VersionConstraint::range(SemVer{1, 0, 0}, SemVer{2, 0, 0})},
        Dependency{"com.pulse.ui", VersionConstraint::atLeast(SemVer{0, 5, 3})},
    };
    m.permissions = {"hooking", "ui", "network"};
    m.settings = {
        SettingDecl{"jumpBoost", "int", std::int64_t{42}},
        SettingDecl{"gravity", "float", 9.81},
        SettingDecl{"enabled", "bool", true},
        SettingDecl{"label", "string", std::string{"hello"}},
    };
    return m;
}

// --- Presenza dei campi (Req 16.1) -----------------------------------------

TEST(ManifestModel, DeclaresRequiredFields) {
    Manifest m = makeRichManifest();

    // id non vuoto, <=256.
    EXPECT_FALSE(m.id.empty());
    EXPECT_LE(m.id.size(), 256u);

    // version SemVer.
    EXPECT_EQ(m.version.major, 1u);
    EXPECT_EQ(m.version.minor, 2u);
    EXPECT_EQ(m.version.patch, 0u);

    // almeno un entry point.
    ASSERT_GE(m.entryPoints.size(), 1u);
    EXPECT_EQ(m.entryPoints.front().kind, "init");

    // dipendenze con vincolo di versione.
    ASSERT_EQ(m.dependencies.size(), 2u);
    EXPECT_EQ(m.dependencies[0].id, "com.pulse.core");
    EXPECT_TRUE(m.dependencies[0].versionConstraint.satisfiedBy(SemVer{1, 5, 0}));
    EXPECT_FALSE(m.dependencies[0].versionConstraint.satisfiedBy(SemVer{2, 0, 0}));

    // permessi.
    EXPECT_EQ(m.permissions.size(), 3u);

    // settings dichiarati.
    EXPECT_EQ(m.settings.size(), 4u);
}

// --- Round-trip (Req 16.5) --------------------------------------------------

TEST(ManifestRoundTrip, SerializeThenParseYieldsEqualManifest) {
    Manifest m = makeRichManifest();

    std::string toml = pulse::manifest::serialize(m);
    auto parsed = pulse::manifest::parse(toml);

    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.manifest, m);
}

TEST(ManifestRoundTrip, ParseSerializeParseIsStable) {
    // parse∘serialize∘parse == parse (forma canonica idempotente, Req 16.5).
    Manifest m = makeRichManifest();
    std::string toml = pulse::manifest::serialize(m);

    auto p1 = pulse::manifest::parse(toml);
    ASSERT_TRUE(p1.ok) << p1.error;

    std::string toml2 = pulse::manifest::serialize(p1.manifest);
    auto p2 = pulse::manifest::parse(toml2);
    ASSERT_TRUE(p2.ok) << p2.error;

    EXPECT_EQ(p1.manifest, p2.manifest);
    // La forma canonica è stabile: serialize(parse(x)) è un punto fisso.
    EXPECT_EQ(toml, toml2);
}

TEST(ManifestRoundTrip, MinimalManifestWithSingleEntryPoint) {
    Manifest m;
    m.id = "a";
    m.version = SemVer{0, 0, 1};
    m.name = "";
    m.type = ModType::Script;
    m.entryPoints = {EntryPoint{"init", "main"}};
    // nessuna dipendenza, nessun permesso, nessun setting.

    std::string toml = pulse::manifest::serialize(m);
    auto parsed = pulse::manifest::parse(toml);

    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.manifest, m);
    EXPECT_EQ(parsed.manifest.type, ModType::Script);
    EXPECT_TRUE(parsed.manifest.permissions.empty());
    EXPECT_TRUE(parsed.manifest.dependencies.empty());
}

TEST(ManifestRoundTrip, VersionConstraintsRoundTrip) {
    // atLeast (solo min) e range (min + maxExclusive) devono entrambi sopravvivere.
    Manifest m;
    m.id = "deps.test";
    m.version = SemVer{3, 0, 0};
    m.entryPoints = {EntryPoint{"init", "go"}};
    m.dependencies = {
        Dependency{"only.min", VersionConstraint::atLeast(SemVer{2, 1, 0})},
        Dependency{"with.range",
                   VersionConstraint::range(SemVer{1, 0, 0}, SemVer{1, 5, 0})},
        Dependency{"any", VersionConstraint::any()},
    };

    auto parsed = pulse::manifest::parse(pulse::manifest::serialize(m));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    ASSERT_EQ(parsed.manifest.dependencies.size(), 3u);

    EXPECT_EQ(parsed.manifest.dependencies[0].versionConstraint.min, (SemVer{2, 1, 0}));
    EXPECT_FALSE(parsed.manifest.dependencies[0].versionConstraint.maxExclusive.has_value());

    EXPECT_EQ(parsed.manifest.dependencies[1].versionConstraint.min, (SemVer{1, 0, 0}));
    ASSERT_TRUE(parsed.manifest.dependencies[1].versionConstraint.maxExclusive.has_value());
    EXPECT_EQ(*parsed.manifest.dependencies[1].versionConstraint.maxExclusive, (SemVer{1, 5, 0}));

    EXPECT_EQ(parsed.manifest, m);
}

TEST(ManifestRoundTrip, TypedSettingDefaultsRoundTrip) {
    Manifest m;
    m.id = "settings.test";
    m.version = SemVer{1, 0, 0};
    m.entryPoints = {EntryPoint{"init", "go"}};
    m.settings = {
        SettingDecl{"i", "int", std::int64_t{-7}},
        SettingDecl{"f", "float", 0.5},
        SettingDecl{"b", "bool", false},
        SettingDecl{"s", "string", std::string{"with \"quote\" and \\ backslash"}},
    };

    auto parsed = pulse::manifest::parse(pulse::manifest::serialize(m));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    ASSERT_EQ(parsed.manifest.settings.size(), 4u);

    EXPECT_EQ(std::get<std::int64_t>(parsed.manifest.settings[0].defaultValue), -7);
    EXPECT_DOUBLE_EQ(std::get<double>(parsed.manifest.settings[1].defaultValue), 0.5);
    EXPECT_EQ(std::get<bool>(parsed.manifest.settings[2].defaultValue), false);
    EXPECT_EQ(std::get<std::string>(parsed.manifest.settings[3].defaultValue),
              "with \"quote\" and \\ backslash");

    EXPECT_EQ(parsed.manifest, m);
}

TEST(ManifestRoundTrip, StringEscapingRoundTrips) {
    // id/name con caratteri che richiedono escaping TOML.
    Manifest m;
    m.id = "id.with\ttab";
    m.version = SemVer{1, 0, 0};
    m.name = "line1\nline2 \"q\" \\b";
    m.entryPoints = {EntryPoint{"init", "go"}};
    m.permissions = {"perm \"x\"", "perm\ttab"};

    auto parsed = pulse::manifest::parse(pulse::manifest::serialize(m));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.manifest, m);
}

// --- Tolleranza del parser --------------------------------------------------

TEST(ManifestParse, ToleratesCommentsAndBlankLines) {
    const std::string toml = R"(# pulse.toml di esempio
schema_version = 1

# sezione mod
[mod]
id = "com.example.commented"
version = "2.3.4"
name = "Commented Mod"
type = "native"

[[entry_points]]
kind = "init"
symbol = "entry"

[permissions]
required = ["hooking"]
)";

    auto parsed = pulse::manifest::parse(toml);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.manifest.id, "com.example.commented");
    EXPECT_EQ(parsed.manifest.version, (SemVer{2, 3, 4}));
    ASSERT_EQ(parsed.manifest.entryPoints.size(), 1u);
    EXPECT_EQ(parsed.manifest.entryPoints[0].symbol, "entry");
    ASSERT_EQ(parsed.manifest.permissions.size(), 1u);
    EXPECT_EQ(parsed.manifest.permissions[0], "hooking");
}

TEST(ManifestParse, RejectsMalformedSemVer) {
    const std::string toml = R"(schema_version = 1
[mod]
id = "x"
version = "1.2"
name = "x"
type = "native"
)";
    auto parsed = pulse::manifest::parse(toml);
    EXPECT_FALSE(parsed.ok);
    EXPECT_FALSE(parsed.error.empty());
}

TEST(ManifestParse, RejectsLineWithoutAssignment) {
    const std::string toml = R"(schema_version = 1
[mod]
this line has no equals
)";
    auto parsed = pulse::manifest::parse(toml);
    EXPECT_FALSE(parsed.ok);
}

// --- Riconciliazione con il DependencyResolver ------------------------------

TEST(ManifestReconciliation, ProjectsToResolverManifest) {
    Manifest m = makeRichManifest();
    pulse::lifecycle::ResolverManifest rm = m.toResolverManifest();

    EXPECT_EQ(rm.id, m.id);
    EXPECT_EQ(rm.version, m.version);
    ASSERT_EQ(rm.dependencies.size(), m.dependencies.size());
    EXPECT_EQ(rm.dependencies[0].id, "com.pulse.core");

    // Il resolver deve poter consumare la proiezione senza escludere la mod
    // (qui senza le sue dipendenze installate viene esclusa per dipendenza
    // mancante: verifichiamo solo che il tipo sia accettato dall'API).
    pulse::lifecycle::DependencyResolver resolver;
    pulse::lifecycle::LoadPlan plan = resolver.resolve({rm});
    // Con sole dipendenze mancanti, `rm` è esclusa ma l'API ha accettato il tipo.
    EXPECT_EQ(plan.order.size() + plan.excluded.size(), 1u);
}

}  // namespace
