//! Implementazione di `pulse new` — scaffolding di una nuova mod (Req 14.1, 14.2).
//!
//! Genera una struttura di progetto contenente un `pulse.toml` valido (prodotto
//! dal serializer del modulo [`crate::manifest`]) e i file sorgente iniziali
//! (Req 14.1). Se la directory di destinazione esiste già ed è NON vuota,
//! l'operazione viene interrotta senza modificare alcun file esistente e viene
//! restituito un errore di conflitto di directory (Req 14.2).

use std::fs;
use std::path::{Path, PathBuf};

use crate::manifest::{
    EntryPoint, Manifest, ModInfo, ModType, Permissions, SemVer,
};

/// Errori dello scaffolding di `pulse new`.
#[derive(Debug, thiserror::Error)]
pub enum ScaffoldError {
    /// La directory di destinazione esiste già ed è non vuota (Req 14.2).
    #[error(
        "conflitto di directory: '{0}' esiste già e non è vuota; \
         interrompo senza modificare i file esistenti"
    )]
    DirectoryConflict(PathBuf),

    /// Il percorso di destinazione esiste ma non è una directory.
    #[error("il percorso di destinazione '{0}' esiste ma non è una directory")]
    NotADirectory(PathBuf),

    /// Errore di I/O durante la generazione della struttura.
    #[error("errore di I/O durante lo scaffolding di '{path}': {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },

    /// Errore durante la serializzazione del manifest.
    #[error("impossibile serializzare il manifest: {0}")]
    Manifest(#[from] crate::manifest::ManifestError),
}

/// Esito dello scaffolding: percorso del progetto, id scelto ed elenco dei file
/// creati (relativi alla root del progetto).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ScaffoldOutcome {
    pub project_dir: PathBuf,
    pub mod_id: String,
    pub created_files: Vec<String>,
}

