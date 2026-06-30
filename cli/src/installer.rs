//! Installer del Pulse_Loader in una `GD_Installation` (Requisito 7).
//!
//! Questo modulo implementa la parte *host-testabile* dell'Installer su un
//! **albero di GD finto** (una directory temporanea che imita
//! `Geometry Dash.app`). Le operazioni reali sul binario di GD
//! (`patch_lc_load_dylib`, ad-hoc resign) appartengono al percorso nativo di
//! Fase E e sono cablate altrove.
//!
//! Mappa dei requisiti coperti da questa attività (7.1):
//!   - **7.5** — `recognize_gd`: riconosce la struttura del bundle
//!     (`Contents/MacOS/Geometry Dash`, `CFBundleIdentifier` atteso, Mach-O di
//!     GD). Se il path non è una `GD_Installation` riconosciuta, rifiuta
//!     **senza modificare nulla** e segnala l'installazione non riconosciuta.
//!   - **7.9** — `ensure_not_already_installed`: se il backup manifest esiste
//!     già oppure la dylib del loader è già presente, l'install rifiuta
//!     **senza modificare nulla**.
//!   - **7.8** — guard di uninstall: in assenza di un backup manifest (restore
//!     record), l'uninstall non cambia nulla e segnala il "restore record
//!     mancante".
//!
//! Le attività successive estendono questo stesso modulo:
//!   - **7.2** aggiunge il backup transazionale e `install` (record dei file
//!     prima di toccarli, copia della dylib, patch + resign);
//!   - **7.4** aggiunge l'uninstall byte-esatto e il rollback su install
//!     fallita.
//!
//! I tipi `BackupManifest`/`BackupEntry`/`BackupKind` e la persistenza del
//! restore record sono definiti qui perché sono il fondamento condiviso di
//! tutte e tre le attività.

use std::fs;
use std::path::{Path, PathBuf};

use sha2::{Digest, Sha256};

// ---------------------------------------------------------------------------
// Costanti del layout del bundle macOS (Steam) — vedi design §6.
// ---------------------------------------------------------------------------

/// Percorso relativo dell'eseguibile Mach-O di GD all'interno del bundle.
pub const GD_EXECUTABLE_REL: &str = "Contents/MacOS/Geometry Dash";

/// Percorso relativo dell'`Info.plist` del bundle.
pub const INFO_PLIST_REL: &str = "Contents/Info.plist";

/// Percorso relativo del file di firma sostituito dal resign ad-hoc.
pub const CODE_SIGNATURE_REL: &str = "Contents/_CodeSignature/CodeResources";

/// Nome del file della dylib del loader copiata in `Contents/MacOS/`.
pub const LOADER_DYLIB_NAME: &str = "libpulse_loader.dylib";

/// Path di load del `LC_LOAD_DYLIB` aggiunto all'eseguibile di GD, relativo al
/// percorso dell'eseguibile stesso (la dylib è copiata accanto, in
/// `Contents/MacOS/`).
pub const LOADER_DYLIB_LOAD_PATH: &str = "@loader_path/libpulse_loader.dylib";

/// `CFBundleIdentifier` atteso per una `GD_Installation` riconosciuta.
///
/// È il bundle id della build macOS (Steam) di Geometry Dash, confermato dalla
/// firma del binario reale (`codesign -dv` → `Identifier=com.robtop.geometrydashmac`).
/// In CI il riconoscimento opera su un albero di GD finto che dichiara questo id.
pub const EXPECTED_BUNDLE_ID: &str = "com.robtop.geometrydashmac";

/// Directory che ospita il backup manifest e i file `.orig` di backup.
pub const BACKUP_DIR_REL: &str = ".pulse-backup";

/// Nome del file del backup manifest (restore record).
pub const BACKUP_MANIFEST_NAME: &str = "manifest.txt";

/// Directory di backup (restore record + file `.orig`) per una
/// `GD_Installation`. È un **sibling** del bundle `.app` (es.
/// `Geometry Dash.app.pulse-backup`), **non** una sottocartella del bundle:
/// scrivere file dentro il bundle romperebbe la firma con "unsealed contents
/// present in the bundle root" quando `codesign` sigilla il bundle.
fn backup_root_for(gd_root: &Path) -> PathBuf {
    let mut s = gd_root.as_os_str().to_os_string();
    s.push(".pulse-backup");
    PathBuf::from(s)
}

// ---------------------------------------------------------------------------
// Errori dell'Installer.
// ---------------------------------------------------------------------------

/// Errori delle operazioni dell'Installer. In ogni caso d'errore di
/// riconoscimento/guard, **nessun file** della `GD_Installation` viene
/// modificato.
#[derive(Debug, thiserror::Error)]
pub enum InstallError {
    /// Il path indicato non è una directory (atteso un bundle `.app`).
    #[error("percorso non valido: '{0}' non è una directory di bundle GD")]
    NotADirectory(PathBuf),

    /// Il path non è una `GD_Installation` riconosciuta (Req 7.5): nulla
    /// modificato.
    #[error("installazione non riconosciuta: '{path}' — {reason}")]
    UnrecognizedInstallation { path: PathBuf, reason: String },

    /// Il Pulse_Loader risulta già installato (Req 7.9): nulla modificato.
    #[error("Pulse_Loader già installato in '{path}': {reason}")]
    AlreadyInstalled { path: PathBuf, reason: String },

    /// Uninstall richiesto senza restore record (Req 7.8): nulla modificato.
    #[error("restore record mancante: nessun backup manifest trovato in '{0}', impossibile disinstallare")]
    MissingRestoreRecord(PathBuf),

    /// Il restore record esiste ma è illeggibile o corrotto.
    #[error("restore record corrotto in '{path}': {reason}")]
    CorruptRestoreRecord { path: PathBuf, reason: String },

    /// La patch reale del `LC_LOAD_DYLIB` sull'eseguibile Mach-O è fallita
    /// (Fase E, path nativo macOS). Su fallimento l'install esegue il rollback.
    #[error("patch LC_LOAD_DYLIB fallita su '{path}': {reason}")]
    MachoPatch { path: PathBuf, reason: String },

    /// L'ad-hoc resign reale (`codesign`) è fallito (Fase E, path nativo macOS).
    /// Su fallimento l'install esegue il rollback.
    #[error("ad-hoc resign (codesign) fallito su '{path}': {reason}")]
    Codesign { path: PathBuf, reason: String },

    /// Errore di I/O su un percorso del bundle o del backup.
    #[error("errore di I/O su '{path}': {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
}

// ---------------------------------------------------------------------------
// GD_Installation riconosciuta.
// ---------------------------------------------------------------------------

/// Una `GD_Installation` riconosciuta: i percorsi assoluti rilevanti del bundle.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GdInstallation {
    /// Radice del bundle (`.../Geometry Dash.app`).
    pub root: PathBuf,
}

impl GdInstallation {
    /// Eseguibile Mach-O di GD (`Contents/MacOS/Geometry Dash`).
    pub fn executable(&self) -> PathBuf {
        self.root.join(GD_EXECUTABLE_REL)
    }

    /// `Info.plist` del bundle.
    pub fn info_plist(&self) -> PathBuf {
        self.root.join(INFO_PLIST_REL)
    }

    /// File di firma (`_CodeSignature/CodeResources`).
    pub fn code_signature(&self) -> PathBuf {
        self.root.join(CODE_SIGNATURE_REL)
    }

    /// Destinazione della dylib del loader (`Contents/MacOS/libpulse_loader.dylib`).
    pub fn dylib_dest(&self) -> PathBuf {
        self.root
            .join("Contents/MacOS")
            .join(LOADER_DYLIB_NAME)
    }

    /// Directory che contiene il backup manifest e i file `.orig` (sibling del
    /// bundle, non dentro di esso).
    pub fn backup_dir(&self) -> PathBuf {
        backup_root_for(&self.root)
    }

    /// Percorso del backup manifest (restore record).
    pub fn backup_manifest_path(&self) -> PathBuf {
        self.backup_dir().join(BACKUP_MANIFEST_NAME)
    }
}

// ---------------------------------------------------------------------------
// Riconoscimento della GD_Installation (Req 7.5).
// ---------------------------------------------------------------------------

/// Magic number Mach-O accettati (32/64 bit, big/little endian e FAT).
const MACHO_MAGICS: [u32; 6] = [
    0xfeed_face, // MH_MAGIC (32-bit)
    0xfeed_facf, // MH_MAGIC_64 (64-bit)
    0xcefa_edfe, // MH_CIGAM (32-bit, byte-swapped)
    0xcffa_edfe, // MH_CIGAM_64 (64-bit, byte-swapped)
    0xcafe_babe, // FAT_MAGIC (universal)
    0xbeba_feca, // FAT_CIGAM (universal, byte-swapped)
];

/// Riconosce una `GD_Installation` a `gd_root` (Req 7.5).
///
/// Verifica, **senza modificare alcun file**:
///   1. che `gd_root` sia una directory di bundle;
///   2. che esista l'eseguibile `Contents/MacOS/Geometry Dash`;
///   3. che `Contents/Info.plist` dichiari il `CFBundleIdentifier` atteso;
///   4. che l'eseguibile inizi con un magic number Mach-O valido.
///
/// Se una qualsiasi verifica fallisce, restituisce
/// [`InstallError::UnrecognizedInstallation`] lasciando il path invariato.
pub fn recognize_gd(gd_root: &Path) -> Result<GdInstallation, InstallError> {
    if !gd_root.is_dir() {
        return Err(InstallError::NotADirectory(gd_root.to_path_buf()));
    }

    let install = GdInstallation {
        root: gd_root.to_path_buf(),
    };

    // 1. Eseguibile presente.
    let executable = install.executable();
    if !executable.is_file() {
        return Err(InstallError::UnrecognizedInstallation {
            path: gd_root.to_path_buf(),
            reason: format!("eseguibile atteso assente: '{}'", executable.display()),
        });
    }

    // 2. CFBundleIdentifier atteso nell'Info.plist.
    let info_plist = install.info_plist();
    if !info_plist.is_file() {
        return Err(InstallError::UnrecognizedInstallation {
            path: gd_root.to_path_buf(),
            reason: format!("Info.plist assente: '{}'", info_plist.display()),
        });
    }
    let plist_bytes = fs::read(&info_plist).map_err(|source| InstallError::Io {
        path: info_plist.clone(),
        source,
    })?;
    if !plist_declares_bundle_id(&plist_bytes, EXPECTED_BUNDLE_ID) {
        return Err(InstallError::UnrecognizedInstallation {
            path: gd_root.to_path_buf(),
            reason: format!(
                "CFBundleIdentifier atteso '{EXPECTED_BUNDLE_ID}' non trovato in '{}'",
                info_plist.display()
            ),
        });
    }

    // 3. Magic Mach-O dell'eseguibile.
    if !is_macho(&executable)? {
        return Err(InstallError::UnrecognizedInstallation {
            path: gd_root.to_path_buf(),
            reason: format!(
                "l'eseguibile '{}' non è un binario Mach-O di GD",
                executable.display()
            ),
        });
    }

    Ok(install)
}

