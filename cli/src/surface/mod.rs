//! GD API Surface — strumentazione build-time della Pulse_CLI.
//!
//! Questo modulo costruisce **sopra** la `bindings-pipeline` (`cli/src/bindings/`)
//! la *superficie d'API* curata di Pulse: porta i `.pbind` *"dai bindings a una
//! vera API"*. Vive **interamente** nella Pulse_CLI (Rust) e **non
//! re-implementa** alcun componente della pipeline: **riusa** i tipi condivisi
//! (`SymbolId`, `Signature`, `GdVersion`, `TargetPlatform`, `TargetPair`,
//! `SENTINEL_VALUE`) esposti da [`crate::bindings`] e il `Binding_Catalog` come
//! **unica** sorgente di offset (la superficie legge offset e `verified`, non li
//! possiede).
//!
//! Questo file (`mod.rs`) definisce la **doppia identità** di un simbolo della
//! superficie e l'enum d'errore fail-closed del modulo:
//!
//! - [`SurfaceSymbol`] porta sia l'identità **canonica** ([`SymbolId`], es.
//!   `"MenuLayer::init"`, usata per la risoluzione runtime a corrispondenza
//!   esatta) sia il **`cpp_token`** C++-safe (es. `"MenuLayer_init"`, usato per
//!   i nomi/identificatori negli header SDK generati).
//! - [`cpp_token_of`] deriva il token dal canonico in modo **deterministico** e
//!   **iniettivo** sui simboli del catalogo: mappa `::` → `_` e rifiuta in
//!   chiusura ogni simbolo che non produca un identificatore C++ valido con
//!   [`SurfaceError::InvalidCppToken`].
//!
//! _Requisiti: 1.2, 10.2._

use std::path::PathBuf;

use serde::{Deserialize, Serialize};

// I tipi condivisi della pipeline sono **riusati**, non ridefiniti.
pub use crate::bindings::{
    GdVersion, Signature, SymbolId, TargetPair, TargetPlatform, SENTINEL_VALUE,
};

// Il gate di auditabilità e il suo errore vivono nella `bindings-pipeline`
// (`cli/src/bindings/provenance.rs`) e sono **riusati** dal `Surface_Validator`
// (task 9.2): la superficie non re-implementa la completezza della provenienza,
// la **riusa** via `gate_distributable`. Re-esportiamo l'errore per poterlo
// trasportare nella variante [`SurfaceError::Auditability`].
pub use crate::bindings::provenance::AuditabilityError;

/// Modello on-disk del `Surface_Manifest` e suo caricamento fail-closed.
pub mod manifest;

/// Tipi di dominio della `Surface_IR` (rappresentazione language-agnostic).
pub mod ir;

/// `Surface_Compiler`: fusione `Surface_Manifest × Binding_Catalog` → `Surface_IR`.
pub mod compiler;

/// `Cpp_Generator`: emissione header-only deterministica dell'API SDK C++.
pub mod cppgen;

/// `Surface_Linter`: aggregazione fail-closed delle anomalie del manifest/IR
/// (duplicati, simboli mancanti, schema non supportato) senza produrre artefatti.
pub mod linter;

/// `Surface_Validator`: gate di completezza della provenienza per coppia
/// (riuso di `gate_distributable`/`AuditabilityError`).
pub mod validation;

// ---------------------------------------------------------------------------
// SurfaceError — errore fail-closed del modulo `surface` (estensibile).
// ---------------------------------------------------------------------------

