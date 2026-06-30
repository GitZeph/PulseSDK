// tests/property1_mod_manifest_round_trip_compat_test.cpp
// Feature: external-mod-loading, Property 1 — Round-trip del Mod_Manifest
// (inclusa la compatibilità GD).
// Validates: Requirements 3.5 (Requisito 3.5)
//
// Property 1 (design.md / Req 3.5): per ogni Mod_Manifest VALIDO generato in
// modo randomizzato — COMPRESI la piattaforma bersaglio e l'intervallo di
// versione di GD `[min, max]` della sezione `[compat]` — deve valere:
//   (a) parse(serialize(m)).ok == true  &&  parse(serialize(m)).manifest == m
//       — la coppia serialize→parse è l'identità campo-per-campo sui
//       Mod_Manifest validi, inclusi `compat.platform` e `compat.gdVersion`;
//   (b) serialize(parse(serialize(m)).manifest) == serialize(m)
//       — `serialize` è un punto fisso canonico: ri-serializzare il
//       Mod_Manifest ottenuto dal parsing produce esattamente la stessa
//       stringa, quindi la forma testuale (compresa la tabella `[compat]`) è
//       canonica e stabile sotto ulteriori round-trip.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si costruisce un Mod_Manifest valido pescando componenti randomizzati
//     (id non vuoto, ≥1 entry point, SemVer, dipendenze, permessi, settings con
//     default coerente al tipo), riusando la stessa impostazione del round-trip
//     base (Property 34 di pulse-sdk);
//   * la sezione `[compat]` è generata lungo TUTTE le sue forme rilevanti per
//     coprire il round-trip della compatibilità (Req 3.5):
//       - `platform`: assente OPPURE uno dei quattro valori dell'insieme finito
//         (macos-arm64, windows, android, ios);
//       - `gdVersion`: assente OPPURE un intervallo `[min, max]` con `min <=
//         max` (estremi inclusi, coerente con la semantica del campo);
//     così si esercitano i casi: compat completamente assente, solo platform,
//     solo intervallo, ed entrambi presenti — tutti devono round-trippare.
//
// Header del modello/parser in loader/lifecycle/ (include relativo alla radice
// loader/); `manifest.hpp` è header-only. Integrazione RapidCheck+GoogleTest
// in extras/gtest.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lifecycle/manifest.hpp"

