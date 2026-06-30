//! Property test P12 — rollback transazionale su install fallita.
//!
//! Feature: pulse-gd-integration, Property 12
//! Validates: Requisiti 7.4
//!
//! Su un **albero di GD finto** generato casualmente, per **ogni punto di
//! fallimento iniettato** durante `install_with`, si verifica che (Req 7.4):
//!
//!   1. l'install ritorni un **errore con causa** (`InstallError` il cui
//!      messaggio `Display` è non vuoto);
//!   2. l'albero dei file del bundle sia **byte-per-byte identico** allo stato
//!      pre-installazione — cioè il rollback transazionale abbia ripristinato
//!      ogni file modificato ai byte originali, eliminato i file creati
//!      (la dylib del loader) e rimosso il restore record (`.pulse-backup`).
//!
//! I punti di fallimento iniettati coprono i tre casi del design §6:
//!
//!   - **Patch** — `Injector::patch_lc_load_dylib` fallisce. A quel punto la
//!     dylib è già stata copiata: il rollback deve eliminarla.
//!   - **Resign** — `patch_lc_load_dylib` **riesce e modifica davvero**
//!     l'eseguibile (come il seam host), poi `adhoc_resign_with_disable_lv`
//!     **modifica la firma e fallisce**: il rollback deve ripristinare sia
//!     l'eseguibile sia la firma ai byte originali ed eliminare la dylib.
//!   - **MissingArtifact** — l'artefatto sorgente non esiste, quindi la copia
//!     della dylib fallisce **prima** di qualunque iniezione: nulla deve
//!     restare modificato.
//!
//! Generatori: contenuti binari casuali per eseguibile, firma e artefatto, più
//! un insieme casuale di asset di gioco extra (rumore sotto
//! `Contents/Resources/`) che non devono mai essere alterati dal rollback. Ogni
//! caso usa una directory temporanea isolata con pulizia automatica.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use proptest::prelude::*;

use pulse_cli::installer::{
    install_with, GdInstallation, InstallError, Injector, EXPECTED_BUNDLE_ID,
};

// ---------------------------------------------------------------------------
// Directory temporanea isolata.
// ---------------------------------------------------------------------------

/// Directory temporanea isolata, con pulizia automatica alla `Drop`.
struct TempGuard {
    path: PathBuf,
}

