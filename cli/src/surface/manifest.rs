//! Modello on-disk del `Surface_Manifest` e suo caricamento fail-closed.
//!
//! Il `Surface_Manifest` è l'**unico** artefatto nuovo che il Maintainer scrive
//! a mano: un file TOML **diffabile** e revisionabile sotto
//! `mod-index/surface/surface.toml`. Descrive *quali* simboli del
//! `Binding_Catalog` esporre, con quale **priorità**, in quale **classe**, se
//! sono **Hook_Point**, più eventuali **override di mappatura di tipo**.
//!
//! **Confine preciso:** il manifest **non contiene offset né firme**. Gli
//! offset e le `Signature` restano di esclusiva proprietà del `Binding_Catalog`
//! (la superficie li *legge*, non li *possiede*). Qui modelliamo solo la
//! *selezione curata*; la fusione `manifest × catalog` (la `Surface_IR`) è il
//! compito del `Surface_Compiler` nel task successivo.
//!
//! Coerente con la disciplina **fail-closed** della pipeline, [`load_manifest`]
//! non produce alcuna selezione su anomalia: rifiuta file illeggibili
//! ([`SurfaceError::Io`]), TOML malformato ([`SurfaceError::Parse`]),
//! `schema_version` non supportata ([`SurfaceError::UnsupportedSchemaVersion`])
//! e ogni simbolo che compaia in **due** voci `[[class.method]]` dell'intero
//! manifest ([`SurfaceError::DuplicateSymbol`], Req 2.4).
//!
//! _Requisiti: 1.1, 1.3, 2.4._

use std::collections::HashSet;
use std::path::Path;

use serde::{Deserialize, Serialize};

use super::{SurfaceError, SymbolId};

/// Versione di schema del manifest on-disk supportata da questo parser.
///
/// Coincide con il campo `schema_version` di `surface.toml`: un valore diverso
/// viene rifiutato (fail-closed) anziché interpretato a caso, in analogia con
/// `CATALOG_SCHEMA_VERSION` della pipeline.
pub const SURFACE_SCHEMA_VERSION: u32 = 1;

// ---------------------------------------------------------------------------
// Modello di dominio del Surface_Manifest.
// ---------------------------------------------------------------------------

/// Il `Surface_Manifest`: la selezione curata di classi e gli eventuali
/// override di mappatura di tipo (Req 1, 2.3).
///
/// **Non contiene offset né firme**: solo *quali* simboli esporre, in quale
/// classe, con quale priorità, e come mappare i tipi GD → C++.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SurfaceManifest {
    /// Le classi esposte (raggruppamento di `Class_Binding`).
    pub classes: Vec<ManifestClass>,
    /// Le regole di mappatura di tipo GD → C++ (Req 3.3); può essere vuota.
    pub type_map: Vec<TypeMapRule>,
}

/// Una classe esposta dalla superficie: un raggruppamento di metodi sotto un
/// nome e un tipo C++ generato.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ManifestClass {
    /// Nome della classe (`Class_Binding`), es. `"MenuLayer"`.
    pub name: String,
    /// Tipo C++ generato per la classe; opaco se non altrimenti definito.
    pub cpp_type: String,
    /// I metodi selezionati per questa classe.
    pub methods: Vec<ManifestMethod>,
}

/// Un metodo selezionato dalla superficie: il join 1:1 con una `Catalog_Entry`
/// per `symbol`, con priorità ordinabile ed eventuale flag di Hook_Point.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ManifestMethod {
    /// Identificatore canonico del simbolo (join 1:1 col catalogo, Req 1.2, 2.1).
    pub symbol: SymbolId,
    /// Valore ordinabile di priorità della `Surface_Selection` (Req 1.3).
    pub priority: i64,
    /// `true` se è un `Hook_Point` dichiarabile con `PULSE_GD_HOOK` (Req 1.1).
    pub hook: bool,
}

/// Una regola di mappatura di tipo da un tipo GD a un tipo C++ (Req 3.3).
///
/// Deriva `Serialize`/`Deserialize` (aggiunte in 5.1) perché è **trasportata
/// come dato** nella `Surface_IR` (`SurfaceIr::type_overrides`) e quindi nel
/// `surface.ir.json`. La presenza del campo `cpp` **non** rende la IR
/// dipendente dal C++: è un dato di configurazione applicato **solo** dal
/// `Cpp_Generator`; la IR non lo interpreta (Req 10.2).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TypeMapRule {
    /// Tipo lato Geometry Dash, es. `"cocos2d::CCObject*"`.
    pub gd: String,
    /// Tipo C++ corrispondente (opaco, forward-declared), es. `"pulse::gd::CCObject*"`.
    pub cpp: String,
}

