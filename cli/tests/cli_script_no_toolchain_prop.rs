//! Property test P9 — le script mod non invocano la toolchain nativa.
//!
//! Feature: pulse-gd-integration, Property 9
//! Validates: Requisiti 6.6
//!
//! *Per ogni* `Manifest` di tipo **script** che supera la validazione, la
//! compilazione attraverso il vero [`NativeCompiler`] impacchetta i sorgenti
//! **senza** invocare la Native_Toolchain (Req 6.6).
//!
//! La proprietà è verificata tramite due seam osservabili indipendenti, che
//! devono valere entrambi per ogni caso generato:
//!
//!   (A) **Toolchain sabotata (unix).** Prima della compilazione si installa
//!       una toolchain C++ fittizia (`CXX` punta a uno script che, se mai
//!       eseguito, scrive un file *marker* e fallisce). Se il ramo script
//!       regredisse e cadesse in `compile_native`, `discover_toolchain`
//!       risolverebbe `CXX`, lo eseguirebbe e il marker comparirebbe. Per una
//!       script mod il marker NON deve mai comparire: la toolchain non è stata
//!       invocata.
//!
//!   (B) **Artefatti dai sorgenti.** Gli artefatti prodotti devono coincidere
//!       byte-per-byte con i file sorgente impacchettati (stesso percorso
//!       relativo, stessi byte) e nessuno di essi deve essere il modulo nativo
//!       della piattaforma (`<platform>.<dylib|dll|so>`): prova che non c'è
//!       stata compilazione nativa, ma solo impacchettamento dei sorgenti.
//!
//! Il test non modifica alcun file sotto `cli/src/*` né altri test: usa
//! esclusivamente le API pubbliche `NativeCompiler`/`Compiler`.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use proptest::prelude::*;

use pulse_cli::builder::Compiler;
use pulse_cli::manifest::{
    Dependency, EntryPoint, Manifest, ModInfo, ModType, Permissions, SemVer, SettingDecl,
    SettingValue, VersionConstraint,
};
use pulse_cli::native_compiler::{NativeCompiler, TargetPlatform};

// ---------------------------------------------------------------------------
// Directory di progetto temporanea con pulizia automatica.
// ---------------------------------------------------------------------------

/// Progetto isolato su filesystem, eliminato al `Drop`.
struct TempProject {
    path: PathBuf,
}

impl TempProject {
    fn new() -> Self {
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let n = COUNTER.fetch_add(1, Ordering::Relaxed);
        let pid = std::process::id();
        let path = std::env::temp_dir().join(format!("pulse-p9-{pid}-{n}"));
        let _ = fs::remove_dir_all(&path);
        fs::create_dir_all(&path).unwrap();
        TempProject { path }
    }

    fn path(&self) -> &Path {
        &self.path
    }
}

