//! Modello on-disk del `Binding_Catalog` (fonte di verità) e suo parsing.
//!
//! Il `Binding_Catalog` è la **fonte di verità versionata, human-diffable**,
//! **distinta** dai `.pbind` generati. È un albero di file **TOML** sotto
//! `mod-index/catalog/`, con **un file per simbolo** sotto
//! `mod-index/catalog/symbols/*.toml`: aggiungere o aggiornare un simbolo tocca
//! un solo file e i diff restano locali (Req 2, 8.1).
//!
//! Questo modulo definisce i tipi di dominio del catalogo — [`CatalogEntry`],
//! [`OffsetRecord`], [`ProvenanceRecord`] e [`BindingCatalog`] — e il **parsing**
//! `serde`/`toml` di un singolo file-per-simbolo (sezioni `schema_version`,
//! `symbol`, `[signature]`, `[[offset]]` con `gd_version`/`platform`/`rva`/
//! `verified` e `[offset.provenance]`).
//!
//! Convenzioni del modello:
//!
//! - La firma è **una sola** per simbolo, valida per tutte le coppie (Req 1.6):
//!   nel TOML usa le chiavi `return`/`params`, mappate qui su
//!   [`Signature::return_type`]/[`Signature::param_types`].
//! - Ogni offset è indicizzato da una coppia `(GD_Version, Target_Platform)`
//!   (Req 1.1); `gd_version` è una stringa come `"2.2081"` mappata su
//!   [`GdVersion`] `{ major: 2, minor: 2081 }`.
//! - L'**assenza** di `rva` (`None`) rappresenta il **sentinel logico**
//!   ([`SENTINEL_VALUE`]) con `verified = false` (Req 1.4): un offset senza RVA
//!   è sempre non verificato (fail-closed).
//!
//! Il caricamento dell'intero albero con dedup e signature-consistency
//! (`load_catalog`, Req 1.5/1.6/1.7/3.7) è implementato nel task successivo;
//! qui viviamo a livello di **singola** `Catalog_Entry`.
//!
//! _Requisiti: 1.1, 1.2, 1.3, 1.4._

use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};

use serde::Deserialize;

use super::{GdVersion, Signature, SymbolId, TargetPair, TargetPlatform, SENTINEL_VALUE};

/// Versione di schema del catalogo on-disk supportata da questo parser.
///
/// Coincide con il campo `schema_version` dei file TOML del catalogo: un valore
/// diverso viene rifiutato (fail-closed) anziché interpretato a caso.
pub const CATALOG_SCHEMA_VERSION: u32 = 1;

// ---------------------------------------------------------------------------
// Tipi di dominio del catalogo.
// ---------------------------------------------------------------------------

/// Record di auditabilità per un singolo offset (versione base).
///
/// Documenta, per un offset di una `Catalog_Entry`, la fonte dell'`Address_Data`
/// e gli esiti di `Observational_Cross_Check` e `Prologue_Verification`
/// (Req 10.1). Conservato inline nel TOML del catalogo nella sezione
/// `[offset.provenance]`.
///
/// Questa è una versione **base** sufficiente al parsing: il `Provenance_Store`
/// completo e i suoi gate (Req 5.5, 10.2–10.5) sono definiti nel task dedicato.
/// I campi di esito sono mantenuti come [`Option<String>`] per restare
/// parseabili anche quando la provenienza non è ancora documentata; `symbol` e
/// `pair` sono derivati dal contesto della voce/offset (non dal TOML).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProvenanceRecord {
    /// Simbolo a cui si riferisce il record (derivato dalla `Catalog_Entry`).
    pub symbol: SymbolId,
    /// Coppia `(GD_Version, Target_Platform)` dell'offset (derivata dall'offset).
    pub pair: TargetPair,
    /// Fonte dell'`Address_Data`, es. `"contributor:tomas"` (Req 2.2, 10.1).
    pub address_source: Option<String>,
    /// Esito dell'`Observational_Cross_Check`, es. `"concordant"` (Req 4.3/4.4).
    pub cross_check: Option<String>,
    /// La provenienza documenta l'uso solo numerico osservativo (Req 4.6).
    pub cross_check_no_reuse: bool,
    /// Metodo della `Prologue_Verification`, es. `"otool-manual"` (Req 5.5).
    pub prologue_method: Option<String>,
    /// Esito della `Prologue_Verification`, es. `"match"` (Req 5.5, 10.2).
    pub prologue_outcome: Option<String>,
}

impl ProvenanceRecord {
    /// Crea un `Provenance_Record` vuoto (nessun esito documentato) per la
    /// coppia indicata. Default sensato quando il file TOML non riporta la
    /// sezione `[offset.provenance]`.
    pub fn empty(symbol: SymbolId, pair: TargetPair) -> Self {
        Self {
            symbol,
            pair,
            address_source: None,
            cross_check: None,
            cross_check_no_reuse: false,
            prologue_method: None,
            prologue_outcome: None,
        }
    }
}