// ---------------------------------------------------------------------------
// Strutture grezze di deserializzazione (mirror 1:1 del TOML on-disk).
// ---------------------------------------------------------------------------

/// Mirror della radice di `surface.toml`.
#[derive(Debug, Deserialize)]
struct RawManifest {
    schema_version: u32,
    /// `[[class]]` ripetuto; assente ⇒ nessuna classe selezionata.
    #[serde(default, rename = "class")]
    class: Vec<RawClass>,
    /// `[[type_map]]` ripetuto; assente ⇒ nessuna regola di mappatura.
    #[serde(default, rename = "type_map")]
    type_map: Vec<RawTypeMap>,
}

/// Mirror di un blocco `[[class]]` con i suoi `[[class.method]]` annidati.
#[derive(Debug, Deserialize)]
struct RawClass {
    name: String,
    cpp_type: String,
    /// `[[class.method]]` ripetuto; assente ⇒ classe senza metodi.
    #[serde(default, rename = "method")]
    method: Vec<RawMethod>,
}

/// Mirror di un blocco `[[class.method]]`.
#[derive(Debug, Deserialize)]
struct RawMethod {
    symbol: String,
    priority: i64,
    #[serde(default)]
    hook: bool,
}

/// Mirror di un blocco `[[type_map]]`.
#[derive(Debug, Deserialize)]
struct RawTypeMap {
    gd: String,
    cpp: String,
}

// ---------------------------------------------------------------------------
// Parsing e caricamento.
// ---------------------------------------------------------------------------

/// Analizza il contenuto in memoria di un `Surface_Manifest` (host-testabile).
///
/// `path` è usato solo per arricchire gli errori di parsing/lettura; non viene
/// letto. Fallisce in chiusura su TOML malformato
/// ([`SurfaceError::Parse`]), `schema_version` non supportata
/// ([`SurfaceError::UnsupportedSchemaVersion`]) e simbolo presente in due voci
/// `[[class.method]]` dell'intero manifest ([`SurfaceError::DuplicateSymbol`],
/// Req 2.4).
pub fn parse_manifest(content: &str, path: &Path) -> Result<SurfaceManifest, SurfaceError> {
    let raw: RawManifest = toml::from_str(content).map_err(|source| SurfaceError::Parse {
        path: path.to_path_buf(),
        source,
    })?;

    if raw.schema_version != SURFACE_SCHEMA_VERSION {
        return Err(SurfaceError::UnsupportedSchemaVersion {
            found: raw.schema_version,
            supported: SURFACE_SCHEMA_VERSION,
        });
    }

    // Dedup fail-closed: un simbolo non può comparire in due `[[class.method]]`
    // dell'intero manifest, né nella stessa classe né in classi diverse (Req 2.4).
    let mut seen: HashSet<SymbolId> = HashSet::new();
    let mut classes: Vec<ManifestClass> = Vec::with_capacity(raw.class.len());

    for raw_class in raw.class {
        let mut methods: Vec<ManifestMethod> = Vec::with_capacity(raw_class.method.len());
        for raw_method in raw_class.method {
            let symbol = SymbolId::new(raw_method.symbol);
            if !seen.insert(symbol.clone()) {
                return Err(SurfaceError::DuplicateSymbol { symbol });
            }
            methods.push(ManifestMethod {
                symbol,
                priority: raw_method.priority,
                hook: raw_method.hook,
            });
        }
        classes.push(ManifestClass {
            name: raw_class.name,
            cpp_type: raw_class.cpp_type,
            methods,
        });
    }

    let type_map = raw
        .type_map
        .into_iter()
        .map(|rule| TypeMapRule {
            gd: rule.gd,
            cpp: rule.cpp,
        })
        .collect();

    Ok(SurfaceManifest { classes, type_map })
}