/// Verifica se una directory contiene almeno una voce (file o sottocartella).
fn dir_is_non_empty(path: &Path) -> Result<bool, ScaffoldError> {
    let mut entries = fs::read_dir(path).map_err(|source| ScaffoldError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    Ok(entries.next().is_some())
}

/// Deriva un identificatore di mod di default dal nome della directory di
/// destinazione, quando l'utente non fornisce `--id`.
///
/// I caratteri non alfanumerici sono normalizzati in `-`; il risultato è
/// prefissato con `com.pulse.` per ottenere un id plausibile e non vuoto.
fn default_id_from_path(path: &Path) -> String {
    let raw = path
        .file_name()
        .and_then(|s| s.to_str())
        .unwrap_or("mymod");

    let mut slug = String::with_capacity(raw.len());
    let mut prev_dash = false;
    for ch in raw.chars() {
        if ch.is_ascii_alphanumeric() {
            slug.push(ch.to_ascii_lowercase());
            prev_dash = false;
        } else if !prev_dash {
            slug.push('-');
            prev_dash = true;
        }
    }
    let slug = slug.trim_matches('-').to_string();
    let slug = if slug.is_empty() {
        "mymod".to_string()
    } else {
        slug
    };
    format!("com.pulse.{slug}")
}

/// Costruisce il [`Manifest`] iniziale per una nuova mod con l'id indicato.
fn initial_manifest(mod_id: &str) -> Manifest {
    Manifest {
        schema_version: 1,
        mod_info: ModInfo {
            id: mod_id.to_string(),
            version: SemVer::new(0, 1, 0),
            name: "New Pulse Mod".to_string(),
            mod_type: ModType::Native,
        },
        entry_points: vec![EntryPoint {
            kind: "init".to_string(),
            symbol: "pulse_mod_init".to_string(),
        }],
        dependencies: Vec::new(),
        permissions: Permissions::default(),
        settings: Vec::new(),
    }
}

/// Contenuto del sorgente iniziale `src/lib.rs`-equivalente per una mod nativa.
fn initial_source(mod_id: &str) -> String {
    format!(
        "// Sorgente iniziale della mod Pulse `{mod_id}` (scaffold `pulse new`).\n\
         //\n\
         // Il punto di ingresso dichiarato nel manifest (`pulse_mod_init`) è\n\
         // invocato dal Pulse_Loader quando la mod viene abilitata (Req 4.6).\n\
         \n\
         #include <pulse/pulse.hpp>\n\
         \n\
         // Entry point della mod: registra hook, eventi e UI.\n\
         extern \"C\" void pulse_mod_init() {{\n\
         \x20\x20\x20\x20// TODO: registra qui gli hook dichiarativi (PULSE_HOOK) della tua mod.\n\
         }}\n"
    )
}

/// Contenuto del README iniziale del progetto.
fn initial_readme(mod_id: &str) -> String {
    format!(
        "# {mod_id}\n\
         \n\
         Mod Pulse generata con `pulse new`.\n\
         \n\
         ## Struttura\n\
         \n\
         - `pulse.toml` — Manifest della mod (identità, versione, entry point, permessi).\n\
         - `src/` — sorgenti della mod.\n\
         - `code/` — risorse e asset della mod.\n\
         \n\
         ## Comandi\n\
         \n\
         - `pulse build` — compila la mod e produce un pacchetto `.pulse`.\n\
         - `pulse publish` — pubblica il pacchetto sul Marketplace.\n"
    )
}

/// Esegue lo scaffolding di una nuova mod nella directory `dest`.
///
/// - Se `dest` non esiste o è una directory vuota, genera la struttura completa
///   con un `pulse.toml` valido e i sorgenti iniziali (Req 14.1).
/// - Se `dest` esiste ed è una directory NON vuota, restituisce
///   [`ScaffoldError::DirectoryConflict`] senza modificare alcun file (Req 14.2).
///
/// `id` opzionale: se assente, viene derivato dal nome della directory.
pub fn scaffold_new(dest: &Path, id: Option<&str>) -> Result<ScaffoldOutcome, ScaffoldError> {
    // --- Controllo di conflitto PRIMA di qualsiasi scrittura (Req 14.2). ---
    if dest.exists() {
        if !dest.is_dir() {
            return Err(ScaffoldError::NotADirectory(dest.to_path_buf()));
        }
        if dir_is_non_empty(dest)? {
            // Directory esistente e non vuota: abortisci senza toccare nulla.
            return Err(ScaffoldError::DirectoryConflict(dest.to_path_buf()));
        }
    }

    let mod_id = match id {
        Some(s) if !s.trim().is_empty() => s.trim().to_string(),
        _ => default_id_from_path(dest),
    };

    // Crea la directory di destinazione (e gli antenati) se non esiste già.
    fs::create_dir_all(dest).map_err(|source| ScaffoldError::Io {
        path: dest.to_path_buf(),
        source,
    })?;

    // Sottocartelle del progetto.
    let src_dir = dest.join("src");
    let code_dir = dest.join("code");
    fs::create_dir_all(&src_dir).map_err(|source| ScaffoldError::Io {
        path: src_dir.clone(),
        source,
    })?;
    fs::create_dir_all(&code_dir).map_err(|source| ScaffoldError::Io {
        path: code_dir.clone(),
        source,
    })?;

    // Manifest valido prodotto dal serializer del modulo manifest (Req 14.1).
    let manifest = initial_manifest(&mod_id);
    let manifest_text = manifest.serialize()?;

    let files: [(PathBuf, String); 4] = [
        (dest.join("pulse.toml"), manifest_text),
        (src_dir.join("mod.cpp"), initial_source(&mod_id)),
        (
            code_dir.join(".gitkeep"),
            "# Segnaposto per risorse e asset della mod.\n".to_string(),
        ),
        (dest.join("README.md"), initial_readme(&mod_id)),
    ];

    let mut created_files = Vec::with_capacity(files.len());
    for (path, contents) in files {
        fs::write(&path, contents).map_err(|source| ScaffoldError::Io {
            path: path.clone(),
            source,
        })?;
        // Memorizza il percorso relativo alla root del progetto.
        let rel = path
            .strip_prefix(dest)
            .map(|p| p.to_path_buf())
            .unwrap_or_else(|_| path.clone());
        created_files.push(rel.to_string_lossy().replace('\\', "/"));
    }
    created_files.sort();

    Ok(ScaffoldOutcome {
        project_dir: dest.to_path_buf(),
        mod_id,
        created_files,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    /// Crea una directory temporanea univoca per l'isolamento dei test e ne
    /// restituisce il percorso. La pulizia è gestita da [`TempGuard`].
    struct TempGuard {
        path: PathBuf,
    }

    impl TempGuard {
        fn new(tag: &str) -> Self {
            static COUNTER: AtomicU64 = AtomicU64::new(0);
            let n = COUNTER.fetch_add(1, Ordering::Relaxed);
            let pid = std::process::id();
            let path = std::env::temp_dir().join(format!("pulse-scaffold-{tag}-{pid}-{n}"));
            // Assicura uno stato pulito.
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

    /// Req 14.1 — lo scaffolding in una directory nuova produce un manifest
    /// valido (parsabile) e i file attesi.
    #[test]
    fn scaffold_into_new_dir_creates_valid_manifest_and_files() {
        let guard = TempGuard::new("new");
        let dest = guard.path();

        let outcome = scaffold_new(dest, Some("com.example.demo")).expect("scaffold");

        assert_eq!(outcome.mod_id, "com.example.demo");
        // File attesi.
        assert!(dest.join("pulse.toml").is_file());
        assert!(dest.join("src/mod.cpp").is_file());
        assert!(dest.join("code/.gitkeep").is_file());
        assert!(dest.join("README.md").is_file());

        // Il pulse.toml generato è un Manifest valido e parsabile (Req 14.1).
        let text = fs::read_to_string(dest.join("pulse.toml")).unwrap();
        let manifest = Manifest::parse(&text).expect("manifest parsabile");
        assert_eq!(manifest.mod_info.id, "com.example.demo");
        assert_eq!(manifest.mod_info.version, SemVer::new(0, 1, 0));
        // Almeno un entry point (Req 16.1).
        assert!(!manifest.entry_points.is_empty());
    }

    /// Req 14.1 — senza `--id` l'id è derivato dal nome della directory.
    #[test]
    fn scaffold_derives_id_from_dir_name_when_absent() {
        let guard = TempGuard::new("My Cool Mod");
        let dest = guard.path();

        let outcome = scaffold_new(dest, None).expect("scaffold");
        // Il nome della cartella include il tag con spazi -> slug normalizzato.
        assert!(
            outcome.mod_id.starts_with("com.pulse."),
            "id derivato inatteso: {}",
            outcome.mod_id
        );
        let text = fs::read_to_string(dest.join("pulse.toml")).unwrap();
        Manifest::parse(&text).expect("manifest parsabile");
    }

    /// Req 14.1 — scaffolding in una directory esistente ma VUOTA ha successo.
    #[test]
    fn scaffold_into_existing_empty_dir_succeeds() {
        let guard = TempGuard::new("empty");
        let dest = guard.path();
        fs::create_dir_all(dest).unwrap();
        assert!(!dir_is_non_empty(dest).unwrap());

        let outcome = scaffold_new(dest, Some("com.example.empty")).expect("scaffold");
        assert!(dest.join("pulse.toml").is_file());
        assert_eq!(outcome.mod_id, "com.example.empty");
    }

    /// Req 14.2 — scaffolding in una directory NON vuota fallisce con un errore
    /// di conflitto e NON modifica i file esistenti.
    #[test]
    fn scaffold_into_non_empty_dir_errors_and_leaves_files_untouched() {
        let guard = TempGuard::new("nonempty");
        let dest = guard.path();
        fs::create_dir_all(dest).unwrap();

        // File preesistente con contenuto noto.
        let existing = dest.join("existing.txt");
        let original_contents = "contenuto originale da NON toccare";
        fs::write(&existing, original_contents).unwrap();

        let err = scaffold_new(dest, Some("com.example.conflict")).unwrap_err();
        assert!(
            matches!(err, ScaffoldError::DirectoryConflict(_)),
            "errore inatteso: {err:?}"
        );

        // Il file esistente è rimasto invariato (Req 14.2).
        let after = fs::read_to_string(&existing).unwrap();
        assert_eq!(after, original_contents);

        // Nessun artefatto dello scaffolding è stato creato.
        assert!(!dest.join("pulse.toml").exists());
        assert!(!dest.join("src").exists());
        assert!(!dest.join("code").exists());
        assert!(!dest.join("README.md").exists());
    }

    /// Il manifest generato sopravvive a un round-trip parse/serialize (Req 16.5).
    #[test]
    fn generated_manifest_round_trips() {
        let guard = TempGuard::new("roundtrip");
        let dest = guard.path();
        scaffold_new(dest, Some("com.example.rt")).unwrap();

        let text = fs::read_to_string(dest.join("pulse.toml")).unwrap();
        let parsed = Manifest::parse(&text).unwrap();
        let reserialized = parsed.serialize().unwrap();
        let parsed_again = Manifest::parse(&reserialized).unwrap();
        assert_eq!(parsed, parsed_again);
    }
}
