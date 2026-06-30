//! Property test P11 — round-trip install→uninstall dell'Installer.
//!
//! Feature: pulse-gd-integration, Property 11
//! Validates: Requisiti 7.1, 7.3
//!
//! Su un **albero di GD finto** generato casualmente si verifica la proprietà
//! di round-trip dell'Installer:
//!
//!   - `install` (Req 7.1) piazza il Loader_Artifact nella `GD_Installation`,
//!     modificando solo i file richiesti per caricarlo;
//!   - `uninstall` (Req 7.3) ripristina **ogni** file modificato ai byte
//!     originali ed elimina i file che erano assenti prima dell'installazione.
//!
//! Quindi, catturando uno **snapshot** dell'intero albero del bundle PRIMA
//! dell'install e confrontandolo con uno snapshot DOPO l'uninstall, l'albero
//! deve risultare **byte-per-byte identico** all'originale: stessi file, stessi
//! contenuti, stesse directory, nessun residuo (né la dylib del loader né la
//! directory di backup `.pulse-backup`).
//!
//! Generatori: contenuti binari casuali per eseguibile, firma e artefatto, più
//! un insieme casuale di **asset di gioco extra** (sotto `Contents/Resources/`)
//! che fungono da rumore e devono restare intatti attraverso il round-trip.
//! Ogni caso usa una directory temporanea isolata con pulizia automatica.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use proptest::prelude::*;

use pulse_cli::installer::{install, recognize_gd, uninstall, EXPECTED_BUNDLE_ID};

/// Directory temporanea isolata, con pulizia automatica alla `Drop`.
struct TempGuard {
    path: PathBuf,
}

impl TempGuard {
    fn new() -> Self {
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let n = COUNTER.fetch_add(1, Ordering::Relaxed);
        let pid = std::process::id();
        let path = std::env::temp_dir().join(format!("pulse-p11-{pid}-{n}"));
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

/// Costruisce un albero di GD finto **valido** sotto `root` con i contenuti
/// indicati e gli asset extra dati, e restituisce il path del bundle `.app`.
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

    // Eseguibile Mach-O 64-bit valido: magic feedfacf (big-endian) + corpo
    // casuale, così il riconoscimento (magic Mach-O) passa.
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

    // Asset di gioco extra (rumore): devono restare intatti attraverso il
    // round-trip.
    for (name, bytes) in extra_assets {
        fs::write(app.join("Contents/Resources").join(name), bytes).unwrap();
    }

    app
}

/// Cattura uno snapshot ricorsivo dell'albero a `root`:
///   - `files`: percorso relativo → byte di ogni file regolare;
///   - `dirs`: insieme dei percorsi relativi di ogni directory.
/// Permette un confronto byte-per-byte di contenuti e struttura.
#[derive(Debug, PartialEq, Eq)]
struct TreeSnapshot {
    files: BTreeMap<PathBuf, Vec<u8>>,
    dirs: BTreeSet<PathBuf>,
}

fn snapshot_tree(root: &Path) -> TreeSnapshot {
    let mut files = BTreeMap::new();
    let mut dirs = BTreeSet::new();
    walk(root, root, &mut files, &mut dirs);
    TreeSnapshot { files, dirs }
}

fn walk(
    root: &Path,
    current: &Path,
    files: &mut BTreeMap<PathBuf, Vec<u8>>,
    dirs: &mut BTreeSet<PathBuf>,
) {
    let entries = match fs::read_dir(current) {
        Ok(e) => e,
        Err(_) => return,
    };
    for entry in entries {
        let entry = entry.unwrap();
        let path = entry.path();
        let rel = path.strip_prefix(root).unwrap().to_path_buf();
        let file_type = entry.file_type().unwrap();
        if file_type.is_dir() {
            dirs.insert(rel);
            walk(root, &path, files, dirs);
        } else {
            // Tratta i symlink come file regolari leggendone i byte; per i
            // bundle finti host-testabili non ci sono symlink.
            let bytes = fs::read(&path).unwrap();
            files.insert(rel, bytes);
        }
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
        // Deduplica per nome così da non sovrascrivere file generati.
        let mut seen = BTreeSet::new();
        v.retain(|(name, _)| seen.insert(name.clone()));
        v
    })
}

// ---------------------------------------------------------------------------
// Property 11 — round-trip install→uninstall (Req 7.1, 7.3).
// ---------------------------------------------------------------------------

proptest! {
    // Almeno 100 iterazioni (default proptest = 256).
    #![proptest_config(ProptestConfig::with_cases(256))]

    /// Feature: pulse-gd-integration, Property 11
    /// Validates: Requisiti 7.1, 7.3
    #[test]
    fn prop11_install_then_uninstall_is_identity(
        exe_body in arb_body(),
        sig_body in arb_body(),
        artifact_body in arb_body(),
        extra_assets in arb_extra_assets(),
    ) {
        let guard = TempGuard::new();
        let app = build_fake_gd(guard.path(), &exe_body, &sig_body, &extra_assets);

        // Artefatto (dylib sorgente) con contenuto casuale, fuori dal bundle
        // così da non comparire nello snapshot del bundle stesso.
        let artifact = guard.path().join("libpulse_loader.dylib");
        fs::write(&artifact, &artifact_body).unwrap();

        // Il bundle finto deve essere riconosciuto prima del round-trip.
        recognize_gd(&app).expect("il bundle finto deve essere riconosciuto");

        // Snapshot dell'albero ORIGINALE (prima dell'install).
        let before = snapshot_tree(&app);

        // --- install (Req 7.1) ---
        let installed = install(&app, &artifact).expect("install deve riuscire");
        // L'install deve davvero aver modificato qualcosa (il round-trip è
        // significativo solo se l'install ha agito).
        prop_assert!(
            !installed.0.is_empty(),
            "install deve riportare almeno un file modificato"
        );

        // --- uninstall (Req 7.3) ---
        uninstall(&app).expect("uninstall deve riuscire");

        // Snapshot dell'albero DOPO il round-trip.
        let after = snapshot_tree(&app);

        // (Req 7.1 + 7.3) L'albero deve essere byte-per-byte identico
        // all'originale: stesse directory, stessi file, stessi contenuti.
        prop_assert_eq!(
            before.dirs,
            after.dirs,
            "le directory del bundle devono coincidere dopo il round-trip"
        );

        // Confronto file-per-file per diagnostica precisa in caso di mismatch.
        let before_keys: BTreeSet<&PathBuf> = before.files.keys().collect();
        let after_keys: BTreeSet<&PathBuf> = after.files.keys().collect();
        prop_assert_eq!(
            &before_keys,
            &after_keys,
            "l'insieme dei file deve coincidere dopo il round-trip (nessun residuo né file mancante)"
        );
        for (rel, orig_bytes) in &before.files {
            let new_bytes = after.files.get(rel).unwrap();
            prop_assert!(
                orig_bytes == new_bytes,
                "il file {:?} non è byte-per-byte identico dopo il round-trip",
                rel
            );
        }
    }
}