/// Verifica che i byte di un `Info.plist` dichiarino il `CFBundleIdentifier`
/// indicato. Tollerante al formato (XML o binario): cerca la chiave e il valore
/// come sottosequenze di byte, così da funzionare sia su plist testuali sia su
/// plist binari.
fn plist_declares_bundle_id(plist: &[u8], bundle_id: &str) -> bool {
    contains_subsequence(plist, b"CFBundleIdentifier")
        && contains_subsequence(plist, bundle_id.as_bytes())
}

/// Restituisce `true` se `haystack` contiene `needle` come sottosequenza di byte.
fn contains_subsequence(haystack: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() {
        return true;
    }
    if needle.len() > haystack.len() {
        return false;
    }
    haystack
        .windows(needle.len())
        .any(|window| window == needle)
}

/// Legge i primi 4 byte di `path` e verifica che siano un magic Mach-O valido.
fn is_macho(path: &Path) -> Result<bool, InstallError> {
    let bytes = fs::read(path).map_err(|source| InstallError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    if bytes.len() < 4 {
        return Ok(false);
    }
    let magic = u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
    Ok(MACHO_MAGICS.contains(&magic))
}

// ---------------------------------------------------------------------------
// Guard di idempotenza dell'install (Req 7.9).
// ---------------------------------------------------------------------------

/// Verifica che il Pulse_Loader **non** sia già installato (Req 7.9).
///
/// Rifiuta — senza modificare nulla — se il backup manifest esiste già oppure
/// se la dylib del loader è già presente nel bundle.
pub fn ensure_not_already_installed(gd: &GdInstallation) -> Result<(), InstallError> {
    if gd.backup_manifest_path().exists() {
        return Err(InstallError::AlreadyInstalled {
            path: gd.root.clone(),
            reason: format!(
                "esiste già un backup manifest: '{}'",
                gd.backup_manifest_path().display()
            ),
        });
    }
    if gd.dylib_dest().exists() {
        return Err(InstallError::AlreadyInstalled {
            path: gd.root.clone(),
            reason: format!(
                "la dylib del loader è già presente: '{}'",
                gd.dylib_dest().display()
            ),
        });
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// Backup manifest / restore record (Req 7.2, fondamento condiviso).
// ---------------------------------------------------------------------------

/// Tipo di voce di backup per un file della `GD_Installation`.
///
/// - `Modified` registra i byte (e l'hash) originali di un file che l'Installer
///   modificherà, così l'uninstall può riscriverli byte-per-byte (Req 7.3).
/// - `CreatedAbsent` marca un file che l'Installer crea e che era assente prima
///   dell'installazione, così l'uninstall lo elimina (Req 7.3).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BackupKind {
    /// File preesistente che verrà modificato: byte e hash originali.
    Modified { bytes: Vec<u8>, hash: String },
    /// File creato dall'Installer, assente prima dell'installazione.
    CreatedAbsent,
}

/// Una voce del backup manifest: il percorso (relativo alla radice del bundle)
/// e il suo stato pre-installazione.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BackupEntry {
    /// Percorso relativo alla radice della `GD_Installation`.
    pub rel_path: PathBuf,
    /// Stato pre-installazione del file.
    pub kind: BackupKind,
}

impl BackupEntry {
    /// Crea una voce `Modified` calcolando l'hash dei byte originali.
    pub fn modified(rel_path: impl Into<PathBuf>, bytes: Vec<u8>) -> Self {
        let hash = sha256_hex(&bytes);
        BackupEntry {
            rel_path: rel_path.into(),
            kind: BackupKind::Modified { bytes, hash },
        }
    }

    /// Crea una voce `CreatedAbsent`.
    pub fn created_absent(rel_path: impl Into<PathBuf>) -> Self {
        BackupEntry {
            rel_path: rel_path.into(),
            kind: BackupKind::CreatedAbsent,
        }
    }
}

/// Il backup manifest (restore record) di una `GD_Installation`.
///
/// Viene reso durevole **prima** che l'Installer tocchi qualunque file (Req
/// 7.2); la sua **esistenza** è anche l'already-installed guard (Req 7.9) e la
/// sua **assenza** è la condizione d'errore dell'uninstall (Req 7.8).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BackupManifest {
    /// Radice della `GD_Installation` cui il manifest si riferisce.
    pub gd_root: PathBuf,
    /// Voci di backup registrate.
    pub entries: Vec<BackupEntry>,
}

impl BackupManifest {
    /// Crea un manifest vuoto per la `GD_Installation` indicata.
    pub fn new(gd: &GdInstallation) -> Self {
        BackupManifest {
            gd_root: gd.root.clone(),
            entries: Vec::new(),
        }
    }

    /// Percorso del file manifest a partire dalla radice del bundle.
    pub fn path_for(gd_root: &Path) -> PathBuf {
        backup_root_for(gd_root).join(BACKUP_MANIFEST_NAME)
    }

    /// `true` se esiste un restore record per la `GD_Installation` indicata.
    pub fn exists(gd_root: &Path) -> bool {
        Self::path_for(gd_root).exists()
    }

    /// Rende durevole il manifest su disco: scrive il file manifest e, per ogni
    /// voce `Modified`, un file `.orig` con i byte originali.
    pub fn persist(&self) -> Result<(), InstallError> {
        let backup_dir = backup_root_for(&self.gd_root);
        fs::create_dir_all(&backup_dir).map_err(|source| InstallError::Io {
            path: backup_dir.clone(),
            source,
        })?;

        let mut manifest = String::new();
        manifest.push_str("pulse-backup\t1\n");
        manifest.push_str(&format!("gd_root\t{}\n", encode_field(&self.gd_root.to_string_lossy())));

        for (idx, entry) in self.entries.iter().enumerate() {
            let rel = entry.rel_path.to_string_lossy();
            match &entry.kind {
                BackupKind::Modified { bytes, hash } => {
                    let backup_name = format!("{idx}.orig");
                    let backup_path = backup_dir.join(&backup_name);
                    fs::write(&backup_path, bytes).map_err(|source| InstallError::Io {
                        path: backup_path.clone(),
                        source,
                    })?;
                    manifest.push_str(&format!(
                        "entry\tmodified\t{}\t{}\t{}\n",
                        encode_field(&rel),
                        hash,
                        encode_field(&backup_name)
                    ));
                }
                BackupKind::CreatedAbsent => {
                    manifest.push_str(&format!(
                        "entry\tcreated_absent\t{}\n",
                        encode_field(&rel)
                    ));
                }
            }
        }

        let manifest_path = Self::path_for(&self.gd_root);
        fs::write(&manifest_path, manifest).map_err(|source| InstallError::Io {
            path: manifest_path,
            source,
        })?;
        Ok(())
    }

    /// Carica il restore record per la `GD_Installation` indicata.
    ///
    /// Se il manifest è assente, restituisce
    /// [`InstallError::MissingRestoreRecord`] (Req 7.8): il chiamante non deve
    /// modificare nulla.
    pub fn load(gd_root: &Path) -> Result<Self, InstallError> {
        let manifest_path = Self::path_for(gd_root);
        if !manifest_path.exists() {
            return Err(InstallError::MissingRestoreRecord(gd_root.to_path_buf()));
        }

        let backup_dir = backup_root_for(gd_root);
        let text = fs::read_to_string(&manifest_path).map_err(|source| InstallError::Io {
            path: manifest_path.clone(),
            source,
        })?;

        let corrupt = |reason: String| InstallError::CorruptRestoreRecord {
            path: manifest_path.clone(),
            reason,
        };

        let mut lines = text.lines();
        match lines.next() {
            Some(header) if header == "pulse-backup\t1" => {}
            other => {
                return Err(corrupt(format!(
                    "header inatteso: {:?}",
                    other.unwrap_or("<vuoto>")
                )))
            }
        }

        let mut gd_root_field: Option<PathBuf> = None;
        let mut entries: Vec<BackupEntry> = Vec::new();

        for line in lines {
            if line.is_empty() {
                continue;
            }
            let fields: Vec<&str> = line.split('\t').collect();
            match fields.as_slice() {
                ["gd_root", value] => {
                    gd_root_field = Some(PathBuf::from(decode_field(value)));
                }
                ["entry", "modified", rel, hash, backup_name] => {
                    let backup_path = backup_dir.join(decode_field(backup_name));
                    let bytes = fs::read(&backup_path).map_err(|source| InstallError::Io {
                        path: backup_path.clone(),
                        source,
                    })?;
                    let actual = sha256_hex(&bytes);
                    if actual != *hash {
                        return Err(corrupt(format!(
                            "hash del backup '{}' non corrisponde (atteso {hash}, trovato {actual})",
                            backup_path.display()
                        )));
                    }
                    entries.push(BackupEntry {
                        rel_path: PathBuf::from(decode_field(rel)),
                        kind: BackupKind::Modified {
                            bytes,
                            hash: (*hash).to_string(),
                        },
                    });
                }
                ["entry", "created_absent", rel] => {
                    entries.push(BackupEntry::created_absent(PathBuf::from(decode_field(rel))));
                }
                other => {
                    return Err(corrupt(format!("riga non riconosciuta: {other:?}")));
                }
            }
        }

        let gd_root_field =
            gd_root_field.ok_or_else(|| corrupt("campo 'gd_root' assente".to_string()))?;

        Ok(BackupManifest {
            gd_root: gd_root_field,
            entries,
        })
    }

    /// Registra lo stato **pre-modifica** di un file della `GD_Installation`
    /// (Req 7.2). Va chiamata *prima* di toccare il file.
    ///
    /// - Se il file **esiste**, ne salva byte e hash come voce `Modified`, così
    ///   l'uninstall/rollback può riscriverli byte-per-byte (Req 7.3, 7.4).
    /// - Se il file è **assente**, lo marca `CreatedAbsent`, così l'uninstall
    ///   lo eliminerà.
    ///
    /// `abs_path` deve trovarsi all'interno della radice della
    /// `GD_Installation`; viene memorizzato come percorso relativo.
    pub fn record_before_modify(&mut self, abs_path: &Path) -> Result<(), InstallError> {
        let rel = self.rel_of(abs_path)?;
        if abs_path.exists() {
            let bytes = fs::read(abs_path).map_err(|source| InstallError::Io {
                path: abs_path.to_path_buf(),
                source,
            })?;
            self.entries.push(BackupEntry::modified(rel, bytes));
        } else {
            self.entries.push(BackupEntry::created_absent(rel));
        }
        Ok(())
    }

