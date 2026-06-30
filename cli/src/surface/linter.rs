//! `Surface_Linter` — aggregazione **fail-closed** delle anomalie della
//! superficie (Req 1.5, 2.4).
//!
//! Il linter **non re-implementa** alcun controllo: **orchestra** e **fa
//! emergere** (colleziona, non va in panico) le anomalie già rilevate dai pezzi
//! fail-closed esistenti del modulo, presentandole insieme in un unico report:
//!
//! - **simbolo duplicato nel manifest** (Req 2.4) — già rilevato da
//!   [`parse_manifest`](crate::surface::manifest::parse_manifest)/[`load_manifest`](crate::surface::manifest::load_manifest)
//!   come [`SurfaceError::DuplicateSymbol`]: qui viene **fatto emergere** come
//!   finding di chiusura;
//! - **`schema_version` non supportata** — già rilevata in fase di
//!   `parse`/`load` come [`SurfaceError::UnsupportedSchemaVersion`]: idem;
//! - **simbolo privo di `Catalog_Entry`** (Req 1.5) — già raccolto da
//!   [`compile_surface`](crate::surface::compiler::compile_surface) in
//!   [`CompiledSurface::diagnostics`](crate::surface::compiler::CompiledSurface::diagnostics)
//!   come [`SurfaceError::MissingCatalogEntry`]: qui viene **propagato** tra i
//!   findings.
//!
//! ## Relazione con `load_manifest` / `compile_surface`
//!
//! Le tre anomalie nascono in **due momenti distinti** del flusso:
//!
//! 1. **al caricamento** del manifest (`parse_manifest`/`load_manifest`): i
//!    duplicati di simbolo (Req 2.4) e lo schema non supportato sono errori
//!    *globali* che impediscono perfino di ottenere un [`SurfaceManifest`]. Sono
//!    quindi osservabili **solo** a partire dalla **sorgente** del manifest
//!    (TOML), non da un manifest già caricato.
//! 2. **alla compilazione** (`compile_surface`): i simboli privi di
//!    `Catalog_Entry` (Req 1.5) sono errori *per-elemento* raccolti nelle
//!    `diagnostics`, con la superficie che prosegue *escludendo* l'elemento.
//!
//! Per questo il linter espone due entrate coerenti con i due momenti:
//!
//! - [`lint_surface_source`] — entrata **completa**: parte dalla **sorgente**
//!   TOML, così da poter far emergere **tutte e tre** le categorie (duplicati e
//!   schema in fase di parse, mancanti in fase di compile). È la funzione che il
//!   sottocomando `pulse surface lint` (task 11.1) usa.
//! - [`lint_surface`] — entrata su un [`SurfaceManifest`] **già caricato**: il
//!   dedup e lo schema sono già stati superati da `load_manifest`, quindi può
//!   far emergere **solo** i simboli mancanti via `compile_surface`. Comoda
//!   quando il manifest è già in mano al chiamante.
//!
//! ## Disciplina fail-closed
//!
//! Il linter **non produce artefatti**: si limita a *riportare*. Coerente con il
//! modello "exclude-and-continue" del compiler, un [`LintReport`] **non vuoto**
//! ([`LintReport::is_clean`] = `false`) segnala al chiamante che, su anomalia,
//! **non** deve generare alcuna superficie/header (Req 1.5, 2.4). Su un errore
//! *globale* di caricamento (duplicato/schema/IO/parse) il linter riporta quel
//! solo finding e **non** prosegue oltre, perché senza un manifest valido non
//! c'è nulla da compilare.
//!
//! _Requisiti: 1.5, 2.4._

use std::path::Path;

use crate::bindings::catalog::BindingCatalog;

use super::compiler::compile_surface;
use super::manifest::{parse_manifest, SurfaceManifest};
use super::SurfaceError;

/// Report del [`Surface_Linter`]: l'elenco delle anomalie **fatte emergere**
/// (collezionate) dai pezzi fail-closed del modulo, in ordine di scoperta.
///
/// Riusa la lista di [`SurfaceError`] come payload dei findings, anziché
/// introdurre un tipo d'errore parallelo: ogni finding è esattamente l'errore
/// che `load_manifest`/`compile_surface` già emettono (`DuplicateSymbol`,
/// `UnsupportedSchemaVersion`, `MissingCatalogEntry`, …). Così il linter resta
/// un **orchestratore** che fa emergere gli errori esistenti, non un loro
/// duplicato.
///
/// Deriva solo `Debug`: [`SurfaceError`] trasporta sorgenti `io`/`toml` e **non**
/// è `Clone`/`PartialEq`, perciò i test ispezionano le singole varianti con
/// `matches!`.
#[derive(Debug)]
pub struct LintReport {
    /// Le anomalie rilevate, in ordine di scoperta. Vuoto ⇒ superficie pulita.
    pub findings: Vec<SurfaceError>,
}

impl LintReport {
    /// Costruisce un report con un solo finding (errore globale di caricamento).
    fn single(error: SurfaceError) -> Self {
        Self {
            findings: vec![error],
        }
    }

