//! Implementazione di `pulse build` — compila la mod e produce un Pulse Package
//! `.pulse` (Req 14.3, 14.4, 14.5).
//!
//! Flusso del comando:
//!   1. Legge e VALIDA il `pulse.toml` del progetto contro lo schema. Se il
//!      manifest è assente, non analizzabile o non conforme, la build viene
//!      interrotta SENZA produrre alcun pacchetto e viene segnalato l'elenco
//!      completo dei campi non conformi (Req 14.4).
//!   2. Compila i sorgenti della mod attraverso un passo di compilazione
//!      astratto ([`Compiler`]). Se la compilazione fallisce, la build viene
//!      interrotta SENZA produrre alcun pacchetto e vengono segnalate le cause
//!      (Req 14.5).
//!   3. In caso di successo, produce un pacchetto `.pulse`: un container ZIP con
//!      `pulse.toml`, `code/` (artefatti compilati), `resources/` e
//!      `MANIFEST.sha256` (hash di integrità calcolato sulle voci del package).
//!
//! Il passo di compilazione è astratto dietro il trait [`Compiler`] così da
//! poter iniettare implementazioni nei test (compilazione fittizia, fallimenti
//! deterministici) senza richiedere una toolchain C++ reale. Il compilatore di
//! default ([`PlaceholderCompiler`]) impacchetta i sorgenti del progetto in un
//! artefatto segnaposto: è il comportamento adeguato per le mod prodotte da
//! `pulse new` finché l'integrazione della toolchain nativa non è disponibile.

use std::fs;
use std::io::{Cursor, Write};
use std::path::{Path, PathBuf};

use sha2::{Digest, Sha256};
use zip::write::SimpleFileOptions;
use zip::ZipWriter;

use crate::manifest::{FieldViolation, Manifest, ModType};
use crate::native_compiler::NativeCompiler;

/// Nome del file manifest all'interno del progetto e del package.
pub const MANIFEST_FILE: &str = "pulse.toml";

/// Nome del file di integrità all'interno del package (Req 28.6).
pub const INTEGRITY_FILE: &str = "MANIFEST.sha256";

/// Prefisso delle voci di codice compilato nel package.
pub const CODE_DIR: &str = "code";

/// Prefisso delle voci di risorse nel package.
pub const RESOURCES_DIR: &str = "resources";

// ---------------------------------------------------------------------------
// Artefatti di compilazione e astrazione del compilatore.
// ---------------------------------------------------------------------------

/// Un artefatto prodotto dalla compilazione: nome del file (relativo a `code/`)
/// e contenuto in byte.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CompiledArtifact {
    /// Nome del file dentro `code/` (es. `windows-x64.dll`, `script.lua`).
    pub file_name: String,
    /// Contenuto binario dell'artefatto.
    pub bytes: Vec<u8>,
}

/// Esito di una compilazione riuscita: gli artefatti da impacchettare in `code/`.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct CompileOutput {
    pub artifacts: Vec<CompiledArtifact>,
}

/// Astrazione del passo di compilazione (Req 14.5). Implementata da
/// [`PlaceholderCompiler`] (default) e iniettabile nei test.
pub trait Compiler {
    /// Compila i sorgenti della mod nella directory del progetto.
    ///
    /// In caso di successo restituisce gli artefatti compilati; in caso di
    /// fallimento restituisce l'elenco delle cause (Req 14.5), che la build
    /// propaga senza produrre alcun pacchetto.
    fn compile(&self, project_dir: &Path, manifest: &Manifest) -> Result<CompileOutput, Vec<String>>;
}

/// Compilatore di default: non invoca una toolchain nativa reale, ma raccoglie
/// i sorgenti del progetto (`src/`) in un artefatto segnaposto. È sufficiente
/// per impacchettare le mod generate da `pulse new` e per i test, e fornisce un
/// punto di estensione naturale per cablare in futuro un compilatore reale.
#[derive(Debug, Default, Clone, Copy)]
pub struct PlaceholderCompiler;

impl PlaceholderCompiler {
    pub fn new() -> Self {
        PlaceholderCompiler
    }