/// Errore della strumentazione di superficie.
///
/// Coerente con la disciplina **fail-closed** ereditata dalla pipeline: su
/// anomalia non viene prodotta alcuna superficie/artefatto derivato. L'enum è
/// `#[non_exhaustive]` perché le fasi successive (compiler, generator,
/// validator) vi aggiungeranno altre varianti (`MissingCatalogEntry`,
/// `SignatureMismatch`, `Auditability`, …) senza rompere i chiamanti.
///
/// Le derive sono limitate a `Debug` + `thiserror::Error` (come `CatalogError`
/// nella pipeline): le varianti [`SurfaceError::Io`] e [`SurfaceError::Parse`]
/// trasportano sorgenti (`std::io::Error`, `toml::de::Error`) che non sono
/// `Clone`/`PartialEq`, quindi l'enum non può derivarle. I confronti nei test
/// usano `matches!` anziché `==`.
#[derive(Debug, thiserror::Error)]
#[non_exhaustive]
pub enum SurfaceError {
    /// Il `SymbolId` canonico non produce un `cpp_token` che sia un
    /// identificatore C++ valido (Req 1.2, 10.2). Dopo la mappatura `::` → `_`,
    /// il risultato deve combaciare con `[A-Za-z_][A-Za-z0-9_]*`; ogni altro
    /// carattere (cifra iniziale, spazi, operatori, …) è **rifiutato in
    /// chiusura** e non genera alcun simbolo di superficie.
    #[error("token C++ non valido per il simbolo '{symbol}': {reason}")]
    InvalidCppToken {
        /// Simbolo canonico che ha prodotto un token non valido.
        symbol: SymbolId,
        /// Causa leggibile del rifiuto (carattere/posizione non ammessi).
        reason: String,
    },

    /// Lo stesso `SymbolId` compare in due voci del `Surface_Manifest`.
    ///
    /// Un metodo presente in due `[[class.method]]` (nella stessa classe o in
    /// classi diverse) viola l'unicità della selezione: il manifest è
    /// **rifiutato in chiusura** segnalando il simbolo duplicato, senza
    /// produrre alcuna superficie (Req 2.4).
    #[error("simbolo duplicato nel manifest: {symbol}")]
    DuplicateSymbol {
        /// Identificatore di simbolo che compare in due voci del manifest.
        symbol: SymbolId,
    },

    /// Un `ManifestMethod.symbol` non ha alcuna `Catalog_Entry` nel
    /// `Binding_Catalog` (Req 1.5).
    ///
    /// L'`API_Element` corrispondente è **escluso** dalla superficie e l'errore
    /// nomina il simbolo, **proseguendo** con la compilazione degli altri
    /// elementi (vedi [`crate::surface::compiler::compile_surface`]). È un errore
    /// *per-elemento* raccolto in
    /// [`crate::surface::compiler::CompiledSurface::diagnostics`], non un rifiuto
    /// dell'intera compilazione.
    #[error("nessuna Catalog_Entry per il simbolo del manifest: {symbol}")]
    MissingCatalogEntry {
        /// Simbolo del manifest privo di voce nel catalogo.
        symbol: SymbolId,
    },

    /// La firma dichiarata per un simbolo non coincide con quella della
    /// `Catalog_Entry` (Req 2.1): un elemento è esposto **se e solo se** esiste
    /// una `Catalog_Entry` con lo stesso `SymbolId` **e** la stessa `Signature`.
    ///
    /// L'elemento è **escluso** e l'errore riporta entrambe le firme. Come
    /// [`SurfaceError::MissingCatalogEntry`], è un errore *per-elemento* raccolto
    /// nelle diagnostiche, non un rifiuto dell'intera compilazione.
    ///
    /// **Nota (stato attuale, Req 2.1):** il `Surface_Manifest` **non** dichiara
    /// alcuna firma (porta solo i simboli, vedi `manifest.rs`), quindi non esiste
    /// una "firma del manifest" da confrontare: la `Signature` del catalogo è
    /// l'unica autorità e la [`CanonicalSignature`](crate::surface::ir::CanonicalSignature)
    /// è da essa **derivata**. Questa variante è definita per completezza e per
    /// l'uso futuro dal `Surface_Linter`/`Build_Check` quando una firma dichiarata
    /// esplicitamente entrerà in gioco; il compiler 3.1 non la emette.
    #[error("firma discordante per il simbolo {symbol}: manifest {manifest} vs catalogo {catalog}")]
    SignatureMismatch {
        /// Simbolo con firma discordante.
        symbol: SymbolId,
        /// Firma dichiarata (lato manifest/dichiarazione).
        manifest: String,
        /// Firma autorevole della `Catalog_Entry`.
        catalog: String,
    },