/// Offset di un simbolo per una specifica coppia `(GD_Version, Target_Platform)`.
///
/// `rva = None` rappresenta il **sentinel logico** ([`SENTINEL_VALUE`]) con
/// `verified = false` (Req 1.4): un offset senza RVA non è mai risolvibile.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OffsetRecord {
    /// Coppia che indicizza questo offset.
    pub pair: TargetPair,
    /// RVA non negativo relativo alla base immagine (Req 1.2); `None` ⇒ sentinel.
    pub rva: Option<u64>,
    /// `Verified_Flag` dell'offset (Req 1.3); sempre `false` quando `rva` è `None`.
    pub verified: bool,
    /// Record di provenienza/auditabilità dell'offset (Req 10.1).
    pub provenance: ProvenanceRecord,
}

impl OffsetRecord {
    /// RVA effettivo dell'offset: il valore concreto se presente, altrimenti il
    /// [`SENTINEL_VALUE`] che marca l'assenza (Req 1.4).
    pub fn effective_rva(&self) -> u64 {
        self.rva.unwrap_or(SENTINEL_VALUE)
    }

    /// `true` se l'offset è il sentinel logico (RVA assente o pari al sentinel).
    pub fn is_sentinel(&self) -> bool {
        matches!(self.rva, None | Some(SENTINEL_VALUE))
    }
}

/// Una voce del `Binding_Catalog`: un simbolo, la sua firma unica e gli offset
/// per ogni coppia `(GD_Version, Target_Platform)` (Req 1.1, 1.6).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CatalogEntry {
    /// Identificatore univoco del simbolo, es. `"MenuLayer::init"`.
    pub symbol: SymbolId,
    /// Firma unica valida per tutte le coppie (Req 1.6).
    pub signature: Signature,
    /// Offset indicizzati per coppia `(GD_Version, Target_Platform)`.
    pub offsets: Vec<OffsetRecord>,
}

/// L'intero `Binding_Catalog` caricato in memoria: l'insieme delle
/// [`CatalogEntry`].
///
/// Il caricamento dell'albero con dedup e signature-consistency è implementato
/// nel task successivo; qui il tipo è la destinazione di tale caricamento.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct BindingCatalog {
    /// Tutte le voci del catalogo.
    pub entries: Vec<CatalogEntry>,
}

impl BindingCatalog {
    /// Crea un catalogo vuoto.
    pub fn new() -> Self {
        Self::default()
    }
}

// ---------------------------------------------------------------------------
// Errori di caricamento/parsing.
// ---------------------------------------------------------------------------

/// Errore di caricamento o parsing del `Binding_Catalog`.
///
/// Tutte le varianti **falliscono in chiusura**: nessun output derivato viene
/// prodotto su un catalogo illeggibile o malformato (coerente con Req 3.7).
#[derive(Debug, thiserror::Error)]
pub enum CatalogError {
    /// Il file del simbolo non è leggibile da disco.
    #[error("file del catalogo illeggibile {path}: {source}")]
    Io {
        /// Percorso del file coinvolto.
        path: PathBuf,
        /// Causa di I/O sottostante.
        #[source]
        source: std::io::Error,
    },

    /// Il contenuto TOML è malformato e non analizzabile.
    #[error("file del catalogo malformato {path}: {source}")]
    Parse {
        /// Percorso del file coinvolto.
        path: PathBuf,
        /// Causa di parsing TOML sottostante.
        #[source]
        source: toml::de::Error,
    },