impl Drop for TempProject {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

// ---------------------------------------------------------------------------
// Toolchain sabotata (seam osservabile, solo unix).
// ---------------------------------------------------------------------------

/// Installa una toolchain C++ fittizia una sola volta e restituisce il percorso
/// del file *marker*. Lo script, se invocato, scrive il marker ed esce con
/// errore — così qualsiasi invocazione accidentale della toolchain per una
/// script mod sarebbe osservabile (marker presente) e farebbe fallire il test.
#[cfg(unix)]
fn sabotaged_toolchain_marker() -> PathBuf {
    use std::os::unix::fs::PermissionsExt;
    use std::sync::OnceLock;

    static ONCE: OnceLock<PathBuf> = OnceLock::new();
    ONCE.get_or_init(|| {
        let dir = std::env::temp_dir().join(format!("pulse-p9-toolchain-{}", std::process::id()));
        let _ = fs::create_dir_all(&dir);
        let marker = dir.join("toolchain-was-invoked.marker");
        let script = dir.join("fake-cxx.sh");
        let body = format!("#!/bin/sh\necho invoked > \"{}\"\nexit 1\n", marker.display());
        fs::write(&script, body).unwrap();
        let mut perms = fs::metadata(&script).unwrap().permissions();
        perms.set_mode(0o755);
        fs::set_permissions(&script, perms).unwrap();
        // `discover_toolchain` consulta per primo `CXX`: lo facciamo puntare alla
        // toolchain fittizia (path esplicito con separatore → risolto come file).
        std::env::set_var("CXX", &script);
        marker
    })
    .clone()
}

// ---------------------------------------------------------------------------
// Strategie di generazione.
// ---------------------------------------------------------------------------

/// Token non vuoto, stampabile, senza spazi/control-char (id, kind, symbol,
/// nomi di setting).
fn arb_token() -> impl Strategy<Value = String> {
    proptest::string::string_regex("[a-zA-Z0-9._-]{1,40}").unwrap()
}

/// Testo libero (eventualmente vuoto) per `mod.name`.
fn arb_text() -> impl Strategy<Value = String> {
    proptest::string::string_regex("[a-zA-Z0-9 ._-]{0,30}").unwrap()
}

fn arb_semver() -> impl Strategy<Value = SemVer> {
    (0u32..10_000, 0u32..10_000, 0u32..10_000).prop_map(|(a, b, c)| SemVer::new(a, b, c))
}

fn arb_constraint() -> impl Strategy<Value = VersionConstraint> {
    (arb_semver(), proptest::option::of(arb_semver())).prop_map(|(min, max)| match max {
        Some(max_exclusive) => VersionConstraint::range(min, max_exclusive),
        None => VersionConstraint::at_least(min),
    })
}

fn arb_dependency() -> impl Strategy<Value = Dependency> {
    (arb_token(), arb_constraint()).prop_map(|(id, version)| Dependency { id, version })
}

/// Permesso riconosciuto dallo schema (Req 17.1).
fn arb_permission() -> impl Strategy<Value = String> {
    prop_oneof![
        Just("network"),
        Just("filesystem"),
        Just("hooking"),
        Just("ui"),
        Just("events"),
    ]
    .prop_map(|s| s.to_string())
}

/// Float finito (no NaN/inf) per round-trip stabile dei default `float`.
fn arb_finite_f64() -> impl Strategy<Value = f64> {
    any::<f64>().prop_filter("solo float finiti", |f| f.is_finite())
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

/// `Manifest` VALIDO di tipo **script**: id non vuoto, almeno un entry point,
/// dipendenze/permessi/settings ben formati. `mod_type` è sempre `Script`.
fn arb_script_manifest() -> impl Strategy<Value = Manifest> {
    (
        1i32..100,
        arb_token(),
        arb_semver(),
        arb_text(),
        prop::collection::vec((arb_token(), arb_token()), 1..4),
        prop::collection::vec(arb_dependency(), 0..3),
        prop::collection::vec(arb_permission(), 0..3),
        prop::collection::vec(arb_setting(), 0..3),
    )
        .prop_map(
            |(schema_version, id, version, name, eps, dependencies, perms, settings)| Manifest {
                schema_version,
                mod_info: ModInfo {
                    id,
                    version,
                    name,
                    mod_type: ModType::Script,
                },
                entry_points: eps
                    .into_iter()
                    .map(|(kind, symbol)| EntryPoint { kind, symbol })
                    .collect(),
                dependencies,
                permissions: Permissions { required: perms },
                settings,
                offsets: Vec::new(),
            },
        )
}

/// Insieme non vuoto di sorgenti script `(percorso_relativo, byte)` con
/// percorsi univoci (alcuni annidati in sottocartelle), per esercitare anche
/// l'impacchettamento ricorsivo.
fn arb_sources() -> impl Strategy<Value = Vec<(String, Vec<u8>)>> {
    prop::collection::vec(prop::collection::vec(any::<u8>(), 0..64), 1..6).prop_map(|contents| {
        contents
            .into_iter()
            .enumerate()
            .map(|(i, bytes)| {
                let rel = if i % 2 == 0 {
                    format!("mod{i}.script")
                } else {
                    format!("sub/helper{i}.lua")
                };
                (rel, bytes)
            })
            .collect()
    })
}

// ---------------------------------------------------------------------------
// Property 9 — le script mod non invocano la toolchain nativa (Req 6.6).
// ---------------------------------------------------------------------------

proptest! {
    // Almeno 100 iterazioni (default proptest = 256).
    #![proptest_config(ProptestConfig::with_cases(256))]

    /// Feature: pulse-gd-integration, Property 9 — Validates: Requisiti 6.6
    #[test]
    fn prop9_script_mods_skip_native_toolchain(
        manifest in arb_script_manifest(),
        sources in arb_sources(),
    ) {
        // Precondizione: il manifest di tipo script è valido e davvero `Script`.
        let validation = manifest.validate();
        prop_assert!(
            validation.ok(),
            "manifest script non valido: {:?}",
            validation.violations
        );
        prop_assert_eq!(manifest.mod_info.mod_type, ModType::Script);

        // (A) Seam osservabile (unix): installa la toolchain sabotata e azzera il
        //     marker prima della compilazione.
        #[cfg(unix)]
        let marker = {
            let m = sabotaged_toolchain_marker();
            let _ = fs::remove_file(&m);
            m
        };

        // Progetto isolato con i sorgenti script sotto `src/`.
        let proj = TempProject::new();
        let src_dir = proj.path().join("src");
        fs::create_dir_all(&src_dir).unwrap();
        let mut expected: BTreeMap<String, Vec<u8>> = BTreeMap::new();
        for (rel, bytes) in &sources {
            let full = src_dir.join(rel);
            fs::create_dir_all(full.parent().unwrap()).unwrap();
            fs::write(&full, bytes).unwrap();
            expected.insert(rel.clone(), bytes.clone());
        }

        // Compilazione della script mod attraverso il vero NativeCompiler.
        let compiler = NativeCompiler::new(TargetPlatform::host());
        let out = compiler
            .compile(proj.path(), &manifest)
            .map_err(|e| TestCaseError::fail(format!("impacchettamento script fallito: {e:?}")))?;

        // (A) La toolchain nativa non è mai stata invocata: nessun marker.
        #[cfg(unix)]
        prop_assert!(
            !marker.exists(),
            "la toolchain nativa è stata invocata per una script mod"
        );

        // (B) Gli artefatti provengono dai sorgenti (byte-per-byte) e nessun
        //     modulo nativo della piattaforma è stato prodotto.
        let native_name = TargetPlatform::host().module_file_name();
        let mut got: BTreeMap<String, Vec<u8>> = BTreeMap::new();
        for a in &out.artifacts {
            prop_assert_ne!(
                a.file_name.as_str(),
                native_name,
                "prodotto un modulo nativo '{}' per una script mod",
                native_name
            );
            got.insert(a.file_name.clone(), a.bytes.clone());
        }
        prop_assert_eq!(
            got,
            expected,
            "gli artefatti non corrispondono ai sorgenti impacchettati"
        );
    }
}