    /// Registra un file che l'Installer **creerà** e che era **assente** prima
    /// dell'installazione (Req 7.2, prior-absence), così l'uninstall lo elimina.
    pub fn record_created_absent(&mut self, abs_path: &Path) -> Result<(), InstallError> {
        let rel = self.rel_of(abs_path)?;
        self.entries.push(BackupEntry::created_absent(rel));
        Ok(())
    }

    /// Converte un percorso assoluto interno al bundle nel suo percorso
    /// relativo alla radice della `GD_Installation`.
    fn rel_of(&self, abs_path: &Path) -> Result<PathBuf, InstallError> {
        abs_path
            .strip_prefix(&self.gd_root)
            .map(Path::to_path_buf)
            .map_err(|_| InstallError::Io {
                path: abs_path.to_path_buf(),
                source: std::io::Error::new(
                    std::io::ErrorKind::InvalidInput,
                    format!(
                        "il percorso non è interno alla GD_Installation '{}'",
                        self.gd_root.display()
                    ),
                ),
            })
    }
}

// ---------------------------------------------------------------------------
// Guard di uninstall (Req 7.8).
// ---------------------------------------------------------------------------

/// Carica il restore record richiesto da un'operazione di uninstall (Req 7.8).
///
/// È il primo passo che l'uninstall (implementato nell'attività 7.4) deve
/// eseguire: se non esiste un backup manifest per la `GD_Installation`,
/// restituisce [`InstallError::MissingRestoreRecord`] **senza modificare
/// nulla**.
pub fn require_restore_record(gd_root: &Path) -> Result<BackupManifest, InstallError> {
    BackupManifest::load(gd_root)
}

// ---------------------------------------------------------------------------
// Install transazionale del Pulse_Loader (Req 7.1, 7.2, 7.6, 7.7).
// ---------------------------------------------------------------------------

/// Elenco completo dei file della `GD_Installation` modificati da
/// un'operazione dell'Installer (Req 7.7). L'ordine riflette i file toccati:
/// dylib copiata, eseguibile patchato, firma ri-generata.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModifiedFiles(pub Vec<PathBuf>);

impl ModifiedFiles {
    /// I percorsi assoluti dei file modificati.
    pub fn paths(&self) -> &[PathBuf] {
        &self.0
    }
}

/// Installa il Loader_Artifact in una `GD_Installation` riconosciuta (Req 7.1).
///
/// Sequenza **transazionale** (design §6):
///   1. `recognize_gd` (Req 7.5) e `ensure_not_already_installed` (Req 7.9) —
///      se falliscono, **nulla** è modificato.
///   2. Si registra in un [`BackupManifest`] lo stato pre-installazione di
///      **ogni** file che verrà toccato e lo si rende **durevole su disco PRIMA
///      di modificare qualunque file** (Req 7.2): l'eseguibile (patch
///      `LC_LOAD_DYLIB`), la firma (ad-hoc resign) e la dylib di destinazione
///      (creata, prior-absence).
///   3. Si applicano le modifiche: copia della dylib in `Contents/MacOS/`,
///      patch del `LC_LOAD_DYLIB` (`@loader_path/libpulse_loader.dylib`) e
///      ad-hoc resign con `disable-library-validation`. Si modificano **solo**
///      {eseguibile, firma, dylib di destinazione} (Req 7.6).
///   4. Su qualunque errore di modifica si ripristina lo stato
///      pre-installazione dal backup (seam condiviso con l'attività 7.4,
///      Req 7.4) e si propaga l'errore.
///
/// In caso di successo restituisce l'elenco completo dei file modificati
/// (Req 7.7).
///
/// **Seam iniettabile (task 7.2 + 9.1).** Le due operazioni native — patch del
/// `LC_LOAD_DYLIB` e ad-hoc resign — sono dietro il trait [`Injector`]. La
/// versione di default ([`install`]) usa [`HostSeamInjector`], lo stub
/// host-testabile che opera sull'albero di GD finto e preserva i test delle
/// attività 7.2/7.4. Il path nativo macOS reale di Fase E è
/// [`install_native`], che usa `NativeMacOsInjector` (vera modifica del Mach-O
/// e `codesign`) senza cambiare la sequenza transazionale.
pub fn install(gd_root: &Path, artifact: &Path) -> Result<ModifiedFiles, InstallError> {
    install_with(gd_root, artifact, &HostSeamInjector)
}

/// Variante **nativa macOS** dell'install (Fase E / task 9.1, Req 2.1, 2.2, 2.9).
///
/// Esegue la stessa sequenza transazionale di [`install`] ma applica la **vera**
/// patch `LC_LOAD_DYLIB` sull'eseguibile Mach-O di GD e il **vero** ad-hoc
/// resign (`codesign -f -s - --entitlements pulse.entitlements` con
/// `com.apple.security.cs.disable-library-validation`), così che dyld carichi
/// `libpulse_loader.dylib` in early-load **prima** di `MenuLayer::init` e il
/// binario Steam di GD non rifiuti la dylib sotto hardened-runtime /
/// library-validation. Con questa iniezione in essere, dentro il processo GD
/// reale `MacOSBootstrap::inject()` ritorna `success()` invece di
/// `UnsupportedHost` (Req 2.9).
///
/// **Passi manuali dell'User (NON automatizzabili in CI, design §2):** l'avvio
/// del gioco da Steam e l'accettazione degli eventuali prompt di Gatekeeper al
/// primo avvio restano azioni dell'User; questa funzione prepara solo il bundle.
///
/// Su piattaforme diverse da macOS la funzione ritorna un errore esplicito
/// senza modificare nulla: l'iniezione nativa richiede il toolchain
/// `codesign` di macOS e il binario reale di GD.
pub fn install_native(gd_root: &Path, artifact: &Path) -> Result<ModifiedFiles, InstallError> {
    #[cfg(target_os = "macos")]
    {
        install_with(gd_root, artifact, &NativeMacOsInjector)
    }
    #[cfg(not(target_os = "macos"))]
    {
        let _ = (gd_root, artifact);
        Err(InstallError::UnrecognizedInstallation {
            path: gd_root.to_path_buf(),
            reason: "l'iniezione nativa LC_LOAD_DYLIB + ad-hoc resign è disponibile solo su macOS"
                .to_string(),
        })
    }
}

/// Install transazionale parametrizzata sul seam [`Injector`] (Req 7.1, 7.2,
/// 7.6, 7.7).
///
/// Sequenza **transazionale** (design §6):
///   1. `recognize_gd` (Req 7.5) e `ensure_not_already_installed` (Req 7.9) —
///      se falliscono, **nulla** è modificato.
///   2. Si registra in un [`BackupManifest`] lo stato pre-installazione di
///      **ogni** file che verrà toccato e lo si rende **durevole su disco PRIMA
///      di modificare qualunque file** (Req 7.2): l'eseguibile (patch
///      `LC_LOAD_DYLIB`), la firma (ad-hoc resign) e la dylib di destinazione
///      (creata, prior-absence).
///   3. Si applicano le modifiche tramite `injector`: copia della dylib in
///      `Contents/MacOS/`, patch del `LC_LOAD_DYLIB`
///      (`@loader_path/libpulse_loader.dylib`) e ad-hoc resign con
///      `disable-library-validation`. Si modificano **solo** {eseguibile,
///      firma, dylib di destinazione} (Req 7.6).
///   4. Su qualunque errore di modifica si ripristina lo stato
///      pre-installazione dal backup (Req 7.4) e si propaga l'errore.
///
/// In caso di successo restituisce l'elenco completo dei file modificati
/// (Req 7.7).
pub fn install_with(
    gd_root: &Path,
    artifact: &Path,
    injector: &dyn Injector,
) -> Result<ModifiedFiles, InstallError> {
    let gd = recognize_gd(gd_root)?; // Req 7.5
    ensure_not_already_installed(&gd)?; // Req 7.9

    let executable = gd.executable();
    let code_signature = gd.code_signature();
    let dylib_dest = gd.dylib_dest();

    // (2) Backup completo PRIMA di toccare qualunque file (Req 7.2).
    let mut backup = BackupManifest::new(&gd);
    backup.record_before_modify(&executable)?; // patch LC_LOAD_DYLIB
    backup.record_before_modify(&code_signature)?; // ad-hoc resign
    backup.record_created_absent(&dylib_dest)?; // dylib creata (prior-absence)
    backup.persist()?; // durevole prima di agire

    // (3) Modifiche. (4) Su qualunque errore → rollback completo (Req 7.4).
    let outcome = (|| -> Result<(), InstallError> {
        copy_file(artifact, &dylib_dest)?;
        injector.patch_lc_load_dylib(&executable, LOADER_DYLIB_LOAD_PATH)?;
        injector.adhoc_resign_with_disable_lv(&gd)?;
        Ok(())
    })();

    if let Err(err) = outcome {
        // Best-effort: riporta l'albero allo stato pre-installazione.
        restore_from_backup(&backup);
        return Err(err);
    }

    // Req 7.6 + 7.7: SOLO questi file sono stati modificati; elenco completo.
    Ok(ModifiedFiles(vec![dylib_dest, executable, code_signature]))
}

/// Ripristina la `GD_Installation` allo stato registrato nel backup manifest.
///
/// **Fondamento condiviso** (seam) con l'uninstall dell'attività 7.4: ripristina
/// ogni file `Modified` ai byte originali, elimina i file `CreatedAbsent` e
/// rimuove la directory di backup, riportando l'albero allo stato
/// pre-installazione (Req 7.4). Restituisce l'elenco dei file
/// ripristinati/eliminati (Req 7.7). È best-effort: ignora gli errori dei
/// singoli passi così da rimuovere quanto più possibile.
pub(crate) fn restore_from_backup(backup: &BackupManifest) -> Vec<PathBuf> {
    let mut touched = Vec::new();
    for entry in &backup.entries {
        let abs = backup.gd_root.join(&entry.rel_path);
        match &entry.kind {
            BackupKind::Modified { bytes, .. } => {
                if fs::write(&abs, bytes).is_ok() {
                    touched.push(abs);
                }
            }
            BackupKind::CreatedAbsent => {
                if abs.exists() {
                    let _ = fs::remove_file(&abs);
                }
                touched.push(abs);
            }
        }
    }
    // Rimuove il restore record (sibling del bundle) per un albero pristino.
    let _ = fs::remove_dir_all(backup_root_for(&backup.gd_root));
    touched
}