    /// La `schema_version` del file non è supportata da questo parser.
    #[error(
        "schema_version non supportata in {path}: trovata {found}, supportata {supported}"
    )]
    UnsupportedSchemaVersion {
        /// Percorso del file coinvolto.
        path: PathBuf,
        /// Versione di schema trovata nel file.
        found: u32,
        /// Versione di schema supportata.
        supported: u32,
    },

    /// La stringa `gd_version` non rispetta il formato `"<major>.<minor>"`.
    #[error("gd_version malformata in {path}: {value:?} (atteso \"<major>.<minor>\")")]
    MalformedGdVersion {
        /// Percorso del file coinvolto.
        path: PathBuf,
        /// Valore grezzo non interpretabile.
        value: String,
    },

    /// Il `Binding_Catalog` è vuoto: nessun file-per-simbolo è stato trovato
    /// sotto `root/symbols/*.toml`.
    ///
    /// Fail-closed: un catalogo vuoto non può produrre alcun output derivato,
    /// quindi è un errore con causa esplicita (Req 3.7).
    #[error("catalogo vuoto: nessun file-per-simbolo trovato sotto {symbols_dir}")]
    EmptyCatalog {
        /// Directory `symbols/` ispezionata.
        symbols_dir: PathBuf,
    },

    /// Lo stesso identificatore di simbolo compare due volte per la medesima
    /// coppia `(GD_Version, Target_Platform)`.
    ///
    /// Il duplicato può nascere da due file-per-simbolo che dichiarano lo stesso
    /// simbolo, oppure da due blocchi `[[offset]]` con la stessa coppia nello
    /// stesso file. In entrambi i casi il catalogo è rifiutato senza output
    /// derivato (Req 1.5).
    #[error("simbolo duplicato {symbol} per la coppia {pair}: il catalogo è rifiutato")]
    DuplicateSymbolPair {
        /// Identificatore di simbolo duplicato.
        symbol: SymbolId,
        /// Coppia `(GD_Version, Target_Platform)` interessata.
        pair: TargetPair,
    },

    /// Lo stesso identificatore di simbolo presenta firme divergenti tra
    /// file/coppie diverse.
    ///
    /// Viola l'invariante "una sola firma valida per simbolo su tutte le coppie"
    /// (Req 1.6): il catalogo è rifiutato segnalando il simbolo e le due firme in
    /// conflitto, senza produrre output derivato (Req 1.7).
    #[error("firme in conflitto per il simbolo {symbol}: {first} vs {second}")]
    SignatureConflict {
        /// Identificatore di simbolo con firme divergenti.
        symbol: SymbolId,
        /// Prima firma osservata (formato `return(param, …)`).
        first: String,
        /// Seconda firma, in conflitto con la prima.
        second: String,
    },
}

// ---------------------------------------------------------------------------
// Strutture grezze di deserializzazione (mirror 1:1 del TOML on-disk).
// ---------------------------------------------------------------------------

/// Mirror della radice del file TOML di un simbolo.
#[derive(Debug, Deserialize)]
struct RawCatalogEntry {
    schema_version: u32,
    symbol: String,
    signature: RawSignature,
    /// `[[offset]]` ripetuto; assente ⇒ nessun offset registrato.
    #[serde(default, rename = "offset")]
    offset: Vec<RawOffset>,
}

/// Mirror della sezione `[signature]` (chiavi `return`/`params`).
#[derive(Debug, Deserialize)]
struct RawSignature {
    #[serde(rename = "return")]
    return_type: String,
    params: Vec<String>,
}

/// Mirror di un blocco `[[offset]]`.
#[derive(Debug, Deserialize)]
struct RawOffset {
    /// Versione GD come stringa, es. `"2.2081"`.
    gd_version: String,
    /// Piattaforma; deserializzata via `platform_id` (`"macos-arm64"`, …).
    platform: TargetPlatform,
    /// RVA opzionale; assente ⇒ sentinel logico (Req 1.4).
    rva: Option<u64>,
    /// `Verified_Flag` dichiarato; forzato a `false` se `rva` è assente.
    #[serde(default)]
    verified: bool,
    /// Sezione `[offset.provenance]` opzionale.
    provenance: Option<RawProvenance>,
}

/// Mirror della sezione `[offset.provenance]`.
#[derive(Debug, Deserialize)]
struct RawProvenance {
    address_source: Option<String>,
    cross_check: Option<String>,
    #[serde(default)]
    cross_check_no_reuse: bool,
    prologue_method: Option<String>,
    prologue_outcome: Option<String>,
}

// ---------------------------------------------------------------------------
// Parsing.
// ---------------------------------------------------------------------------

/// Interpreta una stringa `"<major>.<minor>"` come [`GdVersion`].
///
/// Esempio: `"2.2081"` → `GdVersion { major: 2, minor: 2081 }`. Lo split avviene
/// sul **primo** punto; entrambe le parti devono essere interi `u32`.
fn parse_gd_version(value: &str, path: &Path) -> Result<GdVersion, CatalogError> {
    let malformed = || CatalogError::MalformedGdVersion {
        path: path.to_path_buf(),
        value: value.to_owned(),
    };

    let (major_str, minor_str) = value.split_once('.').ok_or_else(malformed)?;
    let major: u32 = major_str.parse().map_err(|_| malformed())?;
    let minor: u32 = minor_str.parse().map_err(|_| malformed())?;
    Ok(GdVersion::new(major, minor))
}

