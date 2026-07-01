//! Controlli built-in di `pulse doctor` — i tre controlli del `Dev_Environment`
//! (Req 1.1, 1.3, 1.4).
//!
//! Questo modulo implementa i tre controlli concreti restituiti da
//! [`crate::doctor::default_registry`]:
//!
//! - [`GdVersionCheck`] — scopre la `GD_Installation` (via
//!   [`installer::recognize_gd`]) e ne legge la versione dall'`Info.plist`;
//!   assente o non rilevabile entro il timeout ⇒ `Problema` con azione
//!   correttiva (Req 1.4).
//! - [`CppToolchainCheck`] — **riusa**
//!   [`native_compiler::discover_toolchain`] con
//!   [`TargetPlatform::host`](native_compiler::TargetPlatform::host): toolchain
//!   assente ⇒ `Problema`; presente ma con versione non ideale ⇒ `Warning`.
//! - [`ToolchainConfigCheck`] — verifica la coerenza della configurazione del
//!   toolchain (variabile `CXX`, presenza di file toolchain sotto
//!   `cmake/toolchains/`).
//!
//! **Invariante di `pulse doctor`**: ogni voce con esito `Warning` o `Problema`
//! porta **almeno un'azione correttiva** concreta e **non vuota** (Req 1.3). I
//! costruttori [`CheckReportItem::warning`](crate::doctor::report::CheckReportItem::warning)
//! e [`CheckReportItem::problema`](crate::doctor::report::CheckReportItem::problema)
//! richiedono l'azione correttiva per costruzione; questo modulo passa sempre
//! stringhe non vuote.
//!
//! **Nessun binario ridistribuito** (Req 9.5): la scoperta della
//! `GD_Installation` opera solo sulla copia legalmente posseduta e individuabile
//! localmente; l'assenza è un `Problema` con azione correttiva, mai una
//! richiesta di binari ridistribuiti.

use std::path::{Path, PathBuf};
use std::process::Command;

use crate::doctor::report::CheckReportItem;
use crate::doctor::{run_with_timeout, Check, CHECK_TIMEOUT};
use crate::installer;
use crate::native_compiler::{self, TargetPlatform, ToolchainKind};

// ---------------------------------------------------------------------------
// GdVersionCheck — versione della GD_Installation (Req 1.1, 1.3, 1.4).
// ---------------------------------------------------------------------------

/// Controllo (a): versione di Geometry Dash installata.
///
/// Scopre la `GD_Installation` con [`installer::recognize_gd`] su un insieme di
/// posizioni note e ne legge la versione dall'`Info.plist`. Se nessuna copia è
/// individuabile (o non è rilevabile entro il timeout di 30 s) l'esito è
/// `Problema` con un'azione correttiva concreta (Req 1.4). Non richiede né
/// ridistribuisce alcun binario di Geometry Dash (Req 9.5).
#[derive(Debug, Default, Clone, Copy)]
pub struct GdVersionCheck;

impl GdVersionCheck {
    /// Identificatore univoco del controllo.
    pub const ID: &'static str = "gd-version";
    /// Descrizione del componente verificato.
    pub const DESCRIPTION: &'static str = "Versione di Geometry Dash installata";

    /// Azione correttiva usata quando GD non è individuabile o la versione non
    /// è leggibile.
    fn corrective_action() -> String {
        "Installa Geometry Dash (versione macOS/Steam) e assicurati che il bundle \
         'Geometry Dash.app' sia nella posizione standard, oppure indicane il \
         percorso ai comandi che lo richiedono con --gd. Pulse non richiede né \
         ridistribuisce alcun binario di Geometry Dash."
            .to_string()
    }
}

impl Check for GdVersionCheck {
    fn id(&self) -> &str {
        Self::ID
    }

    fn describe(&self) -> &str {
        Self::DESCRIPTION
    }