/// Disinstalla il Pulse_Loader da una `GD_Installation` (Req 7.3, 7.7, 7.8).
///
/// Sequenza:
///   1. Carica il restore record via [`require_restore_record`]. Se **nessun**
///      backup manifest esiste per la `GD_Installation`, restituisce
///      [`InstallError::MissingRestoreRecord`] **senza modificare nulla**
///      (Req 7.8).
///   2. Applica il **core di ripristino condiviso** [`restore_from_backup`]
///      (lo stesso usato dal rollback su install fallita): riscrive ogni file
///      `Modified` ai byte originali byte-per-byte, elimina i file
///      `CreatedAbsent` (la dylib del loader) e rimuove la directory di backup
///      (il manifest), riportando il bundle allo stato pre-installazione
///      (Req 7.3).
///   3. Restituisce l'elenco completo dei file ripristinati/eliminati (Req 7.7).
///
/// Poiché il loader non scrive mai sul bundle a runtime, l'uninstall riporta la
/// `GD_Installation` esattamente allo stato che aveva prima dell'install.
pub fn uninstall(gd_root: &Path) -> Result<ModifiedFiles, InstallError> {
    // (1) Req 7.8 — senza restore record: errore, nulla cambia.
    let backup = require_restore_record(gd_root)?;
    // (2) Req 7.3 — ripristino byte-esatto + rimozione del manifest.
    // (3) Req 7.7 — elenco dei file toccati.
    let touched = restore_from_backup(&backup);
    Ok(ModifiedFiles(touched))
}

/// Copia `src` in `dest`, creando le directory genitrici se necessario.
fn copy_file(src: &Path, dest: &Path) -> Result<(), InstallError> {
    if let Some(parent) = dest.parent() {
        fs::create_dir_all(parent).map_err(|source| InstallError::Io {
            path: parent.to_path_buf(),
            source,
        })?;
    }
    fs::copy(src, dest).map_err(|source| InstallError::Io {
        path: src.to_path_buf(),
        source,
    })?;
    Ok(())
}

// ---------------------------------------------------------------------------
// Seam iniettabile delle operazioni native (task 7.2 stub + 9.1 reale).
// ---------------------------------------------------------------------------

/// Contenuto del file entitlements ad-hoc applicato all'eseguibile di GD dal
/// resign nativo (Fase E, Req 2.2). È l'**unica** fonte del contenuto: l'asset
/// `cli/assets/pulse.entitlements` è incluso a compile-time così che il binario
/// `pulse` non dipenda da un file su disco a runtime. Dichiara
/// `com.apple.security.cs.disable-library-validation` per far accettare a dyld
/// la dylib ad-hoc del loader sotto hardened runtime.
pub const PULSE_ENTITLEMENTS_PLIST: &str = include_str!("../assets/pulse.entitlements");

/// Le due operazioni native dell'install — patch del `LC_LOAD_DYLIB` e ad-hoc
/// resign — dietro un seam iniettabile.
///
/// - [`HostSeamInjector`] è lo **stub host-testabile** (default di [`install`]):
///   opera sull'albero di GD finto in modo deterministico e reversibile, così i
///   test delle attività 7.2/7.4 restano validi in CI.
/// - `NativeMacOsInjector` è il **path reale di Fase E** (usato da
///   [`install_native`] su macOS): inserisce davvero un load command
///   `LC_LOAD_DYLIB` nell'eseguibile Mach-O di GD e invoca `codesign` per il
///   resign ad-hoc con `disable-library-validation`.
pub trait Injector {
    /// Fa sì che dyld carichi la dylib del loader in early-load aggiungendo il
    /// path di load `load_path` (`@loader_path/libpulse_loader.dylib`)
    /// all'eseguibile di GD (Req 7.1, 2.1).
    fn patch_lc_load_dylib(&self, executable: &Path, load_path: &str)
        -> Result<(), InstallError>;

    /// Ri-firma ad-hoc l'eseguibile (e la dylib) della `GD_Installation` con
    /// l'entitlement `com.apple.security.cs.disable-library-validation`, così
    /// dyld accetti la dylib del loader sotto hardened runtime (Req 2.2).
    fn adhoc_resign_with_disable_lv(&self, gd: &GdInstallation) -> Result<(), InstallError>;
}

/// Stub **host-testabile** del seam (default di [`install`]).
///
/// Simula la patch `LC_LOAD_DYLIB` appendendo un marker deterministico
/// all'eseguibile e il resign riscrivendo il file di firma, rendendo le
/// modifiche osservabili e mantenendo verificabile il ripristino byte-esatto
/// (il backup conserva i byte originali). Non richiede né il binario reale di
/// GD né il toolchain `codesign`.
pub struct HostSeamInjector;

impl Injector for HostSeamInjector {
    fn patch_lc_load_dylib(
        &self,
        executable: &Path,
        load_path: &str,
    ) -> Result<(), InstallError> {
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

    fn adhoc_resign_with_disable_lv(&self, gd: &GdInstallation) -> Result<(), InstallError> {
        let code_signature = gd.code_signature();
        if let Some(parent) = code_signature.parent() {
            fs::create_dir_all(parent).map_err(|source| InstallError::Io {
                path: parent.to_path_buf(),
                source,
            })?;
        }
        fs::write(
            &code_signature,
            b"<pulse ad-hoc resign: com.apple.security.cs.disable-library-validation>",
        )
        .map_err(|source| InstallError::Io {
            path: code_signature.clone(),
            source,
        })?;
        Ok(())
    }
}

/// Injector **nativo macOS reale** (Fase E / task 9.1, Req 2.1, 2.2, 2.9).
///
/// `patch_lc_load_dylib` inserisce un vero load command `LC_LOAD_DYLIB`
/// nell'eseguibile Mach-O di GD (riusando il padding dell'header, senza
/// spostare i dati delle sezioni — vedi [`macho`]); `adhoc_resign_with_disable_lv`
/// invoca il tool `codesign` di macOS per ri-firmare ad-hoc l'eseguibile con
/// l'entitlement `com.apple.security.cs.disable-library-validation` e firmare
/// ad-hoc anche la dylib del loader.
///
/// **Solo su macOS** (`#[cfg(target_os = "macos")]`): richiede il binario reale
/// di GD e il toolchain `codesign`, quindi **non è auto-verificabile in CI** —
/// l'avvio del gioco e i prompt di Gatekeeper restano passi manuali dell'User.
#[cfg(target_os = "macos")]
pub struct NativeMacOsInjector;

#[cfg(target_os = "macos")]
impl Injector for NativeMacOsInjector {
    fn patch_lc_load_dylib(
        &self,
        executable: &Path,
        load_path: &str,
    ) -> Result<(), InstallError> {
        let mut bytes = fs::read(executable).map_err(|source| InstallError::Io {
            path: executable.to_path_buf(),
            source,
        })?;
        macho::add_load_dylib_command(&mut bytes, load_path).map_err(|reason| {
            InstallError::MachoPatch {
                path: executable.to_path_buf(),
                reason,
            }
        })?;
        // Scrive i byte patchati in un file NUOVO accanto al target e poi lo
        // rinomina sopra l'originale. Un file appena creato non eredita
        // resource fork / Finder info ("detritus") che farebbero rifiutare la
        // firma a `codesign`; la riscrittura in-place dell'inode esistente li
        // conserverebbe. Il rename è atomico (stessa directory/filesystem).
        write_fresh_replace(executable, &bytes)
    }

