//! Unit/integration test degli **example** dell'Installer (task 7.7).
//!
//! Verificano, attraverso la **API pubblica** di `pulse_cli::installer` e su un
//! **albero di GD finto**, i tre example della spec:
//!
//!   - **7.5** — un target non riconosciuto è rifiutato **senza modificare
//!     nulla** (sia da `recognize_gd`, sia dall'`install` end-to-end).
//!   - **7.8** — `uninstall` senza restore record fallisce con
//!     "restore record mancante" e **non cambia nulla**.
//!   - **7.9** — un `install` quando il Pulse_Loader è **già presente** è
//!     rifiutato **senza modificare nulla**.
//!
//! Questi test vivono in un file di integrazione separato per non toccare i
//! test inline di `cli/src/installer.rs` (di proprietà di altre attività) e
//! usano esclusivamente i simboli esportati dal crate.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use pulse_cli::installer::{
    install, recognize_gd, uninstall, InstallError, EXPECTED_BUNDLE_ID, GD_EXECUTABLE_REL,
    LOADER_DYLIB_NAME,
};

// ---------------------------------------------------------------------------
// Helper di test (locali a questo file di integrazione).
// ---------------------------------------------------------------------------

/// Directory temporanea isolata con pulizia automatica.
struct TempGuard {
    path: PathBuf,
}

