//! Binding_Extractor — derivazione automatica di `Catalog_Entry` dai binari
//! first-party di Geometry Dash (Pulse_CLI, sottocomando `pulse bindings extract`).
//!
//! Questo modulo **alimenta** la pipeline esistente e **non la re-implementa**:
//! il suo unico output sono altri file `Catalog_Entry` nello stesso formato TOML
//! (`mod-index/catalog/symbols/*.toml`) che `load_catalog`, il
//! `Binding_Generator`, il `binding_verifier`, i provider e la `gd-api-surface`
//! consumano **invariati**. Tutta la logica nuova vive sotto `cli/src/extract/`.
//!
//! Questo file (`mod.rs`) definisce lo **scaffold** del modulo:
//!
//! - il **riuso** dei tipi condivisi della pipeline (`SymbolId`, `Signature`,
//!   `GdVersion`, `TargetPlatform`, `TargetPair`, `SENTINEL_VALUE` da
//!   `crate::bindings`; `CatalogEntry`, `OffsetRecord`, `ProvenanceRecord` da
//!   `crate::bindings::catalog`) — **mai** ridefiniti qui;
//! - [`ExtractError`], l'errore fail-closed dell'estrattore (percorso / formato /
//!   coppia / conflitto), seguendo la convenzione `thiserror` del catalogo;
//! - [`Architecture`], derivata dal `platform_id()` testuale della
//!   `Target_Platform` (Req 8.1);
//! - [`BinaryFormat`], il formato eseguibile atteso/rilevato (ELF / Mach-O / PE);
//! - [`SUPPORTED_PLATFORMS`] / [`is_supported_platform`], l'insieme **finito e
//!   chiuso** delle `Target_Platform` supportate dall'estrattore.
//!
//! _Requisiti: 8.1, 8.2._

use std::path::PathBuf;

// Riuso dei tipi condivisi della pipeline: NON ridefiniti qui (Req 8.2).
pub use crate::bindings::catalog::{CatalogEntry, OffsetRecord, ProvenanceRecord};
pub use crate::bindings::{
    GdVersion, Signature, SymbolId, TargetPair, TargetPlatform, SENTINEL_VALUE,
};

pub mod binary;
pub mod demangle;
pub mod elf;
pub mod elf_vtable;
pub mod matcher;
pub mod prologue;
pub mod provenance;
pub mod report;
pub mod rtti;
pub mod vtable;
pub mod writer;

/// Insieme **finito e chiuso** delle `Target_Platform` supportate
/// dall'estrattore: `{macos-arm64, macos-x64, windows-x64, android-arm64}`
/// (Req 8.1).
///
/// È un **sottoinsieme** dell'insieme `TargetPlatform::ALL` riusato dalla
/// pipeline (che include anche `ios-arm64`): l'estrattore non deriva binding per
/// piattaforme fuori da questo insieme (Req 8.3).
pub const SUPPORTED_PLATFORMS: [TargetPlatform; 4] = [
    TargetPlatform::MacosArm64,
    TargetPlatform::MacosX64,
    TargetPlatform::WindowsX64,
    TargetPlatform::AndroidArm64,
];

/// `true` se la `Target_Platform` appartiene all'insieme supportato
/// dall'estrattore (Req 8.1, 8.3).
pub fn is_supported_platform(platform: TargetPlatform) -> bool {
    SUPPORTED_PLATFORMS.contains(&platform)
}

// ---------------------------------------------------------------------------
// Architecture — CPU rilevante per il Prologue_Sanity_Check (Req 5, 8.1).
// ---------------------------------------------------------------------------

/// Architettura CPU di un `Source_Binary`, rilevante per il
/// `Prologue_Sanity_Check` (Req 5).
///
/// L'insieme è chiuso e mappa il `platform_id()` testuale della
/// `Target_Platform`: `macos-arm64`/`android-arm64` → [`Architecture::Arm64`];
/// `macos-x64`/`windows-x64` → [`Architecture::X86_64`] (Req 8.1).
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Architecture {
    Arm64,
    X86_64,
}