/// Converte una struttura grezza in una [`CatalogEntry`] di dominio.
///
/// `path` è usato solo per arricchire gli errori; non viene letto.
fn entry_from_raw(raw: RawCatalogEntry, path: &Path) -> Result<CatalogEntry, CatalogError> {
    if raw.schema_version != CATALOG_SCHEMA_VERSION {
        return Err(CatalogError::UnsupportedSchemaVersion {
            path: path.to_path_buf(),
            found: raw.schema_version,
            supported: CATALOG_SCHEMA_VERSION,
        });
    }

    let symbol = SymbolId::new(raw.symbol);
    let signature = Signature::new(raw.signature.return_type, raw.signature.params);

    let mut offsets = Vec::with_capacity(raw.offset.len());
    for raw_offset in raw.offset {
        let gd = parse_gd_version(&raw_offset.gd_version, path)?;
        let pair = TargetPair::new(gd, raw_offset.platform);

        // Fail-closed: l'assenza di RVA è il sentinel logico → `verified = false`
        // indipendentemente dal flag dichiarato (Req 1.4).
        let rva = raw_offset.rva;
        let verified = raw_offset.verified && rva.is_some();

        let provenance = match raw_offset.provenance {
            Some(p) => ProvenanceRecord {
                symbol: symbol.clone(),
                pair,
                address_source: p.address_source,
                cross_check: p.cross_check,
                cross_check_no_reuse: p.cross_check_no_reuse,
                prologue_method: p.prologue_method,
                prologue_outcome: p.prologue_outcome,
            },
            None => ProvenanceRecord::empty(symbol.clone(), pair),
        };

        offsets.push(OffsetRecord {
            pair,
            rva,
            verified,
            provenance,
        });
    }

    Ok(CatalogEntry {
        symbol,
        signature,
        offsets,
    })
}

/// Analizza il contenuto TOML di un **singolo** file-per-simbolo in una
/// [`CatalogEntry`].
///
/// `path` serve esclusivamente a contestualizzare gli errori (non viene letto).
/// Fallisce in chiusura su TOML malformato, `schema_version` non supportata o
/// `gd_version` malformata.
pub fn parse_catalog_entry(content: &str, path: &Path) -> Result<CatalogEntry, CatalogError> {
    let raw: RawCatalogEntry = toml::from_str(content).map_err(|source| CatalogError::Parse {
        path: path.to_path_buf(),
        source,
    })?;
    entry_from_raw(raw, path)
}

