//! `Contribution_Flow` — aggiunta e aggiornamento di simboli nel
//! `Binding_Catalog` (fonte di verità).
//!
//! Questo modulo implementa il flusso con cui un `Contributor` amplia la
//! superficie d'API del catalogo. Ogni comando muta i file TOML **un file per
//! simbolo** sotto `mod-index/catalog/symbols/*.toml`, mantenendo i diff locali
//! e tracciabili (Req 2, 8.1).
//!
//! In questa unità è implementato **solo** [`add`] (Req 2.1):
//!
//! - crea una [`CatalogEntry`] da un identificatore di simbolo + firma;
//! - per ogni coppia `(GD_Version, Target_Platform)` richiesta **senza offset**
//!   registra l'offset come [`SENTINEL_VALUE`] (rappresentato on-disk
//!   dall'assenza di `rva`, il sentinel logico) con `Verified_Flag = false`;
//! - persiste la voce come file TOML del simbolo, ri-analizzabile dal parser di
//!   [`catalog`].
//!
//! Oltre a `add`, in questa unità è implementato anche [`set_offset`]
//! (Req 2.2/2.4):
//!
//! - carica la `Catalog_Entry` esistente del simbolo (errore se assente);
//! - associa l'offset (`rva`) all'[`OffsetRecord`] della coppia richiesta,
//!   creandolo se la coppia non era ancora registrata;
//! - registra il `Contributor` come `address_source` nel `Provenance_Record`
//!   dell'offset (Req 2.2);
//! - **mantiene `Verified_Flag = false`** finché la `Prologue_Verification` non
//!   passa; in particolare, aggiornare l'offset di una voce già
//!   `verified = true` riporta il flag a `false` per quell'offset (Req 2.4).
//!
//! I comandi `set-signature` (Req 2.5) e i rifiuti di contribuzione
//! (Req 2.3/2.6/2.7) sono implementati in questo modulo:
//!
//! - **Req 2.3** — un `Address_Data` (`set_offset`/`set_signature`) per un
//!   simbolo privo di `Catalog_Entry` è rifiutato con un errore che identifica il
//!   simbolo, lasciando il catalogo invariato (nessun file creato);
//! - **Req 2.6** — `add` per un simbolo già presente per la stessa coppia è
//!   rifiutato e la voce esistente resta invariata byte-per-byte: `add` non
//!   sovrascrive mai;
//! - **Req 2.7** — un update privo dell'identificatore di simbolo
//!   ([`ContributionError::MissingSymbol`]) o privo del campo di firma
//!   obbligatorio ([`ContributionError::MissingSignature`], tipo di ritorno
//!   vuoto) è rifiutato con un errore che nomina il campo mancante, lasciando lo
//!   stato del catalogo invariato.
//!
//! Tutti i rifiuti **falliscono in chiusura**: la validazione precede ogni
//! scrittura, quindi nessuna mutazione parziale raggiunge il disco.
//!
//! _Requisiti: 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7._
//!
//! La **serializzazione** di una `Catalog_Entry` nel formato TOML del catalogo è
//! self-contained in questo modulo (struct grezze speculari allo schema
//! on-disk) e produce un file che il parser di [`catalog`]
//! (`parse_catalog_entry`/`load_catalog_entry`) ri-analizza per round-trip.

use std::path::{Path, PathBuf};

use serde::Serialize;
use sha2::{Digest, Sha256};

use super::catalog::{
    load_catalog_entry, CatalogEntry, CatalogError, OffsetRecord, ProvenanceRecord,
    CATALOG_SCHEMA_VERSION,
};
use super::{Signature, SymbolId, TargetPair, TargetPlatform};

// ---------------------------------------------------------------------------
// Errori del flusso di contribuzione.
// ---------------------------------------------------------------------------

/// Errore di una operazione del `Contribution_Flow`.
///
/// Tutte le varianti **falliscono in chiusura** lasciando il `Binding_Catalog`
/// in uno stato coerente: in particolare `add` non sovrascrive mai una voce già
/// esistente ([`ContributionError::AlreadyExists`], coerente con Req 2.6 che
/// sarà completato nel task dei rifiuti).
#[derive(Debug, thiserror::Error)]
pub enum ContributionError {
    /// L'identificatore di simbolo è vuoto: campo obbligatorio mancante.
    ///
    /// Un update (`add`, `set_offset`, `set_signature`) privo dell'identificatore
    /// di simbolo è rifiutato lasciando lo stato del catalogo invariato; l'errore
    /// identifica il campo obbligatorio mancante (Req 2.7).
    #[error("identificatore di simbolo mancante: campo obbligatorio")]
    MissingSymbol,

    /// La firma è priva di un campo obbligatorio (tipo di ritorno vuoto).
    ///
    /// Un update che fornisce una firma (`add`, `set_signature`) priva del tipo
    /// di ritorno è rifiutato lasciando lo stato del catalogo invariato; l'errore
    /// **identifica il campo obbligatorio mancante** (Req 2.7). Il caso naturale
    /// è una [`Signature`] con `return_type` vuoto: senza tipo di ritorno la
    /// firma non è confrontabile dalla `Prologue_Verification`.
    #[error("firma incompleta: campo obbligatorio mancante ({field})")]
    MissingSignature {
        /// Nome del campo di firma mancante (es. `"return"`).
        field: String,
    },

    /// Esiste già un file-per-simbolo per questo simbolo: `add` non sovrascrive.
    #[error("la Catalog_Entry per il simbolo {symbol} esiste già: {path}")]
    AlreadyExists {
        /// Simbolo già presente nel catalogo.
        symbol: SymbolId,
        /// Percorso del file-per-simbolo esistente.
        path: PathBuf,
    },

    /// Non esiste alcuna `Catalog_Entry` per il simbolo richiesto.
    ///
    /// `set_offset`/`set_signature` necessitano di una voce preesistente da
    /// mutare: fornire un `Address_Data` o aggiornare la firma per un simbolo
    /// privo di `Catalog_Entry` è un rifiuto (Req 2.3). L'errore identifica il
    /// simbolo e lo stato del catalogo resta invariato.
    #[error("nessuna Catalog_Entry per il simbolo {symbol}: {path}")]
    NotFound {
        /// Simbolo privo di `Catalog_Entry`.
        symbol: SymbolId,
        /// Percorso del file-per-simbolo atteso ma assente.
        path: PathBuf,
    },

    /// Il file-per-simbolo esistente non è caricabile/analizzabile.
    ///
    /// Fail-closed: se la voce esistente è illeggibile o malformata la mutazione
    /// è abortita senza alterare lo stato del catalogo.
    #[error("caricamento della Catalog_Entry per {symbol} fallito: {source}")]
    Load {
        /// Simbolo la cui voce non è caricabile.
        symbol: SymbolId,
        /// Causa di caricamento/parsing sottostante.
        #[source]
        source: CatalogError,
    },

    /// Errore di I/O nella creazione della directory o scrittura del file TOML.
    #[error("scrittura del file del catalogo fallita {path}: {source}")]
    Io {
        /// Percorso coinvolto.
        path: PathBuf,
        /// Causa di I/O sottostante.
        #[source]
        source: std::io::Error,
    },

