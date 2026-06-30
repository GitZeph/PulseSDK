//! Property test P10 — completezza del backup pre-installazione.
//!
//! Feature: pulse-gd-integration, Property 10
//! Validates: Requisiti 7.2, 7.6, 7.7
//!
//! Su un **albero di GD finto** generato casualmente, a fine fase di backup
//! (cioè dopo che `install` ha reso durevole il restore record) si verifica
//! che:
//!
//!   1. **Completezza/tipo corretto (Req 7.2):** il backup manifest caricato da
//!      disco copre **esattamente** i file che l'Installer modifica o crea, con
//!      il `BackupKind` corretto:
//!        - l'eseguibile (`Contents/MacOS/Geometry Dash`) → `Modified` con i
//!          byte **originali** pre-installazione;
//!        - la firma (`Contents/_CodeSignature/CodeResources`) → `Modified` con
//!          i byte **originali** pre-installazione;
//!        - la dylib del loader (`Contents/MacOS/libpulse_loader.dylib`) →
//!          `CreatedAbsent` (prior-absence).
//!      Nessuna voce aggiuntiva, rimossa o di tipo errato.
//!   2. **Sottoinsieme consentito (Req 7.6):** l'insieme dei file modificati
//!      registrati nel backup è un sottoinsieme di
//!      {eseguibile, firma, dylib}; nessun altro file di gioco (Info.plist,
//!      asset casuali) compare nel manifest.
//!   3. **Elenco dei file modificati (Req 7.7):** l'elenco restituito da
//!      `install` è anch'esso un sottoinsieme di {eseguibile, firma, dylib} e
//!      coincide con i path coperti dal backup.
//!
//! Generatori: contenuti casuali per eseguibile, firma e artefatto, più un
//! insieme casuale di **asset di gioco extra** (sotto `Contents/Resources/`)
//! che non devono mai entrare nel backup né nell'insieme modificato — così la
//! proprietà di "esattamente questi tre file" è esercitata in presenza di
//! rumore. Ogni caso usa una directory temporanea isolata.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use proptest::prelude::*;

use pulse_cli::installer::{
    install, recognize_gd, BackupKind, BackupManifest, CODE_SIGNATURE_REL, EXPECTED_BUNDLE_ID,
    GD_EXECUTABLE_REL, LOADER_DYLIB_NAME,
};

/// Directory temporanea isolata, con pulizia automatica alla `Drop`.
struct TempGuard {
    path: PathBuf,
}