impl TempGuard {
    fn new() -> Self {
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let n = COUNTER.fetch_add(1, Ordering::Relaxed);
        let pid = std::process::id();
        let path = std::env::temp_dir().join(format!("pulse-p12-{pid}-{n}"));
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

// ---------------------------------------------------------------------------
// Albero di GD finto + snapshot byte-per-byte.
// ---------------------------------------------------------------------------

/// Costruisce un albero di GD finto **valido** sotto `root` e restituisce il
/// path del bundle `.app`.
fn build_fake_gd(
    root: &Path,
    exe_body: &[u8],
    sig_body: &[u8],
    extra_assets: &[(String, Vec<u8>)],
) -> PathBuf {
    let app = root.join("Geometry Dash.app");
    let macos = app.join("Contents/MacOS");
    fs::create_dir_all(&macos).unwrap();
    fs::create_dir_all(app.join("Contents/_CodeSignature")).unwrap();
    fs::create_dir_all(app.join("Contents/Resources")).unwrap();

    // Eseguibile Mach-O 64-bit valido: magic feedfacf (big-endian su disco) +
    // corpo casuale, così il riconoscimento (magic Mach-O) passa.
    let mut exe = vec![0xcf, 0xfa, 0xed, 0xfe];
    exe.extend_from_slice(exe_body);
    fs::write(macos.join("Geometry Dash"), &exe).unwrap();

    // Info.plist XML che dichiara il CFBundleIdentifier atteso.
    let plist = format!(
        "<?xml version=\"1.0\"?>\n<plist><dict>\n\
         <key>CFBundleIdentifier</key>\n<string>{EXPECTED_BUNDLE_ID}</string>\n\
         </dict></plist>\n"
    );
    fs::write(app.join("Contents/Info.plist"), plist).unwrap();

    // Firma originale (contenuto casuale).
    fs::write(app.join("Contents/_CodeSignature/CodeResources"), sig_body).unwrap();

    // Asset di gioco extra (rumore): non devono mai essere alterati.
    for (name, bytes) in extra_assets {
        fs::write(app.join("Contents/Resources").join(name), bytes).unwrap();
    }

    app
}

/// Cattura uno snapshot byte-per-byte di tutti i file sotto `root`, indicizzati
/// per percorso relativo a `root`.
fn snapshot_tree(root: &Path) -> BTreeMap<PathBuf, Vec<u8>> {
    let mut out = BTreeMap::new();
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let entries = match fs::read_dir(&dir) {
            Ok(e) => e,
            Err(_) => continue,
        };
        for entry in entries.flatten() {
            let p = entry.path();
            let ty = match entry.file_type() {
                Ok(t) => t,
                Err(_) => continue,
            };
            if ty.is_dir() {
                stack.push(p);
            } else if ty.is_file() {
                let rel = p.strip_prefix(root).unwrap().to_path_buf();
                let bytes = fs::read(&p).unwrap();
                out.insert(rel, bytes);
            }
        }
    }
    out
}

// ---------------------------------------------------------------------------
// Injector che fallisce in un punto iniettato.
// ---------------------------------------------------------------------------

/// I punti di fallimento iniettati durante l'install.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum FailPoint {
    /// La patch `LC_LOAD_DYLIB` fallisce (la dylib è già stata copiata).
    Patch,
    /// La patch riesce e modifica l'eseguibile, poi l'ad-hoc resign modifica la
    /// firma e fallisce.
    Resign,
}

/// Injector che esegue le modifiche fino al punto di fallimento indicato e poi
/// ritorna un errore con causa, così da esercitare il rollback transazionale.
struct FailingInjector {
    fail_at: FailPoint,
}

impl Injector for FailingInjector {
    fn patch_lc_load_dylib(
        &self,
        executable: &Path,
        load_path: &str,
    ) -> Result<(), InstallError> {
        match self.fail_at {
            FailPoint::Patch => Err(InstallError::MachoPatch {
                path: executable.to_path_buf(),
                reason: "punto di fallimento iniettato: patch LC_LOAD_DYLIB".to_string(),
            }),
            FailPoint::Resign => {
                // La patch riesce e modifica DAVVERO l'eseguibile (come il seam
                // host), così il rollback deve poterlo ripristinare byte-esatto.
                let mut bytes = fs::read(executable).map_err(|source| InstallError::Io {
                    path: executable.to_path_buf(),
                    source,
                })?;
                bytes.extend_from_slice(b"\n__PULSE_LC_LOAD_DYLIB__\t");
                bytes.extend_from_slice(load_path.as_bytes());
                bytes.push(b'\n');
                fs::write(executable, &bytes).map_err(|source| InstallError::Io {
                    path: executable.to_path_buf(),
                    source,
                })?;
                Ok(())
            }
        }
    }

    fn adhoc_resign_with_disable_lv(&self, gd: &GdInstallation) -> Result<(), InstallError> {
        // Raggiunto solo nel caso `Resign`: modifica la firma e POI fallisce,
        // così il rollback deve ripristinare anche la firma ai byte originali.
        let code_signature = gd.code_signature();
        if let Some(parent) = code_signature.parent() {
            let _ = fs::create_dir_all(parent);
        }
        let _ = fs::write(&code_signature, b"<pulse resign parziale prima del fallimento>");
        Err(InstallError::Codesign {
            path: gd.executable(),
            reason: "punto di fallimento iniettato: ad-hoc resign".to_string(),
        })
    }
}

// ---------------------------------------------------------------------------
// Strategie di generazione.
// ---------------------------------------------------------------------------