    /// Raccoglie ricorsivamente i file sotto `dir`, ordinati per percorso
    /// relativo, restituendo coppie `(percorso_relativo, byte)`.
    fn collect_files(dir: &Path) -> Result<Vec<(String, Vec<u8>)>, Vec<String>> {
        let mut out: Vec<(String, Vec<u8>)> = Vec::new();
        if !dir.is_dir() {
            return Ok(out);
        }
        let mut stack = vec![dir.to_path_buf()];
        while let Some(current) = stack.pop() {
            let entries = fs::read_dir(&current).map_err(|e| {
                vec![format!("impossibile leggere la directory '{}': {e}", current.display())]
            })?;
            for entry in entries {
                let entry = entry.map_err(|e| {
                    vec![format!("voce di directory non leggibile in '{}': {e}", current.display())]
                })?;
                let path = entry.path();
                if path.is_dir() {
                    stack.push(path);
                } else if path.is_file() {
                    let bytes = fs::read(&path).map_err(|e| {
                        vec![format!("impossibile leggere il sorgente '{}': {e}", path.display())]
                    })?;
                    let rel = path
                        .strip_prefix(dir)
                        .map(|p| p.to_path_buf())
                        .unwrap_or_else(|_| path.clone());
                    out.push((rel.to_string_lossy().replace('\\', "/"), bytes));
                }
            }
        }
        out.sort_by(|a, b| a.0.cmp(&b.0));
        Ok(out)
    }
}

impl Compiler for PlaceholderCompiler {
    fn compile(&self, project_dir: &Path, manifest: &Manifest) -> Result<CompileOutput, Vec<String>> {
        let src_dir = project_dir.join("src");
        let sources = Self::collect_files(&src_dir)?;

        if sources.is_empty() {
            return Err(vec![format!(
                "nessun file sorgente trovato in '{}': impossibile compilare la mod",
                src_dir.display()
            )]);
        }

        // Produce un artefatto segnaposto deterministico a partire dai sorgenti.
        // L'estensione riflette il tipo di mod dichiarato nel manifest.
        let mut payload = Vec::new();
        let _ = writeln!(payload, "# Pulse placeholder artifact for {}", manifest.mod_info.id);
        let _ = writeln!(payload, "# version {}", manifest.mod_info.version);
        for (rel, bytes) in &sources {
            let _ = writeln!(payload, "## source: {rel} ({} bytes)", bytes.len());
            payload.extend_from_slice(bytes);
            if !bytes.ends_with(b"\n") {
                payload.push(b'\n');
            }
        }

        let file_name = match manifest.mod_info.mod_type {
            ModType::Native => "module.pulsebin".to_string(),
            ModType::Script => "module.script".to_string(),
        };

        Ok(CompileOutput {
            artifacts: vec![CompiledArtifact {
                file_name,
                bytes: payload,
            }],
        })
    }
}

// ---------------------------------------------------------------------------
// Errori e esito della build.
// ---------------------------------------------------------------------------

/// Errori del comando `pulse build`. In ogni caso di errore NON viene prodotto
/// alcun Pulse Package (Req 14.4, 14.5).
#[derive(Debug, thiserror::Error)]
pub enum BuildError {
    /// La directory del progetto o il `pulse.toml` non esistono.
    #[error("manifest non trovato: '{0}' non esiste (atteso un pulse.toml di progetto)")]
    ManifestNotFound(PathBuf),

    /// Errore di I/O durante la lettura del manifest o dei sorgenti.
    #[error("errore di I/O su '{path}': {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },

    /// Il `pulse.toml` non è analizzabile (manifest non valido, Req 14.4).
    #[error("pulse.toml non analizzabile: {0}")]
    ManifestParse(String),

    /// Il manifest è analizzabile ma non conforme allo schema: build interrotta,
    /// nessun pacchetto, elenco completo dei campi non conformi (Req 14.4).
    #[error("manifest non valido: {} campo/i non conforme/i", .0.len())]
    InvalidManifest(Vec<FieldViolation>),

    /// La compilazione dei sorgenti è fallita: build interrotta, nessun
    /// pacchetto, elenco delle cause (Req 14.5).
    #[error("compilazione fallita: {} causa/e", .0.len())]
    CompilationFailed(Vec<String>),

    /// Errore durante la scrittura del container `.pulse`.
    #[error("creazione del Pulse Package fallita: {0}")]
    Package(String),
}

/// Esito di una build riuscita.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BuildOutcome {
    /// Percorso del pacchetto `.pulse` prodotto.
    pub package_path: PathBuf,
    /// Identificatore della mod impacchettata.
    pub mod_id: String,
    /// Nomi delle voci contenute nel package (ordinati).
    pub entries: Vec<String>,
}

