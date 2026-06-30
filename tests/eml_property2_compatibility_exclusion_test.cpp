// tests/eml_property2_compatibility_exclusion_test.cpp
// Feature: external-mod-loading, Property 2 — Esclusione per incompatibilità di
// piattaforma/versione (estremi inclusi).
// Validates: Requirements 3.2, 3.3, 3.4, 3.6 (Requisiti 3.2, 3.3, 3.4, 3.6)
//
// Property 2 (design.md): per ogni Mod_Manifest e per ogni Runtime_Context, la
// mod è ammessa al caricamento SE E SOLO SE:
//   * la sezione [compat] dichiara SIA la piattaforma SIA l'intervallo di
//     versione di GD (Req 3.6), E
//   * la piattaforma bersaglio dichiarata coincide con la proiezione della
//     piattaforma del Runtime_Context sull'insieme finito (Req 3.2/3.3), E
//   * la versione di GD del Runtime_Context — promossa a SemVer{major, minor, 0}
//     — cade nell'intervallo [min, max] a ESTREMI INCLUSI (Req 3.2/3.4).
// Altrimenti la mod è esclusa con ESATTAMENTE UNA causa tra
//   { MissingCompat, PlatformMismatch, VersionOutOfRange }.
//
// L'ordine deterministico delle cause documentato nel design (§4) è:
//   compat assente → piattaforma → versione.
//
// Strategia (RapidCheck, ≥100 iterazioni di default — qui forzate a ≥100):
//   * si genera `ctx.platform` su TUTTO l'enum runtime (compresi Linux/Unknown
//     non rappresentabili nell'insieme finito → nessuna piattaforma bersaglio
//     può combaciare);
//   * si genera la versione di GD del Runtime_Context con componenti PICCOLE
//     (0..4) così che gli ESTREMI dell'intervallo (uguaglianza con min/max,
//     patch=0 del valore promosso) siano colpiti spesso;
//   * si decide indipendentemente se la piattaforma bersaglio e/o l'intervallo
//     sono DICHIARATI (per esercitare MissingCompat in tutte le sue forme);
//   * si genera l'intervallo [min, max] con componenti piccole (anche min>max,
//     intervallo vuoto → sempre fuori range).
//   L'oracolo è INDIPENDENTE da `check_compatibility`: ricalcola la proiezione
//   di piattaforma e l'appartenenza all'intervallo a partire dalla specifica.

#include "lifecycle/compatibility.hpp"
#include "lifecycle/manifest.hpp"
#include "core/runtime_context.hpp"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <array>
#include <cstdint>
#include <optional>