    fn adhoc_resign_with_disable_lv(&self, gd: &GdInstallation) -> Result<(), InstallError> {
        let dylib = gd.dylib_dest();

        // L'entitlement è scritto in un file temporaneo dal contenuto embedded
        // (PULSE_ENTITLEMENTS_PLIST) e passato a `codesign --entitlements`.
        let entitlements = write_temp_entitlements()?;
        let entitlements_cleanup = entitlements.clone();

        // 1) Firma ad-hoc la dylib del loader (nessun entitlement): così, quando
        //    si sigilla il bundle, è già "nested code" firmato.
        let dylib_res = codesign_adhoc(&dylib, None, false);

        // 2) Firma ad-hoc l'INTERO bundle `.app` con l'entitlement
        //    disable-library-validation. Firmare il bundle (non solo
        //    l'eseguibile) sigilla i contenuti — inclusa la dylib aggiunta in
        //    Contents/MacOS/ — in `_CodeSignature/CodeResources` e applica
        //    l'entitlement all'eseguibile principale. Firmare solo l'eseguibile
        //    fallisce con "unsealed contents present in the bundle root" perché
        //    la dylib resterebbe non sigillata (Req 2.2).
        let bundle_res = dylib_res.and_then(|_| codesign_adhoc(&gd.root, Some(&entitlements), true));

        // Pulizia best-effort del file temporaneo degli entitlements.
        let _ = fs::remove_file(&entitlements_cleanup);

        bundle_res
    }
}

/// Scrive il contenuto degli entitlements ([`PULSE_ENTITLEMENTS_PLIST`]) in un
/// file temporaneo univoco e ne restituisce il percorso (Fase E).
#[cfg(target_os = "macos")]
fn write_temp_entitlements() -> Result<PathBuf, InstallError> {
    use std::sync::atomic::{AtomicU64, Ordering};
    static COUNTER: AtomicU64 = AtomicU64::new(0);
    let n = COUNTER.fetch_add(1, Ordering::Relaxed);
    let path = std::env::temp_dir().join(format!(
        "pulse-entitlements-{}-{n}.plist",
        std::process::id()
    ));
    fs::write(&path, PULSE_ENTITLEMENTS_PLIST).map_err(|source| InstallError::Io {
        path: path.clone(),
        source,
    })?;
    Ok(path)
}

/// Invoca `codesign -f -s - [--entitlements <ent>] <target>` (ad-hoc resign).
///
/// `-f` forza la sovrascrittura della firma esistente, `-s -` seleziona la
/// firma **ad-hoc** (nessuna identità/Team ID). Restituisce
/// [`InstallError::Codesign`] con lo stderr di `codesign` su fallimento.
/// Scrive `bytes` in un file temporaneo nella stessa directory di `target` e
/// lo rinomina sopra `target` (rename atomico). Un file appena creato non
/// eredita resource fork / Finder info dell'originale, evitando l'errore
/// "detritus not allowed" di `codesign`. Imposta il permesso eseguibile
/// (rwxr-xr-x) sul nuovo file, dato che `target` è un eseguibile.
#[cfg(target_os = "macos")]
fn write_fresh_replace(target: &Path, bytes: &[u8]) -> Result<(), InstallError> {
    use std::os::unix::fs::PermissionsExt;
    let dir = target.parent().unwrap_or_else(|| Path::new("."));
    let tmp = dir.join(format!(".pulse-patch-{}.tmp", std::process::id()));
    fs::write(&tmp, bytes).map_err(|source| InstallError::Io {
        path: tmp.clone(),
        source,
    })?;
    let _ = fs::set_permissions(&tmp, fs::Permissions::from_mode(0o755));
    fs::rename(&tmp, target).map_err(|source| InstallError::Io {
        path: target.to_path_buf(),
        source,
    })?;
    Ok(())
}

#[cfg(target_os = "macos")]
fn codesign_adhoc(
    target: &Path,
    entitlements: Option<&Path>,
    deep: bool,
) -> Result<(), InstallError> {
    use std::process::Command;

    // `codesign` rifiuta di firmare file con attributi estesi (resource fork /
    // Finder info / quarantine) — "resource fork, Finder information, or
    // similar detritus not allowed". Li azzeriamo (best-effort) prima di
    // firmare; spesso provengono dalla copia del bundle (`cp -R` preserva gli
    // xattr).
    let _ = Command::new("/usr/bin/xattr").arg("-c").arg(target).output();

    let mut cmd = Command::new("codesign");
    cmd.arg("-f").arg("-s").arg("-");
    if deep {
        // Firma anche tutto il codice annidato (es. Contents/Frameworks/*.dylib,
        // come `Geode.dylib` se GD ha già Geode installato) ad-hoc, così la
        // sigillatura del bundle non fallisce con "code object is not signed at
        // all" su una subcomponente non firmata.
        cmd.arg("--deep");
    }
    if let Some(ent) = entitlements {
        cmd.arg("--entitlements").arg(ent);
    }
    cmd.arg(target);

    let output = cmd.output().map_err(|source| InstallError::Codesign {
        path: target.to_path_buf(),
        reason: format!("impossibile eseguire `codesign`: {source}"),
    })?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        return Err(InstallError::Codesign {
            path: target.to_path_buf(),
            reason: format!(
                "codesign è uscito con {} — {}",
                output.status,
                stderr.trim()
            ),
        });
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// Inserimento reale del load command LC_LOAD_DYLIB nel Mach-O (Fase E).
// ---------------------------------------------------------------------------

/// Manipolazione Mach-O minimale per inserire un load command `LC_LOAD_DYLIB`
/// nell'eseguibile di GD (Fase E / task 9.1, Req 2.1).
///
/// La strategia (la stessa di `insert_dylib`) **non sposta i dati delle
/// sezioni**: riusa il padding di zeri tra la fine dei load command e l'inizio
/// del primo dato di sezione (l'"header padding"). Per questo la dimensione del
/// file/slice resta invariata e, nei binari universali (FAT), gli offset di
/// ogni arch restano validi senza riscriverli.
///
/// Supporta Mach-O 64-bit little-endian (arm64/arm64e/x86_64 — il
/// Prioritized_Target) sia thin sia FAT (32/64-bit). I formati non supportati
/// (big-endian, 32-bit thin, padding insufficiente) producono un errore
/// descrittivo: l'install esegue il rollback e l'eseguibile resta invariato.
#[cfg(target_os = "macos")]
mod macho {
    // Magic numbers Mach-O / FAT.
    const MH_MAGIC_64: u32 = 0xfeed_facf; // 64-bit, host-endian (LE su macOS arm64/x86_64)
    const MH_CIGAM_64: u32 = 0xcffa_edfe; // 64-bit, byte-swapped (big-endian) — non supportato
    const MH_MAGIC_32: u32 = 0xfeed_face; // 32-bit thin — non supportato
    const MH_CIGAM_32: u32 = 0xcefa_edfe;
    const FAT_MAGIC: u32 = 0xcafe_babe; // FAT, big-endian su disco
    const FAT_CIGAM: u32 = 0xbeba_feca;
    const FAT_MAGIC_64: u32 = 0xcafe_babf;
    const FAT_CIGAM_64: u32 = 0xbfba_feca;

    const LC_SEGMENT_64: u32 = 0x19;
    const LC_LOAD_DYLIB: u32 = 0x0c;

    const MACH_HEADER_64_SIZE: usize = 32;

    /// Inserisce un `LC_LOAD_DYLIB` per `load_path` nell'eseguibile contenuto in
    /// `buf` (thin o FAT). Modifica `buf` in place.
    pub fn add_load_dylib_command(buf: &mut [u8], load_path: &str) -> Result<(), String> {
        if buf.len() < 4 {
            return Err("file troppo piccolo per essere un Mach-O".to_string());
        }
        let magic_le = u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]);
        let magic_be = u32::from_be_bytes([buf[0], buf[1], buf[2], buf[3]]);

        match magic_le {
            MH_MAGIC_64 => patch_thin_64(buf, 0, load_path),
            MH_CIGAM_64 | MH_MAGIC_32 | MH_CIGAM_32 => Err(format!(
                "formato Mach-O non supportato (magic {magic_le:#010x}): \
                 atteso 64-bit little-endian"
            )),
            _ => match magic_be {
                FAT_MAGIC | FAT_CIGAM => patch_fat(buf, load_path, false),
                FAT_MAGIC_64 | FAT_CIGAM_64 => patch_fat(buf, load_path, true),
                _ => Err(format!(
                    "magic non riconosciuto ({magic_le:#010x}): non è un Mach-O 64-bit né un FAT"
                )),
            },
        }
    }

    /// Patcha ogni slice 64-bit little-endian di un binario universale (FAT).
    fn patch_fat(buf: &mut [u8], load_path: &str, fat64: bool) -> Result<(), String> {
        // fat_header: magic (be u32), nfat_arch (be u32).
        let nfat = read_u32_be(buf, 4)? as usize;
        // Offset e size di ogni slice (raccolti prima di mutare il buffer).
        let mut slices: Vec<usize> = Vec::with_capacity(nfat);
        let mut cursor = 8usize;
        for _ in 0..nfat {
            if fat64 {
                // fat_arch_64: cputype,cpusubtype (u32), offset,size (u64), align,reserved (u32).
                let offset = read_u64_be(buf, cursor + 8)? as usize;
                slices.push(offset);
                cursor += 32;
            } else {
                // fat_arch: cputype,cpusubtype,offset,size,align (u32).
                let offset = read_u32_be(buf, cursor + 8)? as usize;
                slices.push(offset);
                cursor += 20;
            }
        }

        let mut patched_any = false;
        let mut errors: Vec<String> = Vec::new();
        for base in slices {
            if base + 4 > buf.len() {
                errors.push(format!("slice FAT con offset {base} fuori dal file"));
                continue;
            }
            let magic = u32::from_le_bytes([buf[base], buf[base + 1], buf[base + 2], buf[base + 3]]);
            if magic == MH_MAGIC_64 {
                match patch_thin_64(buf, base, load_path) {
                    Ok(()) => patched_any = true,
                    Err(e) => errors.push(format!("slice@{base}: {e}")),
                }
            } else {
                // Slice non 64-bit-LE (es. arch a 32-bit): saltata.
                errors.push(format!(
                    "slice@{base}: magic {magic:#010x} non 64-bit-LE, saltata"
                ));
            }
        }

        if patched_any {
            Ok(())
        } else {
            Err(format!(
                "nessuna slice 64-bit little-endian patchabile nel FAT: {}",
                errors.join("; ")
            ))
        }
    }

    /// Inserisce il load command nel Mach-O 64-bit thin con base `base`.
    fn patch_thin_64(buf: &mut [u8], base: usize, load_path: &str) -> Result<(), String> {
        if base + MACH_HEADER_64_SIZE > buf.len() {
            return Err("header Mach-O 64-bit troncato".to_string());
        }
        let ncmds = read_u32_le(buf, base + 16)? as usize;
        let sizeofcmds = read_u32_le(buf, base + 20)? as usize;

        let cmds_start = base + MACH_HEADER_64_SIZE;
        let cmds_end = cmds_start + sizeofcmds;
        if cmds_end > buf.len() {
            return Err("load commands oltre la fine del file".to_string());
        }

        // Offset minimo dei dati di sezione (per misurare l'header padding).
        let min_section_offset =
            min_section_file_offset(buf, base, cmds_start, ncmds)?.unwrap_or(sizeofcmds);

        // Costruisce il nuovo dylib_command.
        let new_cmd = build_dylib_command(load_path);
        let cmd_size = new_cmd.len();

        // Spazio libero (header padding) disponibile dopo i load command.
        // È relativo a `base`: i dati di sezione iniziano a base+min_section_offset.
        let free_start = cmds_end; // assoluto
        let free_end = base + min_section_offset; // assoluto
        if free_end < free_start {
            return Err("layout Mach-O inatteso (offset sezione < fine load commands)".to_string());
        }
        let available = free_end - free_start;
        if available < cmd_size {
            return Err(format!(
                "header padding insufficiente: servono {cmd_size} byte, disponibili {available}"
            ));
        }

        // Il padding deve essere tutto zero, per non sovrascrivere dati reali.
        if buf[free_start..free_start + cmd_size].iter().any(|&b| b != 0) {
            return Err(
                "l'area dopo i load commands non è padding di zeri: inserimento non sicuro"
                    .to_string(),
            );
        }

        // Scrive il nuovo comando e aggiorna l'header (ncmds, sizeofcmds).
        buf[free_start..free_start + cmd_size].copy_from_slice(&new_cmd);
        write_u32_le(buf, base + 16, (ncmds + 1) as u32);
        write_u32_le(buf, base + 20, (sizeofcmds + cmd_size) as u32);
        Ok(())
    }

    /// Scorre i load command e restituisce l'offset-file minimo (>0) tra le
    /// sezioni dei segmenti `LC_SEGMENT_64`, ovvero l'inizio dei dati reali.
    fn min_section_file_offset(
        buf: &[u8],
        _base: usize,
        cmds_start: usize,
        ncmds: usize,
    ) -> Result<Option<usize>, String> {
        let mut min_off: Option<usize> = None;
        let mut off = cmds_start;
        for _ in 0..ncmds {
            if off + 8 > buf.len() {
                return Err("load command troncato".to_string());
            }
            let cmd = read_u32_le(buf, off)?;
            let cmdsize = read_u32_le(buf, off + 4)? as usize;
            if cmdsize < 8 || off + cmdsize > buf.len() {
                return Err(format!("cmdsize non valido ({cmdsize}) a offset {off}"));
            }
            if cmd == LC_SEGMENT_64 {
                // segment_command_64: nsects è a +64, le sezioni iniziano a +72.
                let nsects = read_u32_le(buf, off + 64)? as usize;
                let mut sec = off + 72;
                for _ in 0..nsects {
                    if sec + 80 > buf.len() {
                        return Err("section_64 troncata".to_string());
                    }
                    // section_64.offset (u32) è a +48 nella struct di sezione.
                    let sec_off = read_u32_le(buf, sec + 48)? as usize;
                    // section_64.size (u64) a +40 — solo sezioni con dati su file.
                    let sec_size = read_u64_le(buf, sec + 40)? as usize;
                    if sec_size > 0 && sec_off > 0 {
                        // In un Mach-O (anche dentro un FAT) gli offset dei load
                        // command sono relativi all'INIZIO DELLA SLICE, non del
                        // file fat: `sec_off` è già slice-relative. La posizione
                        // assoluta nel file è `base + sec_off`, quindi qui
                        // teniamo l'offset slice-relative così com'è (i chiamanti
                        // sommano `base`). Sottrarre `base` era errato per le
                        // slice con base != 0 (binari universali).
                        let rel = sec_off;
                        min_off = Some(match min_off {
                            Some(m) => m.min(rel),
                            None => rel,
                        });
                    }
                    sec += 80;
                }
            }
            off += cmdsize;
        }
        Ok(min_off)
    }