    fn run(&self) -> CheckReportItem {
        run_with_timeout(
            Self::ID,
            Self::DESCRIPTION,
            Self::corrective_action(),
            CHECK_TIMEOUT,
            || match discover_gd_installation() {
                Some(install) => match read_gd_version(&install.info_plist()) {
                    Some(version) => CheckReportItem::ok(
                        Self::ID,
                        format!("{} (rilevata: {version})", Self::DESCRIPTION),
                    ),
                    // GD riconosciuta ma versione non estraibile: utilizzabile,
                    // ma segnaliamo l'anomalia non bloccante con un'azione.
                    None => CheckReportItem::warning(
                        Self::ID,
                        Self::DESCRIPTION,
                        "Geometry Dash è stata individuata ma non è stato possibile \
                         leggerne la versione dall'Info.plist; verifica che il bundle \
                         non sia danneggiato o reinstallalo.",
                    ),
                },
                // GD non individuabile ⇒ Problema con azione correttiva (Req 1.4).
                None => CheckReportItem::problema(
                    Self::ID,
                    Self::DESCRIPTION,
                    Self::corrective_action(),
                ),
            },
        )
    }
}

/// Tenta la scoperta locale di una `GD_Installation` in posizioni note (macOS,
/// installazione Steam). Restituisce la prima copia **riconosciuta** da
/// [`installer::recognize_gd`], oppure `None` — mai un binario ridistribuito
/// (Req 9.5).
fn discover_gd_installation() -> Option<installer::GdInstallation> {
    candidate_install_paths()
        .into_iter()
        .find_map(|candidate| installer::recognize_gd(&candidate).ok())
}

/// Percorsi candidati per la scoperta locale della `GD_Installation` (macOS).
fn candidate_install_paths() -> Vec<PathBuf> {
    let mut candidates = Vec::new();
    if let Some(home) = std::env::var_os("HOME") {
        let home = PathBuf::from(home);
        candidates.push(home.join(
            "Library/Application Support/Steam/steamapps/common/Geometry Dash/Geometry Dash.app",
        ));
    }
    candidates
}

/// Legge la versione (`CFBundleShortVersionString`, con fallback su
/// `CFBundleVersion`) da un `Info.plist` XML. Best-effort e tollerante: se il
/// file non è leggibile o la chiave è assente restituisce `None`.
fn read_gd_version(info_plist: &Path) -> Option<String> {
    let text = std::fs::read_to_string(info_plist).ok()?;
    extract_plist_string(&text, "CFBundleShortVersionString")
        .or_else(|| extract_plist_string(&text, "CFBundleVersion"))
}

/// Estrae il valore `<string>…</string>` che segue `<key>{key}</key>` in un
/// plist XML. Tollerante agli spazi bianchi tra i tag. Restituisce `None` se la
/// chiave o il valore stringa non sono presenti.
fn extract_plist_string(plist_xml: &str, key: &str) -> Option<String> {
    let key_tag = format!("<key>{key}</key>");
    let after_key = &plist_xml[plist_xml.find(&key_tag)? + key_tag.len()..];
    let open = after_key.find("<string>")? + "<string>".len();
    let rest = &after_key[open..];
    let close = rest.find("</string>")?;
    let value = rest[..close].trim();
    if value.is_empty() {
        None
    } else {
        Some(value.to_string())
    }
}

// ---------------------------------------------------------------------------
// CppToolchainCheck — toolchain/compilatori C++ (Req 1.1, 1.3).
// ---------------------------------------------------------------------------

/// Versione major minima "ideale" per una toolchain Clang (supporto C++20).
const MIN_CLANG_MAJOR: u32 = 12;
/// Versione major minima "ideale" per una toolchain GCC (`g++`).
const MIN_GCC_MAJOR: u32 = 11;

/// Controllo (b): presenza e versione del toolchain/compilatori C++.
///
/// **Riusa** [`native_compiler::discover_toolchain`] con la piattaforma host
/// ([`TargetPlatform::host`](native_compiler::TargetPlatform::host)): toolchain
/// assente ⇒ `Problema`; presente ma con versione non ideale ⇒ `Warning`. In
/// entrambi i casi la voce porta un'azione correttiva concreta e non vuota
/// (Req 1.3).
#[derive(Debug, Default, Clone, Copy)]
pub struct CppToolchainCheck;

impl CppToolchainCheck {
    /// Identificatore univoco del controllo.
    pub const ID: &'static str = "cpp-toolchain";
    /// Descrizione del componente verificato.
    pub const DESCRIPTION: &'static str = "Toolchain e compilatori C++";

    /// Azione correttiva usata quando nessuna toolchain è individuabile.
    fn missing_action() -> String {
        "Installa un compilatore C++ (clang++, c++/g++ oppure cl.exe su Windows) \
         e assicurati che sia nel PATH, oppure imposta la variabile d'ambiente \
         CXX al percorso del compilatore."
            .to_string()
    }
}