namespace {

using pulse::lifecycle::CompatCause;
using pulse::lifecycle::CompatResult;
using pulse::lifecycle::check_compatibility;
using pulse::loader::GdVersion;
using pulse::loader::Platform;
using pulse::loader::RuntimeContext;
using pulse::manifest::Compatibility;
using pulse::manifest::GdVersionRange;
using pulse::manifest::Manifest;
using pulse::manifest::SemVer;
using pulse::manifest::TargetPlatform;

// --- Oracolo INDIPENDENTE: proiezione della Platform runtime sull'insieme
// finito delle TargetPlatform dichiarabili (Req 3.1/3.2). Codifica la stessa
// specifica del design, ma in modo autonomo rispetto a `check_compatibility`.
// Le piattaforme non rappresentabili (Linux, Unknown e — fuori da una build
// arm64 — macOS) restituiscono `nullopt`: nessuna piattaforma bersaglio può
// combaciare, dunque la mod è esclusa per PlatformMismatch.
std::optional<TargetPlatform> oracleRuntimeTarget(Platform p) {
    switch (p) {
        case Platform::WindowsX64:
            return TargetPlatform::Windows;
        case Platform::MacOS:
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
            return TargetPlatform::MacOSArm64;
#else
            return std::nullopt;
#endif
        case Platform::AndroidArm64:
        case Platform::AndroidArmV7:
            return TargetPlatform::Android;
        case Platform::IOSArm64:
            return TargetPlatform::IOS;
        case Platform::Linux:
        case Platform::Unknown:
        default:
            return std::nullopt;
    }
}

Platform genPlatform() {
    static constexpr std::array<Platform, 7> kAll{
        Platform::Unknown,      Platform::WindowsX64,   Platform::MacOS,
        Platform::Linux,        Platform::AndroidArm64, Platform::AndroidArmV7,
        Platform::IOSArm64};
    const int idx = *rc::gen::inRange(0, static_cast<int>(kAll.size()));
    return kAll[static_cast<std::size_t>(idx)];
}

TargetPlatform genTargetPlatform() {
    static constexpr std::array<TargetPlatform, 4> kAll{
        TargetPlatform::MacOSArm64, TargetPlatform::Windows,
        TargetPlatform::Android, TargetPlatform::IOS};
    const int idx = *rc::gen::inRange(0, static_cast<int>(kAll.size()));
    return kAll[static_cast<std::size_t>(idx)];
}

// Componenti SemVer piccole per massimizzare collisioni sugli estremi.
SemVer genSmallSemVer() {
    const auto a = static_cast<std::uint32_t>(*rc::gen::inRange(0, 5));
    const auto b = static_cast<std::uint32_t>(*rc::gen::inRange(0, 5));
    const auto c = static_cast<std::uint32_t>(*rc::gen::inRange(0, 5));
    return SemVer{a, b, c};
}

// --- Property 2 — esclusione per incompatibilità piattaforma/versione -------
// Feature: external-mod-loading, Property 2.
// Validates: Requirements 3.2, 3.3, 3.4, 3.6.
RC_GTEST_PROP(EmlProperty2CompatibilityExclusion,
              AdmittedIffPlatformMatchesAndVersionInInclusiveRange,
              ()) {
    // --- Runtime_Context generato -----------------------------------------
    RuntimeContext ctx;
    ctx.platform = genPlatform();
    const auto rtMajor = static_cast<std::uint32_t>(*rc::gen::inRange(0, 5));
    const auto rtMinor = static_cast<std::uint32_t>(*rc::gen::inRange(0, 5));
    ctx.gdVersion = GdVersion{rtMajor, rtMinor};
    // `platformId` lasciato vuoto: il predicato ricade su platform_id(ctx).

    // --- Mod_Manifest: sezione [compat] generata --------------------------
    const bool declarePlatform = *rc::gen::arbitrary<bool>();
    const bool declareVersion = *rc::gen::arbitrary<bool>();

    Manifest m;
    m.id = "mod.test.eml.p2";

    TargetPlatform declaredPlatform{};
    if (declarePlatform) {
        declaredPlatform = genTargetPlatform();
        m.compat.platform = declaredPlatform;
    }

    GdVersionRange declaredRange{};
    if (declareVersion) {
        declaredRange.min = genSmallSemVer();
        declaredRange.max = genSmallSemVer();  // anche min>max (intervallo vuoto)
        m.compat.gdVersion = declaredRange;
    }

    // --- Oracolo indipendente ---------------------------------------------
    const bool compatPresent = declarePlatform && declareVersion;

    const std::optional<TargetPlatform> rtTarget = oracleRuntimeTarget(ctx.platform);
    const bool platformMatches =
        rtTarget.has_value() && declarePlatform && *rtTarget == declaredPlatform;

    const SemVer promoted{ctx.gdVersion.major, ctx.gdVersion.minor, 0};
    const bool versionInRange =
        declareVersion && (declaredRange.min <= promoted) &&
        (promoted <= declaredRange.max);

    // Causa attesa secondo l'ordine deterministico: compat → piattaforma → versione.
    CompatCause expectedCause;
    if (!compatPresent) {
        expectedCause = CompatCause::MissingCompat;
    } else if (!platformMatches) {
        expectedCause = CompatCause::PlatformMismatch;
    } else if (!versionInRange) {
        expectedCause = CompatCause::VersionOutOfRange;
    } else {
        expectedCause = CompatCause::Ok;
    }
    const bool expectedCompatible =
        compatPresent && platformMatches && versionInRange;

    // --- Predicato sotto test ---------------------------------------------
    const CompatResult res = check_compatibility(m, ctx);

    // (1) IFF di ammissione: ammessa sse compat presente && piattaforma
    //     coincidente && versione nell'intervallo inclusivo (Req 3.2/3.4/3.6).
    RC_ASSERT(res.compatible == expectedCompatible);

    // (2) Coerenza compatible ⇔ cause==Ok.
    RC_ASSERT(res.compatible == (res.cause == CompatCause::Ok));

    // (3) Causa esatta dell'esito (ESATTAMENTE una causa, Req 3.3/3.4/3.6).
    RC_ASSERT(res.cause == expectedCause);

    // (4) In caso di esclusione, la causa è una delle tre dell'insieme chiuso
    //     e mai Ok; il messaggio diagnostico non è vuoto.
    if (!res.compatible) {
        RC_ASSERT(res.cause == CompatCause::MissingCompat ||
                  res.cause == CompatCause::PlatformMismatch ||
                  res.cause == CompatCause::VersionOutOfRange);
        RC_ASSERT(!res.message.empty());
    }
}

}  // namespace
