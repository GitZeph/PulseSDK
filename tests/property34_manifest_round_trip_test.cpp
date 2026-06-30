// tests/property34_manifest_round_trip_test.cpp
// Feature: pulse-sdk, Property 34 — Round-trip del Manifest (`pulse.toml`).
// Validates: Requirements 16.5 (Requisito 16.5)
//
// Property 34 (design.md / Req 16.5): per ogni Manifest VALIDO generato in
// modo randomizzato deve valere:
//   (a) parse(serialize(m)).ok == true  &&  parse(serialize(m)).manifest == m
//       — la coppia serialize→parse è l'identità sui Manifest validi;
//   (b) serialize(parse(serialize(m)).manifest) == serialize(m)
//       — `serialize` è un punto fisso canonico: ri-serializzare il Manifest
//       ottenuto dal parsing produce esattamente la stessa stringa, quindi la
//       forma testuale è canonica e stabile sotto ulteriori round-trip.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si costruisce un Manifest valido pescando componenti randomizzati:
//       - `id` NON vuoto (Req 16.1) ed `entryPoints` con ALMENO uno
//         (vincolo "almeno un punto di ingresso", Req 16.1);
//       - `version` SemVer casuale, `type` (Native/Script) casuale,
//         `schemaVersion` casuale;
//       - `dependencies` con id arbitrario e vincolo di versione casuale tra
//         {any, atLeast(min), range(lo, hi)};
//       - `permissions` come stringhe arbitrarie (il serializer le effettua
//         l'escaping in stringhe TOML "basic", quindi round-trippano);
//       - `settings` con default tipizzato COERENTE col campo `type`
//         (int→int64, float→double, bool→bool, string→string);
//   * le stringhe arbitrarie esercitano l'escaping (\\, \", \n, \r, \t);
//   * i `double` dei default float sono limitati ai valori FINITI: il
//     serializer usa std::to_chars (rappresentazione più corta che
//     round-trippa) quindi ogni double finito si riottiene esattamente; si
//     escludono NaN/inf che non hanno una forma canonica round-trippabile.
//
// Header del modello/parser in loader/lifecycle/ (include relativo alla radice
// loader/); `manifest.hpp` è header-only. Integrazione RapidCheck+GoogleTest
// in extras/gtest.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "lifecycle/manifest.hpp"

namespace {

using pulse::manifest::Dependency;
using pulse::manifest::EntryPoint;
using pulse::manifest::Manifest;
using pulse::manifest::ModType;
using pulse::manifest::ParseResult;
using pulse::manifest::SemVer;
using pulse::manifest::SettingDecl;
using pulse::manifest::VersionConstraint;
using pulse::manifest::parse;
using pulse::manifest::serialize;

// Stringa arbitraria: esercita l'escaping del serializer (compresi \n, \r, \t,
// virgolette e backslash). RapidCheck può inserire caratteri di controllo, che
// devono comunque round-trippare grazie all'escaping/decoding.
std::string genString() { return *rc::gen::arbitrary<std::string>(); }

// Stringa NON vuota (per l'id del Manifest, Req 16.1).
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

// Manifest VALIDO randomizzato (id non vuoto, >=1 entry point).
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

    return m;
}

// --- Property 34 — round-trip e canonicità del Manifest -------------------
// Feature: pulse-sdk, Property 34. Validates: Requirements 16.5.
//
// (a) parse(serialize(m)) ok ed uguale a m;
// (b) serialize(parse(serialize(m)).manifest) == serialize(m) (punto fisso).
RC_GTEST_PROP(Property34ManifestRoundTrip,
              SerializeParseIsIdentityAndCanonical,
              ()) {
    const Manifest m = genManifest();

    // serialize → parse
    const std::string text = serialize(m);
    const ParseResult parsed = parse(text);

    // (a) il parsing riesce e restituisce un Manifest uguale all'originale.
    RC_ASSERT(parsed.ok);
    RC_ASSERT(parsed.manifest == m);

    // (b) ri-serializzare il Manifest parsato riproduce la stessa stringa
    //     canonica (punto fisso): serialize è stabile sotto round-trip.
    const std::string text2 = serialize(parsed.manifest);
    RC_ASSERT(text2 == text);

    // Doppio round-trip: parse(serialize(parse(serialize(m)).manifest)) stabile.
    const ParseResult parsed2 = parse(text2);
    RC_ASSERT(parsed2.ok);
    RC_ASSERT(parsed2.manifest == m);
    RC_ASSERT(serialize(parsed2.manifest) == text);
}

}  // namespace