impl Check for CppToolchainCheck {
    fn id(&self) -> &str {
        Self::ID
    }

    fn describe(&self) -> &str {
        Self::DESCRIPTION
    }

    fn run(&self) -> CheckReportItem {
        run_with_timeout(
            Self::ID,
            Self::DESCRIPTION,
            Self::missing_action(),
            CHECK_TIMEOUT,
            || match native_compiler::discover_toolchain(TargetPlatform::host()) {
                // Toolchain assente ⇒ Problema con azione correttiva.
                None => CheckReportItem::problema(
                    Self::ID,
                    Self::DESCRIPTION,
                    Self::missing_action(),
                ),
                Some(toolchain) => {
                    let program = toolchain.program.display().to_string();
                    let version_output = probe_version(&toolchain.program);
                    match version_output.as_deref().and_then(parse_compiler_major) {
                        Some(major) if is_ideal_version(toolchain.kind, major) => {
                            CheckReportItem::ok(
                                Self::ID,
                                format!(
                                    "{} (rilevato: {program}, versione {major})",
                                    Self::DESCRIPTION
                                ),
                            )
                        }
                        // Versione presente ma non ideale ⇒ Warning con azione.
                        Some(major) => CheckReportItem::warning(
                            Self::ID,
                            format!(
                                "{} (rilevato: {program}, versione {major})",
                                Self::DESCRIPTION
                            ),
                            format!(
                                "Il compilatore rilevato ha una versione major {major} \
                                 inferiore a quella consigliata per il supporto C++20; \
                                 aggiorna la toolchain a una versione più recente."
                            ),
                        ),
                        // Toolchain individuata ma versione non verificabile:
                        // anomalia non bloccante ⇒ Warning con azione.
                        None => CheckReportItem::warning(
                            Self::ID,
                            format!("{} (rilevato: {program})", Self::DESCRIPTION),
                            "Il compilatore è stato individuato ma non è stato possibile \
                             verificarne la versione; assicurati che sia eseguibile e \
                             che supporti C++20."
                                .to_string(),
                        ),
                    }
                }
            },
        )
    }
}

/// Esegue `<program> --version` e restituisce l'output combinato, oppure `None`
/// se il comando non è eseguibile.
fn probe_version(program: &Path) -> Option<String> {
    let output = Command::new(program).arg("--version").output().ok()?;
    let mut text = String::from_utf8_lossy(&output.stdout).into_owned();
    text.push_str(&String::from_utf8_lossy(&output.stderr));
    if text.trim().is_empty() {
        None
    } else {
        Some(text)
    }
}

/// Estrae la versione *major* dalla prima occorrenza di un pattern
/// `major.minor…` nell'output `--version` di un compilatore. Funzione pura e
/// testabile. Restituisce `None` se nessun numero di versione è riconoscibile.
fn parse_compiler_major(version_output: &str) -> Option<u32> {
    let bytes = version_output.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i].is_ascii_digit() {
            // Raccogli la sequenza di cifre.
            let start = i;
            while i < bytes.len() && bytes[i].is_ascii_digit() {
                i += 1;
            }
            // È un numero di versione solo se seguito da un punto e da una cifra
            // (es. "14.0.0"), così da ignorare anni o numeri isolati.
            if i + 1 < bytes.len() && bytes[i] == b'.' && bytes[i + 1].is_ascii_digit() {
                return version_output[start..i].parse().ok();
            }
        } else {
            i += 1;
        }
    }
    None
}

/// `true` se la versione major rilevata è considerata "ideale" per il tipo di
/// toolchain (supporto C++20 adeguato).
fn is_ideal_version(kind: ToolchainKind, major: u32) -> bool {
    // `kind` non distingue g++ da clang++ (entrambi `Clang`); usiamo perciò la
    // soglia più permissiva tra Clang e GCC per evitare falsi positivi sui
    // Warning di versione.
    let threshold = match kind {
        ToolchainKind::Clang => MIN_CLANG_MAJOR.min(MIN_GCC_MAJOR),
        ToolchainKind::Msvc => MIN_CLANG_MAJOR,
    };
    major >= threshold
}

