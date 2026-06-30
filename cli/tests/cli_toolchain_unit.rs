//! Task 5.4 — Unit/integration test della toolchain assente e di `LDFLAGS`
//! rimosso dall'ambiente del processo figlio.
//!
//! Copre due acceptance criteria del Requisito 6 (compilazione nativa nella
//! CLI), esercitando l'API pubblica del crate `pulse-cli`:
//!
//!   - **Req 6.4** — se nessuna toolchain C++ è disponibile, `discover_toolchain`
//!     ritorna `None` e `NativeCompiler` (via `build_with`) interrompe la build
//!     con un errore che identifica la toolchain mancante, **senza produrre alcun
//!     pacchetto `.pulse`**.
//!   - **Req 6.5** — su host macOS la toolchain viene invocata in un ambiente in
//!     cui `LDFLAGS` è rimosso, così che un eventuale `-fuse-ld=mold` globale non
//!     rompa il link AppleClang. Il test usa un compilatore-wrapper iniettato via
//!     `CXX` che registra l'ambiente effettivamente visto dal processo figlio.
//!
//! I test mutano variabili d'ambiente di processo (`PATH`, `CXX`, `LDFLAGS`):
//! sono perciò serializzati da un mutex globale e ripristinano lo stato
//! precedente al termine.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Mutex, MutexGuard, OnceLock};

use pulse_cli::builder::{build_with, BuildError};
use pulse_cli::native_compiler::{discover_toolchain, NativeCompiler, TargetPlatform};

// ---------------------------------------------------------------------------
// Serializzazione delle mutazioni di ambiente.
// ---------------------------------------------------------------------------

/// Mutex globale: i test che toccano l'ambiente di processo non devono girare in
/// parallelo (le variabili d'ambiente sono condivise da tutto il processo di
/// test).
fn env_lock() -> MutexGuard<'static, ()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(|p| p.into_inner())
}

/// Salva e ripristina automaticamente un insieme di variabili d'ambiente.
struct EnvRestore {
    saved: Vec<(String, Option<String>)>,
}

impl EnvRestore {
    fn capture(keys: &[&str]) -> Self {
        let saved = keys
            .iter()
            .map(|k| ((*k).to_string(), std::env::var(k).ok()))
            .collect();
        EnvRestore { saved }
    }
}