    /// La `schema_version` del manifest non è supportata da questo parser.
    ///
    /// Un valore diverso da quello supportato viene **rifiutato in chiusura**
    /// anziché interpretato a caso.
    #[error("schema_version del manifest non supportata: trovata {found}, supportata {supported}")]
    UnsupportedSchemaVersion {
        /// Versione di schema trovata nel manifest.
        found: u32,
        /// Versione di schema supportata da questo parser.
        supported: u32,
    },

    /// La provenienza del binding sottostante a un `API_Element` esposto come
    /// risolvibile è **incompleta** per una coppia `(GD_Version,
    /// Target_Platform)` (Req 8.2).
    ///
    /// È il riuso del gate della pipeline: il `Surface_Validator` (task 9.2)
    /// chiama [`crate::bindings::provenance::gate_distributable`] per ogni offset
    /// di una `Catalog_Entry`; quando un offset con `verified = true` non
    /// documenta la provenienza necessaria, il gate restituisce un
    /// [`AuditabilityError`] che identifica **simbolo + coppia**. La superficie
    /// **declassa** l'elemento a non risolvibile per quella coppia e trasporta
    /// l'errore qui senza re-implementare alcun controllo di completezza.
    ///
    /// `#[error(transparent)]` delega `Display` all'[`AuditabilityError`]
    /// sottostante, che già nomina simbolo, coppia e l'esito mancante.
    #[error(transparent)]
    Auditability(#[from] AuditabilityError),

    /// Il file del manifest non è leggibile da disco.
    #[error("manifest illeggibile {path}: {source}")]
    Io {
        /// Percorso del file coinvolto.
        path: PathBuf,
        /// Causa di I/O sottostante.
        #[source]
        source: std::io::Error,
    },

    /// Il contenuto TOML del manifest è malformato e non analizzabile.
    #[error("manifest malformato {path}: {source}")]
    Parse {
        /// Percorso del file coinvolto.
        path: PathBuf,
        /// Causa di parsing TOML sottostante.
        #[source]
        source: toml::de::Error,
    },

    /// La [`SurfaceIr`](crate::surface::ir::SurfaceIr) non è serializzabile in
    /// JSON verso `surface.ir.json` (Req 10.2).
    ///
    /// È un errore **fail-closed**: su anomalia non viene scritto alcun
    /// `surface.ir.json` (né file parziale). In pratica non accade per i tipi
    /// della IR (solo stringhe, interi e vettori), ma è modellato in chiusura
    /// per non assumere mai l'infallibilità della serializzazione.
    #[error("serializzazione della Surface_IR fallita: {source}")]
    SerializeIr {
        /// Causa di serializzazione JSON sottostante.
        #[source]
        source: serde_json::Error,
    },

    /// Il `surface.ir.json` non è scrivibile su disco in modo atomico (Req 10.2).
    ///
    /// Disciplina **fail-closed** coerente col `Cpp_Generator`/pipeline: su
    /// errore l'eventuale file precedente resta intatto byte-per-byte e nessun
    /// file temporaneo parziale è lasciato a terra.
    #[error("scrittura della Surface_IR fallita {path}: {source}")]
    WriteIr {
        /// Percorso del file di destinazione coinvolto.
        path: PathBuf,
        /// Causa di I/O sottostante.
        #[source]
        source: std::io::Error,
    },

    /// Un header C++ generato (`{types,bindings,hooks}.gen.hpp`) non è
    /// scrivibile su disco in modo atomico (Req 10.1).
    ///
    /// Disciplina **fail-closed** coerente con `ir.rs`/`generator.rs`: su errore
    /// l'eventuale `.gen.hpp` precedente resta intatto byte-per-byte e nessun
    /// file temporaneo parziale è lasciato a terra.
    #[error("generazione/scrittura dell'header C++ fallita {path}: {source}")]
    GenerateCpp {
        /// Percorso del file (o directory) di destinazione coinvolto.
        path: PathBuf,
        /// Causa di I/O sottostante.
        #[source]
        source: std::io::Error,
    },
}

// ---------------------------------------------------------------------------
// SurfaceSymbol — doppia identità (canonico + cpp_token) (Req 1.2).
// ---------------------------------------------------------------------------

/// Doppia identità di un simbolo della superficie: il [`SymbolId`] **canonico**
/// e il suo **`cpp_token`** C++-safe.
///
/// Il canonico (`"MenuLayer::init"`) è ciò che la risoluzione runtime usa a
/// corrispondenza **esatta** (coerente con `BindingSet::resolve`); il
/// `cpp_token` (`"MenuLayer_init"`) è l'identificatore usato nei nomi degli
/// header SDK generati (`BindingTraits`, marcatori `in_surface<token>`, macro
/// `PULSE_GD_HOOK`). Le due identità sono legate da [`cpp_token_of`]: la
/// derivazione è deterministica e iniettiva sui simboli del catalogo, così che
/// token distinti corrispondano a canonici distinti.
///
/// Deriva `Serialize`/`Deserialize` (aggiunte in 5.1) per essere serializzata
/// nella `Surface_IR` (`surface.ir.json`): entrambi i campi sono language-
/// agnostic (un `SymbolId` canonico e un identificatore C++-safe), nessun tipo
/// C++ vi è coinvolto. `SymbolId` già deriva serde nella pipeline.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub struct SurfaceSymbol {
    /// Identità canonica del simbolo (sorgente di verità per il runtime).
    pub canonical: SymbolId,
    /// Token C++-safe derivato dal canonico (per gli header generati).
    pub cpp_token: String,
}