impl Architecture {
    /// Deriva l'[`Architecture`] dal `platform_id()` testuale di una
    /// `Target_Platform` dell'insieme supportato (Req 8.1).
    ///
    /// Restituisce `None` per le piattaforme fuori dall'insieme supportato
    /// dall'estrattore (es. `ios-arm64`), coerente con il fail-closed di
    /// Req 8.3.
    pub fn from_platform(platform: TargetPlatform) -> Option<Architecture> {
        match platform.platform_id() {
            "macos-arm64" | "android-arm64" => Some(Architecture::Arm64),
            "macos-x64" | "windows-x64" => Some(Architecture::X86_64),
            _ => None,
        }
    }

    /// Identificatore testuale stabile dell'architettura.
    pub fn arch_id(self) -> &'static str {
        match self {
            Architecture::Arm64 => "arm64",
            Architecture::X86_64 => "x86-64",
        }
    }
}

impl std::fmt::Display for Architecture {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.arch_id())
    }
}

// ---------------------------------------------------------------------------
// BinaryFormat — formato eseguibile atteso/rilevato (Req 2.7, 3.6).
// ---------------------------------------------------------------------------

/// Formato eseguibile di un `Source_Binary`.
///
/// `Elf` è l'`Android_Symbol_Source` (master di nomi/firme); `MachO` e `Pe`
/// sono le sorgenti RTTI/vtable per macOS e Windows.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum BinaryFormat {
    Elf,
    MachO,
    Pe,
}

impl BinaryFormat {
    /// Identificatore testuale stabile del formato.
    pub fn format_id(self) -> &'static str {
        match self {
            BinaryFormat::Elf => "elf",
            BinaryFormat::MachO => "mach-o",
            BinaryFormat::Pe => "pe",
        }
    }
}

impl std::fmt::Display for BinaryFormat {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.format_id())
    }
}

// ---------------------------------------------------------------------------
// ExtractError — errore fail-closed dell'estrattore (Req 8.3, 10.4, 11.4).
// ---------------------------------------------------------------------------

/// Errore del `Binding_Extractor`.
///
/// Tutte le varianti **falliscono in chiusura**: un percorso/formato/coppia non
/// valido non produce alcun output per quell'ingresso, e l'errore identifica
/// sempre l'ingresso coinvolto e la causa (Req 8.3, 10.4). Un errore su un
/// percorso **non** interrompe l'elaborazione degli altri percorsi forniti: il
/// chiamante raccoglie i `Result` per-percorso (Req 10.4).
///
/// Segue la convenzione `thiserror` del [`crate::bindings::catalog::CatalogError`].
#[derive(Debug, thiserror::Error)]
pub enum ExtractError {
    /// Il `Source_Binary` non è leggibile da disco (assente o non accessibile).
    #[error("Source_Binary illeggibile {path}: {source}")]
    Io {
        /// Percorso del file coinvolto.
        path: PathBuf,
        /// Causa di I/O sottostante.
        #[source]
        source: std::io::Error,
    },

    /// Il percorso fornito non è un percorso locale a un file regolare
    /// (es. uno schema di rete o una directory): l'estrattore legge **solo** da
    /// percorsi locali forniti dall'utente (Req 10.1, 10.2, 10.4).
    #[error("percorso del Source_Binary non locale o non valido {path}: {reason}")]
    NonLocalPath {
        /// Percorso del file coinvolto.
        path: PathBuf,
        /// Causa della non-località/non-validità del percorso.
        reason: String,
    },

    /// Il contenuto del file non è riconoscibile come un formato eseguibile
    /// supportato (ELF / Mach-O / PE).
    #[error("formato del Source_Binary non riconoscibile {path}: {reason}")]
    UnrecognizedFormat {
        /// Percorso del file coinvolto.
        path: PathBuf,
        /// Causa della mancata rilevazione del formato.
        reason: String,
    },

    /// Il formato rilevato non coincide con quello atteso per quell'ingresso
    /// (Req 2.7, 3.6).
    #[error("formato del Source_Binary non corrispondente {path}: atteso {expected}, trovato {found}")]
    FormatMismatch {
        /// Percorso del file coinvolto.
        path: PathBuf,
        /// Formato atteso dal chiamante.
        expected: BinaryFormat,
        /// Formato effettivamente rilevato.
        found: BinaryFormat,
    },