// ---------------------------------------------------------------------------
// Build.
// ---------------------------------------------------------------------------

/// Esegue la build con il compilatore nativo di default ([`NativeCompiler`]).
///
/// `project_dir` è la radice del progetto (contiene `pulse.toml`). Il pacchetto
/// `.pulse` viene scritto in `project_dir`. Il compilatore viene scelto in base
/// al `mod_type` del manifest: le mod native invocano la toolchain C++ reale,
/// le mod script vengono impacchettate senza toolchain (Req 6.1, 6.6). Per i
/// test si usa [`build_with`] con un [`Compiler`] iniettato.
pub fn build(project_dir: &Path) -> Result<BuildOutcome, BuildError> {
    build_with(project_dir, &NativeCompiler::for_host(), project_dir)
}

/// Esegue la build con un [`Compiler`] iniettato e una directory di output
/// esplicita. Usata dai test per iniettare compilazioni fittizie/fallimenti.
pub fn build_with(
    project_dir: &Path,
    compiler: &dyn Compiler,
    output_dir: &Path,
) -> Result<BuildOutcome, BuildError> {
    // --- 1. Lettura e validazione del manifest (Req 14.4). ----------------
    let manifest_path = project_dir.join(MANIFEST_FILE);
    if !manifest_path.is_file() {
        return Err(BuildError::ManifestNotFound(manifest_path));
    }
    let manifest_text = fs::read_to_string(&manifest_path).map_err(|source| BuildError::Io {
        path: manifest_path.clone(),
        source,
    })?;

    let manifest = match Manifest::parse(&manifest_text) {
        Ok(m) => m,
        Err(e) => return Err(BuildError::ManifestParse(e.to_string())),
    };

    let validation = manifest.validate();
    if !validation.ok() {
        // Manifest non valido: abortisci senza produrre alcun pacchetto (Req 14.4).
        return Err(BuildError::InvalidManifest(validation.violations));
    }

    // --- 2. Compilazione dei sorgenti (Req 14.5). -------------------------
    let compiled = compiler
        .compile(project_dir, &manifest)
        .map_err(BuildError::CompilationFailed)?;

    // --- 3. Raccolta delle voci del package (in memoria). -----------------
    // Ordine: pulse.toml, code/*, resources/*; MANIFEST.sha256 calcolato su queste.
    let mut entries: Vec<(String, Vec<u8>)> = Vec::new();
    entries.push((MANIFEST_FILE.to_string(), manifest_text.into_bytes()));

    for artifact in &compiled.artifacts {
        entries.push((
            format!("{CODE_DIR}/{}", artifact.file_name),
            artifact.bytes.clone(),
        ));
    }

    let resources_dir = project_dir.join(RESOURCES_DIR);
    let resources = collect_resources(&resources_dir)?;
    for (rel, bytes) in resources {
        entries.push((format!("{RESOURCES_DIR}/{rel}"), bytes));
    }

    // --- 4. Calcolo del file di integrità MANIFEST.sha256. ----------------
    let integrity = compute_integrity(&entries);
    entries.push((INTEGRITY_FILE.to_string(), integrity.into_bytes()));

    // --- 5. Scrittura del container .pulse. -------------------------------
    let package_bytes = write_zip(&entries).map_err(|e| BuildError::Package(e.to_string()))?;

    let file_stem = package_file_stem(&manifest.mod_info.id);
    let package_path = output_dir.join(format!("{file_stem}.pulse"));

    if let Some(parent) = package_path.parent() {
        fs::create_dir_all(parent).map_err(|source| BuildError::Io {
            path: parent.to_path_buf(),
            source,
        })?;
    }
    fs::write(&package_path, &package_bytes).map_err(|source| BuildError::Io {
        path: package_path.clone(),
        source,
    })?;

    let entry_names: Vec<String> = entries.into_iter().map(|(name, _)| name).collect();

    Ok(BuildOutcome {
        package_path,
        mod_id: manifest.mod_info.id,
        entries: entry_names,
    })
}

/// Deriva un nome di file di pacchetto sicuro dall'id della mod.
fn package_file_stem(mod_id: &str) -> String {
    let mut stem = String::with_capacity(mod_id.len());
    for ch in mod_id.chars() {
        if ch.is_ascii_alphanumeric() || ch == '.' || ch == '-' || ch == '_' {
            stem.push(ch);
        } else {
            stem.push('_');
        }
    }
    if stem.is_empty() {
        "mod".to_string()
    } else {
        stem
    }
}