    /// Serializzazione della `Catalog_Entry` nel formato TOML fallita.
    #[error("serializzazione TOML del simbolo {symbol} fallita: {source}")]
    Serialize {
        /// Simbolo in fase di serializzazione.
        symbol: SymbolId,
        /// Causa di serializzazione TOML sottostante.
        #[source]
        source: toml::ser::Error,
    },
}

// ---------------------------------------------------------------------------
// Convenzione di naming del file-per-simbolo.
// ---------------------------------------------------------------------------

/// Lunghezza massima (in byte ASCII) del file-stem prima di applicare il
/// troncamento+hash anti-collisione.
///
/// I filesystem comuni limitano il singolo componente di percorso a 255 byte;
/// gli identificatori di simbolo template-pesanti del C++ possono superarlo una
/// volta espansi le sequenze di escape. Manteniamo un margine ampio (`200`)
/// così che `<prefisso>__h<hash>` resti comodamente sotto il limite.
const MAX_STEM_LEN: usize = 200;

/// `true` se `c` può comparire **direttamente** in un file-stem del catalogo:
/// lettere ASCII, cifre, `_`, `.` e `-`. Ogni altro carattere è codificato (vedi
/// [`symbol_file_stem`]).
fn is_stem_safe(c: char) -> bool {
    c.is_ascii_alphanumeric() || matches!(c, '_' | '.' | '-')
}

/// Converte un identificatore di simbolo nel nome del file-per-simbolo del
/// catalogo, **senza** estensione, producendo uno stem **filesystem-safe e
/// iniettivo** per qualunque simbolo (inclusi gli overload di operatore C++ come
/// `cocos2d::CCPoint::operator/`, che contengono caratteri vietati nei nomi di
/// file, separatori inclusi).
///
/// Schema di derivazione:
///
/// 1. Il separatore di scope `::` diventa `__` — **comportamento legacy
///    preservato byte-per-byte**: `"MenuLayer::init"` → `"MenuLayer__init"`,
///    così la seed esistente `mod-index/catalog/symbols/MenuLayer__init.toml`
///    mantiene il nome identico.
/// 2. Ogni carattere **non** in `[A-Za-z0-9_.-]` è codificato come `_x<HEX>_`,
///    dove `<HEX>` è il codepoint Unicode in esadecimale **maiuscolo** e il `_`
///    finale rende la sequenza **auto-delimitante**. La codifica è quindi
///    iniettiva sui caratteri vietati: codepoint distinti → sequenze distinte
///    (es. `operator/` → `..._x2F_` vs `operator%` → `..._x25_`), e il
///    delimitatore finale impedisce ambiguità tra escape adiacenti
///    (`//` → `_x2F__x2F_`). L'output è interamente ASCII.
/// 3. Guardia di lunghezza: se lo stem supera [`MAX_STEM_LEN`] byte, viene
///    troncato a un prefisso e suffissato con `__h<hash>`, dove `<hash>` sono i
///    primi 16 nibble esadecimali dello SHA-256 del simbolo **originale**; ciò
///    mantiene l'unicità anche per identificatori patologicamente lunghi
///    mantenendo il nome entro i limiti del filesystem.
///
/// Nota sull'iniettività: la codifica degli escape è iniettiva e l'output è un
/// nome di file privo di separatori. L'unico aliasing teorico residuo è quello
/// **già presente** nel comportamento legacy (`A::B` e un letterale `A__B`),
/// trascurabile nel dominio dei simboli C++ demangled; in ogni caso il writer
/// (`extract::writer`) effettua un controllo di collisione di percorso **in fase
/// di pianificazione** e fallisce in chiusura prima di scrivere alcunché, così
/// due simboli distinti non possono mai sovrascrivere lo stesso file.
pub fn symbol_file_stem(symbol: &SymbolId) -> String {
    // 1) Comportamento legacy: il separatore di scope `::` diventa `__`.
    let scoped = symbol.as_str().replace("::", "__");

    // 2) Sanitizzazione iniettiva dei caratteri vietati nei nomi di file.
    let mut stem = String::with_capacity(scoped.len());
    for c in scoped.chars() {
        if is_stem_safe(c) {
            stem.push(c);
        } else {
            stem.push_str(&format!("_x{:X}_", c as u32));
        }
    }

    // 3) Guardia di lunghezza: tronca + suffisso di hash per restare entro i
    //    limiti del filesystem preservando l'unicità. L'output è ASCII, quindi
    //    `len()` (byte) coincide col numero di caratteri.
    if stem.len() > MAX_STEM_LEN {
        let suffix = stem_hash_suffix(symbol.as_str());
        let mut truncated: String = stem.chars().take(MAX_STEM_LEN).collect();
        truncated.push_str("__h");
        truncated.push_str(&suffix);
        return truncated;
    }

    stem
}

/// Calcola il suffisso di disambiguazione per la guardia di lunghezza di
/// [`symbol_file_stem`]: i primi 16 nibble esadecimali (minuscoli) dello SHA-256
/// del simbolo originale. Deterministico e stabile fra esecuzioni/piattaforme.
fn stem_hash_suffix(symbol: &str) -> String {
    let mut hasher = Sha256::new();
    hasher.update(symbol.as_bytes());
    let digest = hasher.finalize();
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(16);
    for byte in digest.iter().take(8) {
        out.push(HEX[(byte >> 4) as usize] as char);
        out.push(HEX[(byte & 0x0f) as usize] as char);
    }
    out
}

/// Percorso assoluto del file-per-simbolo sotto `catalog_root/symbols/`.
fn symbol_file_path(catalog_root: &Path, symbol: &SymbolId) -> PathBuf {
    catalog_root
        .join("symbols")
        .join(format!("{}.toml", symbol_file_stem(symbol)))
}

// ---------------------------------------------------------------------------
// Validazione dei campi obbligatori dell'update (Req 2.7).
// ---------------------------------------------------------------------------

/// Rifiuta un identificatore di simbolo mancante (vuoto o solo spazi).
///
/// Un update privo del simbolo è un rifiuto che lascia lo stato del catalogo
/// invariato, segnalando il campo obbligatorio mancante (Req 2.7).
fn require_symbol(symbol: &SymbolId) -> Result<(), ContributionError> {
    if symbol.as_str().trim().is_empty() {
        return Err(ContributionError::MissingSymbol);
    }
    Ok(())
}