impl TempGuard {
    fn new() -> Self {
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let n = COUNTER.fetch_add(1, Ordering::Relaxed);
        let pid = std::process::id();
        let path = std::env::temp_dir().join(format!("pulse-p10-{pid}-{n}"));
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

    // Asset di gioco extra (rumore): non devono mai entrare nel backup.
    for (name, bytes) in extra_assets {
        fs::write(app.join("Contents/Resources").join(name), bytes).unwrap();
    }

    app
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
// Property 10 — completezza del backup pre-installazione (Req 7.2, 7.6, 7.7).
// ---------------------------------------------------------------------------

proptest! {
    // Almeno 100 iterazioni (default proptest = 256).
    #![proptest_config(ProptestConfig::with_cases(256))]

    /// Feature: pulse-gd-integration, Property 10
    /// Validates: Requisiti 7.2, 7.6, 7.7
    #[test]
    fn prop10_backup_is_complete_and_subset(
        exe_body in arb_body(),
        sig_body in arb_body(),
        artifact_body in arb_body(),
        extra_assets in arb_extra_assets(),
    ) {
        let guard = TempGuard::new();
        let app = build_fake_gd(guard.path(), &exe_body, &sig_body, &extra_assets);

        // Byte originali pre-installazione dei file che verranno modificati.
        let exe_orig = fs::read(app.join(GD_EXECUTABLE_REL)).unwrap();
        let sig_orig = fs::read(app.join(CODE_SIGNATURE_REL)).unwrap();

        // Artefatto (dylib sorgente) con contenuto casuale.
        let artifact = guard.path().join("libpulse_loader.dylib");
        fs::write(&artifact, &artifact_body).unwrap();

        let gd = recognize_gd(&app).expect("il bundle finto deve essere riconosciuto");

        // I tre path consentiti (Req 7.6): eseguibile, firma, dylib.
        let exe_abs = gd.executable();
        let sig_abs = gd.code_signature();
        let dylib_abs = gd.dylib_dest();
        let allowed: BTreeSet<PathBuf> =
            [exe_abs.clone(), sig_abs.clone(), dylib_abs.clone()].into_iter().collect();

        // --- Fase di install (include la fase di backup durevole). ---
        let modified = install(&app, &artifact).expect("install deve riuscire");

        // (Req 7.7) L'elenco dei file modificati è un sottoinsieme di
        // {eseguibile, firma, dylib}.
        let modified_set: BTreeSet<PathBuf> = modified.0.iter().cloned().collect();
        prop_assert!(
            modified_set.is_subset(&allowed),
            "insieme modificato {:?} non è sottoinsieme di {:?}",
            modified_set,
            allowed
        );

        // --- Carica il backup manifest reso durevole su disco. ---
        let backup = BackupManifest::load(&app).expect("backup manifest caricabile");

        // Indicizza le voci per percorso relativo, garantendo l'assenza di
        // duplicati (no voci ridondanti).
        let mut by_rel: BTreeMap<PathBuf, BackupKind> = BTreeMap::new();
        for e in &backup.entries {
            let dup = by_rel.insert(e.rel_path.clone(), e.kind.clone());
            prop_assert!(dup.is_none(), "voce di backup duplicata per {:?}", e.rel_path);
        }

        // (Req 7.2 + 7.6) Il backup copre ESATTAMENTE i tre file attesi.
        let exe_rel = PathBuf::from(GD_EXECUTABLE_REL);
        let sig_rel = PathBuf::from(CODE_SIGNATURE_REL);
        let dylib_rel = PathBuf::from("Contents/MacOS").join(LOADER_DYLIB_NAME);

        let covered: BTreeSet<PathBuf> = by_rel.keys().cloned().collect();
        let expected: BTreeSet<PathBuf> =
            [exe_rel.clone(), sig_rel.clone(), dylib_rel.clone()].into_iter().collect();
        prop_assert_eq!(
            &covered,
            &expected,
            "il backup deve coprire esattamente {{eseguibile, firma, dylib}}"
        );

        // (Req 7.2) Tipo corretto + byte originali completi per i file Modified.
        match by_rel.get(&exe_rel).unwrap() {
            BackupKind::Modified { bytes, .. } => {
                prop_assert_eq!(bytes, &exe_orig, "byte originali dell'eseguibile incompleti");
            }
            other => prop_assert!(false, "eseguibile deve essere Modified, trovato {:?}", other),
        }
        match by_rel.get(&sig_rel).unwrap() {
            BackupKind::Modified { bytes, .. } => {
                prop_assert_eq!(bytes, &sig_orig, "byte originali della firma incompleti");
            }
            other => prop_assert!(false, "firma deve essere Modified, trovato {:?}", other),
        }
        prop_assert_eq!(
            by_rel.get(&dylib_rel).unwrap(),
            &BackupKind::CreatedAbsent,
            "la dylib deve essere CreatedAbsent (prior-absence)"
        );

        // (Req 7.6) Nessun asset extra è finito nel backup.
        for (name, _) in &extra_assets {
            let asset_rel = PathBuf::from("Contents/Resources").join(name);
            prop_assert!(
                !covered.contains(&asset_rel),
                "asset extra {:?} non deve comparire nel backup",
                asset_rel
            );
        }

        // I path assoluti coperti dal backup coincidono con i file consentiti.
        let covered_abs: BTreeSet<PathBuf> =
            covered.iter().map(|rel| app.join(rel)).collect();
        prop_assert_eq!(
            &covered_abs,
            &allowed,
            "i path del backup devono coincidere con {{eseguibile, firma, dylib}}"
        );
    }
}