namespace {

using pulse::manifest::Compatibility;
using pulse::manifest::Dependency;
using pulse::manifest::EntryPoint;
using pulse::manifest::GdVersionRange;
using pulse::manifest::Manifest;
using pulse::manifest::ModType;
using pulse::manifest::ParseResult;
using pulse::manifest::SemVer;
using pulse::manifest::SettingDecl;
using pulse::manifest::TargetPlatform;
using pulse::manifest::VersionConstraint;
using pulse::manifest::parse;
using pulse::manifest::serialize;

// Stringa arbitraria: esercita l'escaping del serializer (compresi \n, \r, \t,
// virgolette e backslash). RapidCheck può inserire caratteri di controllo, che
// devono comunque round-trippare grazie all'escaping/decoding.
std::string genString() { return *rc::gen::arbitrary<std::string>(); }

// Stringa NON vuota (per l'id del Mod_Manifest, Req 16.1).
std::string genNonEmptyString() {
    return *rc::gen::suchThat(rc::gen::arbitrary<std::string>(),
                              [](const std::string& s) { return !s.empty(); });
}

// SemVer casuale (campi uint32 entro un intervallo ampio ma to_string-friendly).
SemVer genSemVer() {
    SemVer v;
    v.major = *rc::gen::inRange<std::uint32_t>(0, 1000000u);
    v.minor = *rc::gen::inRange<std::uint32_t>(0, 1000000u);
    v.patch = *rc::gen::inRange<std::uint32_t>(0, 1000000u);
    return v;
}

// Double FINITO (esclude NaN/inf): std::to_chars produce la forma più corta che
// round-trippa esattamente per ogni double finito.
double genFiniteDouble() {
    return *rc::gen::suchThat(rc::gen::arbitrary<double>(),
                              [](double x) { return std::isfinite(x); });
}

// Vincolo di versione casuale: any / atLeast(min) / range(lo, hi).
VersionConstraint genConstraint() {
    const int kind = *rc::gen::inRange(0, 3);
    if (kind == 0) {
        return VersionConstraint::any();
    }
    if (kind == 1) {
        return VersionConstraint::atLeast(genSemVer());
    }
    return VersionConstraint::range(genSemVer(), genSemVer());
}

Dependency genDependency() {
    Dependency d;
    d.id = genString();  // l'id di una dipendenza può essere qualunque stringa
    d.versionConstraint = genConstraint();
    return d;
}

EntryPoint genEntryPoint() {
    EntryPoint ep;
    ep.kind = genString();
    ep.symbol = genString();
    return ep;
}

// Setting con default tipizzato COERENTE con `type`.
SettingDecl genSetting() {
    SettingDecl s;
    s.name = genString();
    const int t = *rc::gen::inRange(0, 4);
    switch (t) {
        case 0:
            s.type = "int";
            s.defaultValue = *rc::gen::arbitrary<std::int64_t>();
            break;
        case 1:
            s.type = "float";
            s.defaultValue = genFiniteDouble();
            break;
        case 2:
            s.type = "bool";
            s.defaultValue = *rc::gen::arbitrary<bool>();
            break;
        default:
            s.type = "string";
            s.defaultValue = genString();
            break;
    }
    return s;
}

// Piattaforma bersaglio casuale dall'insieme FINITO riconosciuto (Req 3.1).
TargetPlatform genTargetPlatform() {
    return *rc::gen::element(TargetPlatform::MacOSArm64, TargetPlatform::Windows,
                             TargetPlatform::Android, TargetPlatform::IOS);
}

// Intervallo di versione di GD bersaglio con `min <= max` (estremi inclusi,
// Req 3.1/3.4): si ordinano due SemVer generati così l'intervallo è ben formato.
GdVersionRange genGdVersionRange() {
    SemVer a = genSemVer();
    SemVer b = genSemVer();
    GdVersionRange r;
    if (b < a) {
        r.min = b;
        r.max = a;
    } else {
        r.min = a;
        r.max = b;
    }
    return r;
}

// Sezione `[compat]` casuale (Req 3.1): platform assente/presente e gdVersion
// assente/presente in modo indipendente, così si coprono tutte e quattro le
// combinazioni (nessuna, solo platform, solo intervallo, entrambe).
Compatibility genCompatibility() {
    Compatibility c;
    if (*rc::gen::arbitrary<bool>()) {
        c.platform = genTargetPlatform();
    }
    if (*rc::gen::arbitrary<bool>()) {
        c.gdVersion = genGdVersionRange();
    }
    return c;
}

// Mod_Manifest VALIDO randomizzato (id non vuoto, >=1 entry point) con sezione
// di compatibilità GD randomizzata.
Manifest genManifest() {
    Manifest m;
    m.schemaVersion = *rc::gen::inRange(0, 1000);
    m.id = genNonEmptyString();
    m.version = genSemVer();
    m.name = genString();
    m.type = *rc::gen::element(ModType::Native, ModType::Script);

    const std::size_t nEntry = *rc::gen::inRange<std::size_t>(1, 5);  // >=1
    for (std::size_t i = 0; i < nEntry; ++i) {
        m.entryPoints.push_back(genEntryPoint());
    }

    const std::size_t nDep = *rc::gen::inRange<std::size_t>(0, 5);
    for (std::size_t i = 0; i < nDep; ++i) {
        m.dependencies.push_back(genDependency());
    }

    const std::size_t nPerm = *rc::gen::inRange<std::size_t>(0, 5);
    for (std::size_t i = 0; i < nPerm; ++i) {
        m.permissions.push_back(genString());
    }

    const std::size_t nSet = *rc::gen::inRange<std::size_t>(0, 5);
    for (std::size_t i = 0; i < nSet; ++i) {
        m.settings.push_back(genSetting());
    }

    m.compat = genCompatibility();  // sezione `[compat]` (Req 3.1, 3.5)

    return m;
}

// --- Property 1 — round-trip del Mod_Manifest con compatibilità GD ---------
// Feature: external-mod-loading, Property 1. Validates: Requirements 3.5.
//
// (a) parse(serialize(m)) ok ed uguale a m (incl. compat.platform/gdVersion);
// (b) serialize(parse(serialize(m)).manifest) == serialize(m) (punto fisso).
RC_GTEST_PROP(Property1ModManifestRoundTripCompat,
              ParseOfSerializeIsIdentityIncludingCompat,
              ()) {
    const Manifest m = genManifest();

    // serialize → parse
    const std::string text = serialize(m);
    const ParseResult parsed = parse(text);

    // (a) il parsing riesce e restituisce un Mod_Manifest uguale campo-per-campo
    //     all'originale, INCLUSA la sezione `[compat]` (platform + intervallo).
    RC_ASSERT(parsed.ok);
    RC_ASSERT(parsed.manifest == m);
    RC_ASSERT(parsed.manifest.compat == m.compat);

    // (b) ri-serializzare il Mod_Manifest parsato riproduce la stessa stringa
    //     canonica (punto fisso): serialize è stabile sotto round-trip, anche
    //     per la tabella `[compat]`.
    const std::string text2 = serialize(parsed.manifest);
    RC_ASSERT(text2 == text);

    // Doppio round-trip: parse∘serialize∘parse∘serialize stabile.
    const ParseResult parsed2 = parse(text2);
    RC_ASSERT(parsed2.ok);
    RC_ASSERT(parsed2.manifest == m);
    RC_ASSERT(serialize(parsed2.manifest) == text);
}

}  // namespace