impl SurfaceSymbol {
    /// Costruisce una [`SurfaceSymbol`] da un [`SymbolId`] canonico, derivando
    /// il `cpp_token` con [`cpp_token_of`].
    ///
    /// Fallisce con [`SurfaceError::InvalidCppToken`] se il canonico non produce
    /// un identificatore C++ valido (fail-closed).
    pub fn from_canonical(canonical: SymbolId) -> Result<Self, SurfaceError> {
        let cpp_token = cpp_token_of(&canonical)?;
        Ok(Self {
            canonical,
            cpp_token,
        })
    }
}

// ---------------------------------------------------------------------------
// cpp_token_of — derivazione deterministica e iniettiva del token C++.
// ---------------------------------------------------------------------------

/// Deriva il `cpp_token` C++-safe dal [`SymbolId`] canonico.
///
/// La derivazione è una mappatura **deterministica** e **iniettiva** sui
/// simboli del catalogo:
///
/// 1. ogni separatore di scope `::` è sostituito da un singolo `_`
///    (es. `"MenuLayer::init"` → `"MenuLayer_init"`);
/// 2. il risultato deve essere un identificatore C++ valido, ossia combaciare
///    con `[A-Za-z_][A-Za-z0-9_]*`.
///
/// Qualsiasi simbolo che, dopo la mappatura, contenga caratteri non ammessi
/// (spazi, operatori, punteggiatura) o inizi con una cifra è **rifiutato in
/// chiusura** con [`SurfaceError::InvalidCppToken`]: non viene generato alcun
/// token parziale.
///
/// _Requisiti: 1.2, 10.2._
pub fn cpp_token_of(symbol: &SymbolId) -> Result<String, SurfaceError> {
    let canonical = symbol.as_str();

    if canonical.is_empty() {
        return Err(SurfaceError::InvalidCppToken {
            symbol: symbol.clone(),
            reason: "il simbolo canonico è vuoto".to_owned(),
        });
    }

    // (1) Mappatura `::` → `_`. La sostituzione del solo separatore di scope è
    // iniettiva sui simboli del catalogo (identificatori separati da `::`):
    // segmenti distinti restano distinti perché `_` non può comparire come
    // confine di scope nel canonico.
    let token = canonical.replace("::", "_");

    // (2) Validazione `[A-Za-z_][A-Za-z0-9_]*`: il primo carattere non può essere
    // una cifra e ogni carattere deve essere alfanumerico ASCII o `_`.
    let mut chars = token.char_indices();
    match chars.next() {
        Some((_, first)) if first.is_ascii_alphabetic() || first == '_' => {}
        Some((_, first)) => {
            return Err(SurfaceError::InvalidCppToken {
                symbol: symbol.clone(),
                reason: format!(
                    "il token derivato '{token}' inizia con '{first}', non un carattere \
                     iniziale valido per un identificatore C++ ([A-Za-z_])"
                ),
            });
        }
        None => {
            return Err(SurfaceError::InvalidCppToken {
                symbol: symbol.clone(),
                reason: "il token derivato è vuoto".to_owned(),
            });
        }
    }

    for (idx, ch) in chars {
        if !(ch.is_ascii_alphanumeric() || ch == '_') {
            return Err(SurfaceError::InvalidCppToken {
                symbol: symbol.clone(),
                reason: format!(
                    "il token derivato '{token}' contiene il carattere non ammesso \
                     '{ch}' in posizione {idx}; ammessi solo [A-Za-z0-9_]"
                ),
            });
        }
    }

    Ok(token)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cpp_token_maps_scope_separator_to_underscore() {
        let symbol = SymbolId::new("MenuLayer::init");
        assert_eq!(cpp_token_of(&symbol).unwrap(), "MenuLayer_init");
    }

    #[test]
    fn cpp_token_passes_through_a_plain_identifier() {
        let symbol = SymbolId::new("init");
        assert_eq!(cpp_token_of(&symbol).unwrap(), "init");
    }

    #[test]
    fn cpp_token_maps_nested_scopes() {
        let symbol = SymbolId::new("cocos2d::CCNode::setPosition");
        assert_eq!(
            cpp_token_of(&symbol).unwrap(),
            "cocos2d_CCNode_setPosition"
        );
    }

    #[test]
    fn cpp_token_rejects_leading_digit() {
        let symbol = SymbolId::new("3Layer::init");
        let err = cpp_token_of(&symbol).unwrap_err();
        assert!(matches!(err, SurfaceError::InvalidCppToken { .. }));
    }

    #[test]
    fn cpp_token_rejects_whitespace() {
        let symbol = SymbolId::new("Menu Layer::init");
        let err = cpp_token_of(&symbol).unwrap_err();
        assert!(matches!(err, SurfaceError::InvalidCppToken { .. }));
    }

    #[test]
    fn cpp_token_rejects_operator_characters() {
        let symbol = SymbolId::new("MenuLayer::operator+");
        let err = cpp_token_of(&symbol).unwrap_err();
        assert!(matches!(err, SurfaceError::InvalidCppToken { .. }));
    }

    #[test]
    fn cpp_token_rejects_empty_symbol() {
        let symbol = SymbolId::new("");
        let err = cpp_token_of(&symbol).unwrap_err();
        assert!(matches!(err, SurfaceError::InvalidCppToken { .. }));
    }

    #[test]
    fn cpp_token_is_deterministic() {
        let symbol = SymbolId::new("MenuLayer::init");
        let first = cpp_token_of(&symbol).unwrap();
        let second = cpp_token_of(&symbol).unwrap();
        assert_eq!(first, second);
    }

    #[test]
    fn surface_symbol_carries_both_canonical_and_token() {
        let symbol = SurfaceSymbol::from_canonical(SymbolId::new("MenuLayer::init")).unwrap();
        assert_eq!(symbol.canonical, SymbolId::new("MenuLayer::init"));
        assert_eq!(symbol.cpp_token, "MenuLayer_init");
    }

    #[test]
    fn surface_symbol_from_canonical_propagates_rejection() {
        let err = SurfaceSymbol::from_canonical(SymbolId::new("3Layer::init")).unwrap_err();
        assert!(matches!(err, SurfaceError::InvalidCppToken { .. }));
    }

    #[test]
    fn cpp_token_is_injective_on_distinct_catalog_symbols() {
        // Simboli di catalogo distinti producono token distinti.
        let a = cpp_token_of(&SymbolId::new("MenuLayer::init")).unwrap();
        let b = cpp_token_of(&SymbolId::new("MenuLayer::onPlay")).unwrap();
        let c = cpp_token_of(&SymbolId::new("PlayLayer::init")).unwrap();
        assert_ne!(a, b);
        assert_ne!(a, c);
        assert_ne!(b, c);
    }
}