// ---------------------------------------------------------------------------
// ToolchainConfigCheck — coerenza della configurazione (Req 1.1, 1.3).
// ---------------------------------------------------------------------------

/// Controllo (c): validità della configurazione del toolchain.
///
/// Verifica la coerenza della configurazione: la variabile d'ambiente `CXX`
/// (se impostata) deve risolvere a un compilatore esistente, e devono essere
/// presenti file toolchain sotto `cmake/toolchains/`. Le incoerenze producono
/// `Warning` con un'azione correttiva concreta e non vuota (Req 1.3).
#[derive(Debug, Default, Clone, Copy)]
pub struct ToolchainConfigCheck;

impl ToolchainConfigCheck {
    /// Identificatore univoco del controllo.
    pub const ID: &'static str = "toolchain-config";
    /// Descrizione del componente verificato.
    pub const DESCRIPTION: &'static str = "Configurazione del toolchain";

    /// Azione correttiva generica usata dal fallback di timeout.
    fn timeout_action() -> String {
        "Verifica manualmente la variabile d'ambiente CXX e la presenza dei file \
         toolchain sotto cmake/toolchains/."
            .to_string()
    }
}

impl Check for ToolchainConfigCheck {
    fn id(&self) -> &str {
        Self::ID
    }

    fn describe(&self) -> &str {
        Self::DESCRIPTION
    }

    fn run(&self) -> CheckReportItem {
        run_with_timeout(
            Self::ID,
            Self::DESCRIPTION,
            Self::timeout_action(),
            CHECK_TIMEOUT,
            || {
                let mut problems: Vec<String> = Vec::new();

                // (1) CXX, se impostata, deve risolvere a un eseguibile.
                if let Ok(cxx) = std::env::var("CXX") {
                    let cxx = cxx.trim().to_string();
                    if !cxx.is_empty() && resolve_program(&cxx).is_none() {
                        problems.push(format!(
                            "la variabile d'ambiente CXX punta a un compilatore non \
                             risolvibile ('{cxx}'): correggila indicando un compilatore \
                             valido nel PATH o un percorso assoluto esistente, oppure \
                             rimuovila."
                        ));
                    }
                }

                // (2) File toolchain sotto cmake/toolchains/.
                if !toolchains_dir_present() {
                    problems.push(
                        "nessun file toolchain trovato sotto 'cmake/toolchains/': \
                         esegui la CLI dalla radice del repository Pulse oppure \
                         ripristina la directory 'cmake/toolchains/' con i file di \
                         toolchain del progetto."
                            .to_string(),
                    );
                }

                if problems.is_empty() {
                    CheckReportItem::ok(Self::ID, Self::DESCRIPTION)
                } else {
                    // Almeno un'azione correttiva concreta e non vuota (Req 1.3).
                    CheckReportItem::warning(Self::ID, Self::DESCRIPTION, problems.join(" "))
                }
            },
        )
    }
}

/// Risolve un valore in stile `CXX`: se contiene un separatore di percorso lo
/// tratta come path esplicito (deve esistere come file), altrimenti lo cerca
/// nel `PATH`.
fn resolve_program(value: &str) -> Option<PathBuf> {
    let p = Path::new(value);
    if value.contains('/') || value.contains('\\') {
        return if p.is_file() {
            Some(p.to_path_buf())
        } else {
            None
        };
    }
    find_in_path(value)
}

/// Cerca un eseguibile nelle directory del `PATH`. Su Windows considera anche
/// le estensioni eseguibili comuni se il nome non ne ha già una.
fn find_in_path(name: &str) -> Option<PathBuf> {
    let path_var = std::env::var_os("PATH")?;
    let has_ext = Path::new(name).extension().is_some();
    let win_exts = ["exe", "bat", "cmd"];
    for dir in std::env::split_paths(&path_var) {
        let direct = dir.join(name);
        if direct.is_file() {
            return Some(direct);
        }
        if cfg!(windows) && !has_ext {
            for ext in win_exts {
                let candidate = dir.join(format!("{name}.{ext}"));
                if candidate.is_file() {
                    return Some(candidate);
                }
            }
        }
    }
    None
}