    /// `true` se non è stata rilevata alcuna anomalia: la superficie copre
    /// l'intero manifest e nessun controllo fail-closed è scattato. Solo in
    /// questo caso il chiamante può procedere a generare gli artefatti.
    pub fn is_clean(&self) -> bool {
        self.findings.is_empty()
    }

    /// Numero di anomalie rilevate.
    pub fn len(&self) -> usize {
        self.findings.len()
    }

    /// `true` se il report non contiene findings (alias di [`LintReport::is_clean`]).
    pub fn is_empty(&self) -> bool {
        self.findings.is_empty()
    }
}

/// Esegue il lint a partire dalla **sorgente** TOML del `Surface_Manifest`
/// (entrata completa).
///
/// `path` è usato solo per contestualizzare gli errori di parse/IO; non viene
/// letto. Fa emergere, in chiusura e **senza produrre artefatti**:
///
/// 1. gli errori **globali** di caricamento — simbolo duplicato
///    ([`SurfaceError::DuplicateSymbol`], Req 2.4), `schema_version` non
///    supportata ([`SurfaceError::UnsupportedSchemaVersion`]) e TOML malformato
///    ([`SurfaceError::Parse`]) — riusando
///    [`parse_manifest`](crate::surface::manifest::parse_manifest). In presenza
///    di uno di questi, il report contiene **quel solo** finding e il lint si
///    ferma: senza un manifest valido non c'è nulla da compilare;
/// 2. altrimenti, gli errori **per-elemento** del join — simbolo privo di
///    `Catalog_Entry` ([`SurfaceError::MissingCatalogEntry`], Req 1.5) e
///    eventuali discordanze di firma — propagati dalle
///    [`CompiledSurface::diagnostics`](crate::surface::compiler::CompiledSurface::diagnostics).
pub fn lint_surface_source(
    content: &str,
    path: &Path,
    catalog: &BindingCatalog,
) -> LintReport {
    // (1) Caricamento del manifest: dedup (Req 2.4) e schema sono validati qui.
    // Un fallimento è un errore GLOBALE: lo facciamo emergere come unico finding
    // e ci fermiamo (niente da compilare senza manifest valido).
    let manifest = match parse_manifest(content, path) {
        Ok(manifest) => manifest,
        Err(error) => return LintReport::single(error),
    };

    // (2) Manifest valido: facciamo emergere gli errori per-elemento del join.
    lint_surface(&manifest, catalog)
}