    /// Un `Source_Binary` già aperto non è (ri)leggibile come binario valido del
    /// formato atteso al momento del parsing di livello superiore — es. un ELF
    /// che non espone una tabella dei simboli analizzabile (Req 2.7, 3.6).
    ///
    /// A questo livello non disponiamo più del percorso originale (i byte sono
    /// già stati letti e il `SourceBinary` ne conserva solo la
    /// [`crate::extract::binary::BinaryIdentity`]); il binario è quindi
    /// identificato dalla sua identità tracciabile (piattaforma + prefisso
    /// dell'hash del contenuto), così l'errore resta riconducibile al file
    /// senza re-emetterne il contenuto (Req 10.3).
    #[error("Source_Binary {identity} non leggibile come {expected}: {reason}")]
    InvalidBinary {
        /// Identità tracciabile del binario (`<platform>:<hash-prefix>`).
        identity: String,
        /// Formato atteso per quel binario.
        expected: BinaryFormat,
        /// Causa della non leggibilità/validità.
        reason: String,
    },

    /// L'architettura del `Source_Binary` non è fra quelle supportate
    /// (arm64 / x86-64).
    #[error("architettura del Source_Binary non supportata {path}: {reason}")]
    UnsupportedArchitecture {
        /// Percorso del file coinvolto.
        path: PathBuf,
        /// Causa: architettura non riconosciuta o non supportata.
        reason: String,
    },

    /// La coppia `(GD_Version, Target_Platform)` ha una `Target_Platform` fuori
    /// dall'insieme supportato `{macos-arm64, macos-x64, windows-x64,
    /// android-arm64}` (Req 8.3).
    #[error("coppia (GD_Version, Target_Platform) non supportata {pair}: {reason}")]
    UnsupportedPair {
        /// Coppia fornita (rappresentazione testuale `"{version}/{platform}"`).
        pair: String,
        /// Causa della non-supportabilità della coppia.
        reason: String,
    },

    /// Conflitto di simbolo/firma per la stessa coppia: la `Catalog_Entry`
    /// esistente è lasciata invariata e l'estrazione fallisce (Req 11.4).
    #[error("conflitto per il simbolo {symbol} sulla coppia {pair}: {reason}")]
    Conflict {
        /// Simbolo in conflitto.
        symbol: String,
        /// Coppia coinvolta (rappresentazione testuale).
        pair: String,
        /// Natura del conflitto.
        reason: String,
    },

    /// Il file-stem derivato da un simbolo non produce un nome di file valido
    /// (vuoto o con separatori di percorso) oppure due simboli **distinti**
    /// collidono sullo stesso percorso su disco.
    ///
    /// Rilevato in **fase di pianificazione**, prima di qualunque scrittura: con
    /// la sanitizzazione di [`crate::bindings::contribution::symbol_file_stem`]
    /// nessuno stem dovrebbe risultare invalido, ma la validazione garantisce
    /// che un eventuale caso patologico futuro **fallisca in chiusura** invece di
    /// produrre una scrittura parziale a metà commit.
    #[error("file-stem non valido per il simbolo {symbol}: {reason}")]
    InvalidStem {
        /// Simbolo il cui stem non è utilizzabile come nome di file.
        symbol: String,
        /// Causa: stem vuoto, con separatori, o collisione di percorso.
        reason: String,
    },

    /// L'ingresso `Geode_Reference` è stato **rifiutato dal `Geode_Firewall`
    /// riusato** della pipeline (`cli/src/bindings/crosscheck.rs`): contiene
    /// sorgente/header/struttura, è malformato, non numerico o con chiavi
    /// duplicate (Req 1.4, 1.5). L'errore sottostante identifica già l'ingresso
    /// e la natura del contenuto vietato; lo propaghiamo invariato per non
    /// duplicare la diagnostica del firewall.
    ///
    /// Nessun **secondo** firewall è introdotto qui: l'estrattore instrada
    /// l'ingresso Geode attraverso l'**unico** firewall numerico-only esistente
    /// e si limita a riconfezionarne l'errore nel proprio tipo fail-closed.
    #[error("ingresso Geode_Reference rifiutato dal Geode_Firewall: {source}")]
    GeodeFirewall {
        /// Errore originale del `Geode_Firewall` riusato (identifica ingresso +
        /// contenuto vietato).
        #[source]
        source: crate::bindings::crosscheck::FirewallError,
    },
}