impl Drop for EnvRestore {
    fn drop(&mut self) {
        for (k, v) in &self.saved {
            match v {
                Some(val) => std::env::set_var(k, val),
                None => std::env::remove_var(k),
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Directory temporanea con pulizia automatica.
// ---------------------------------------------------------------------------

struct TempDir {
    path: PathBuf,
}

impl TempDir {
    fn new(tag: &str) -> Self {
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let n = COUNTER.fetch_add(1, Ordering::Relaxed);
        let pid = std::process::id();
        let path = std::env::temp_dir().join(format!("pulse-task54-{tag}-{pid}-{n}"));
        let _ = std::fs::remove_dir_all(&path);
        std::fs::create_dir_all(&path).unwrap();
        TempDir { path }
    }

    fn path(&self) -> &Path {
        &self.path
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.path);
    }
}

// ---------------------------------------------------------------------------
// Helper di progetto.
// ---------------------------------------------------------------------------

/// Scrive un progetto di mod **nativa** minimale (pulse.toml valido + un
/// sorgente C++) nella directory indicata.
fn write_native_project(dir: &Path) {
    let manifest = r#"schema_version = 1

[mod]
id = "com.example.native"
version = "0.1.0"
name = "Native"
type = "native"

[[entry_points]]
kind = "init"
symbol = "pulse_mod_init"
"#;
    std::fs::write(dir.join("pulse.toml"), manifest).unwrap();
    let src = dir.join("src");
    std::fs::create_dir_all(&src).unwrap();
    std::fs::write(src.join("mod.cpp"), "extern \"C\" void pulse_mod_init() {}\n").unwrap();
}

/// Conta i file `.pulse` presenti nella directory (non ricorsivo).
fn count_pulse_packages(dir: &Path) -> usize {
    std::fs::read_dir(dir)
        .map(|rd| {
            rd.filter_map(|e| e.ok())
                .filter(|e| {
                    e.path()
                        .extension()
                        .and_then(|x| x.to_str())
                        .map(|x| x == "pulse")
                        .unwrap_or(false)
                })
                .count()
        })
        .unwrap_or(0)
}

// ---------------------------------------------------------------------------
// Req 6.4 — toolchain assente.
// ---------------------------------------------------------------------------

/// Quando nessuna toolchain C++ è individuabile, `discover_toolchain` ritorna
/// `None` e la build nativa fallisce con un errore che identifica la toolchain
/// mancante, **senza** lasciare alcun pacchetto `.pulse` su disco (Req 6.4).
#[test]
fn missing_toolchain_aborts_build_with_no_package() {
    let _guard = env_lock();
    let _restore = EnvRestore::capture(&["PATH", "CXX"]);

    // Ambiente senza alcun compilatore: PATH punta a una directory vuota e
    // l'override esplicito CXX è rimosso.
    let empty_path = TempDir::new("emptypath");
    std::env::set_var("PATH", empty_path.path());
    std::env::remove_var("CXX");

    let target = TargetPlatform::host();

    // 1. La discovery non trova alcuna toolchain.
    assert!(
        discover_toolchain(target).is_none(),
        "con PATH vuoto e CXX assente nessuna toolchain deve essere individuata"
    );

    // 2. La build nativa si interrompe con un errore che nomina la toolchain
    //    mancante.
    let proj = TempDir::new("missing-tc-proj");
    write_native_project(proj.path());
    let out = TempDir::new("missing-tc-out");

    let compiler = NativeCompiler::new(target);
    let err = build_with(proj.path(), &compiler, out.path())
        .expect_err("una toolchain assente deve interrompere la build");

    match err {
        BuildError::CompilationFailed(causes) => {
            assert!(
                causes.iter().any(|c| c.to_lowercase().contains("toolchain")),
                "le cause devono identificare la toolchain mancante: {causes:?}"
            );
        }
        other => panic!("atteso CompilationFailed per toolchain mancante, trovato: {other:?}"),
    }

    // 3. Nessun pacchetto (neanche parziale) lasciato su disco.
    assert_eq!(
        count_pulse_packages(out.path()),
        0,
        "nessun .pulse deve essere prodotto quando la toolchain manca"
    );
    assert_eq!(
        count_pulse_packages(proj.path()),
        0,
        "nessun .pulse deve essere prodotto nella directory di progetto"
    );
}

// ---------------------------------------------------------------------------
// Req 6.5 — LDFLAGS rimosso dall'ambiente del figlio (su macOS).
// ---------------------------------------------------------------------------

/// Crea un compilatore-wrapper eseguibile che (a) registra se `LDFLAGS` è
/// presente nel proprio ambiente nel file indicato da `PULSE_TEST_ENV_DUMP`, e
/// (b) crea il file di output passato dopo `-o` così che la compilazione sia
/// considerata riuscita. Ritorna il percorso dello script.
#[cfg(unix)]
fn write_recording_cxx(dir: &Path) -> PathBuf {
    use std::os::unix::fs::PermissionsExt;

    let script = dir.join("recording_cxx.sh");
    let body = r#"#!/bin/sh
# Registra la presenza di LDFLAGS nell'ambiente di QUESTO processo figlio.
if [ -n "${LDFLAGS+x}" ]; then
  printf 'PRESENT' > "$PULSE_TEST_ENV_DUMP"
else
  printf 'ABSENT' > "$PULSE_TEST_ENV_DUMP"
fi
# Individua l'output (-o <path>) e crealo, così il compile risulta riuscito.
out=""
prev=""
for a in "$@"; do
  if [ "$prev" = "-o" ]; then out="$a"; fi
  prev="$a"
done
if [ -n "$out" ]; then printf 'fake-module' > "$out"; fi
exit 0
"#;
    std::fs::write(&script, body).unwrap();
    let mut perms = std::fs::metadata(&script).unwrap().permissions();
    perms.set_mode(0o755);
    std::fs::set_permissions(&script, perms).unwrap();
    script
}

/// Su macOS, la toolchain nativa è invocata con `LDFLAGS` **assente**
/// dall'ambiente del processo figlio (Req 6.5). Su altre piattaforme il quirk
/// non si applica e la variabile resta visibile (documenta il comportamento
/// platform-specific). Il test inietta un compilatore-wrapper via `CXX` che
/// registra l'ambiente effettivo del figlio: questo esercita il vero
/// `Command`/`env_remove` di `NativeCompiler` senza modificare i sorgenti.
#[cfg(unix)]
#[test]
fn ldflags_stripped_from_child_env_on_macos() {
    let _guard = env_lock();
    let _restore = EnvRestore::capture(&["CXX", "LDFLAGS", "PULSE_TEST_ENV_DUMP"]);

    let tools = TempDir::new("rec-cxx");
    let wrapper = write_recording_cxx(tools.path());
    let dump = tools.path().join("env_dump.txt");

    // Il parent imposta LDFLAGS (il quirk `-fuse-ld=mold`) e indica al wrapper
    // dove registrare l'ambiente che vede.
    std::env::set_var("LDFLAGS", "-fuse-ld=mold");
    std::env::set_var("PULSE_TEST_ENV_DUMP", &dump);
    std::env::set_var("CXX", &wrapper);

    let target = TargetPlatform::host();
    // Il wrapper deve essere selezionato come toolchain (override via CXX).
    let tc = discover_toolchain(target).expect("CXX deve fornire una toolchain");
    assert_eq!(tc.program, wrapper, "CXX deve avere priorità nella discovery");

    let proj = TempDir::new("ldflags-proj");
    write_native_project(proj.path());
    let out = TempDir::new("ldflags-out");

    let compiler = NativeCompiler::new(target);
    let outcome = build_with(proj.path(), &compiler, out.path());

    // Il wrapper "compila" con successo creando il modulo di output, quindi la
    // build deve riuscire e aver invocato il processo figlio.
    assert!(
        outcome.is_ok(),
        "il wrapper deve produrre il modulo e far riuscire la build: {outcome:?}"
    );

    let seen = std::fs::read_to_string(&dump)
        .expect("il wrapper deve aver registrato l'ambiente visto dal figlio");

    if cfg!(target_os = "macos") {
        // Req 6.5: su macOS LDFLAGS è rimosso dall'ambiente del figlio.
        assert_eq!(
            seen, "ABSENT",
            "su macOS LDFLAGS deve essere rimosso dall'ambiente del processo figlio"
        );
    } else {
        // Su altre piattaforme il quirk non si applica: LDFLAGS resta visibile.
        assert_eq!(
            seen, "PRESENT",
            "su piattaforme non-macOS LDFLAGS non viene rimosso"
        );
    }
}