/// Carica e analizza un **singolo** file-per-simbolo da disco in una
/// [`CatalogEntry`] (es. `mod-index/catalog/symbols/MenuLayer__init.toml`).
pub fn load_catalog_entry(path: &Path) -> Result<CatalogEntry, CatalogError> {
    let content = std::fs::read_to_string(path).map_err(|source| CatalogError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    parse_catalog_entry(&content, path)
}

// ---------------------------------------------------------------------------
// Caricamento dell'intero albero del catalogo (dedup + signature-consistency).
// ---------------------------------------------------------------------------

/// Rappresentazione di un simbolo in fase di aggregazione: la sua firma, gli
/// offset accumulati e l'insieme delle coppie già viste (per il dedup).
struct MergedEntry {
    signature: Signature,
    offsets: Vec<OffsetRecord>,
    seen_pairs: HashSet<TargetPair>,
}

/// Formatta una [`Signature`] in modo leggibile per i messaggi d'errore,
/// es. `bool(MenuLayer*)` o `void(PlayLayer*, float)`.
fn signature_to_string(signature: &Signature) -> String {
    format!(
        "{}({})",
        signature.return_type,
        signature.param_types.join(", ")
    )
}

/// Carica e **valida** l'intero `Binding_Catalog` sotto `root` (la directory
/// `mod-index/catalog/`), aggregando ogni file-per-simbolo di
/// `root/symbols/*.toml` in un [`BindingCatalog`].
///
/// Il caricamento **fallisce in chiusura senza produrre alcun output derivato**
/// su qualsiasi anomalia:
///
/// - **catalogo illeggibile/malformato/vuoto** → errore con causa
///   ([`CatalogError::Io`]/[`CatalogError::Parse`]/[`CatalogError::EmptyCatalog`],
///   Req 3.7);
/// - **simbolo duplicato per la stessa coppia** `(GD_Version, Target_Platform)`
///   → [`CatalogError::DuplicateSymbolPair`] con simbolo+coppia (Req 1.5); il
///   duplicato è rilevato sia tra file diversi sia tra blocchi `[[offset]]` dello
///   stesso file;
/// - **stesso simbolo con firme divergenti** tra file/coppie →
///   [`CatalogError::SignatureConflict`] con simbolo+firme in conflitto (Req 1.7),
///   così da garantire **una sola firma valida per simbolo** su tutte le coppie
///   (Req 1.6).
///
/// L'ordine di elaborazione dei file è deterministico (ordinamento lessicale del
/// percorso), così errori e aggregazione sono riproducibili. Le voci risultanti
/// conservano l'ordine di prima apparizione del simbolo.
pub fn load_catalog(root: &Path) -> Result<BindingCatalog, CatalogError> {
    let symbols_dir = root.join("symbols");

    // Scansione di `symbols/`. Directory mancante o illeggibile → fail-closed
    // con causa esplicita (Req 3.7).
    let read_dir = std::fs::read_dir(&symbols_dir).map_err(|source| CatalogError::Io {
        path: symbols_dir.clone(),
        source,
    })?;

    let mut toml_paths: Vec<PathBuf> = Vec::new();
    for dir_entry in read_dir {
        let dir_entry = dir_entry.map_err(|source| CatalogError::Io {
            path: symbols_dir.clone(),
            source,
        })?;
        let path = dir_entry.path();
        let is_toml = path.extension().and_then(|ext| ext.to_str()) == Some("toml");
        if is_toml && path.is_file() {
            toml_paths.push(path);
        }
    }

    // Ordine deterministico di elaborazione e di reporting degli errori.
    toml_paths.sort();

    // Catalogo vuoto (nessun file-per-simbolo) → errore (Req 3.7).
    if toml_paths.is_empty() {
        return Err(CatalogError::EmptyCatalog { symbols_dir });
    }

    // Aggregazione per simbolo con dedup di coppia e signature-consistency.
    // `order` conserva l'ordine di prima apparizione per un output deterministico.
    let mut order: Vec<SymbolId> = Vec::new();
    let mut by_symbol: HashMap<SymbolId, MergedEntry> = HashMap::new();

    for path in &toml_paths {
        let entry = load_catalog_entry(path)?;
        let symbol = entry.symbol.clone();

        match by_symbol.get_mut(&symbol) {
            // Prima apparizione del simbolo.
            None => {
                let mut merged = MergedEntry {
                    signature: entry.signature,
                    offsets: Vec::with_capacity(entry.offsets.len()),
                    seen_pairs: HashSet::new(),
                };
                for offset in entry.offsets {
                    // Dedup di coppia anche all'interno di un singolo file (Req 1.5).
                    if !merged.seen_pairs.insert(offset.pair) {
                        return Err(CatalogError::DuplicateSymbolPair {
                            symbol,
                            pair: offset.pair,
                        });
                    }
                    merged.offsets.push(offset);
                }
                by_symbol.insert(symbol.clone(), merged);
                order.push(symbol);
            }
            // Il simbolo è già comparso in un altro file.
            Some(merged) => {
                // Una sola firma valida per simbolo su tutte le coppie (Req 1.6/1.7).
                if merged.signature != entry.signature {
                    return Err(CatalogError::SignatureConflict {
                        symbol,
                        first: signature_to_string(&merged.signature),
                        second: signature_to_string(&entry.signature),
                    });
                }
                for offset in entry.offsets {
                    // Dedup di coppia tra file diversi che dichiarano lo stesso
                    // simbolo (Req 1.5).
                    if !merged.seen_pairs.insert(offset.pair) {
                        return Err(CatalogError::DuplicateSymbolPair {
                            symbol,
                            pair: offset.pair,
                        });
                    }
                    merged.offsets.push(offset);
                }
            }
        }
    }

    // Materializza le voci nell'ordine di prima apparizione del simbolo.
    let entries = order
        .into_iter()
        .map(|symbol| {
            let merged = by_symbol
                .remove(&symbol)
                .expect("simbolo presente in by_symbol per costruzione");
            CatalogEntry {
                symbol,
                signature: merged.signature,
                offsets: merged.offsets,
            }
        })
        .collect();

    Ok(BindingCatalog { entries })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::Path;

    /// File TOML del Prioritized_Target: `MenuLayer::init` verificato su
    /// `(2.2081, macos-arm64)`, e una coppia `macos-x64` priva di `rva`
    /// (sentinel logico, `verified = false`).
    const MENU_LAYER_INIT_TOML: &str = r#"
schema_version = 1
symbol = "MenuLayer::init"

[signature]
return = "bool"
params = ["MenuLayer*"]

[[offset]]
gd_version = "2.2081"
platform   = "macos-arm64"
rva        = 0x316688
verified   = true

  [offset.provenance]
  address_source       = "contributor:tomas"
  cross_check          = "concordant"
  cross_check_no_reuse = true
  prologue_method      = "otool-manual"
  prologue_outcome     = "match"

[[offset]]
gd_version = "2.2081"
platform   = "macos-x64"
# nessun rva → assenza rappresentata come Sentinel + verified=false
verified   = false
"#;

    fn p() -> &'static Path {
        Path::new("symbols/MenuLayer__init.toml")
    }

    #[test]
    fn parses_symbol_signature_and_offsets() {
        let entry = parse_catalog_entry(MENU_LAYER_INIT_TOML, p()).unwrap();

        assert_eq!(entry.symbol, SymbolId::new("MenuLayer::init"));
        assert_eq!(entry.signature.return_type, "bool");
        assert_eq!(entry.signature.param_types, vec!["MenuLayer*".to_owned()]);
        assert_eq!(entry.offsets.len(), 2);
    }

    #[test]
    fn parses_verified_offset_with_rva_and_provenance() {
        let entry = parse_catalog_entry(MENU_LAYER_INIT_TOML, p()).unwrap();

        let arm = entry
            .offsets
            .iter()
            .find(|o| o.pair.platform == TargetPlatform::MacosArm64)
            .expect("offset macos-arm64 presente");

        assert_eq!(arm.pair.gd, GdVersion::new(2, 2081));
        assert_eq!(arm.rva, Some(0x316688));
        assert!(arm.verified);
        assert!(!arm.is_sentinel());
        assert_eq!(arm.effective_rva(), 0x316688);

        assert_eq!(
            arm.provenance.address_source.as_deref(),
            Some("contributor:tomas")
        );
        assert_eq!(arm.provenance.cross_check.as_deref(), Some("concordant"));
        assert!(arm.provenance.cross_check_no_reuse);
        assert_eq!(
            arm.provenance.prologue_method.as_deref(),
            Some("otool-manual")
        );
        assert_eq!(arm.provenance.prologue_outcome.as_deref(), Some("match"));
        // La provenienza deriva simbolo+coppia dal contesto della voce.
        assert_eq!(arm.provenance.symbol, SymbolId::new("MenuLayer::init"));
        assert_eq!(arm.provenance.pair, arm.pair);
    }

    #[test]
    fn missing_rva_is_the_sentinel_with_verified_false() {
        // Req 1.4: l'assenza di `rva` (None) è il sentinel logico, verified=false.
        let entry = parse_catalog_entry(MENU_LAYER_INIT_TOML, p()).unwrap();

        let x64 = entry
            .offsets
            .iter()
            .find(|o| o.pair.platform == TargetPlatform::MacosX64)
            .expect("offset macos-x64 presente");

        assert_eq!(x64.rva, None);
        assert!(!x64.verified);
        assert!(x64.is_sentinel());
        assert_eq!(x64.effective_rva(), SENTINEL_VALUE);
    }

    #[test]
    fn verified_true_without_rva_is_forced_to_false() {
        // Fail-closed: anche se il TOML dichiara `verified = true`, l'assenza di
        // `rva` riporta il flag a `false` (Req 1.4).
        let toml = r#"
schema_version = 1
symbol = "PlayLayer::update"

[signature]
return = "void"
params = ["PlayLayer*", "float"]

[[offset]]
gd_version = "2.2081"
platform   = "macos-x64"
verified   = true
"#;
        let entry = parse_catalog_entry(toml, p()).unwrap();
        let offset = &entry.offsets[0];
        assert_eq!(offset.rva, None);
        assert!(!offset.verified, "verified deve essere forzato a false");
    }

    #[test]
    fn offset_without_provenance_section_gets_sensible_defaults() {
        let toml = r#"
schema_version = 1
symbol = "PlayLayer::update"

[signature]
return = "void"
params = ["PlayLayer*", "float"]

[[offset]]
gd_version = "2.2081"
platform   = "macos-arm64"
rva        = 0x1234
verified   = false
"#;
        let entry = parse_catalog_entry(toml, p()).unwrap();
        let prov = &entry.offsets[0].provenance;
        assert_eq!(prov.address_source, None);
        assert_eq!(prov.cross_check, None);
        assert!(!prov.cross_check_no_reuse);
        assert_eq!(prov.prologue_method, None);
        assert_eq!(prov.prologue_outcome, None);
        assert_eq!(prov.symbol, SymbolId::new("PlayLayer::update"));
    }

    #[test]
    fn entry_without_offsets_parses_to_empty_offsets() {
        let toml = r#"
schema_version = 1
symbol = "MenuLayer::onMoreGames"

[signature]
return = "void"
params = ["MenuLayer*", "cocos2d::CCObject*"]
"#;
        let entry = parse_catalog_entry(toml, p()).unwrap();
        assert!(entry.offsets.is_empty());
    }

    #[test]
    fn gd_version_string_maps_to_major_minor() {
        let gd = parse_gd_version("2.2081", p()).unwrap();
        assert_eq!(gd, GdVersion::new(2, 2081));
    }

    #[test]
    fn malformed_gd_version_is_rejected() {
        let toml = r#"
schema_version = 1
symbol = "X::y"

[signature]
return = "void"
params = []

[[offset]]
gd_version = "garbage"
platform   = "macos-arm64"
rva        = 0x10
verified   = false
"#;
        let err = parse_catalog_entry(toml, p()).unwrap_err();
        assert!(matches!(err, CatalogError::MalformedGdVersion { .. }));
    }

    #[test]
    fn unsupported_schema_version_is_rejected() {
        let toml = r#"
schema_version = 99
symbol = "X::y"

[signature]
return = "void"
params = []
"#;
        let err = parse_catalog_entry(toml, p()).unwrap_err();
        assert!(matches!(
            err,
            CatalogError::UnsupportedSchemaVersion { found: 99, .. }
        ));
    }

    #[test]
    fn malformed_toml_is_rejected_fail_closed() {
        let err = parse_catalog_entry("this is = not [valid toml", p()).unwrap_err();
        assert!(matches!(err, CatalogError::Parse { .. }));
    }

    #[test]
    fn unknown_platform_is_rejected() {
        let toml = r#"
schema_version = 1
symbol = "X::y"

[signature]
return = "void"
params = []

[[offset]]
gd_version = "2.2081"
platform   = "linux-x64"
rva        = 0x10
verified   = false
"#;
        // `linux-x64` non appartiene all'insieme finito chiuso di TargetPlatform.
        let err = parse_catalog_entry(toml, p()).unwrap_err();
        assert!(matches!(err, CatalogError::Parse { .. }));
    }

    // -----------------------------------------------------------------------
    // load_catalog — dedup, signature-consistency e fail-closed (Req 1.5/1.6/1.7/3.7).
    // -----------------------------------------------------------------------

    use std::sync::atomic::{AtomicU32, Ordering};

    /// Directory temporanea auto-pulente per i test del caricamento d'albero.
    ///
    /// Non dipende da `tempfile` (non disponibile come dev-dependency): crea una
    /// directory unica sotto `std::env::temp_dir()` e la rimuove al `Drop`.
    struct TempCatalog {
        root: PathBuf,
    }

    impl TempCatalog {
        fn new() -> Self {
            static COUNTER: AtomicU32 = AtomicU32::new(0);
            let unique = format!(
                "pulse-catalog-test-{}-{}",
                std::process::id(),
                COUNTER.fetch_add(1, Ordering::Relaxed)
            );
            let root = std::env::temp_dir().join(unique);
            std::fs::create_dir_all(root.join("symbols")).unwrap();
            Self { root }
        }

        /// Scrive un file-per-simbolo sotto `symbols/<name>.toml`.
        fn write_symbol(&self, name: &str, content: &str) {
            std::fs::write(self.root.join("symbols").join(format!("{name}.toml")), content)
                .unwrap();
        }
    }

    impl Drop for TempCatalog {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.root);
        }
    }

    const PLAY_LAYER_UPDATE_TOML: &str = r#"