/// Esegue il lint su un [`SurfaceManifest`] **già caricato** (entrata parziale).
///
/// Poiché il manifest è già stato ottenuto da `load_manifest`, le anomalie
/// **globali** (duplicati di simbolo, schema non supportato) sono già state
/// scartate a monte: questa funzione può far emergere **solo** i simboli privi
/// di `Catalog_Entry` (Req 1.5) raccolti da
/// [`compile_surface`](crate::surface::compiler::compile_surface) nelle sue
/// `diagnostics`, **senza** ridurre/duplicare il join.
///
/// Se `compile_surface` fallisce con un errore fail-closed d'intera compilazione
/// (oggi: `cpp_token` non derivabile → [`SurfaceError::InvalidCppToken`]), quel
/// solo errore è riportato come finding.
pub fn lint_surface(manifest: &SurfaceManifest, catalog: &BindingCatalog) -> LintReport {
    match compile_surface(manifest, catalog) {
        Ok(compiled) => LintReport {
            // Le diagnostiche per-elemento (MissingCatalogEntry, …) sono già la
            // forma fail-closed dell'esclusione: le facciamo emergere così come
            // sono, preservando l'ordine di scoperta.
            findings: compiled.diagnostics,
        },
        Err(error) => LintReport::single(error),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bindings::catalog::CatalogEntry;
    use crate::bindings::{Signature, SymbolId};

    /// Catalogo minimale con `MenuLayer::init` (senza offset, non serve al lint).
    fn catalog_with_menulayer_init() -> BindingCatalog {
        BindingCatalog {
            entries: vec![CatalogEntry {
                symbol: SymbolId::new("MenuLayer::init"),
                signature: Signature::new("bool", vec!["MenuLayer*".to_owned()]),
                offsets: Vec::new(),
            }],
        }
    }

    fn fake_path() -> std::path::PathBuf {
        std::path::PathBuf::from("surface.toml")
    }

    const CLEAN_MANIFEST: &str = r#"
schema_version = 1

[[class]]
name = "MenuLayer"
cpp_type = "MenuLayer"

  [[class.method]]
  symbol   = "MenuLayer::init"
  priority = 100
  hook     = true
"#;

    #[test]
    fn clean_manifest_yields_no_findings() {
        // Tutti i simboli del manifest hanno una Catalog_Entry, schema e dedup
        // sono validi: il report è pulito e il chiamante può generare.
        let catalog = catalog_with_menulayer_init();
        let report = lint_surface_source(CLEAN_MANIFEST, &fake_path(), &catalog);
        assert!(report.is_clean(), "atteso report pulito, trovato {report:?}");
        assert_eq!(report.len(), 0);
    }

    #[test]
    fn surfaces_duplicate_symbol_finding() {
        // Req 2.4: lo stesso simbolo in due voci è un errore GLOBALE di
        // caricamento, fatto emergere come unico finding (niente compile).
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
        let catalog = catalog_with_menulayer_init();
        let report = lint_surface_source(content, &fake_path(), &catalog);

        assert!(!report.is_clean());
        assert_eq!(report.len(), 1);
        match &report.findings[0] {
            SurfaceError::DuplicateSymbol { symbol } => {
                assert_eq!(symbol, &SymbolId::new("MenuLayer::init"));
            }
            other => panic!("attesa DuplicateSymbol, trovato {other:?}"),
        }
    }

    #[test]
    fn surfaces_missing_catalog_entry_finding() {
        // Req 1.5: un simbolo del manifest privo di Catalog_Entry è escluso e
        // segnalato; il linter lo fa emergere tra i findings.
        let content = r#"
schema_version = 1

[[class]]
name = "MenuLayer"
cpp_type = "MenuLayer"

  [[class.method]]
  symbol   = "MenuLayer::init"
  priority = 100

  [[class.method]]
  symbol   = "MenuLayer::ghost"
  priority = 50
"#;
        let catalog = catalog_with_menulayer_init();
        let report = lint_surface_source(content, &fake_path(), &catalog);

        assert!(!report.is_clean());
        assert_eq!(report.len(), 1);
        match &report.findings[0] {
            SurfaceError::MissingCatalogEntry { symbol } => {
                assert_eq!(symbol, &SymbolId::new("MenuLayer::ghost"));
            }
            other => panic!("attesa MissingCatalogEntry, trovato {other:?}"),
        }
    }

    #[test]
    fn surfaces_unsupported_schema_finding() {
        // schema_version non supportata è un errore GLOBALE: unico finding.
        let content = r#"
schema_version = 2

[[class]]
name = "MenuLayer"
cpp_type = "MenuLayer"
"#;
        let catalog = catalog_with_menulayer_init();
        let report = lint_surface_source(content, &fake_path(), &catalog);

        assert!(!report.is_clean());
        assert_eq!(report.len(), 1);
        match &report.findings[0] {
            SurfaceError::UnsupportedSchemaVersion { found, supported } => {
                assert_eq!(*found, 2);
                assert_eq!(*supported, 1);
            }
            other => panic!("attesa UnsupportedSchemaVersion, trovato {other:?}"),
        }
    }

    #[test]
    fn lint_surface_on_loaded_manifest_surfaces_missing_entries() {
        // L'entrata su manifest già caricato fa emergere i soli simboli mancanti.
        let manifest = parse_manifest(
            r#"
schema_version = 1

[[class]]
name = "MenuLayer"
cpp_type = "MenuLayer"

  [[class.method]]
  symbol   = "MenuLayer::init"
  priority = 100

  [[class.method]]
  symbol   = "MenuLayer::ghost"
  priority = 50
"#,
            &fake_path(),
        )
        .unwrap();

        let catalog = catalog_with_menulayer_init();
        let report = lint_surface(&manifest, &catalog);

        assert_eq!(report.len(), 1);
        assert!(matches!(
            report.findings[0],
            SurfaceError::MissingCatalogEntry { .. }
        ));
    }

    #[test]
    fn surfaces_multiple_missing_entries_in_discovery_order() {
        let content = r#"
schema_version = 1

[[class]]
name = "MenuLayer"
cpp_type = "MenuLayer"

  [[class.method]]
  symbol   = "MenuLayer::ghostA"
  priority = 1

  [[class.method]]
  symbol   = "MenuLayer::init"
  priority = 2

  [[class.method]]
  symbol   = "MenuLayer::ghostB"
  priority = 3
"#;
        let catalog = catalog_with_menulayer_init();
        let report = lint_surface_source(content, &fake_path(), &catalog);

        // Due simboli mancanti, nell'ordine di scoperta.
        assert_eq!(report.len(), 2);
        match (&report.findings[0], &report.findings[1]) {
            (
                SurfaceError::MissingCatalogEntry { symbol: a },
                SurfaceError::MissingCatalogEntry { symbol: b },
            ) => {
                assert_eq!(a, &SymbolId::new("MenuLayer::ghostA"));
                assert_eq!(b, &SymbolId::new("MenuLayer::ghostB"));
            }
            other => panic!("attese due MissingCatalogEntry, trovato {other:?}"),
        }
    }

    #[test]
    fn surfaces_malformed_toml_finding() {
        let content = "schema_version = 1\n[[class]\nname = \"oops\"";
        let catalog = catalog_with_menulayer_init();
        let report = lint_surface_source(content, &fake_path(), &catalog);
        assert_eq!(report.len(), 1);
        assert!(matches!(report.findings[0], SurfaceError::Parse { .. }));
    }
}