impl TempGuard {
    fn new(tag: &str) -> Self {
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let n = COUNTER.fetch_add(1, Ordering::Relaxed);
        let pid = std::process::id();
        let path = std::env::temp_dir().join(format!("pulse-installer-ex-{tag}-{pid}-{n}"));
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

/// Costruisce un albero di GD finto valido sotto `root` e restituisce il path
/// del bundle `.app`.
fn fake_gd_bundle(root: &Path) -> PathBuf {
    let app = root.join("Geometry Dash.app");
    let macos = app.join("Contents/MacOS");
    fs::create_dir_all(&macos).unwrap();
    fs::create_dir_all(app.join("Contents/_CodeSignature")).unwrap();

    // Eseguibile Mach-O 64-bit (magic feedfacf, byte-swapped) + payload.
    let mut exe = vec![0xcf, 0xfa, 0xed, 0xfe];
    exe.extend_from_slice(b"fake geometry dash mach-o body");
    fs::write(macos.join("Geometry Dash"), &exe).unwrap();

    // Info.plist XML che dichiara il CFBundleIdentifier atteso.
    let plist = format!(
        "<?xml version=\"1.0\"?>\n<plist><dict>\n\
         <key>CFBundleIdentifier</key>\n<string>{EXPECTED_BUNDLE_ID}</string>\n\
         </dict></plist>\n"
    );
    fs::write(app.join("Contents/Info.plist"), plist).unwrap();
    fs::write(
        app.join("Contents/_CodeSignature/CodeResources"),
        b"<original code resources>",
    )
    .unwrap();

    app
}

/// Crea un file artefatto (dylib sorgente finta) e ne restituisce il path.
fn fake_artifact(dir: &Path, contents: &[u8]) -> PathBuf {
    let artifact = dir.join("libpulse_loader.dylib");
    fs::write(&artifact, contents).unwrap();
    artifact
}

/// Cattura lo stato (path relativo → byte) di tutti i file sotto `dir`, per
/// verificare l'invarianza byte-per-byte.
fn snapshot(dir: &Path) -> BTreeMap<PathBuf, Vec<u8>> {
    let mut out = BTreeMap::new();
    let mut stack = vec![dir.to_path_buf()];
    while let Some(current) = stack.pop() {
        for entry in fs::read_dir(&current).unwrap() {
            let path = entry.unwrap().path();
            if path.is_dir() {
                stack.push(path);
            } else {
                let rel = path.strip_prefix(dir).unwrap().to_path_buf();
                out.insert(rel, fs::read(&path).unwrap());
            }
        }
    }
    out
}

// ---------------------------------------------------------------------------
// Example 7.5 — target non riconosciuto rifiutato senza modifiche.
// ---------------------------------------------------------------------------

/// Req 7.5 — `recognize_gd` rifiuta un path che non è una `GD_Installation`
/// (manca `Contents/MacOS/Geometry Dash`) lasciando il path invariato.
#[test]
fn example_7_5_recognize_rejects_unrecognized_target_without_changes() {
    let guard = TempGuard::new("75-recognize");
    // Directory che NON è un bundle GD valido.
    let not_gd = guard.path().join("Some Other.app");
    fs::create_dir_all(not_gd.join("Contents/MacOS")).unwrap();
    fs::write(not_gd.join("Contents/MacOS/Other"), b"nope").unwrap();

    let before = snapshot(guard.path());
    let err = recognize_gd(&not_gd).unwrap_err();
    assert!(
        matches!(err, InstallError::UnrecognizedInstallation { .. }),
        "atteso UnrecognizedInstallation, trovato: {err:?}"
    );
    // Nulla è stato modificato (Req 7.5).
    assert_eq!(before, snapshot(guard.path()), "il path non deve cambiare");
}

/// Req 7.5 — un `install` end-to-end su un target non riconosciuto è rifiutato
/// e non modifica alcun file (né il target né l'artefatto).
#[test]
fn example_7_5_install_refuses_unrecognized_target_without_changes() {
    let guard = TempGuard::new("75-install");
    let not_gd = guard.path().join("Some Other.app");
    fs::create_dir_all(not_gd.join("Contents/MacOS")).unwrap();
    fs::write(not_gd.join("Contents/MacOS/Other"), b"nope").unwrap();
    let artifact = fake_artifact(guard.path(), b"loader dylib payload");

    let before = snapshot(guard.path());
    let err = install(&not_gd, &artifact).unwrap_err();
    assert!(
        matches!(err, InstallError::UnrecognizedInstallation { .. }),
        "atteso UnrecognizedInstallation, trovato: {err:?}"
    );
    // Nessun file modificato (Req 7.5).
    assert_eq!(before, snapshot(guard.path()), "nulla deve cambiare");
}

// ---------------------------------------------------------------------------
// Example 7.8 — uninstall senza restore record: errore, nulla cambia.
// ---------------------------------------------------------------------------

/// Req 7.8 — `uninstall` su un bundle senza restore record fallisce con
/// `MissingRestoreRecord` e lascia l'albero byte-per-byte invariato.
#[test]
fn example_7_8_uninstall_without_restore_record_errors_and_changes_nothing() {
    let guard = TempGuard::new("78-uninstall");
    let app = fake_gd_bundle(guard.path());

    let before = snapshot(&app);
    let err = uninstall(&app).unwrap_err();
    match err {
        InstallError::MissingRestoreRecord(path) => assert_eq!(path, app),
        other => panic!("atteso MissingRestoreRecord, trovato: {other:?}"),
    }
    // Nulla è cambiato (Req 7.8).
    assert_eq!(before, snapshot(&app), "l'albero non deve cambiare");
}

// ---------------------------------------------------------------------------
// Example 7.9 — install già presente: rifiutato senza modifiche.
// ---------------------------------------------------------------------------

/// Req 7.9 — un secondo `install` quando il Pulse_Loader è già installato è
/// rifiutato con `AlreadyInstalled` e non modifica ulteriormente l'albero.
#[test]
fn example_7_9_install_when_already_installed_is_refused_without_changes() {
    let guard = TempGuard::new("79-install");
    let app = fake_gd_bundle(guard.path());
    let artifact = fake_artifact(guard.path(), b"loader dylib payload");

    // Prima install: deve riuscire e lasciare un restore record.
    install(&app, &artifact).expect("la prima install deve riuscire");
    let after_first = snapshot(&app);

    // Seconda install: rifiutata perché già installato (Req 7.9).
    let err = install(&app, &artifact).unwrap_err();
    assert!(
        matches!(err, InstallError::AlreadyInstalled { .. }),
        "atteso AlreadyInstalled, trovato: {err:?}"
    );
    // La seconda install non ha modificato nulla.
    assert_eq!(
        after_first,
        snapshot(&app),
        "una seconda install non deve modificare l'albero"
    );

    // Sanity: la dylib del loader è effettivamente presente nel bundle.
    assert!(
        app.join("Contents/MacOS").join(LOADER_DYLIB_NAME).is_file(),
        "la dylib del loader deve essere presente dopo la prima install"
    );
    // Sanity: il target è ancora un bundle riconosciuto e l'eseguibile esiste.
    assert!(app.join(GD_EXECUTABLE_REL).is_file());
}
