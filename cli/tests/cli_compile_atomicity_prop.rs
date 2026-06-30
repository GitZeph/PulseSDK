//! Property test P8 — atomicità del fallimento di compilazione della CLI.
//!
//! Feature: pulse-gd-integration, Property 8
//! Validates: Requisiti 6.3
//!
//! Per ogni insieme NON VUOTO di cause di fallimento della compilazione,
//! `build_with` (il seam di test della build) deve:
//!
//!   1. interrompere la build restituendo `Err(BuildError::CompilationFailed)`;
//!   2. propagare ESATTAMENTE le cause iniettate dal compilatore;
//!   3. NON produrre alcun Pulse Package `.pulse`;
//!   4. NON lasciare alcun pacchetto parziale su disco nella directory di output.
//!
//! Il fallimento è iniettato tramite un `Compiler` che ritorna sempre
//! `Err(causes)`; il manifest di progetto è valido, così l'unica fonte di
//! fallimento è la compilazione (Req 6.3). Ogni caso usa una directory
//! temporanea isolata per evitare conflitti tra esecuzioni concorrenti.

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use proptest::prelude::*;

use pulse_cli::builder::{build_with, BuildError, CompileOutput, Compiler, MANIFEST_FILE};
use pulse_cli::manifest::Manifest;

/// Compilatore che fallisce sempre con le cause indicate (Req 6.3).
struct FailingCompiler {
    causes: Vec<String>,
}

impl Compiler for FailingCompiler {
    fn compile(&self, _dir: &Path, _m: &Manifest) -> Result<CompileOutput, Vec<String>> {
        Err(self.causes.clone())
    }
}

/// Directory temporanea isolata, con pulizia automatica alla `Drop`.
struct TempGuard {
    path: PathBuf,
}

impl TempGuard {
    fn new() -> Self {
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let n = COUNTER.fetch_add(1, Ordering::Relaxed);
        let pid = std::process::id();
        let path = std::env::temp_dir().join(format!("pulse-p8-{pid}-{n}"));
        let _ = fs::remove_dir_all(&path);
        fs::create_dir_all(&path).unwrap();
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

/// Scrive un `pulse.toml` VALIDO nella directory data, così che l'unica fonte
/// di fallimento della build sia la compilazione iniettata.
fn write_valid_manifest(dir: &Path) {
    let manifest = r#"schema_version = 1

[mod]
id = "com.example.p8"
version = "1.0.0"
name = "P8 Mod"
type = "native"

[[entry_points]]
kind = "init"
symbol = "pulse_mod_init"
"#;
    fs::write(dir.join(MANIFEST_FILE), manifest).unwrap();
}

/// Conta i file `.pulse` presenti (non ricorsivo) in `dir`.
fn count_pulse_files(dir: &Path) -> usize {
    fs::read_dir(dir)
        .map(|rd| {
            rd.filter_map(|e| e.ok())
                .filter(|e| e.path().extension().map(|x| x == "pulse").unwrap_or(false))
                .count()
        })
        .unwrap_or(0)
}

// ---------------------------------------------------------------------------
// Strategie di generazione.
// ---------------------------------------------------------------------------

/// Singola causa di fallimento: testo stampabile, può contenere spazi e
/// dettagli tipici di una diagnostica di toolchain.
fn arb_cause() -> impl Strategy<Value = String> {
    proptest::string::string_regex("[a-zA-Z0-9 ._:/()-]{1,60}").unwrap()
}

/// Insieme NON VUOTO di cause di fallimento (1..=8 elementi).
fn arb_causes() -> impl Strategy<Value = Vec<String>> {
    prop::collection::vec(arb_cause(), 1..=8)
}

// ---------------------------------------------------------------------------
// Property 8 — atomicità del fallimento di compilazione (Req 6.3).
// ---------------------------------------------------------------------------

proptest! {
    // Almeno 100 iterazioni (default proptest = 256).
    #![proptest_config(ProptestConfig::with_cases(256))]

    /// Feature: pulse-gd-integration, Property 8 — Validates: Requisiti 6.3
    #[test]
    fn prop8_compile_failure_is_atomic(causes in arb_causes()) {
        let guard = TempGuard::new();
        let dir = guard.path();
        write_valid_manifest(dir);

        let compiler = FailingCompiler {
            causes: causes.clone(),
        };

        // La build deve fallire (build interrotta) con CompilationFailed.
        let result = build_with(dir, &compiler, dir);
        match result {
            Err(BuildError::CompilationFailed(propagated)) => {
                // Le cause iniettate sono propagate esattamente (Req 6.3).
                prop_assert_eq!(propagated, causes);
            }
            other => prop_assert!(
                false,
                "atteso BuildError::CompilationFailed, ottenuto: {:?}",
                other
            ),
        }

        // Nessun Pulse Package `.pulse`, nessun pacchetto parziale su disco.
        prop_assert_eq!(count_pulse_files(dir), 0);
    }
}