/// Raccoglie ricorsivamente i file di risorse, ordinati per percorso relativo.
fn collect_resources(dir: &Path) -> Result<Vec<(String, Vec<u8>)>, BuildError> {
    let mut out: Vec<(String, Vec<u8>)> = Vec::new();
    if !dir.is_dir() {
        return Ok(out);
    }
    let mut stack = vec![dir.to_path_buf()];
    while let Some(current) = stack.pop() {
        let read = fs::read_dir(&current).map_err(|source| BuildError::Io {
            path: current.clone(),
            source,
        })?;
        for entry in read {
            let entry = entry.map_err(|source| BuildError::Io {
                path: current.clone(),
                source,
            })?;
            let path = entry.path();
            if path.is_dir() {
                stack.push(path);
            } else if path.is_file() {
                let bytes = fs::read(&path).map_err(|source| BuildError::Io {
                    path: path.clone(),
                    source,
                })?;
                let rel = path
                    .strip_prefix(dir)
                    .map(|p| p.to_path_buf())
                    .unwrap_or_else(|_| path.clone());
                out.push((rel.to_string_lossy().replace('\\', "/"), bytes));
            }
        }
    }
    out.sort_by(|a, b| a.0.cmp(&b.0));
    Ok(out)
}

/// Calcola il contenuto di `MANIFEST.sha256`: una riga `"<sha256hex>  <voce>"`
/// per ciascuna voce del package (esclusa l'integrità stessa), ordinate per
/// nome di voce per determinismo (Req 28.6).
fn compute_integrity(entries: &[(String, Vec<u8>)]) -> String {
    let mut lines: Vec<(String, String)> = entries
        .iter()
        .map(|(name, bytes)| {
            let mut hasher = Sha256::new();
            hasher.update(bytes);
            let digest = hasher.finalize();
            (name.clone(), hex_encode(&digest))
        })
        .collect();
    lines.sort_by(|a, b| a.0.cmp(&b.0));

    let mut out = String::new();
    for (name, digest) in lines {
        out.push_str(&digest);
        out.push_str("  ");
        out.push_str(&name);
        out.push('\n');
    }
    out
}

/// Codifica un buffer di byte in esadecimale minuscolo.
fn hex_encode(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut s = String::with_capacity(bytes.len() * 2);
    for &b in bytes {
        s.push(HEX[(b >> 4) as usize] as char);
        s.push(HEX[(b & 0x0f) as usize] as char);
    }
    s
}