/// Corpo binario casuale (può essere vuoto) per eseguibile/firma/artefatto.
fn arb_body() -> impl Strategy<Value = Vec<u8>> {
    prop::collection::vec(any::<u8>(), 0..64)
}

/// Nome di file asset extra: caratteri sicuri, non collide con i file noti.
fn arb_asset_name() -> impl Strategy<Value = String> {
    proptest::string::string_regex("[a-zA-Z0-9_-]{1,16}\\.dat").unwrap()
}

/// Insieme di asset di gioco extra (0..=5) con nomi distinti.
fn arb_extra_assets() -> impl Strategy<Value = Vec<(String, Vec<u8>)>> {
    prop::collection::vec((arb_asset_name(), arb_body()), 0..=5).prop_map(|mut v| {
        let mut seen = BTreeSet::new();
        v.retain(|(name, _)| seen.insert(name.clone()));
        v
    })
}

/// Punto di fallimento iniettato: patch, resign o artefatto mancante.
/// `None` rappresenta il caso "artefatto mancante" (gestito con il seam host
/// e un artefatto inesistente, così la copia della dylib fallisce per prima).
fn arb_fail_mode() -> impl Strategy<Value = Option<FailPoint>> {
    prop_oneof![
        Just(Some(FailPoint::Patch)),
        Just(Some(FailPoint::Resign)),
        Just(None),
    ]
}

// ---------------------------------------------------------------------------
// Property 12 — rollback transazionale su install fallita (Req 7.4).
// ---------------------------------------------------------------------------

proptest! {
    // Almeno 100 iterazioni (default proptest = 256).
    #![proptest_config(ProptestConfig::with_cases(256))]

    /// Feature: pulse-gd-integration, Property 12
    /// Validates: Requisiti 7.4
    #[test]
    fn prop12_failed_install_rolls_back_to_pristine_tree(
        exe_body in arb_body(),
        sig_body in arb_body(),
        artifact_body in arb_body(),
        extra_assets in arb_extra_assets(),
        fail_mode in arb_fail_mode(),
    ) {
        let guard = TempGuard::new();
        let app = build_fake_gd(guard.path(), &exe_body, &sig_body, &extra_assets);

        // Snapshot byte-per-byte dell'albero PRIMA dell'install (stato pristino).
        let before = snapshot_tree(&app);

        // Esegue l'install con il punto di fallimento iniettato.
        let result = match fail_mode {
            Some(fail_at) => {
                // Artefatto valido: il fallimento avviene durante l'iniezione.
                let artifact = guard.path().join("libpulse_loader.dylib");
                fs::write(&artifact, &artifact_body).unwrap();
                let injector = FailingInjector { fail_at };
                install_with(&app, &artifact, &injector)
            }
            None => {
                // Artefatto MANCANTE: la copia della dylib fallisce per prima
                // (nessuna iniezione raggiunta). Si usa il seam host reale.
                let missing = guard.path().join("non-existent-artifact.dylib");
                install_with(&app, &missing, &pulse_cli::installer::HostSeamInjector)
            }
        };

        // (Req 7.4) L'install deve fallire con un errore che identifica la causa.
        let err = match result {
            Err(e) => e,
            Ok(modified) => {
                return Err(TestCaseError::fail(format!(
                    "install_with doveva fallire ({fail_mode:?}) ma ha avuto successo: {modified:?}"
                )));
            }
        };
        let cause = err.to_string();
        prop_assert!(
            !cause.trim().is_empty(),
            "l'errore deve identificare una causa non vuota, trovato: {cause:?}"
        );

        // (Req 7.4) Dopo il rollback l'albero è byte-per-byte identico al
        // pre-installazione: stessi file, stessi byte, nessun residuo
        // (dylib del loader o restore record `.pulse-backup`).
        let after = snapshot_tree(&app);
        prop_assert_eq!(
            &after,
            &before,
            "dopo il rollback ({:?}) l'albero deve essere byte-per-byte identico al pre-installazione",
            fail_mode
        );
    }
}