schema_version = 1
symbol = "PlayLayer::update"

[signature]
return = "void"
params = ["PlayLayer*", "float"]

[[offset]]
gd_version = "2.2081"
platform   = "macos-arm64"
rva        = 0x4000
verified   = false
"#;

    #[test]
    fn load_catalog_aggregates_valid_tree() {
        let tmp = TempCatalog::new();
        tmp.write_symbol("MenuLayer__init", MENU_LAYER_INIT_TOML);
        tmp.write_symbol("PlayLayer__update", PLAY_LAYER_UPDATE_TOML);

        let catalog = load_catalog(&tmp.root).unwrap();
        assert_eq!(catalog.entries.len(), 2);

        let menu = catalog
            .entries
            .iter()
            .find(|e| e.symbol == SymbolId::new("MenuLayer::init"))
            .expect("MenuLayer::init presente");
        assert_eq!(menu.offsets.len(), 2);

        let play = catalog
            .entries
            .iter()
            .find(|e| e.symbol == SymbolId::new("PlayLayer::update"))
            .expect("PlayLayer::update presente");
        assert_eq!(play.signature.return_type, "void");
    }

    #[test]
    fn load_catalog_rejects_empty_directory() {
        // Req 3.7: catalogo vuoto → errore con causa, nessun output derivato.
        let tmp = TempCatalog::new();
        let err = load_catalog(&tmp.root).unwrap_err();
        assert!(matches!(err, CatalogError::EmptyCatalog { .. }));
    }

    #[test]
    fn load_catalog_rejects_missing_symbols_directory() {
        // Req 3.7: catalogo illeggibile (directory inesistente) → errore I/O.
        let missing = std::env::temp_dir().join(format!(
            "pulse-catalog-missing-{}-{}",
            std::process::id(),
            "x"
        ));
        let err = load_catalog(&missing).unwrap_err();
        assert!(matches!(err, CatalogError::Io { .. }));
    }

    #[test]
    fn load_catalog_rejects_malformed_file_fail_closed() {
        // Req 3.7: un solo file malformato fa fallire l'intero caricamento.
        let tmp = TempCatalog::new();
        tmp.write_symbol("MenuLayer__init", MENU_LAYER_INIT_TOML);
        tmp.write_symbol("broken", "this is = not [valid toml");

        let err = load_catalog(&tmp.root).unwrap_err();
        assert!(matches!(err, CatalogError::Parse { .. }));
    }

    #[test]
    fn load_catalog_rejects_duplicate_symbol_pair_across_files() {
        // Req 1.5: stesso simbolo+coppia in due file → DuplicateSymbolPair.
        let tmp = TempCatalog::new();
        let dup = r#"
schema_version = 1
symbol = "MenuLayer::init"

[signature]
return = "bool"
params = ["MenuLayer*"]

[[offset]]
gd_version = "2.2081"
platform   = "macos-arm64"
rva        = 0x316688
verified   = true
"#;
        tmp.write_symbol("a_MenuLayer__init", dup);
        tmp.write_symbol("b_MenuLayer__init", dup);

        let err = load_catalog(&tmp.root).unwrap_err();
        match err {
            CatalogError::DuplicateSymbolPair { symbol, pair } => {
                assert_eq!(symbol, SymbolId::new("MenuLayer::init"));
                assert_eq!(pair.platform, TargetPlatform::MacosArm64);
                assert_eq!(pair.gd, GdVersion::new(2, 2081));
            }
            other => panic!("atteso DuplicateSymbolPair, ottenuto {other:?}"),
        }
    }

    #[test]
    fn load_catalog_rejects_duplicate_pair_within_single_file() {
        // Req 1.5: due blocchi [[offset]] con la stessa coppia nello stesso file.
        let tmp = TempCatalog::new();
        let dup = r#"
schema_version = 1
symbol = "MenuLayer::init"

[signature]
return = "bool"
params = ["MenuLayer*"]

[[offset]]
gd_version = "2.2081"
platform   = "macos-arm64"
rva        = 0x316688
verified   = true

[[offset]]
gd_version = "2.2081"
platform   = "macos-arm64"
rva        = 0x999999
verified   = false
"#;
        tmp.write_symbol("MenuLayer__init", dup);

        let err = load_catalog(&tmp.root).unwrap_err();
        assert!(matches!(err, CatalogError::DuplicateSymbolPair { .. }));
    }

    #[test]
    fn load_catalog_rejects_signature_conflict() {
        // Req 1.6/1.7: stesso simbolo, firme divergenti tra file → SignatureConflict.
        let tmp = TempCatalog::new();
        let arm = r#"
schema_version = 1
symbol = "MenuLayer::init"

[signature]
return = "bool"
params = ["MenuLayer*"]

[[offset]]
gd_version = "2.2081"
platform   = "macos-arm64"
rva        = 0x316688
verified   = true
"#;
        let x64 = r#"
schema_version = 1
symbol = "MenuLayer::init"

[signature]
return = "int"
params = ["MenuLayer*", "bool"]

[[offset]]
gd_version = "2.2081"
platform   = "macos-x64"
rva        = 0x42
verified   = false
"#;
        tmp.write_symbol("a_MenuLayer__init", arm);
        tmp.write_symbol("b_MenuLayer__init", x64);

        let err = load_catalog(&tmp.root).unwrap_err();
        match err {
            CatalogError::SignatureConflict {
                symbol,
                first,
                second,
            } => {
                assert_eq!(symbol, SymbolId::new("MenuLayer::init"));
                // Le due firme in conflitto sono entrambe segnalate.
                assert_ne!(first, second);
            }
            other => panic!("atteso SignatureConflict, ottenuto {other:?}"),
        }
    }

    #[test]
    fn load_catalog_merges_same_symbol_same_signature_across_files() {
        // Stesso simbolo, stessa firma, coppie disgiunte → offset uniti in una voce.
        let tmp = TempCatalog::new();
        let arm = r#"
schema_version = 1
symbol = "MenuLayer::init"

[signature]
return = "bool"
params = ["MenuLayer*"]

[[offset]]
gd_version = "2.2081"
platform   = "macos-arm64"
rva        = 0x316688
verified   = true
"#;
        let x64 = r#"
schema_version = 1
symbol = "MenuLayer::init"

[signature]
return = "bool"
params = ["MenuLayer*"]

[[offset]]
gd_version = "2.2081"
platform   = "macos-x64"
rva        = 0x42
verified   = false
"#;
        tmp.write_symbol("a_MenuLayer__init", arm);
        tmp.write_symbol("b_MenuLayer__init", x64);

        let catalog = load_catalog(&tmp.root).unwrap();
        assert_eq!(catalog.entries.len(), 1);
        assert_eq!(catalog.entries[0].offsets.len(), 2);
    }

    #[test]
    fn load_catalog_ignores_non_toml_files() {
        let tmp = TempCatalog::new();
        tmp.write_symbol("MenuLayer__init", MENU_LAYER_INIT_TOML);
        // Un file non-TOML nella directory non deve essere caricato né rompere il load.
        std::fs::write(tmp.root.join("symbols").join("README.md"), "# note").unwrap();

        let catalog = load_catalog(&tmp.root).unwrap();
        assert_eq!(catalog.entries.len(), 1);
    }
}