/// Rifiuta una firma priva del campo obbligatorio (tipo di ritorno vuoto).
///
/// Quando un update fornisce una firma (`add`, `set_signature`), il tipo di
/// ritorno è obbligatorio: senza di esso la firma non è confrontabile dalla
/// `Prologue_Verification`. La mancanza è un rifiuto che lascia lo stato del
/// catalogo invariato, segnalando il campo mancante (Req 2.7).
fn require_signature(signature: &Signature) -> Result<(), ContributionError> {
    if signature.return_type.trim().is_empty() {
        return Err(ContributionError::MissingSignature {
            field: "return".to_owned(),
        });
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// Costruzione della Catalog_Entry.
// ---------------------------------------------------------------------------

/// Costruisce una [`CatalogEntry`] da simbolo + firma con un offset
/// **sentinel/non verificato** per ogni coppia richiesta (Req 2.1).
///
/// Ogni `OffsetRecord` ha `rva = None` (il sentinel logico, [`SENTINEL_VALUE`])
/// e `verified = false`, con un `Provenance_Record` vuoto. Se `pairs` è vuoto la
/// voce viene creata senza alcun offset (caso "senza offset per una coppia").
fn build_sentinel_entry(symbol: SymbolId, signature: Signature, pairs: &[TargetPair]) -> CatalogEntry {
    let offsets = pairs
        .iter()
        .map(|pair| OffsetRecord {
            pair: *pair,
            // Nessun offset fornito → sentinel logico (rva assente), Req 2.1.
            rva: None,
            // Fail-closed: un offset sentinel non è mai verificato (Req 2.1).
            verified: false,
            provenance: ProvenanceRecord::empty(symbol.clone(), *pair),
        })
        .collect();

    CatalogEntry {
        symbol,
        signature,
        offsets,
    }
}

// ---------------------------------------------------------------------------
// Serializzazione verso il formato TOML del catalogo (round-trip con catalog.rs).
// ---------------------------------------------------------------------------

/// Mirror **di output** della radice del file TOML di un simbolo.
///
/// I campi scalari precedono le tabelle (`[signature]`) e gli array di tabelle
/// (`[[offset]]`) come richiesto dal serializzatore TOML. Lo schema rispecchia
/// esattamente le struct grezze di parsing di `catalog.rs`, garantendo il
/// round-trip parse→serialize→parse.
#[derive(Serialize)]
struct RawEntryOut {
    schema_version: u32,
    symbol: String,
    signature: RawSignatureOut,
    /// `[[offset]]`; omesso se la voce non ha alcun offset.
    #[serde(rename = "offset", skip_serializing_if = "Vec::is_empty")]
    offset: Vec<RawOffsetOut>,
}

/// Mirror di output della sezione `[signature]` (chiavi `return`/`params`).
#[derive(Serialize)]
struct RawSignatureOut {
    #[serde(rename = "return")]
    return_type: String,
    params: Vec<String>,
}

/// Mirror di output di un blocco `[[offset]]`.
///
/// `rva` è omesso quando assente: l'assenza è il sentinel logico letto dal
/// parser come `rva = None` + `verified = false` (Req 1.4, 2.1).
#[derive(Serialize)]
struct RawOffsetOut {
    gd_version: String,
    platform: TargetPlatform,
    #[serde(skip_serializing_if = "Option::is_none")]
    rva: Option<u64>,
    verified: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    provenance: Option<RawProvenanceOut>,
}

/// Mirror di output della sezione `[offset.provenance]`.
///
/// Emessa solo se almeno un campo è documentato; per un offset sentinel appena
/// creato da `add` la provenienza è vuota e quindi viene omessa.
#[derive(Serialize)]
struct RawProvenanceOut {
    #[serde(skip_serializing_if = "Option::is_none")]
    address_source: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    cross_check: Option<String>,
    #[serde(skip_serializing_if = "std::ops::Not::not")]
    cross_check_no_reuse: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    prologue_method: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    prologue_outcome: Option<String>,
}

impl RawProvenanceOut {
    /// Converte un [`ProvenanceRecord`] in mirror di output, restituendo `None`
    /// quando non c'è alcun esito documentato (così la sezione è omessa).
    fn from_record(prov: &ProvenanceRecord) -> Option<Self> {
        let is_empty = prov.address_source.is_none()
            && prov.cross_check.is_none()
            && !prov.cross_check_no_reuse
            && prov.prologue_method.is_none()
            && prov.prologue_outcome.is_none();
        if is_empty {
            return None;
        }
        Some(Self {
            address_source: prov.address_source.clone(),
            cross_check: prov.cross_check.clone(),
            cross_check_no_reuse: prov.cross_check_no_reuse,
            prologue_method: prov.prologue_method.clone(),
            prologue_outcome: prov.prologue_outcome.clone(),
        })
    }
}

/// Serializza una [`CatalogEntry`] nel formato TOML canonico del catalogo,
/// ri-analizzabile da `catalog::parse_catalog_entry`.
///
/// La `gd_version` di ogni offset è emessa come stringa `"<major>.<minor>"`
/// (es. `"2.2081"`) coerente col parser; la `platform` usa il `platform_id`
/// testuale. Gli offset senza `rva` omettono la chiave (sentinel logico).
pub fn serialize_entry(entry: &CatalogEntry) -> Result<String, ContributionError> {
    let raw = RawEntryOut {
        schema_version: CATALOG_SCHEMA_VERSION,
        symbol: entry.symbol.as_str().to_owned(),
        signature: RawSignatureOut {
            return_type: entry.signature.return_type.clone(),
            params: entry.signature.param_types.clone(),
        },
        offset: entry
            .offsets
            .iter()
            .map(|o| RawOffsetOut {
                gd_version: o.pair.gd.to_string(),
                platform: o.pair.platform,
                rva: o.rva,
                verified: o.verified,
                provenance: RawProvenanceOut::from_record(&o.provenance),
            })
            .collect(),
    };

    toml::to_string_pretty(&raw).map_err(|source| ContributionError::Serialize {
        symbol: entry.symbol.clone(),
        source,
    })
}

// ---------------------------------------------------------------------------
// `add` — registrazione di una nuova voce (Req 2.1).
// ---------------------------------------------------------------------------

/// Aggiunge una nuova [`CatalogEntry`] al `Binding_Catalog` (Req 2.1).
///
/// Crea una voce da `symbol` + `signature` e, per ogni coppia in `pairs`
/// **senza offset fornito**, registra un offset pari al `Sentinel_Value`
/// (rappresentato on-disk dall'assenza di `rva`) con `Verified_Flag = false`.
/// Se `pairs` è vuoto la voce viene creata senza offset. Scrive infine il file
/// TOML del simbolo sotto `catalog_root/symbols/<name>.toml` e ne restituisce il
/// percorso.
///
/// Fallisce in chiusura senza alterare lo stato del catalogo se:
///
/// - il simbolo è vuoto ([`ContributionError::MissingSymbol`], Req 2.7);
/// - la firma è priva del tipo di ritorno
///   ([`ContributionError::MissingSignature`], Req 2.7);
/// - esiste già un file-per-simbolo per quel simbolo
///   ([`ContributionError::AlreadyExists`], Req 2.6): `add` non sovrascrive mai
///   e la voce esistente resta invariata byte-per-byte;
/// - la creazione della directory/scrittura fallisce ([`ContributionError::Io`])
///   o la serializzazione TOML fallisce ([`ContributionError::Serialize`]).
pub fn add(
    catalog_root: &Path,
    symbol: SymbolId,
    signature: Signature,
    pairs: &[TargetPair],
) -> Result<PathBuf, ContributionError> {
    if symbol.as_str().trim().is_empty() {
        return Err(ContributionError::MissingSymbol);
    }

    // Un update che fornisce una firma deve includere il tipo di ritorno
    // (campo obbligatorio): in mancanza, rifiuto con stato invariato (Req 2.7).
    require_signature(&signature)?;

    let path = symbol_file_path(catalog_root, &symbol);

    // `add` non sovrascrive una voce esistente: una `Catalog_Entry` con lo stesso
    // simbolo per la stessa coppia è un rifiuto e la voce esistente resta
    // invariata byte-per-byte (fail-closed, Req 2.6).
    if path.exists() {
        return Err(ContributionError::AlreadyExists { symbol, path });
    }

    let entry = build_sentinel_entry(symbol, signature, pairs);
    let content = serialize_entry(&entry)?;

    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|source| ContributionError::Io {
            path: parent.to_path_buf(),
            source,
        })?;
    }

    std::fs::write(&path, content).map_err(|source| ContributionError::Io {
        path: path.clone(),
        source,
    })?;

    Ok(path)
}

