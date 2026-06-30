//! Unit/integration test della CLI Pulse (task 14.6).
//!
//! Feature: pulse-sdk — Validates: Requisiti 14.2, 14.4, 14.5
//!
//! Questo file esercita i comportamenti chiave della CLI attraverso la sua API
//! pubblica (`pulse_cli::scaffold`, `pulse_cli::builder`), in modo end-to-end e
//! indipendente dai test inline dei moduli:
//!
//!   - `pulse new` su directory VUOTA vs NON vuota (Req 14.2): la prima genera
//!     un progetto con `pulse.toml` valido; la seconda viene interrotta senza
//!     modificare alcun file esistente.
//!   - `pulse build` con Manifest NON valido (Req 14.4): build interrotta,
//!     elenco completo dei campi non conformi, nessun pacchetto `.pulse`.
//!   - `pulse build` con compilazione fallita (Req 14.5): build interrotta,
//!     cause segnalate, nessun pacchetto `.pulse`.
//!
//! I test usano una directory temporanea isolata con pulizia automatica
//! (pattern `TempGuard`), senza dipendenze aggiuntive.

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use pulse_cli::builder::{self, BuildError, CompileOutput, Compiler, MANIFEST_FILE};
use pulse_cli::manifest::Manifest;
use pulse_cli::scaffold::{self, ScaffoldError};

/// Directory temporanea isolata per i test, con pulizia automatica al `Drop`.
struct TempGuard {
    path: PathBuf,
}

impl TempGuard {
    fn new(tag: &str) -> Self {
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let n = COUNTER.fetch_add(1, Ordering::Relaxed);
        let pid = std::process::id();
        let path = std::env::temp_dir().join(format!("pulse-cli-it-{tag}-{pid}-{n}"));
        let _ = fs::remove_dir_all(&path);
        TempGuard { path }
    }

    fn path(&self) -> &Path {
        &self.path
    }
}

