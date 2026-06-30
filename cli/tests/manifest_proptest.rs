//! Property test P34 — round-trip del Manifest (lato Rust).
//!
//! Feature: pulse-sdk, Property 34
//! Validates: Requisiti 16.5
//!
//! Controparte Rust della Property 34 (il lato C++ è il task 13.4). Per ogni
//! `Manifest` VALIDO generato casualmente vale:
//!
//!   1. round-trip:           `parse(serialize(m)) == m`
//!   2. punto fisso canonico: `serialize(parse(serialize(m))) == serialize(m)`
//!
//! Le stringhe generate (id, nomi, permessi, default `string`) sono limitate a
//! caratteri stampabili e privi di control-char: il `toml` crate rifiuterebbe
//! (o escaperebbe in modo non banale) i control-char, mentre la proprietà sotto
//! esame è il round-trip della SERIALIZZAZIONE, quindi qualunque stringa che
//! `serde`/`toml` round-trippa è sufficiente. I default tipizzati sono coerenti
//! col campo `type` (int->Int, float->Float finito, bool->Bool, string->Str).

use proptest::prelude::*;

use pulse_cli::manifest::{
    Dependency, EntryPoint, Manifest, ModInfo, ModType, Permissions, SemVer, SettingDecl,
    SettingValue, VersionConstraint,
};

// ---------------------------------------------------------------------------
// Strategie di generazione.
// ---------------------------------------------------------------------------

/// Stringa stampabile e priva di control-char (consente lo spazio). Può essere
/// vuota: adatta a campi liberi come `mod.name`.
fn arb_text() -> impl Strategy<Value = String> {
    proptest::string::string_regex("[a-zA-Z0-9 ._-]{0,30}").unwrap()
}

/// Token non vuoto, stampabile e senza spazi/control-char: id, kind, symbol,
/// nomi di setting e permessi.
fn arb_token() -> impl Strategy<Value = String> {
    proptest::string::string_regex("[a-zA-Z0-9._-]{1,40}").unwrap()
}

/// Float finito (no NaN/inf) entro un intervallo ampio ma stabile per il
/// round-trip testuale.
fn arb_finite_f64() -> impl Strategy<Value = f64> {
    any::<f64>().prop_filter("solo float finiti", |f| f.is_finite())
}

fn arb_semver() -> impl Strategy<Value = SemVer> {
    (0u32..10_000, 0u32..10_000, 0u32..10_000).prop_map(|(a, b, c)| SemVer::new(a, b, c))
}

/// Vincolo di versione: minimo sempre presente, massimo esclusivo opzionale
/// (forme canoniche `>=MIN` e `>=MIN <MAXEXCL`).
fn arb_constraint() -> impl Strategy<Value = VersionConstraint> {
    (arb_semver(), proptest::option::of(arb_semver())).prop_map(|(min, max)| match max {
        Some(max_exclusive) => VersionConstraint::range(min, max_exclusive),
        None => VersionConstraint::at_least(min),
    })
}

fn arb_mod_type() -> impl Strategy<Value = ModType> {
    prop_oneof![Just(ModType::Native), Just(ModType::Script)]
}

fn arb_mod_info() -> impl Strategy<Value = ModInfo> {
    (arb_token(), arb_semver(), arb_text(), arb_mod_type()).prop_map(
        |(id, version, name, mod_type)| ModInfo {
            id,
            version,
            name,
            mod_type,
        },
    )
}

fn arb_entry_point() -> impl Strategy<Value = EntryPoint> {
    (arb_token(), arb_token()).prop_map(|(kind, symbol)| EntryPoint { kind, symbol })
}

fn arb_dependency() -> impl Strategy<Value = Dependency> {
    (arb_token(), arb_constraint()).prop_map(|(id, version)| Dependency { id, version })
}

/// Setting con default tipizzato coerente col campo `type`.
fn arb_setting() -> impl Strategy<Value = SettingDecl> {
    prop_oneof![
        (arb_token(), any::<i64>()).prop_map(|(name, v)| SettingDecl {
            name,
            setting_type: "int".to_string(),
            default: SettingValue::Int(v),
        }),
        (arb_token(), arb_finite_f64()).prop_map(|(name, v)| SettingDecl {
            name,
            setting_type: "float".to_string(),
            default: SettingValue::Float(v),
        }),
        (arb_token(), any::<bool>()).prop_map(|(name, v)| SettingDecl {
            name,
            setting_type: "bool".to_string(),
            default: SettingValue::Bool(v),
        }),
        (arb_token(), arb_text()).prop_map(|(name, v)| SettingDecl {
            name,
            setting_type: "string".to_string(),
            default: SettingValue::Str(v),
        }),
    ]
}

/// Manifest VALIDO casuale: id non vuoto, almeno un entry point, dipendenze,
/// permessi e settings casuali.
fn arb_manifest() -> impl Strategy<Value = Manifest> {
    (
        1i32..100,
        arb_mod_info(),
        prop::collection::vec(arb_entry_point(), 1..4),
        prop::collection::vec(arb_dependency(), 0..4),
        prop::collection::vec(arb_token(), 0..4),
        prop::collection::vec(arb_setting(), 0..4),
    )
        .prop_map(
            |(schema_version, mod_info, entry_points, dependencies, perms, settings)| Manifest {
                schema_version,
                mod_info,
                entry_points,
                dependencies,
                permissions: Permissions { required: perms },
                settings,
            },
        )
}

// ---------------------------------------------------------------------------
// Property 34 — round-trip del Manifest (Req 16.5).
// ---------------------------------------------------------------------------

proptest! {
    // Almeno 100 iterazioni (default proptest = 256).
    #![proptest_config(ProptestConfig::with_cases(256))]

    /// Feature: pulse-sdk, Property 34 — Validates: Requisiti 16.5
    #[test]
    fn prop34_manifest_round_trip(m in arb_manifest()) {
        // (1) round-trip del modello: parse(serialize(m)) == m.
        let serialized = m.serialize().expect("serialize del Manifest");
        let parsed = Manifest::parse(&serialized).expect("parse del Manifest");
        prop_assert_eq!(&parsed, &m);

        // (2) punto fisso canonico: serialize(parse(serialize(m))) == serialize(m).
        let reserialized = parsed.serialize().expect("ri-serialize del Manifest");
        prop_assert_eq!(&reserialized, &serialized);
    }
}