    /// Costruisce i byte di un `dylib_command` (LC_LOAD_DYLIB) per `load_path`.
    fn build_dylib_command(load_path: &str) -> Vec<u8> {
        // dylib_command fisso = 24 byte: cmd, cmdsize, name.offset, timestamp,
        // current_version, compatibility_version (6 × u32).
        const HEADER: usize = 24;
        let path_bytes = load_path.as_bytes();
        // name null-terminato, l'intero comando allineato a 8 byte.
        let unaligned = HEADER + path_bytes.len() + 1;
        let cmdsize = (unaligned + 7) & !7usize; // round-up a multiplo di 8
        let mut cmd = vec![0u8; cmdsize];
        write_u32_le(&mut cmd, 0, LC_LOAD_DYLIB);
        write_u32_le(&mut cmd, 4, cmdsize as u32);
        write_u32_le(&mut cmd, 8, HEADER as u32); // name.offset
        write_u32_le(&mut cmd, 12, 2); // timestamp (convenzione: 2)
        write_u32_le(&mut cmd, 16, 0); // current_version
        write_u32_le(&mut cmd, 20, 0); // compatibility_version
        cmd[HEADER..HEADER + path_bytes.len()].copy_from_slice(path_bytes);
        // il resto resta zero (terminatore + padding di allineamento)
        cmd
    }

    // -- Letture/scritture endian-safe con controllo dei limiti. --

    fn read_u32_le(buf: &[u8], off: usize) -> Result<u32, String> {
        buf.get(off..off + 4)
            .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
            .ok_or_else(|| format!("lettura u32 LE fuori dai limiti a {off}"))
    }

    fn read_u64_le(buf: &[u8], off: usize) -> Result<u64, String> {
        buf.get(off..off + 8)
            .map(|b| u64::from_le_bytes([b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]]))
            .ok_or_else(|| format!("lettura u64 LE fuori dai limiti a {off}"))
    }

    fn read_u32_be(buf: &[u8], off: usize) -> Result<u32, String> {
        buf.get(off..off + 4)
            .map(|b| u32::from_be_bytes([b[0], b[1], b[2], b[3]]))
            .ok_or_else(|| format!("lettura u32 BE fuori dai limiti a {off}"))
    }

    fn read_u64_be(buf: &[u8], off: usize) -> Result<u64, String> {
        buf.get(off..off + 8)
            .map(|b| u64::from_be_bytes([b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]]))
            .ok_or_else(|| format!("lettura u64 BE fuori dai limiti a {off}"))
    }

    fn write_u32_le(buf: &mut [u8], off: usize, value: u32) {
        buf[off..off + 4].copy_from_slice(&value.to_le_bytes());
    }

    #[cfg(test)]
    mod tests {
        use super::*;

        /// Costruisce un Mach-O 64-bit thin minimale: header + un
        /// `LC_SEGMENT_64` (__TEXT) con una sezione i cui dati iniziano dopo un
        /// padding di zeri sufficiente, così l'inserimento del load command è
        /// possibile.
        fn minimal_thin_macho() -> Vec<u8> {
            // Layout: [header 32][LC_SEGMENT_64 (72 + 80 section)][padding zeri][dati]
            let seg_cmdsize = 72 + 80;
            let header_and_cmds = MACH_HEADER_64_SIZE + seg_cmdsize;
            let section_offset = 0x1000; // i dati iniziano a 4KB → tanto padding
            let mut buf = vec![0u8; section_offset + 16];

            // mach_header_64
            write_u32_le(&mut buf, 0, MH_MAGIC_64);
            // cputype/cpusubtype/filetype non rilevanti per il test
            write_u32_le(&mut buf, 16, 1); // ncmds = 1
            write_u32_le(&mut buf, 20, seg_cmdsize as u32); // sizeofcmds

            // LC_SEGMENT_64 a offset 32
            let seg = MACH_HEADER_64_SIZE;
            write_u32_le(&mut buf, seg, LC_SEGMENT_64);
            write_u32_le(&mut buf, seg + 4, seg_cmdsize as u32);
            write_u32_le(&mut buf, seg + 64, 1); // nsects = 1

            // section_64 a offset seg+72
            let sec = seg + 72;
            write_u32_le(&mut buf, sec + 40, 16); // size (u64 low)
            write_u32_le(&mut buf, sec + 48, section_offset as u32); // offset

            // Dati della sezione (non-zero) a section_offset.
            for b in &mut buf[section_offset..section_offset + 16] {
                *b = 0xAB;
            }
            let _ = header_and_cmds;
            buf
        }

        #[test]
        fn inserts_load_dylib_into_thin_macho() {
            let mut buf = minimal_thin_macho();
            let ncmds_before = read_u32_le(&buf, 16).unwrap();
            let sizeofcmds_before = read_u32_le(&buf, 20).unwrap();
            let len_before = buf.len();

            add_load_dylib_command(&mut buf, "@loader_path/libpulse_loader.dylib")
                .expect("inserimento deve riuscire");

            // ncmds e sizeofcmds aggiornati; dimensione file invariata (in-place).
            assert_eq!(read_u32_le(&buf, 16).unwrap(), ncmds_before + 1);
            let cmd = build_dylib_command("@loader_path/libpulse_loader.dylib");
            assert_eq!(
                read_u32_le(&buf, 20).unwrap(),
                sizeofcmds_before + cmd.len() as u32
            );
            assert_eq!(buf.len(), len_before, "la patch non cambia la dimensione");

            // Il nuovo comando è LC_LOAD_DYLIB col path atteso.
            let new_cmd_off = MACH_HEADER_64_SIZE + sizeofcmds_before as usize;
            assert_eq!(read_u32_le(&buf, new_cmd_off).unwrap(), LC_LOAD_DYLIB);
            let name = &buf[new_cmd_off + 24..];
            assert!(
                name.starts_with(b"@loader_path/libpulse_loader.dylib\0"),
                "il path di load deve essere presente e null-terminato"
            );
        }

        #[test]
        fn dylib_command_is_8_byte_aligned() {
            for path in [
                "@loader_path/x.dylib",
                "@loader_path/libpulse_loader.dylib",
                "a",
            ] {
                let cmd = build_dylib_command(path);
                assert_eq!(cmd.len() % 8, 0, "cmdsize deve essere multiplo di 8");
                assert!(cmd.len() >= 24 + path.len() + 1);
            }
        }

        #[test]
        fn rejects_non_macho() {
            let mut buf = b"not a mach-o file at all".to_vec();
            assert!(add_load_dylib_command(&mut buf, "@loader_path/x.dylib").is_err());
        }

        #[test]
        fn rejects_insufficient_padding() {
            // Header + segment senza padding sufficiente: i dati seguono subito.
            let seg_cmdsize = 72 + 80;
            let mut buf = vec![0u8; MACH_HEADER_64_SIZE + seg_cmdsize + 4];
            write_u32_le(&mut buf, 0, MH_MAGIC_64);
            write_u32_le(&mut buf, 16, 1);
            write_u32_le(&mut buf, 20, seg_cmdsize as u32);
            let seg = MACH_HEADER_64_SIZE;
            write_u32_le(&mut buf, seg, LC_SEGMENT_64);
            write_u32_le(&mut buf, seg + 4, seg_cmdsize as u32);
            write_u32_le(&mut buf, seg + 64, 1);
            let sec = seg + 72;
            write_u32_le(&mut buf, sec + 40, 4); // size
            // offset appena dopo i comandi → nessuno spazio per un nuovo comando
            write_u32_le(&mut buf, sec + 48, (MACH_HEADER_64_SIZE + seg_cmdsize) as u32);

            let err = add_load_dylib_command(&mut buf, "@loader_path/libpulse_loader.dylib")
                .unwrap_err();
            assert!(err.contains("padding insufficiente"), "err: {err}");
        }
    }
}

// ---------------------------------------------------------------------------
// Helper.
// ---------------------------------------------------------------------------

/// Calcola l'hash SHA-256 di un buffer come stringa esadecimale minuscola.
fn sha256_hex(bytes: &[u8]) -> String {
    let mut hasher = Sha256::new();
    hasher.update(bytes);
    let digest = hasher.finalize();
    let mut s = String::with_capacity(digest.len() * 2);
    const HEX: &[u8; 16] = b"0123456789abcdef";
    for &b in digest.iter() {
        s.push(HEX[(b >> 4) as usize] as char);
        s.push(HEX[(b & 0x0f) as usize] as char);
    }
    s
}