// ---------------------------------------------------------------------------
// `set-offset` — associazione offset + provenienza, verified resta false (Req 2.2, 2.4).
// ---------------------------------------------------------------------------

/// Associa un offset (`rva`) a una coppia `(GD_Version, Target_Platform)` di una
/// `Catalog_Entry` esistente, registrando il `Contributor` come fonte
/// dell'`Address_Data` (Req 2.2, 2.4).
///
/// Comportamento:
///
/// - carica la voce esistente del simbolo da
///   `catalog_root/symbols/<name>.toml`; se il file non esiste fallisce con
///   [`ContributionError::NotFound`] **senza** creare nulla (un `Address_Data`
///   per un simbolo privo di `Catalog_Entry` è un rifiuto, Req 2.3);
/// - individua l'[`OffsetRecord`] della coppia richiesta, **creandolo** se la
///   coppia non era ancora registrata;
/// - imposta `rva = Some(rva)` e registra `contributor` come `address_source`
///   nel `Provenance_Record` dell'offset (Req 2.2);
/// - **mantiene `Verified_Flag = false`** finché la `Prologue_Verification` non
///   passa: in particolare, se l'offset era `verified = true`, aggiornarlo
///   riporta il flag a `false` (Req 2.4). Poiché l'RVA cambia, gli esiti di
///   `Observational_Cross_Check` e `Prologue_Verification` precedentemente
///   registrati si riferiscono al vecchio offset e vengono **invalidati**: la
///   verifica andrà rieseguita rispetto al nuovo offset.
///
/// Riscrive infine il file TOML del simbolo (round-trip con il parser di
/// [`catalog`]) e ne restituisce il percorso. Fallisce in chiusura lasciando lo
/// stato invariato se la voce è illeggibile/malformata
/// ([`ContributionError::Load`]), se la serializzazione fallisce
/// ([`ContributionError::Serialize`]) o su errore di I/O
/// ([`ContributionError::Io`]).
pub fn set_offset(
    catalog_root: &Path,
    symbol: &SymbolId,
    pair: TargetPair,
    rva: u64,
    contributor: &str,
) -> Result<PathBuf, ContributionError> {
    // Un Address_Data privo dell'identificatore di simbolo è un rifiuto con
    // stato invariato (Req 2.7).
    require_symbol(symbol)?;

    let path = symbol_file_path(catalog_root, symbol);

    // Un Address_Data per un simbolo privo di Catalog_Entry è un rifiuto: lo
    // stato del catalogo resta invariato, l'errore identifica il simbolo (Req 2.3).
    if !path.exists() {
        return Err(ContributionError::NotFound {
            symbol: symbol.clone(),
            path,
        });
    }

    // Carica la voce esistente da mutare; fail-closed su voce illeggibile.
    let mut entry = load_catalog_entry(&path).map_err(|source| ContributionError::Load {
        symbol: symbol.clone(),
        source,
    })?;

    // Provenance che registra il Contributor come fonte dell'Address_Data
    // (Req 2.2). Gli esiti di cross-check/prologo non sono ancora documentati
    // per il nuovo offset: restano assenti finché la verifica non viene
    // rieseguita (il gate di completezza impedirà verified→true senza di essi).
    let provenance = ProvenanceRecord {
        symbol: symbol.clone(),
        pair,
        address_source: Some(contributor.to_owned()),
        cross_check: None,
        cross_check_no_reuse: false,
        prologue_method: None,
        prologue_outcome: None,
    };

    match entry.offsets.iter_mut().find(|o| o.pair == pair) {
        // La coppia è già registrata: aggiorna l'offset e riporta verified=false
        // (Req 2.4). Conserva eventuale altra provenienza solo riscrivendo la
        // fonte e azzerando gli esiti ora obsoleti rispetto al nuovo RVA.
        Some(existing) => {
            existing.rva = Some(rva);
            existing.verified = false;
            existing.provenance = provenance;
        }
        // La coppia non era registrata: crea un nuovo OffsetRecord con
        // verified=false finché la Prologue_Verification non passa (Req 2.2).
        None => {
            entry.offsets.push(OffsetRecord {
                pair,
                rva: Some(rva),
                verified: false,
                provenance,
            });
        }
    }

    let content = serialize_entry(&entry)?;

    std::fs::write(&path, content).map_err(|source| ContributionError::Io {
        path: path.clone(),
        source,
    })?;

    Ok(path)
}

// ---------------------------------------------------------------------------
// `set-signature` — aggiornamento firma + reset di tutti gli offset (Req 2.5).
// ---------------------------------------------------------------------------