impl Drop for TempGuard {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

/// Compilatore iniettabile che fallisce sempre con le cause indicate (Req 14.5).
struct AlwaysFailCompiler {
    causes: Vec<String>,
}

impl Compiler for AlwaysFailCompiler {
    fn compile(&self, _dir: &Path, _m: &Manifest) -> Result<CompileOutput, Vec<String>> {
        Err(self.causes.clone())
    }
}

/// Restituisce i file con estensione `.pulse` presenti nella directory.
fn pulse_packages_in(dir: &Path) -> Vec<PathBuf> {
    fs::read_dir(dir)
        .map(|rd| {
            rd.filter_map(|e| e.ok())
                .map(|e| e.path())
                .filter(|p| p.extension().map(|x| x == "pulse").unwrap_or(false))
                .collect()
        })
        .unwrap_or_default()
}

// ---------------------------------------------------------------------------
// Req 14.2 — `pulse new` su directory vuota vs non vuota.
// ---------------------------------------------------------------------------

/// Req 14.2 — scaffold su directory vuota: genera un progetto con `pulse.toml`
/// valido e i sorgenti iniziali.
#[test]
fn cli_scaffold_into_empty_dir_generates_valid_project() {
    let guard = TempGuard::new("scaffold-empty");
    let dest = guard.path();
    // Directory esistente ma vuota.
    fs::create_dir_all(dest).unwrap();

    let outcome = scaffold::scaffold_new(dest, Some("com.example.cli")).expect("scaffold");

    assert_eq!(outcome.mod_id, "com.example.cli");
    assert!(dest.join("pulse.toml").is_file(), "manca pulse.toml");
    assert!(dest.join("src").is_dir(), "manca src/");

    // Il manifest generato è valido e conforme allo schema.
    let text = fs::read_to_string(dest.join("pulse.toml")).unwrap();
    let manifest = Manifest::parse(&text).expect("manifest parsabile");
    assert!(
        manifest.validate().ok(),
        "il manifest scaffoldato deve essere conforme allo schema"
    );
}

/// Req 14.2 — scaffold su directory NON vuota: interrotto con conflitto, i file
/// preesistenti restano invariati e non viene creato alcun artefatto.
#[test]
fn cli_scaffold_into_non_empty_dir_aborts_without_touching_files() {
    let guard = TempGuard::new("scaffold-nonempty");
    let dest = guard.path();
    fs::create_dir_all(dest).unwrap();

    let preexisting = dest.join("keep.txt");
    let original = "non toccare questo file";
    fs::write(&preexisting, original).unwrap();

    let err = scaffold::scaffold_new(dest, Some("com.example.conflict")).unwrap_err();
    assert!(
        matches!(err, ScaffoldError::DirectoryConflict(_)),
        "atteso DirectoryConflict, ottenuto: {err:?}"
    );

    // File preesistente intatto, nessun artefatto di scaffolding creato (Req 14.2).
    assert_eq!(fs::read_to_string(&preexisting).unwrap(), original);
    assert!(!dest.join("pulse.toml").exists());
    assert!(!dest.join("src").exists());
}

// ---------------------------------------------------------------------------
// Req 14.4 — `pulse build` interrotto su Manifest non valido.
// ---------------------------------------------------------------------------

/// Req 14.4 — build con Manifest non conforme: interrotta con l'elenco completo
/// dei campi non conformi e senza produrre alcun pacchetto.
#[test]
fn cli_build_aborts_on_invalid_manifest_listing_fields_and_no_package() {
    let guard = TempGuard::new("build-invalid");
    let dir = guard.path();
    fs::create_dir_all(dir).unwrap();

    // Manifest analizzabile ma NON conforme: id vuoto e nessun entry point.
    let bad = r#"
        schema_version = 1

        [mod]
        id = ""
        version = "1.0.0"
        name = "Bad"
        type = "native"
    "#;
    fs::write(dir.join(MANIFEST_FILE), bad).unwrap();
    fs::create_dir_all(dir.join("src")).unwrap();
    fs::write(dir.join("src/mod.cpp"), b"int x;").unwrap();

    let err = builder::build(dir).unwrap_err();
    match err {
        BuildError::InvalidManifest(violations) => {
            assert!(!violations.is_empty(), "atteso elenco di campi non conformi");
            let fields: Vec<String> = violations.iter().map(|v| v.field.clone()).collect();
            assert!(fields.iter().any(|f| f == "mod.id"), "campi: {fields:?}");
            assert!(
                fields.iter().any(|f| f == "entry_points"),
                "campi: {fields:?}"
            );
        }
        other => panic!("atteso InvalidManifest, ottenuto: {other:?}"),
    }

    // Nessun pacchetto prodotto (Req 14.4).
    assert!(
        pulse_packages_in(dir).is_empty(),
        "nessun .pulse atteso su manifest non valido"
    );
}

// ---------------------------------------------------------------------------
// Req 14.5 — `pulse build` interrotto su compilazione fallita.
// ---------------------------------------------------------------------------

/// Req 14.5 — build con compilazione fallita (compilatore iniettato): interrotta
/// con le cause segnalate e senza produrre alcun pacchetto.
#[test]
fn cli_build_aborts_on_compile_failure_with_causes_and_no_package() {
    let guard = TempGuard::new("build-compilefail");
    let dir = guard.path();
    // Progetto valido scaffoldato: il manifest passa la validazione, così la
    // build raggiunge il passo di compilazione.
    scaffold::scaffold_new(dir, Some("com.example.cf")).unwrap();

    let compiler = AlwaysFailCompiler {
        causes: vec![
            "src/mod.cpp:42: errore di compilazione".to_string(),
            "linker: simbolo non risolto pulse_mod_init".to_string(),
        ],
    };

    let err = builder::build_with(dir, &compiler, dir).unwrap_err();
    match err {
        BuildError::CompilationFailed(causes) => {
            assert_eq!(causes.len(), 2, "attese 2 cause: {causes:?}");
            assert!(causes.iter().any(|c| c.contains("errore di compilazione")));
            assert!(causes.iter().any(|c| c.contains("simbolo non risolto")));
        }
        other => panic!("atteso CompilationFailed, ottenuto: {other:?}"),
    }

    // Nessun pacchetto prodotto (Req 14.5).
    assert!(
        pulse_packages_in(dir).is_empty(),
        "nessun .pulse atteso su compilazione fallita"
    );
}