/// Codifica un campo del manifest neutralizzando i caratteri di separazione
/// (`\t`, `\n`, `\r`, `\\`) così che ogni voce resti su una sola riga e
/// tab-separata in modo non ambiguo.
fn encode_field(value: &str) -> String {
    let mut out = String::with_capacity(value.len());
    for ch in value.chars() {
        match ch {
            '\\' => out.push_str("\\\\"),
            '\t' => out.push_str("\\t"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            other => out.push(other),
        }
    }
    out
}

/// Inverso di [`encode_field`].
fn decode_field(value: &str) -> String {
    let mut out = String::with_capacity(value.len());
    let mut chars = value.chars();
    while let Some(ch) = chars.next() {
        if ch == '\\' {
            match chars.next() {
                Some('\\') => out.push('\\'),
                Some('t') => out.push('\t'),
                Some('n') => out.push('\n'),
                Some('r') => out.push('\r'),
                Some(other) => {
                    out.push('\\');
                    out.push(other);
                }
                None => out.push('\\'),
            }
        } else {
            out.push(ch);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    /// Directory temporanea isolata per i test, con pulizia automatica.
    struct TempGuard {
        path: PathBuf,
    }

    impl TempGuard {
        fn new(tag: &str) -> Self {
            static COUNTER: AtomicU64 = AtomicU64::new(0);
            let n = COUNTER.fetch_add(1, Ordering::Relaxed);
            let pid = std::process::id();
            let path = std::env::temp_dir().join(format!("pulse-installer-{tag}-{pid}-{n}"));
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

    /// Costruisce un albero di GD finto valido sotto `root` e restituisce il
    /// path del bundle `.app`.
    fn fake_gd_bundle(root: &Path) -> PathBuf {
        let app = root.join("Geometry Dash.app");
        let macos = app.join("Contents/MacOS");
        fs::create_dir_all(&macos).unwrap();
        fs::create_dir_all(app.join("Contents/_CodeSignature")).unwrap();

        // Eseguibile Mach-O 64-bit (magic feedfacf) + payload.
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

    /// Cattura lo stato (path→byte) di tutti i file sotto `dir`, per verificare
    /// l'invarianza byte-per-byte.
    fn snapshot(dir: &Path) -> Vec<(PathBuf, Vec<u8>)> {
        let mut out = Vec::new();
        let mut stack = vec![dir.to_path_buf()];
        while let Some(current) = stack.pop() {
            for entry in fs::read_dir(&current).unwrap() {
                let path = entry.unwrap().path();
                if path.is_dir() {
                    stack.push(path);
                } else {
                    let rel = path.strip_prefix(dir).unwrap().to_path_buf();
                    out.push((rel, fs::read(&path).unwrap()));
                }
            }
        }
        out.sort_by(|a, b| a.0.cmp(&b.0));
        out
    }

    /// Req 7.5 — un bundle GD ben formato viene riconosciuto.
    #[test]
    fn recognize_valid_gd_bundle() {
        let guard = TempGuard::new("recognize-ok");
        let app = fake_gd_bundle(guard.path());

        let gd = recognize_gd(&app).expect("il bundle deve essere riconosciuto");
        assert_eq!(gd.root, app);
        assert!(gd.executable().ends_with("Contents/MacOS/Geometry Dash"));
        assert!(gd.dylib_dest().ends_with(LOADER_DYLIB_NAME));
    }

    /// Req 7.5 — un path non riconosciuto è rifiutato senza modificare nulla.
    #[test]
    fn recognize_rejects_unrecognized_path_without_changes() {
        let guard = TempGuard::new("recognize-bad");
        // Directory che NON è un bundle GD (manca Contents/MacOS/Geometry Dash).
        let not_gd = guard.path().join("Some Other.app");
        fs::create_dir_all(not_gd.join("Contents/MacOS")).unwrap();
        fs::write(not_gd.join("Contents/MacOS/Other"), b"nope").unwrap();

        let before = snapshot(guard.path());
        let err = recognize_gd(&not_gd).unwrap_err();
        assert!(matches!(err, InstallError::UnrecognizedInstallation { .. }), "err: {err:?}");
        // Nulla è stato modificato (Req 7.5).
        assert_eq!(before, snapshot(guard.path()));
    }

    /// Req 7.5 — bundle col CFBundleIdentifier sbagliato è rifiutato.
    #[test]
    fn recognize_rejects_wrong_bundle_id() {
        let guard = TempGuard::new("recognize-wrongid");
        let app = fake_gd_bundle(guard.path());
        // Sovrascrive l'Info.plist con un id diverso.
        fs::write(
            app.join("Contents/Info.plist"),
            b"<plist><dict><key>CFBundleIdentifier</key><string>com.other.app</string></dict></plist>",
        )
        .unwrap();

        let err = recognize_gd(&app).unwrap_err();
        assert!(matches!(err, InstallError::UnrecognizedInstallation { .. }), "err: {err:?}");
    }

    /// Req 7.5 — eseguibile non-Mach-O è rifiutato.
    #[test]
    fn recognize_rejects_non_macho_executable() {
        let guard = TempGuard::new("recognize-nonmacho");
        let app = fake_gd_bundle(guard.path());
        // Sostituisce l'eseguibile con byte non-Mach-O.
        fs::write(app.join(GD_EXECUTABLE_REL), b"#!/bin/sh\necho hi\n").unwrap();

        let err = recognize_gd(&app).unwrap_err();
        match err {
            InstallError::UnrecognizedInstallation { reason, .. } => {
                assert!(reason.contains("Mach-O"), "reason: {reason}");
            }
            other => panic!("errore inatteso: {other:?}"),
        }
    }

    /// Req 7.9 — install rifiutata se la dylib è già presente, nulla modificato.
    #[test]
    fn already_installed_when_dylib_present() {
        let guard = TempGuard::new("already-dylib");
        let app = fake_gd_bundle(guard.path());
        let gd = recognize_gd(&app).unwrap();
        // Simula la dylib già installata.
        fs::write(gd.dylib_dest(), b"dylib").unwrap();

        let before = snapshot(&app);
        let err = ensure_not_already_installed(&gd).unwrap_err();
        assert!(matches!(err, InstallError::AlreadyInstalled { .. }), "err: {err:?}");
        assert_eq!(before, snapshot(&app));
    }

    /// Req 7.9 — install rifiutata se il backup manifest esiste già.
    #[test]
    fn already_installed_when_backup_manifest_present() {
        let guard = TempGuard::new("already-manifest");
        let app = fake_gd_bundle(guard.path());
        let gd = recognize_gd(&app).unwrap();
        // Persiste un manifest (restore record) preesistente.
        let manifest = BackupManifest::new(&gd);
        manifest.persist().unwrap();

        let err = ensure_not_already_installed(&gd).unwrap_err();
        assert!(matches!(err, InstallError::AlreadyInstalled { .. }), "err: {err:?}");
    }

    /// Una install "pulita" (nessuna dylib, nessun manifest) supera il guard.
    #[test]
    fn ensure_not_already_installed_passes_on_clean_bundle() {
        let guard = TempGuard::new("clean");
        let app = fake_gd_bundle(guard.path());
        let gd = recognize_gd(&app).unwrap();
        assert!(ensure_not_already_installed(&gd).is_ok());
    }

    /// Req 7.8 — uninstall senza restore record: errore, nulla modificato.
    #[test]
    fn uninstall_without_restore_record_errors_and_changes_nothing() {
        let guard = TempGuard::new("uninstall-norecord");
        let app = fake_gd_bundle(guard.path());

        let before = snapshot(&app);
        let err = require_restore_record(&app).unwrap_err();
        match err {
            InstallError::MissingRestoreRecord(path) => assert_eq!(path, app),
            other => panic!("errore inatteso: {other:?}"),
        }
        // Nulla è cambiato (Req 7.8).
        assert_eq!(before, snapshot(&app));
    }

    /// Il backup manifest fa round-trip persist → load mantenendo le voci.
    #[test]
    fn backup_manifest_round_trips() {
        let guard = TempGuard::new("manifest-roundtrip");
        let app = fake_gd_bundle(guard.path());
        let gd = recognize_gd(&app).unwrap();

        let mut manifest = BackupManifest::new(&gd);
        manifest
            .entries
            .push(BackupEntry::modified("Contents/MacOS/Geometry Dash", b"original-exe".to_vec()));
        manifest.entries.push(BackupEntry::modified(
            "Contents/_CodeSignature/CodeResources",
            b"original-sig".to_vec(),
        ));
        manifest
            .entries
            .push(BackupEntry::created_absent("Contents/MacOS/libpulse_loader.dylib"));
        manifest.persist().unwrap();

        assert!(BackupManifest::exists(&app));

        let loaded = BackupManifest::load(&app).expect("manifest caricabile");
        assert_eq!(loaded.gd_root, manifest.gd_root);
        assert_eq!(loaded.entries, manifest.entries);
    }

    /// Un restore record corrotto (hash di backup non corrispondente) è rilevato.
    #[test]
    fn corrupt_backup_is_detected() {
        let guard = TempGuard::new("manifest-corrupt");
        let app = fake_gd_bundle(guard.path());
        let gd = recognize_gd(&app).unwrap();

        let mut manifest = BackupManifest::new(&gd);
        manifest
            .entries
            .push(BackupEntry::modified("Contents/MacOS/Geometry Dash", b"original".to_vec()));
        manifest.persist().unwrap();

        // Manomette il file .orig così l'hash non corrisponde più.
        fs::write(gd.backup_dir().join("0.orig"), b"tampered").unwrap();

        let err = BackupManifest::load(&app).unwrap_err();
        assert!(matches!(err, InstallError::CorruptRestoreRecord { .. }), "err: {err:?}");
    }

    /// Crea un file artefatto (dylib sorgente finta) e restituisce il suo path.
    fn fake_artifact(dir: &Path, contents: &[u8]) -> PathBuf {
        let artifact = dir.join("libpulse_loader.dylib");
        fs::write(&artifact, contents).unwrap();
        artifact
    }

    /// Req 7.1 — install copia la dylib in `Contents/MacOS/` e applica patch +
    /// resign sull'eseguibile e sulla firma.
    #[test]
    fn install_places_dylib_and_patches_executable() {
        let guard = TempGuard::new("install-ok");
        let app = fake_gd_bundle(guard.path());
        let artifact = fake_artifact(guard.path(), b"loader dylib payload");

        let exe_before = fs::read(app.join(GD_EXECUTABLE_REL)).unwrap();
        let sig_before = fs::read(app.join(CODE_SIGNATURE_REL)).unwrap();

        let modified = install(&app, &artifact).expect("install deve riuscire");

        // La dylib è presente nella destinazione (Req 7.1).
        let dylib_dest = app.join("Contents/MacOS").join(LOADER_DYLIB_NAME);
        assert!(dylib_dest.is_file(), "la dylib deve essere copiata");
        assert_eq!(fs::read(&dylib_dest).unwrap(), b"loader dylib payload");

        // L'eseguibile è stato patchato (LC_LOAD_DYLIB) → byte diversi.
        let exe_after = fs::read(app.join(GD_EXECUTABLE_REL)).unwrap();
        assert_ne!(exe_before, exe_after, "l'eseguibile deve essere patchato");
        assert!(exe_after.starts_with(&exe_before), "la patch estende l'originale");

        // La firma è stata ri-generata (ad-hoc resign) → byte diversi.
        let sig_after = fs::read(app.join(CODE_SIGNATURE_REL)).unwrap();
        assert_ne!(sig_before, sig_after, "la firma deve essere ri-generata");

        // Req 7.7 — l'elenco copre esattamente i file modificati.
        assert_eq!(
            modified.0,
            vec![dylib_dest, app.join(GD_EXECUTABLE_REL), app.join(CODE_SIGNATURE_REL)]
        );
    }

    /// Req 7.2 — il backup manifest è persistito PRIMA di modificare i file e
    /// copre ogni file toccato col tipo corretto.
    #[test]
    fn install_persists_backup_with_correct_entries() {
        let guard = TempGuard::new("install-backup");
        let app = fake_gd_bundle(guard.path());
        let artifact = fake_artifact(guard.path(), b"loader");

        let exe_orig = fs::read(app.join(GD_EXECUTABLE_REL)).unwrap();
        let sig_orig = fs::read(app.join(CODE_SIGNATURE_REL)).unwrap();

        install(&app, &artifact).unwrap();

        // Restore record durevole presente.
        assert!(BackupManifest::exists(&app));
        let backup = BackupManifest::load(&app).unwrap();

        // Le voci coprono eseguibile (Modified), firma (Modified), dylib
        // (CreatedAbsent) — e nient'altro.
        let mut by_rel: std::collections::BTreeMap<PathBuf, BackupKind> = Default::default();
        for e in backup.entries {
            by_rel.insert(e.rel_path, e.kind);
        }
        assert_eq!(by_rel.len(), 3, "esattamente 3 voci di backup");

        match by_rel.get(Path::new(GD_EXECUTABLE_REL)).unwrap() {
            BackupKind::Modified { bytes, .. } => {
                assert_eq!(bytes, &exe_orig, "byte originali dell'eseguibile salvati");
            }
            other => panic!("eseguibile deve essere Modified, trovato {other:?}"),
        }
        match by_rel.get(Path::new(CODE_SIGNATURE_REL)).unwrap() {
            BackupKind::Modified { bytes, .. } => {
                assert_eq!(bytes, &sig_orig, "byte originali della firma salvati");
            }
            other => panic!("firma deve essere Modified, trovato {other:?}"),
        }
        assert_eq!(
            by_rel.get(Path::new("Contents/MacOS/libpulse_loader.dylib")).unwrap(),
            &BackupKind::CreatedAbsent,
            "la dylib deve essere CreatedAbsent"
        );
    }

    /// Req 7.6 — install modifica SOLO {eseguibile, firma, dylib}: nessun altro
    /// file di gioco (es. Info.plist) cambia.
    #[test]
    fn install_modifies_only_allowed_gd_files() {
        let guard = TempGuard::new("install-only-allowed");
        let app = fake_gd_bundle(guard.path());
        let artifact = fake_artifact(guard.path(), b"loader");

        let plist_before = fs::read(app.join(INFO_PLIST_REL)).unwrap();
        let before = snapshot(&app);

        install(&app, &artifact).unwrap();

        // Info.plist (asset di gioco non necessario) è invariato (Req 7.6).
        assert_eq!(fs::read(app.join(INFO_PLIST_REL)).unwrap(), plist_before);

        // Gli unici file di gioco preesistenti cambiati sono eseguibile e firma;
        // gli altri preesistenti restano byte-per-byte invariati.
        let after: std::collections::BTreeMap<_, _> = snapshot(&app).into_iter().collect();
        for (rel, bytes) in before {
            // Salta i file di backup (metadati Pulse, non file di gioco).
            if rel.starts_with(BACKUP_DIR_REL) {
                continue;
            }
            let changed = after.get(&rel).map(|b| b != &bytes).unwrap_or(true);
            let is_allowed = rel == Path::new(GD_EXECUTABLE_REL)
                || rel == Path::new(CODE_SIGNATURE_REL);
            if changed {
                assert!(is_allowed, "file di gioco non consentito modificato: {rel:?}");
            }
        }
    }

    /// Req 7.9 — una seconda install è rifiutata e non modifica ulteriormente
    /// l'albero.
    #[test]
    fn install_twice_is_refused() {
        let guard = TempGuard::new("install-twice");
        let app = fake_gd_bundle(guard.path());
        let artifact = fake_artifact(guard.path(), b"loader");

        install(&app, &artifact).unwrap();
        let after_first = snapshot(&app);

        let err = install(&app, &artifact).unwrap_err();
        assert!(matches!(err, InstallError::AlreadyInstalled { .. }), "err: {err:?}");
        // La seconda install non ha modificato nulla.
        assert_eq!(after_first, snapshot(&app));
    }

    /// Req 7.5 — install su path non riconosciuto: errore, nulla modificato.
    #[test]
    fn install_refuses_unrecognized_path() {
        let guard = TempGuard::new("install-unrecognized");
        let not_gd = guard.path().join("Some Other.app");
        fs::create_dir_all(not_gd.join("Contents/MacOS")).unwrap();
        fs::write(not_gd.join("Contents/MacOS/Other"), b"nope").unwrap();
        let artifact = fake_artifact(guard.path(), b"loader");

        let before = snapshot(guard.path());
        let err = install(&not_gd, &artifact).unwrap_err();
        assert!(matches!(err, InstallError::UnrecognizedInstallation { .. }), "err: {err:?}");
        assert_eq!(before, snapshot(guard.path()));
    }

    /// Req 7.4 (seam) — se una modifica fallisce (artefatto mancante), l'albero
    /// torna pristino e nessun restore record resta.
    #[test]
    fn install_rolls_back_on_missing_artifact() {
        let guard = TempGuard::new("install-rollback");
        let app = fake_gd_bundle(guard.path());
        let missing = guard.path().join("does-not-exist.dylib");

        let before = snapshot(&app);
        let err = install(&app, &missing).unwrap_err();
        assert!(matches!(err, InstallError::Io { .. }), "err: {err:?}");

        // Albero byte-per-byte identico al pre-installazione e nessun manifest.
        assert_eq!(before, snapshot(&app));
        assert!(!BackupManifest::exists(&app));
    }

    /// Req 7.3, 7.7 — uninstall ripristina l'albero byte-per-byte allo stato
    /// pre-installazione: i file `Modified` tornano ai byte originali, i file
    /// `CreatedAbsent` (la dylib) sono eliminati, il manifest è rimosso.
    #[test]
    fn uninstall_restores_tree_byte_exact() {
        let guard = TempGuard::new("uninstall-roundtrip");
        let app = fake_gd_bundle(guard.path());
        let artifact = fake_artifact(guard.path(), b"loader dylib payload");

        // Stato originale dell'intero albero (esclusi i metadati di backup).
        let before = snapshot(&app);

        install(&app, &artifact).unwrap();
        // Dopo l'install l'albero è cambiato e c'è un restore record.
        assert!(BackupManifest::exists(&app));
        assert_ne!(before, snapshot(&app));

        let restored = uninstall(&app).expect("uninstall deve riuscire");

        // Req 7.3 — albero byte-per-byte identico al pre-installazione.
        assert_eq!(before, snapshot(&app), "l'albero deve tornare identico");
        // La dylib (CreatedAbsent) è stata eliminata.
        assert!(!app.join("Contents/MacOS").join(LOADER_DYLIB_NAME).exists());
        // Il manifest (restore record) è stato rimosso.
        assert!(!BackupManifest::exists(&app));
        assert!(!app.join(BACKUP_DIR_REL).exists());

        // Req 7.7 — l'elenco copre tutti i file toccati: eseguibile, firma, dylib.
        let restored_set: std::collections::BTreeSet<PathBuf> =
            restored.0.iter().cloned().collect();
        assert!(restored_set.contains(&app.join(GD_EXECUTABLE_REL)));
        assert!(restored_set.contains(&app.join(CODE_SIGNATURE_REL)));
        assert!(restored_set.contains(&app.join("Contents/MacOS").join(LOADER_DYLIB_NAME)));
    }

    /// Req 7.8 — uninstall senza restore record: errore "restore record
    /// mancante", nulla modificato.
    #[test]
    fn uninstall_fn_without_restore_record_errors_and_changes_nothing() {
        let guard = TempGuard::new("uninstall-no-record");
        let app = fake_gd_bundle(guard.path());

        let before = snapshot(&app);
        let err = uninstall(&app).unwrap_err();
        match err {
            InstallError::MissingRestoreRecord(path) => assert_eq!(path, app),
            other => panic!("errore inatteso: {other:?}"),
        }
        // Nulla è cambiato (Req 7.8).
        assert_eq!(before, snapshot(&app));
    }

    /// Req 7.4 — il rollback su install fallita riporta l'albero allo stato
    /// pre-installazione e l'errore propagato identifica la causa.
    #[test]
    fn install_failure_rollback_is_complete_and_reports_cause() {
        let guard = TempGuard::new("install-rollback-cause");
        let app = fake_gd_bundle(guard.path());
        // Artefatto mancante → la copia della dylib fallisce a metà install.
        let missing = guard.path().join("missing-artifact.dylib");

        let before = snapshot(&app);
        let err = install(&app, &missing).unwrap_err();

        // L'errore identifica la causa (I/O sul path dell'artefatto mancante).
        match &err {
            InstallError::Io { path, .. } => assert_eq!(path, &missing),
            other => panic!("errore inatteso: {other:?}"),
        }
        // La causa è riportata nel messaggio (Req 7.4).
        assert!(!err.to_string().is_empty());

        // Rollback completo: albero byte-per-byte identico al pre-installazione
        // e nessun restore record residuo.
        assert_eq!(before, snapshot(&app));
        assert!(!BackupManifest::exists(&app));
    }

    /// Fase E — l'asset entitlements embedded dichiara l'entitlement richiesto
    /// (`com.apple.security.cs.disable-library-validation`) per l'ad-hoc resign
    /// reale (Req 2.2).
    #[test]
    fn entitlements_asset_declares_disable_library_validation() {
        assert!(
            PULSE_ENTITLEMENTS_PLIST
                .contains("com.apple.security.cs.disable-library-validation"),
            "gli entitlements devono dichiarare disable-library-validation"
        );
        assert!(
            PULSE_ENTITLEMENTS_PLIST.contains("<plist"),
            "gli entitlements devono essere un plist valido"
        );
    }

    /// Fase E — su piattaforme non-macOS `install_native` rifiuta senza
    /// modificare nulla (richiede `codesign` e il binario reale di GD).
    #[cfg(not(target_os = "macos"))]
    #[test]
    fn install_native_unsupported_off_macos() {
        let guard = TempGuard::new("install-native-unsupported");
        let app = fake_gd_bundle(guard.path());
        let artifact = fake_artifact(guard.path(), b"loader");

        let before = snapshot(&app);
        let err = install_native(&app, &artifact).unwrap_err();
        assert!(
            matches!(err, InstallError::UnrecognizedInstallation { .. }),
            "err: {err:?}"
        );
        assert_eq!(before, snapshot(&app));
    }

    /// Fase E — il path nativo macOS rifiuta un eseguibile non-Mach-O valido
    /// (l'albero finto usa un body fittizio) eseguendo il rollback completo,
    /// senza lasciare residui (Req 7.4). Questo verifica il cablaggio del seam
    /// nativo senza richiedere il binario reale di GD.
    #[cfg(target_os = "macos")]
    #[test]
    fn install_native_rolls_back_on_invalid_macho() {
        let guard = TempGuard::new("install-native-invalid");
        let app = fake_gd_bundle(guard.path());
        let artifact = fake_artifact(guard.path(), b"loader dylib payload");

        let before = snapshot(&app);
        // L'eseguibile finto non è un Mach-O 64-bit valido con padding: la vera
        // patch LC_LOAD_DYLIB fallisce e l'install esegue il rollback.
        let err = install_native(&app, &artifact).unwrap_err();
        assert!(
            matches!(err, InstallError::MachoPatch { .. }),
            "atteso errore di patch Mach-O, trovato: {err:?}"
        );

        // Rollback completo: albero invariato, nessun restore record residuo.
        assert_eq!(before, snapshot(&app), "l'albero deve tornare identico");
        assert!(!BackupManifest::exists(&app));
    }
}