/// Carica e valida il `Surface_Manifest` da disco
/// (es. `mod-index/surface/surface.toml`).
///
/// Fallisce in chiusura su file illeggibile ([`SurfaceError::Io`]), TOML
/// malformato ([`SurfaceError::Parse`]), `schema_version` non supportata
/// ([`SurfaceError::UnsupportedSchemaVersion`]) e simbolo duplicato
/// ([`SurfaceError::DuplicateSymbol`], Req 2.4). Su qualsiasi anomalia non viene
/// prodotta alcuna selezione.
pub fn load_manifest(path: &Path) -> Result<SurfaceManifest, SurfaceError> {
    let content = std::fs::read_to_string(path).map_err(|source| SurfaceError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    parse_manifest(&content, path)
}

#[cfg(test)]
mod tests {
    use super::*;

    const VALID_MANIFEST: &str = r#"
schema_version = 1

[[class]]
name = "MenuLayer"
cpp_type = "MenuLayer"

  [[class.method]]
  symbol   = "MenuLayer::init"
  priority = 100
  hook     = true

[[type_map]]
gd  = "cocos2d::CCObject*"
cpp = "pulse::gd::CCObject*"
"#;

    fn fake_path() -> std::path::PathBuf {
        std::path::PathBuf::from("surface.toml")
    }

    #[test]
    fn parse_valid_manifest_one_class_one_method_one_type_map() {
        let manifest = parse_manifest(VALID_MANIFEST, &fake_path()).unwrap();

        assert_eq!(manifest.classes.len(), 1);
        let class = &manifest.classes[0];
        assert_eq!(class.name, "MenuLayer");
        assert_eq!(class.cpp_type, "MenuLayer");

        assert_eq!(class.methods.len(), 1);
        let method = &class.methods[0];
        assert_eq!(method.symbol, SymbolId::new("MenuLayer::init"));
        assert_eq!(method.priority, 100);
        assert!(method.hook);

        assert_eq!(manifest.type_map.len(), 1);
        let rule = &manifest.type_map[0];
        assert_eq!(rule.gd, "cocos2d::CCObject*");
        assert_eq!(rule.cpp, "pulse::gd::CCObject*");
    }

    #[test]
    fn hook_defaults_to_false_when_omitted() {
        let content = r#"
schema_version = 1

[[class]]
name = "PlayLayer"
cpp_type = "PlayLayer"

  [[class.method]]
  symbol   = "PlayLayer::update"
  priority = 50
"#;
        let manifest = parse_manifest(content, &fake_path()).unwrap();
        let method = &manifest.classes[0].methods[0];
        assert!(!method.hook);
        assert_eq!(method.priority, 50);
    }

    #[test]
    fn parse_rejects_duplicate_symbol_across_classes() {
        // Req 2.4: lo stesso simbolo in due `[[class.method]]` di classi diverse
        // è rifiutato in chiusura segnalando il simbolo duplicato.
        let content = r#"
schema_version = 1

[[class]]
name = "MenuLayer"
cpp_type = "MenuLayer"

  [[class.method]]
  symbol   = "MenuLayer::init"
  priority = 100

[[class]]
name = "AliasLayer"
cpp_type = "AliasLayer"

  [[class.method]]
  symbol   = "MenuLayer::init"
  priority = 10
"#;
        let err = parse_manifest(content, &fake_path()).unwrap_err();
        match err {
            SurfaceError::DuplicateSymbol { symbol } => {
                assert_eq!(symbol, SymbolId::new("MenuLayer::init"));
            }
            other => panic!("attesa DuplicateSymbol, trovato {other:?}"),
        }
    }

    #[test]
    fn parse_rejects_duplicate_symbol_within_same_class() {
        let content = r#"
schema_version = 1

[[class]]
name = "MenuLayer"
cpp_type = "MenuLayer"

  [[class.method]]
  symbol   = "MenuLayer::init"
  priority = 100

  [[class.method]]
  symbol   = "MenuLayer::init"
  priority = 200
"#;
        let err = parse_manifest(content, &fake_path()).unwrap_err();
        assert!(matches!(err, SurfaceError::DuplicateSymbol { .. }));
    }

    #[test]
    fn parse_rejects_unsupported_schema_version() {
        let content = r#"
schema_version = 2

[[class]]
name = "MenuLayer"
cpp_type = "MenuLayer"
"#;
        let err = parse_manifest(content, &fake_path()).unwrap_err();
        match err {
            SurfaceError::UnsupportedSchemaVersion { found, supported } => {
                assert_eq!(found, 2);
                assert_eq!(supported, SURFACE_SCHEMA_VERSION);
            }
            other => panic!("attesa UnsupportedSchemaVersion, trovato {other:?}"),
        }
    }

    #[test]
    fn parse_rejects_malformed_toml() {
        let content = "schema_version = 1\n[[class]\nname = \"oops\"";
        let err = parse_manifest(content, &fake_path()).unwrap_err();
        assert!(matches!(err, SurfaceError::Parse { .. }));
    }

    #[test]
    fn parse_accepts_manifest_without_type_map() {
        let content = r#"
schema_version = 1

[[class]]
name = "MenuLayer"
cpp_type = "MenuLayer"

  [[class.method]]
  symbol   = "MenuLayer::init"
  priority = 100
  hook     = true
"#;
        let manifest = parse_manifest(content, &fake_path()).unwrap();
        assert!(manifest.type_map.is_empty());
        assert_eq!(manifest.classes.len(), 1);
    }

    #[test]
    fn load_manifest_reports_io_error_for_missing_file() {
        let missing = std::env::temp_dir().join("pulse-surface-does-not-exist-xyz.toml");
        let err = load_manifest(&missing).unwrap_err();
        assert!(matches!(err, SurfaceError::Io { .. }));
    }
}