/// `true` se esiste una directory `cmake/toolchains/` con almeno un file di
/// toolchain. Cerca sia relativamente alla directory di lavoro corrente sia
/// relativamente alla radice del repository Pulse (derivata dal percorso del
/// sorgente della CLI, come `native_compiler::sdk_include_dir`).
fn toolchains_dir_present() -> bool {
    let mut candidates: Vec<PathBuf> = vec![PathBuf::from("cmake/toolchains")];
    candidates.push(PathBuf::from(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../cmake/toolchains"
    )));
    candidates.iter().any(|dir| dir_has_file(dir))
}

/// `true` se `dir` è una directory che contiene almeno un file regolare.
fn dir_has_file(dir: &Path) -> bool {
    match std::fs::read_dir(dir) {
        Ok(entries) => entries
            .flatten()
            .any(|entry| entry.path().is_file()),
        Err(_) => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::doctor::report::CheckOutcome;

    #[test]
    fn estrae_versione_short_dal_plist() {
        let plist = "<?xml version=\"1.0\"?>\n<plist><dict>\n\
             <key>CFBundleIdentifier</key><string>com.robtop.geometrydashmac</string>\n\
             <key>CFBundleShortVersionString</key><string>2.207</string>\n\
             </dict></plist>\n";
        assert_eq!(
            extract_plist_string(plist, "CFBundleShortVersionString").as_deref(),
            Some("2.207")
        );
    }

    #[test]
    fn versione_assente_nel_plist_e_none() {
        let plist = "<plist><dict><key>CFBundleIdentifier</key><string>x</string></dict></plist>";
        assert_eq!(extract_plist_string(plist, "CFBundleShortVersionString"), None);
    }

    #[test]
    fn versione_vuota_e_none() {
        let plist = "<key>CFBundleVersion</key><string>   </string>";
        assert_eq!(extract_plist_string(plist, "CFBundleVersion"), None);
    }

    #[test]
    fn parse_major_da_output_clang() {
        let out = "Apple clang version 14.0.3 (clang-1403.0.22.14.1)\n";
        assert_eq!(parse_compiler_major(out), Some(14));
    }

    #[test]
    fn parse_major_da_output_gcc() {
        let out = "g++ (Ubuntu 11.4.0) 11.4.0\n";
        assert_eq!(parse_compiler_major(out), Some(11));
    }

    #[test]
    fn parse_major_ignora_output_senza_versione() {
        assert_eq!(parse_compiler_major("nessuna versione qui"), None);
    }

    #[test]
    fn versione_ideale_rispetta_le_soglie() {
        assert!(is_ideal_version(ToolchainKind::Clang, 14));
        assert!(!is_ideal_version(ToolchainKind::Clang, 9));
        assert!(is_ideal_version(ToolchainKind::Clang, MIN_GCC_MAJOR));
    }

    /// I tre controlli built-in espongono id univoci e descrizioni non vuote.
    #[test]
    fn controlli_hanno_id_univoci_e_descrizioni_non_vuote() {
        let checks: Vec<Box<dyn Check>> = vec![
            Box::new(GdVersionCheck),
            Box::new(CppToolchainCheck),
            Box::new(ToolchainConfigCheck),
        ];
        let ids: Vec<&str> = checks.iter().map(|c| c.id()).collect();
        assert_eq!(ids, vec!["gd-version", "cpp-toolchain", "toolchain-config"]);
        // Univoci.
        let mut sorted = ids.clone();
        sorted.sort_unstable();
        sorted.dedup();
        assert_eq!(sorted.len(), ids.len());
        // Descrizioni non vuote.
        assert!(checks.iter().all(|c| !c.describe().is_empty()));
    }

    /// Ogni voce prodotta con esito Warning/Problema porta un'azione correttiva
    /// concreta e non vuota (Req 1.3).
    #[test]
    fn esiti_anomali_portano_azione_correttiva_non_vuota() {
        let checks: Vec<Box<dyn Check>> = vec![
            Box::new(GdVersionCheck),
            Box::new(CppToolchainCheck),
            Box::new(ToolchainConfigCheck),
        ];
        for check in &checks {
            let item = check.run();
            if matches!(item.outcome, CheckOutcome::Warning | CheckOutcome::Problema) {
                let action = item
                    .corrective_action
                    .as_deref()
                    .expect("Warning/Problema deve avere un'azione correttiva");
                assert!(
                    !action.trim().is_empty(),
                    "l'azione correttiva non deve essere vuota per '{}'",
                    item.id
                );
            }
        }
    }
}