/// Aggiorna la firma di una `Catalog_Entry` esistente e riporta a `false` il
/// `Verified_Flag` di **tutti** i suoi offset finché ciascuno non supera di nuovo
/// la `Prologue_Verification` rispetto alla firma aggiornata (Req 2.5).
///
/// Comportamento:
///
/// - carica la voce esistente del simbolo da
///   `catalog_root/symbols/<name>.toml`; se il file non esiste fallisce con
///   [`ContributionError::NotFound`] **senza** creare nulla (non esiste alcuna
///   `Catalog_Entry` da aggiornare, coerente con Req 2.3);
/// - sostituisce `entry.signature` con la `signature` fornita;
/// - per **ogni** offset della voce riporta `verified = false` e **invalida** gli
///   esiti di provenienza relativi alla verifica (`prologue_method`,
///   `prologue_outcome`, `cross_check` → `None`, `cross_check_no_reuse` →
///   `false`): poiché la firma è cambiata, ciascun offset deve superare di nuovo
///   la `Prologue_Verification` rispetto alla firma aggiornata (Req 2.5),
///   coerentemente con il modo in cui [`set_offset`] invalida gli esiti obsoleti;
/// - **preserva** `rva` e `address_source` di ogni offset: cambia solo la firma e
///   lo stato di verifica, non l'`Address_Data` né la sua fonte.
///
/// Riscrive infine il file TOML del simbolo (round-trip con il parser di
/// [`catalog`]) e ne restituisce il percorso. Fallisce in chiusura lasciando lo
/// stato invariato se la voce è illeggibile/malformata
/// ([`ContributionError::Load`]), se la serializzazione fallisce
/// ([`ContributionError::Serialize`]) o su errore di I/O
/// ([`ContributionError::Io`]).
pub fn set_signature(
    catalog_root: &Path,
    symbol: &SymbolId,
    signature: Signature,
) -> Result<PathBuf, ContributionError> {
    // Un update privo dell'identificatore di simbolo o della firma è un rifiuto
    // con stato invariato, segnalando il campo obbligatorio mancante (Req 2.7).
    require_symbol(symbol)?;
    require_signature(&signature)?;

    let path = symbol_file_path(catalog_root, symbol);

    // Non esiste alcuna Catalog_Entry da aggiornare: rifiuto, stato invariato
    // (l'errore identifica il simbolo, Req 2.3).
    if !path.exists() {
        return Err(ContributionError::NotFound {
            symbol: symbol.clone(),
            path,
        });
    }

    // Carica la voce esistente da mutare; fail-closed su voce illeggibile.
    let mut entry = load_catalog_entry(&path).map_err(|source| ContributionError::Load {
        symbol: symbol.clone(),
        source,
    })?;

    // Aggiorna la firma della voce (Req 2.5).
    entry.signature = signature;

    // Reset di ogni offset: la firma è cambiata, quindi nessun esito di verifica
    // precedente è ancora valido (Req 2.5). Preserva rva e address_source.
    for offset in entry.offsets.iter_mut() {
        offset.verified = false;
        offset.provenance.cross_check = None;
        offset.provenance.cross_check_no_reuse = false;
        offset.provenance.prologue_method = None;
        offset.provenance.prologue_outcome = None;
    }

    let content = serialize_entry(&entry)?;

    std::fs::write(&path, content).map_err(|source| ContributionError::Io {
        path: path.clone(),
        source,
    })?;

    Ok(path)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bindings::catalog::{load_catalog_entry, parse_catalog_entry};
    use crate::bindings::{GdVersion, SENTINEL_VALUE};
    use std::sync::atomic::{AtomicU32, Ordering};

    /// Directory temporanea auto-pulente (nessuna dipendenza da `tempfile`):
    /// crea una root unica sotto `std::env::temp_dir()` e la rimuove al `Drop`,
    /// come nei test di `catalog.rs`.
    struct TempCatalog {
        root: PathBuf,
    }

    impl TempCatalog {
        fn new() -> Self {
            static COUNTER: AtomicU32 = AtomicU32::new(0);
            let unique = format!(
                "pulse-contribution-test-{}-{}",
                std::process::id(),
                COUNTER.fetch_add(1, Ordering::Relaxed)
            );
            let root = std::env::temp_dir().join(unique);
            std::fs::create_dir_all(&root).unwrap();
            Self { root }
        }
    }

    impl Drop for TempCatalog {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.root);
        }
    }

    fn menu_layer_signature() -> Signature {
        Signature::new("bool", vec!["MenuLayer*".to_owned()])
    }

    fn arm64_pair() -> TargetPair {
        TargetPair::new(GdVersion::new(2, 2081), TargetPlatform::MacosArm64)
    }

    #[test]
    fn symbol_file_stem_replaces_scope_separator() {
        let symbol = SymbolId::new("MenuLayer::init");
        assert_eq!(symbol_file_stem(&symbol), "MenuLayer__init");
    }

    #[test]
    fn symbol_file_stem_preserves_legacy_safe_symbols() {
        // I simboli "normali" (solo [A-Za-z0-9_.-] + `::`) sono invariati: il
        // comportamento legacy resta byte-per-byte identico.
        assert_eq!(
            symbol_file_stem(&SymbolId::new("cocos2d::CCNode::setPosition")),
            "cocos2d__CCNode__setPosition"
        );
        assert_eq!(
            symbol_file_stem(&SymbolId::new("PlayLayer::update")),
            "PlayLayer__update"
        );
    }

    #[test]
    fn symbol_file_stem_sanitizes_operator_overloads_without_separators() {
        // Regressione: gli overload di operatore C++ contengono caratteri vietati
        // nei nomi di file (incluso `/`). Lo stem non deve contenere separatori.
        let stem = symbol_file_stem(&SymbolId::new("cocos2d::CCPoint::operator/"));
        assert!(!stem.contains('/'), "lo stem non deve contenere `/`: {stem}");
        assert!(!stem.contains('\\'), "lo stem non deve contenere `\\`: {stem}");
        assert!(stem.is_ascii(), "lo stem deve essere ASCII: {stem}");
        // `/` è il codepoint 0x2F → `_x2F_`.
        assert_eq!(stem, "cocos2d__CCPoint__operator_x2F_");
    }

    #[test]
    fn symbol_file_stem_is_injective_across_distinct_operators() {
        // `operator/` vs `operator%` devono produrre stem diversi (iniettività).
        let div = symbol_file_stem(&SymbolId::new("cocos2d::CCPoint::operator/"));
        let modu = symbol_file_stem(&SymbolId::new("cocos2d::CCPoint::operator%"));
        assert_ne!(div, modu);
        assert_eq!(div, "cocos2d__CCPoint__operator_x2F_");
        assert_eq!(modu, "cocos2d__CCPoint__operator_x25_");
    }

    #[test]
    fn symbol_file_stem_self_delimiting_for_adjacent_illegal_chars() {
        // Escape adiacenti restano decodificabili (delimitatore `_` finale):
        // `<>` → `_x3C__x3E_`, `[]` → `_x5B__x5D_`, spazio → `_x20_`.
        assert_eq!(
            symbol_file_stem(&SymbolId::new("gd::vector<int>")),
            "gd__vector_x3C_int_x3E_"
        );
        assert_eq!(
            symbol_file_stem(&SymbolId::new("operator[]")),
            "operator_x5B__x5D_"
        );
        assert_eq!(
            symbol_file_stem(&SymbolId::new("operator new")),
            "operator_x20_new"
        );
    }

    #[test]
    fn symbol_file_stem_guards_pathological_length() {
        // Un identificatore molto lungo è troncato + suffissato con un hash,
        // restando entro MAX_STEM_LEN + suffisso, e resta ASCII.
        let long = "A".repeat(MAX_STEM_LEN + 50);
        let stem = symbol_file_stem(&SymbolId::new(long.clone()));
        assert!(stem.len() <= MAX_STEM_LEN + "__h".len() + 16);
        assert!(stem.contains("__h"), "deve includere il suffisso di hash: {stem}");
        assert!(stem.is_ascii());
        // Due simboli lunghi diversi → suffissi (e quindi stem) diversi.
        let other = format!("{long}B");
        assert_ne!(symbol_file_stem(&SymbolId::new(other)), stem);
    }

    #[test]
    fn add_writes_operator_overload_to_safe_path() {
        // L'overload di operatore deve scrivere un file valido senza creare
        // sotto-directory bogus, e il `symbol` canonico resta intatto nel TOML.
        let tmp = TempCatalog::new();
        let path = add(
            &tmp.root,
            SymbolId::new("cocos2d::CCPoint::operator/"),
            menu_layer_signature(),
            &[arm64_pair()],
        )
        .unwrap();

        assert!(path.exists(), "il file TOML deve esistere: {path:?}");
        assert_eq!(path.parent(), Some(tmp.root.join("symbols").as_path()));
        let entry = load_catalog_entry(&path).unwrap();
        assert_eq!(entry.symbol, SymbolId::new("cocos2d::CCPoint::operator/"));
    }

    #[test]
    fn add_writes_toml_file_at_expected_path() {
        let tmp = TempCatalog::new();
        let path = add(
            &tmp.root,
            SymbolId::new("MenuLayer::init"),
            menu_layer_signature(),
            &[arm64_pair()],
        )
        .unwrap();

        assert_eq!(
            path,
            tmp.root.join("symbols").join("MenuLayer__init.toml")
        );
        assert!(path.exists(), "il file TOML del simbolo deve esistere");
    }

    #[test]
    fn add_registers_sentinel_offset_with_verified_false() {
        // Req 2.1: senza offset per una coppia, offset = Sentinel_Value e
        // Verified_Flag = false.
        let tmp = TempCatalog::new();
        let path = add(
            &tmp.root,
            SymbolId::new("MenuLayer::init"),
            menu_layer_signature(),
            &[arm64_pair()],
        )
        .unwrap();

        let entry = load_catalog_entry(&path).unwrap();
        assert_eq!(entry.symbol, SymbolId::new("MenuLayer::init"));
        assert_eq!(entry.offsets.len(), 1);

        let offset = &entry.offsets[0];
        assert_eq!(offset.pair, arm64_pair());
        assert_eq!(offset.rva, None, "l'assenza di rva è il sentinel logico");
        assert!(offset.is_sentinel());
        assert_eq!(offset.effective_rva(), SENTINEL_VALUE);
        assert!(!offset.verified, "Verified_Flag deve essere false (Req 2.1)");
    }

    #[test]
    fn add_written_file_round_trips_through_catalog_parser() {
        // Il file scritto da `add` deve essere ri-analizzabile dal parser di
        // catalog.rs (round-trip), preservando simbolo, firma e offset sentinel.
        let tmp = TempCatalog::new();
        let pairs = [
            arm64_pair(),
            TargetPair::new(GdVersion::new(2, 2081), TargetPlatform::MacosX64),
        ];
        let path = add(
            &tmp.root,
            SymbolId::new("MenuLayer::init"),
            menu_layer_signature(),
            &pairs,
        )
        .unwrap();

        let content = std::fs::read_to_string(&path).unwrap();
        let entry = parse_catalog_entry(&content, &path).unwrap();

        assert_eq!(entry.symbol, SymbolId::new("MenuLayer::init"));
        assert_eq!(entry.signature, menu_layer_signature());
        assert_eq!(entry.offsets.len(), 2);
        for offset in &entry.offsets {
            assert_eq!(offset.rva, None);
            assert!(!offset.verified);
            assert!(offset.is_sentinel());
        }
        // Le coppie registrate corrispondono a quelle richieste.
        let registered: Vec<TargetPair> = entry.offsets.iter().map(|o| o.pair).collect();
        assert!(registered.contains(&pairs[0]));
        assert!(registered.contains(&pairs[1]));
    }

    #[test]
    fn add_with_no_pairs_creates_entry_without_offsets() {
        // "senza offset per una coppia": pairs vuoto → voce senza offset, comunque
        // ri-analizzabile.
        let tmp = TempCatalog::new();
        let path = add(
            &tmp.root,
            SymbolId::new("MenuLayer::onMoreGames"),
            Signature::new("void", vec!["MenuLayer*".to_owned()]),
            &[],
        )
        .unwrap();

        let entry = load_catalog_entry(&path).unwrap();
        assert!(entry.offsets.is_empty());
    }

    #[test]
    fn add_does_not_overwrite_existing_entry() {
        let tmp = TempCatalog::new();
        add(
            &tmp.root,
            SymbolId::new("MenuLayer::init"),
            menu_layer_signature(),
            &[arm64_pair()],
        )
        .unwrap();

        let err = add(
            &tmp.root,
            SymbolId::new("MenuLayer::init"),
            menu_layer_signature(),
            &[arm64_pair()],
        )
        .unwrap_err();

        assert!(matches!(err, ContributionError::AlreadyExists { .. }));
    }

    #[test]
    fn add_rejects_empty_symbol() {
        let tmp = TempCatalog::new();
        let err = add(&tmp.root, SymbolId::new(""), menu_layer_signature(), &[]).unwrap_err();
        assert!(matches!(err, ContributionError::MissingSymbol));
    }

    #[test]
    fn serialize_entry_omits_rva_and_empty_provenance_for_sentinel() {
        let entry = build_sentinel_entry(
            SymbolId::new("MenuLayer::init"),
            menu_layer_signature(),
            &[arm64_pair()],
        );
        let toml = serialize_entry(&entry).unwrap();

        assert!(toml.contains("schema_version = 1"));
        assert!(toml.contains(r#"symbol = "MenuLayer::init""#));
        assert!(toml.contains(r#"gd_version = "2.2081""#));
        assert!(toml.contains("macos-arm64"));
        assert!(toml.contains("verified = false"));
        assert!(!toml.contains("rva"), "un offset sentinel non emette rva");
        assert!(
            !toml.contains("provenance"),
            "una provenienza vuota non viene emessa"
        );
    }

    // -----------------------------------------------------------------------
    // set_offset — associazione offset + provenienza, verified resta false
    // (Req 2.2, 2.4).
    // -----------------------------------------------------------------------

    #[test]
    fn set_offset_associates_rva_records_source_and_keeps_verified_false() {
        // Req 2.2: associa l'offset alla coppia, registra il Contributor come
        // address_source e mantiene verified=false (la Prologue_Verification non
        // è ancora stata eseguita).
        let tmp = TempCatalog::new();
        let symbol = SymbolId::new("MenuLayer::init");
        add(
            &tmp.root,
            symbol.clone(),
            menu_layer_signature(),
            &[arm64_pair()],
        )
        .unwrap();

        let path = set_offset(&tmp.root, &symbol, arm64_pair(), 0x316688, "contributor:tomas")
            .unwrap();

        let entry = load_catalog_entry(&path).unwrap();
        let offset = entry
            .offsets
            .iter()
            .find(|o| o.pair == arm64_pair())
            .expect("offset della coppia presente");

        assert_eq!(offset.rva, Some(0x316688), "l'offset deve essere associato");
        assert!(!offset.is_sentinel());
        assert!(
            !offset.verified,
            "Verified_Flag resta false finché il prologo non passa (Req 2.2)"
        );
        assert_eq!(
            offset.provenance.address_source.as_deref(),
            Some("contributor:tomas"),
            "il Contributor è registrato come address_source (Req 2.2)"
        );
    }

    #[test]
    fn set_offset_creates_offset_for_unregistered_pair() {
        // La voce esiste ma senza offset per la coppia: set_offset crea il record.
        let tmp = TempCatalog::new();
        let symbol = SymbolId::new("MenuLayer::init");
        add(&tmp.root, symbol.clone(), menu_layer_signature(), &[]).unwrap();

        let path =
            set_offset(&tmp.root, &symbol, arm64_pair(), 0x1000, "contributor:a").unwrap();

        let entry = load_catalog_entry(&path).unwrap();
        assert_eq!(entry.offsets.len(), 1);
        let offset = &entry.offsets[0];
        assert_eq!(offset.pair, arm64_pair());
        assert_eq!(offset.rva, Some(0x1000));
        assert!(!offset.verified);
    }

    #[test]
    fn set_offset_resets_verified_true_to_false() {
        // Req 2.4: aggiornare l'offset di una voce già verified=true riporta il
        // Verified_Flag a false per quell'offset.
        let tmp = TempCatalog::new();
        let symbol = SymbolId::new("MenuLayer::init");

        // Scrive a mano una voce con offset già verificato.
        let verified_toml = r#"
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
  address_source       = "contributor:old"
  cross_check          = "concordant"
  cross_check_no_reuse = true
  prologue_method      = "otool-manual"
  prologue_outcome     = "match"
"#;
        let symbol_path = tmp.root.join("symbols").join("MenuLayer__init.toml");
        std::fs::create_dir_all(symbol_path.parent().unwrap()).unwrap();
        std::fs::write(&symbol_path, verified_toml).unwrap();

        // Pre-condizione: l'offset è verified=true.
        let before = load_catalog_entry(&symbol_path).unwrap();
        assert!(before.offsets[0].verified);

        // Aggiorna l'offset → Verified_Flag torna false (Req 2.4).
        let path =
            set_offset(&tmp.root, &symbol, arm64_pair(), 0x999999, "contributor:new").unwrap();

        let entry = load_catalog_entry(&path).unwrap();
        let offset = &entry.offsets[0];
        assert_eq!(offset.rva, Some(0x999999), "il nuovo offset è associato");
        assert!(
            !offset.verified,
            "aggiornare l'offset riporta verified=false (Req 2.4)"
        );
        assert_eq!(
            offset.provenance.address_source.as_deref(),
            Some("contributor:new"),
            "la fonte è aggiornata al nuovo Contributor"
        );
        // Gli esiti precedenti, relativi al vecchio RVA, sono invalidati.
        assert_eq!(offset.provenance.prologue_outcome, None);
        assert_eq!(offset.provenance.cross_check, None);
    }

    #[test]
    fn set_offset_rejects_missing_symbol_entry() {
        // Req 2.3 (parziale): nessuna Catalog_Entry per il simbolo → rifiuto,
        // stato del catalogo invariato (nessun file creato).
        let tmp = TempCatalog::new();
        let symbol = SymbolId::new("PlayLayer::update");
        let err =
            set_offset(&tmp.root, &symbol, arm64_pair(), 0x1234, "contributor:a").unwrap_err();
        assert!(matches!(err, ContributionError::NotFound { .. }));

        let symbol_path = tmp.root.join("symbols").join("PlayLayer__update.toml");
        assert!(!symbol_path.exists(), "nessun file deve essere creato");
    }

    #[test]
    fn set_offset_preserves_other_offsets_and_round_trips() {
        // set_offset su una coppia non altera gli altri offset della voce.
        let tmp = TempCatalog::new();
        let symbol = SymbolId::new("MenuLayer::init");
        let other_pair = TargetPair::new(GdVersion::new(2, 2081), TargetPlatform::MacosX64);
        add(
            &tmp.root,
            symbol.clone(),
            menu_layer_signature(),
            &[arm64_pair(), other_pair],
        )
        .unwrap();

        let path = set_offset(&tmp.root, &symbol, arm64_pair(), 0x316688, "contributor:a")
            .unwrap();

        let entry = load_catalog_entry(&path).unwrap();
        assert_eq!(entry.offsets.len(), 2);

        let updated = entry.offsets.iter().find(|o| o.pair == arm64_pair()).unwrap();
        assert_eq!(updated.rva, Some(0x316688));

        let untouched = entry.offsets.iter().find(|o| o.pair == other_pair).unwrap();
        assert_eq!(untouched.rva, None, "l'altra coppia resta sentinel");
        assert!(!untouched.verified);
    }

    // -----------------------------------------------------------------------
    // set_signature — aggiornamento firma + reset di tutti gli offset (Req 2.5).
    // -----------------------------------------------------------------------

    #[test]
    fn set_signature_updates_signature_and_resets_all_offsets() {
        // Req 2.5: aggiornare la firma riporta verified=false per OGNI offset
        // (incluso uno già verified=true) e invalida gli esiti di provenienza
        // relativi alla verifica, preservando rva e address_source.
        let tmp = TempCatalog::new();
        let symbol = SymbolId::new("MenuLayer::init");
        let x64_pair = TargetPair::new(GdVersion::new(2, 2081), TargetPlatform::MacosX64);

        // Voce con due offset: uno verificato (con esiti documentati) e uno
        // sentinel non verificato.
        let toml = r#"
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
  address_source       = "contributor:old"
  cross_check          = "concordant"
  cross_check_no_reuse = true
  prologue_method      = "otool-manual"
  prologue_outcome     = "match"

[[offset]]
gd_version = "2.2081"
platform   = "macos-x64"
rva        = 0x401000
verified   = false

  [offset.provenance]
  address_source = "contributor:other"
"#;
        let symbol_path = tmp.root.join("symbols").join("MenuLayer__init.toml");
        std::fs::create_dir_all(symbol_path.parent().unwrap()).unwrap();
        std::fs::write(&symbol_path, toml).unwrap();

        // Pre-condizione: il primo offset è verified=true con esiti documentati.
        let before = load_catalog_entry(&symbol_path).unwrap();
        let arm_before = before.offsets.iter().find(|o| o.pair == arm64_pair()).unwrap();
        assert!(arm_before.verified);
        assert_eq!(arm_before.provenance.prologue_outcome.as_deref(), Some("match"));

        // Aggiorna la firma (es. parametro aggiuntivo).
        let new_signature = Signature::new("void", vec!["MenuLayer*".to_owned(), "int".to_owned()]);
        let path = set_signature(&tmp.root, &symbol, new_signature.clone()).unwrap();

        let entry = load_catalog_entry(&path).unwrap();
        assert_eq!(entry.signature, new_signature, "la firma deve essere aggiornata");
        assert_eq!(entry.offsets.len(), 2, "nessun offset rimosso o aggiunto");

        // Ogni offset: verified=false, esiti di verifica invalidati, rva e
        // address_source preservati.
        let arm = entry.offsets.iter().find(|o| o.pair == arm64_pair()).unwrap();
        assert!(!arm.verified, "verified torna false per l'offset prima verificato (Req 2.5)");
        assert_eq!(arm.rva, Some(0x316688), "l'rva è preservato");
        assert_eq!(
            arm.provenance.address_source.as_deref(),
            Some("contributor:old"),
            "address_source è preservato"
        );
        assert_eq!(arm.provenance.prologue_method, None, "prologue_method invalidato");
        assert_eq!(arm.provenance.prologue_outcome, None, "prologue_outcome invalidato");
        assert_eq!(arm.provenance.cross_check, None, "cross_check invalidato");
        assert!(!arm.provenance.cross_check_no_reuse, "cross_check_no_reuse invalidato");

        let x64 = entry.offsets.iter().find(|o| o.pair == x64_pair).unwrap();
        assert!(!x64.verified, "resta false per l'offset già non verificato");
        assert_eq!(x64.rva, Some(0x401000), "l'rva dell'altro offset è preservato");
        assert_eq!(
            x64.provenance.address_source.as_deref(),
            Some("contributor:other"),
            "address_source dell'altro offset è preservato"
        );
    }

    #[test]
    fn set_signature_rejects_missing_symbol_entry() {
        // Req 2.5/2.3: nessuna Catalog_Entry per il simbolo → rifiuto, stato
        // del catalogo invariato (nessun file creato).
        let tmp = TempCatalog::new();
        let symbol = SymbolId::new("PlayLayer::update");
        let err = set_signature(&tmp.root, &symbol, menu_layer_signature()).unwrap_err();
        assert!(matches!(err, ContributionError::NotFound { .. }));

        let symbol_path = tmp.root.join("symbols").join("PlayLayer__update.toml");
        assert!(!symbol_path.exists(), "nessun file deve essere creato");
    }

    #[test]
    fn set_signature_round_trips_through_catalog_parser() {
        // Il file riscritto da set_signature deve essere ri-analizzabile dal
        // parser di catalog.rs, preservando la nuova firma e gli offset.
        let tmp = TempCatalog::new();
        let symbol = SymbolId::new("MenuLayer::init");
        add(
            &tmp.root,
            symbol.clone(),
            menu_layer_signature(),
            &[arm64_pair()],
        )
        .unwrap();
        set_offset(&tmp.root, &symbol, arm64_pair(), 0x316688, "contributor:a").unwrap();

        let new_signature = Signature::new("int", vec!["MenuLayer*".to_owned()]);
        let path = set_signature(&tmp.root, &symbol, new_signature.clone()).unwrap();

        let content = std::fs::read_to_string(&path).unwrap();
        let entry = parse_catalog_entry(&content, &path).unwrap();
        assert_eq!(entry.signature, new_signature);
        let offset = entry.offsets.iter().find(|o| o.pair == arm64_pair()).unwrap();
        assert_eq!(offset.rva, Some(0x316688), "rva preservato dopo round-trip");
        assert!(!offset.verified);
    }

    // -----------------------------------------------------------------------
    // Rifiuti di contribuzione, stato invariato (Req 2.3, 2.6, 2.7).
    // -----------------------------------------------------------------------

    #[test]
    fn set_offset_rejected_missing_entry_leaves_catalog_unchanged_and_names_symbol() {
        // Req 2.3: Address_Data per un simbolo senza Catalog_Entry → rifiuto con
        // errore che identifica il simbolo; catalogo invariato (nessun file).
        let tmp = TempCatalog::new();
        let symbol = SymbolId::new("PlayLayer::update");

        let err =
            set_offset(&tmp.root, &symbol, arm64_pair(), 0x1234, "contributor:a").unwrap_err();

        match &err {
            ContributionError::NotFound { symbol: s, .. } => {
                assert_eq!(s, &symbol, "l'errore deve identificare il simbolo (Req 2.3)");
            }
            other => panic!("atteso NotFound, trovato {other:?}"),
        }
        // Il messaggio d'errore nomina il simbolo.
        assert!(err.to_string().contains("PlayLayer::update"));

        // Catalogo invariato: nessun file-per-simbolo creato.
        let symbols_dir = tmp.root.join("symbols");
        assert!(
            !symbols_dir.join("PlayLayer__update.toml").exists(),
            "nessun file deve essere creato (Req 2.3)"
        );
    }

    #[test]
    fn add_rejected_on_existing_symbol_leaves_file_byte_unchanged() {
        // Req 2.6: add per un simbolo già presente per la stessa coppia →
        // rifiuto, voce esistente invariata byte-per-byte (add non sovrascrive).
        let tmp = TempCatalog::new();
        let symbol = SymbolId::new("MenuLayer::init");

        // Prima aggiunta + un offset, così il file ha contenuto significativo.
        add(
            &tmp.root,
            symbol.clone(),
            menu_layer_signature(),
            &[arm64_pair()],
        )
        .unwrap();
        let path = set_offset(&tmp.root, &symbol, arm64_pair(), 0x316688, "contributor:tomas")
            .unwrap();

        // Snapshot byte-per-byte del file esistente.
        let before = std::fs::read(&path).unwrap();

        // add con simbolo già presente, anche con firma diversa → rifiuto.
        let err = add(
            &tmp.root,
            symbol.clone(),
            Signature::new("int", vec!["MenuLayer*".to_owned(), "float".to_owned()]),
            &[arm64_pair()],
        )
        .unwrap_err();

        match &err {
            ContributionError::AlreadyExists { symbol: s, .. } => {
                assert_eq!(s, &symbol, "l'errore identifica il simbolo duplicato (Req 2.6)");
            }
            other => panic!("atteso AlreadyExists, trovato {other:?}"),
        }

        // Voce esistente invariata byte-per-byte.
        let after = std::fs::read(&path).unwrap();
        assert_eq!(before, after, "la voce esistente resta invariata (Req 2.6)");
    }

    #[test]
    fn add_rejects_missing_signature_return_type_naming_field() {
        // Req 2.7: update che fornisce una firma priva del campo obbligatorio
        // (tipo di ritorno vuoto) → rifiuto con errore che nomina il campo;
        // stato invariato (nessun file).
        let tmp = TempCatalog::new();
        let err = add(
            &tmp.root,
            SymbolId::new("MenuLayer::init"),
            Signature::new("", vec!["MenuLayer*".to_owned()]),
            &[arm64_pair()],
        )
        .unwrap_err();

        match &err {
            ContributionError::MissingSignature { field } => {
                assert_eq!(field, "return", "l'errore nomina il campo mancante (Req 2.7)");
            }
            other => panic!("atteso MissingSignature, trovato {other:?}"),
        }

        // Nessun file creato: stato invariato.
        assert!(
            !tmp.root.join("symbols").join("MenuLayer__init.toml").exists(),
            "nessun file deve essere creato (Req 2.7)"
        );
    }

    #[test]
    fn add_rejects_empty_symbol_leaves_catalog_unchanged() {
        // Req 2.7: update privo dell'identificatore di simbolo → rifiuto, stato
        // invariato.
        let tmp = TempCatalog::new();
        let err = add(
            &tmp.root,
            SymbolId::new("   "),
            menu_layer_signature(),
            &[arm64_pair()],
        )
        .unwrap_err();
        assert!(matches!(err, ContributionError::MissingSymbol));
        assert!(
            !tmp.root.join("symbols").exists(),
            "nessuna directory/file deve essere creata (Req 2.7)"
        );
    }

    #[test]
    fn set_offset_rejects_empty_symbol() {
        // Req 2.7: set-offset privo dell'identificatore di simbolo → rifiuto.
        let tmp = TempCatalog::new();
        let err = set_offset(&tmp.root, &SymbolId::new(""), arm64_pair(), 0x10, "c").unwrap_err();
        assert!(matches!(err, ContributionError::MissingSymbol));
    }

    #[test]
    fn set_signature_rejects_missing_signature_return_type_naming_field_state_unchanged() {
        // Req 2.7: set-signature con firma priva del tipo di ritorno → rifiuto
        // che nomina il campo; lo stato della voce esistente resta invariato
        // byte-per-byte (la validazione precede ogni scrittura).
        let tmp = TempCatalog::new();
        let symbol = SymbolId::new("MenuLayer::init");
        let path = add(
            &tmp.root,
            symbol.clone(),
            menu_layer_signature(),
            &[arm64_pair()],
        )
        .unwrap();
        let before = std::fs::read(&path).unwrap();

        let err = set_signature(
            &tmp.root,
            &symbol,
            Signature::new("  ", vec!["MenuLayer*".to_owned()]),
        )
        .unwrap_err();

        match &err {
            ContributionError::MissingSignature { field } => {
                assert_eq!(field, "return", "l'errore nomina il campo mancante (Req 2.7)");
            }
            other => panic!("atteso MissingSignature, trovato {other:?}"),
        }

        // Voce esistente invariata byte-per-byte: nessuna scrittura parziale.
        let after = std::fs::read(&path).unwrap();
        assert_eq!(before, after, "lo stato del catalogo resta invariato (Req 2.7)");
    }

    #[test]
    fn set_signature_rejects_empty_symbol() {
        // Req 2.7: set-signature privo dell'identificatore di simbolo → rifiuto.
        let tmp = TempCatalog::new();
        let err = set_signature(&tmp.root, &SymbolId::new(""), menu_layer_signature())
            .unwrap_err();
        assert!(matches!(err, ContributionError::MissingSymbol));
    }
}