/// Scrive le voci in un container ZIP (il formato `.pulse`) restituendo i byte.
fn write_zip(entries: &[(String, Vec<u8>)]) -> Result<Vec<u8>, zip::result::ZipError> {
    let mut cursor = Cursor::new(Vec::new());
    {
        let mut zip = ZipWriter::new(&mut cursor);
        // Opzioni deterministiche: voci STORED (non compresse) + timestamp fisso.
        // STORED consente al loader C++ di leggere il `.pulse` senza un
        // implementazione di inflate, mantenendo la build C++ priva di
        // dipendenze (compressed size == uncompressed size). I test della CLI
        // leggono le voci via `zip::ZipArchive`, che gestisce STORED, quindi
        // restano validi.
        let options = SimpleFileOptions::default()
            .compression_method(zip::CompressionMethod::Stored)
            .last_modified_time(zip::DateTime::default());
        for (name, bytes) in entries {
            zip.start_file(name, options)?;
            zip.write_all(bytes)?;
        }
        zip.finish()?;
    }
    Ok(cursor.into_inner())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    use crate::scaffold;

    /// Directory temporanea isolata per i test, con pulizia automatica.
    struct TempGuard {
        path: PathBuf,
    }

    impl TempGuard {
        fn new(tag: &str) -> Self {
            static COUNTER: AtomicU64 = AtomicU64::new(0);
            let n = COUNTER.fetch_add(1, Ordering::Relaxed);
            let pid = std::process::id();
            let path = std::env::temp_dir().join(format!("pulse-build-{tag}-{pid}-{n}"));
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

    /// Compilatore che fallisce sempre con le cause indicate (Req 14.5).
    struct FailingCompiler {
        causes: Vec<String>,
    }

    impl Compiler for FailingCompiler {
        fn compile(&self, _dir: &Path, _m: &Manifest) -> Result<CompileOutput, Vec<String>> {
            Err(self.causes.clone())
        }
    }

    /// Compilatore fittizio che restituisce un artefatto fisso, senza I/O.
    struct StubCompiler;

    impl Compiler for StubCompiler {
        fn compile(&self, _dir: &Path, _m: &Manifest) -> Result<CompileOutput, Vec<String>> {
            Ok(CompileOutput {
                artifacts: vec![CompiledArtifact {
                    file_name: "windows-x64.dll".to_string(),
                    bytes: b"\x00fake-binary\x00".to_vec(),
                }],
            })
        }
    }

    /// Legge i nomi delle voci da un container `.pulse` su disco.
    fn read_package_entries(path: &Path) -> Vec<String> {
        let file = fs::File::open(path).expect("apri package");
        let mut archive = zip::ZipArchive::new(file).expect("zip valido");
        let mut names = Vec::new();
        for i in 0..archive.len() {
            let entry = archive.by_index(i).unwrap();
            names.push(entry.name().to_string());
        }
        names.sort();
        names
    }

    /// Req 14.3 — una build con manifest valido produce un `.pulse` contenente
    /// pulse.toml, code/*, resources/* e MANIFEST.sha256.
    #[test]
    fn build_valid_project_produces_pulse_with_expected_entries() {
        let guard = TempGuard::new("valid");
        let dir = guard.path();
        // Scaffold genera un progetto valido con sorgenti e pulse.toml.
        scaffold::scaffold_new(dir, Some("com.example.demo")).unwrap();
        // Aggiungi una risorsa per verificarne l'inclusione.
        let res_dir = dir.join(RESOURCES_DIR);
        fs::create_dir_all(&res_dir).unwrap();
        fs::write(res_dir.join("icon.txt"), b"icon").unwrap();

        let outcome = build_with(dir, &StubCompiler, dir).expect("build riuscita");

        assert!(outcome.package_path.is_file(), "il .pulse deve esistere");
        assert_eq!(outcome.mod_id, "com.example.demo");

        let entries = read_package_entries(&outcome.package_path);
        assert!(entries.iter().any(|e| e == "pulse.toml"), "manca pulse.toml: {entries:?}");
        assert!(entries.iter().any(|e| e == "code/windows-x64.dll"), "manca code/: {entries:?}");
        assert!(entries.iter().any(|e| e == "resources/icon.txt"), "manca resources/: {entries:?}");
        assert!(entries.iter().any(|e| e == "MANIFEST.sha256"), "manca MANIFEST.sha256: {entries:?}");
    }

    /// Req 6.6 — `build()` con il compilatore di default impacchetta una mod
    /// script senza invocare alcuna toolchain nativa.
    #[test]
    fn build_with_default_compiler_packages_script_sources() {
        let guard = TempGuard::new("default-script");
        let dir = guard.path();

        // Manifest script valido + un sorgente script.
        let manifest = r#"
            schema_version = 1

            [mod]
            id = "com.example.script"
            version = "0.1.0"
            name = "Script Mod"
            type = "script"

            [[entry_points]]
            kind = "init"
            symbol = "init"
        "#;
        fs::write(dir.join(MANIFEST_FILE), manifest).unwrap();
        fs::create_dir_all(dir.join("src")).unwrap();
        fs::write(dir.join("src/mod.lua"), b"-- lua mod\n").unwrap();

        let outcome = build(dir).expect("build riuscita");
        let entries = read_package_entries(&outcome.package_path);
        // Il compilatore di default (NativeCompiler) impacchetta lo script senza
        // toolchain sotto code/ (Req 6.6).
        assert!(
            entries.iter().any(|e| e == "code/mod.lua"),
            "atteso sorgente script impacchettato: {entries:?}"
        );
    }

    /// Req 14.4 — manifest non valido: build interrotta, nessun pacchetto,
    /// elenco completo dei campi non conformi.
    #[test]
    fn build_invalid_manifest_aborts_with_field_list_and_no_package() {
        let guard = TempGuard::new("invalid");
        let dir = guard.path();
        // Manifest analizzabile ma NON conforme: id vuoto, nessun entry point,
        // permesso non riconosciuto.
        let bad = r#"
            schema_version = 1

            [mod]
            id = ""
            version = "1.0.0"
            name = "Bad"
            type = "native"

            [permissions]
            required = ["telepathy"]
        "#;
        fs::write(dir.join(MANIFEST_FILE), bad).unwrap();
        // Sorgente presente per isolare la causa al manifest.
        fs::create_dir_all(dir.join("src")).unwrap();
        fs::write(dir.join("src/mod.cpp"), b"int x;").unwrap();

        let err = build_with(dir, &StubCompiler, dir).unwrap_err();
        match err {
            BuildError::InvalidManifest(violations) => {
                let fields: Vec<String> = violations.iter().map(|v| v.field.clone()).collect();
                assert!(fields.iter().any(|f| f == "mod.id"), "campi: {fields:?}");
                assert!(fields.iter().any(|f| f == "entry_points"), "campi: {fields:?}");
                assert!(
                    fields.iter().any(|f| f == "permissions.required[0]"),
                    "campi: {fields:?}"
                );
            }
            other => panic!("errore inatteso: {other:?}"),
        }

        // Nessun pacchetto prodotto (Req 14.4).
        let pulse_files: Vec<_> = fs::read_dir(dir)
            .unwrap()
            .filter_map(|e| e.ok())
            .filter(|e| e.path().extension().map(|x| x == "pulse").unwrap_or(false))
            .collect();
        assert!(pulse_files.is_empty(), "nessun .pulse atteso, trovati: {pulse_files:?}");
    }

    /// Req 14.4 — un pulse.toml non analizzabile interrompe la build senza pacchetto.
    #[test]
    fn build_unparseable_manifest_aborts_with_no_package() {
        let guard = TempGuard::new("unparseable");
        let dir = guard.path();
        fs::write(dir.join(MANIFEST_FILE), b"this is = = not toml [[[").unwrap();

        let err = build_with(dir, &StubCompiler, dir).unwrap_err();
        assert!(matches!(err, BuildError::ManifestParse(_)), "errore: {err:?}");
        assert!(!dir.join("com.example.demo.pulse").exists());
    }

    /// Req 14.5 — fallimento di compilazione iniettato: build interrotta,
    /// nessun pacchetto, cause segnalate.
    #[test]
    fn build_compile_failure_aborts_with_causes_and_no_package() {
        let guard = TempGuard::new("compilefail");
        let dir = guard.path();
        scaffold::scaffold_new(dir, Some("com.example.cf")).unwrap();

        let compiler = FailingCompiler {
            causes: vec![
                "src/mod.cpp:10: errore di sintassi".to_string(),
                "linker: simbolo non risolto pulse_mod_init".to_string(),
            ],
        };
        let err = build_with(dir, &compiler, dir).unwrap_err();
        match err {
            BuildError::CompilationFailed(causes) => {
                assert_eq!(causes.len(), 2);
                assert!(causes[0].contains("errore di sintassi"));
                assert!(causes[1].contains("simbolo non risolto"));
            }
            other => panic!("errore inatteso: {other:?}"),
        }

        // Nessun pacchetto prodotto (Req 14.5).
        let pulse_files: Vec<_> = fs::read_dir(dir)
            .unwrap()
            .filter_map(|e| e.ok())
            .filter(|e| e.path().extension().map(|x| x == "pulse").unwrap_or(false))
            .collect();
        assert!(pulse_files.is_empty(), "nessun .pulse atteso, trovati: {pulse_files:?}");
    }

    /// Req 14.3 — il package include un MANIFEST.sha256 con una riga per voce.
    #[test]
    fn integrity_file_lists_one_digest_per_entry() {
        let guard = TempGuard::new("integrity");
        let dir = guard.path();
        scaffold::scaffold_new(dir, Some("com.example.int")).unwrap();

        let outcome = build_with(dir, &StubCompiler, dir).unwrap();

        let file = fs::File::open(&outcome.package_path).unwrap();
        let mut archive = zip::ZipArchive::new(file).unwrap();
        let mut integrity = String::new();
        use std::io::Read;
        archive
            .by_name(INTEGRITY_FILE)
            .unwrap()
            .read_to_string(&mut integrity)
            .unwrap();

        // Una riga per pulse.toml e per code/windows-x64.dll (almeno).
        assert!(integrity.lines().any(|l| l.ends_with("pulse.toml")));
        assert!(integrity.lines().any(|l| l.ends_with("code/windows-x64.dll")));
        // Ogni riga inizia con un digest esadecimale di 64 caratteri.
        for line in integrity.lines() {
            let digest = line.split("  ").next().unwrap();
            assert_eq!(digest.len(), 64, "digest non valido: {line}");
            assert!(digest.chars().all(|c| c.is_ascii_hexdigit()));
        }
    }
}
