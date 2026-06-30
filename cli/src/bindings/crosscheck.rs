//! Observational_Cross_Check + **Geode_Firewall** (Req 4).
//!
//! Questo modulo ospita il confronto **osservativo** tra un indirizzo candidato
//! e il `Geode_Reference` (i valori numerici delle community bindings Geode,
//! campi `m1`/`imac`) per la stessa coppia `(GD_Version, Target_Platform)`. La
//! regola cardine della pipeline è **nessun riuso di codice Geode**: il
//! `Geode_Reference` è trattato **esclusivamente** come dato numerico di
//! indirizzo (Req 4.2).
//!
//! Il modulo implementa due responsabilità complementari:
//!
//! 1. il **firewall/loader** — [`load_geode_reference`] e i suoi tipi
//!    [`GeodeReferenceTable`] / [`FirewallError`] — che ingerisce la tabella
//!    Geode **solo** come dati numerici;
//! 2. il **confronto osservativo** — l'`enum` [`CrossCheck`] e [`cross_check`]
//!    — che esegue l'uguaglianza numerica **bit-per-bit** (Req 4.1) e indica
//!    come l'esito si riflette in provenienza (Req 4.3, 4.4, 4.6).
//!
//! ## Il Geode_Firewall (Req 4.2, 4.5)
//!
//! [`load_geode_reference`] è un **parser restrittivo** che accetta **solo**
//! righe `chiave = numero` (esadecimale `0x…` o decimale) provenienti da
//! `mod-index/catalog/geode-reference/{version}/{platform}.toml`. Se rileva un
//! qualunque token che indica **sorgente, header o struttura dati** — `#include`,
//! `struct`, `class`, `{`/`}`, il `;` di codice, oppure le estensioni `.hpp`/
//! `.cpp` — **rifiuta l'intero file senza incorporarne alcuna parte** (fail-
//! closed) e restituisce [`FirewallError::SourceViolation`].
//!
//! Il firewall non è un disclaimer: è un parser che **fallisce in chiusura** su
//! input non conforme. La sua unica responsabilità in questo task è **rifiutare
//! e restituire un errore**; l'effetto "imposta `Verified_Flag = false`
//! sull'offset associato e registra «rifiutato per violazione» nel
//! `Provenance_Record`" (Req 4.5) è realizzato dal livello di integrazione
//! cross-check/provenienza nei task successivi, che reagisce a questo errore.
//! Mantenendo qui il contratto pulito, il rifiuto è totale: nessuna riga del
//! file viene mai incorporata se il file contiene una violazione.
//!
//! _Requisiti: 4.2, 4.5._

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use super::provenance::CrossCheckOutcome;
use super::{GdVersion, TargetPair, TargetPlatform};

// ---------------------------------------------------------------------------
// Token proibiti — la "firma" di sorgente/header/struttura Geode (Req 4.5).
// ---------------------------------------------------------------------------

/// Token testuali che indicano **sorgente/header/struttura** e la cui presenza
/// fa rifiutare l'intero file (fail-closed, Req 4.5).
///
/// Sono confrontati come sottostringhe **case-sensitive**: un file puramente
/// numerico di indirizzi non li contiene mai, mentre sorgente, header o
/// definizioni di struttura dati Geode sì.
const SOURCE_TOKENS: &[&str] = &["#include", "struct", "class", ".hpp", ".cpp"];

/// Caratteri singoli che indicano **codice/struttura** (parentesi graffe e il
/// `;` di codice) e la cui presenza fa rifiutare l'intero file (Req 4.5).
const SOURCE_CHARS: &[char] = &['{', '}', ';'];

// ---------------------------------------------------------------------------
// GeodeReferenceTable — dato numerico-only (Req 4.2).
// ---------------------------------------------------------------------------

/// Tabella di riferimento Geode caricata in memoria: una mappa
/// `chiave (simbolo) → indirizzo numerico` per una coppia
/// `(GD_Version, Target_Platform)`.
///
/// Per costruzione **contiene solo dati numerici** (Req 4.2): è prodotta
/// esclusivamente da [`load_geode_reference`], che rifiuta qualunque contenuto
/// di sorgente/header/struttura. Gli indirizzi sono `u64`, coerenti con gli RVA
/// del catalogo, e il confronto del task successivo è di **uguaglianza numerica
/// esatta** (Req 4.1).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct GeodeReferenceTable {
    /// Coppia `(GD_Version, Target_Platform)` derivata dal percorso del file, se
    /// interpretabile (`mod-index/catalog/geode-reference/{version}/{platform}.toml`).
    /// È solo contesto: il firewall non la richiede per rifiutare contenuto.
    pub pair: Option<TargetPair>,
    /// Mappa ordinata `chiave → indirizzo numerico`. Ordinata per chiave per un
    /// comportamento deterministico (iterazione/diff stabili).
    entries: BTreeMap<String, u64>,
}

impl GeodeReferenceTable {
    /// Crea una tabella vuota associata (opzionalmente) a una coppia.
    pub fn new(pair: Option<TargetPair>) -> Self {
        Self {
            pair,
            entries: BTreeMap::new(),
        }
    }

    /// Indirizzo numerico associato a `key`, se presente.
    pub fn get(&self, key: &str) -> Option<u64> {
        self.entries.get(key).copied()
    }

    /// `true` se la tabella contiene `key`.
    pub fn contains_key(&self, key: &str) -> bool {
        self.entries.contains_key(key)
    }

    /// Numero di voci `(chiave, indirizzo)`.
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    /// `true` se la tabella non contiene alcuna voce.
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    /// Itera le voci `(chiave, indirizzo)` in ordine di chiave.
    pub fn iter(&self) -> impl Iterator<Item = (&str, u64)> {
        self.entries.iter().map(|(k, v)| (k.as_str(), *v))
    }
}

// ---------------------------------------------------------------------------
// FirewallError — rifiuti fail-closed del Geode_Firewall.
// ---------------------------------------------------------------------------

/// Errore del `Geode_Firewall`: ogni variante **rifiuta l'intero file senza
/// incorporarne alcuna parte** (fail-closed, Req 4.5).
#[derive(Debug, thiserror::Error)]
pub enum FirewallError {
    /// Il file di riferimento non è leggibile da disco.
    #[error("file Geode_Reference illeggibile {path}: {source}")]
    Io {
        /// Percorso del file coinvolto.
        path: PathBuf,
        /// Causa di I/O sottostante.
        #[source]
        source: std::io::Error,
    },

    /// **Violazione del firewall**: rilevato un token di sorgente/header/
    /// struttura. L'intero file è rifiutato; nessuna parte viene incorporata
    /// (Req 4.5). Il livello cross-check/provenienza reagisce impostando
    /// `Verified_Flag = false` e registrando «rifiutato per violazione».
    #[error(
        "violazione del firewall Geode in {path}:{line}: rilevato token di sorgente/header/struttura {token:?} (riga: {snippet:?}); \
         l'intero file è rifiutato senza incorporarne alcuna parte"
    )]
    SourceViolation {
        /// Percorso del file coinvolto.
        path: PathBuf,
        /// Numero di riga (1-based) della violazione.
        line: usize,
        /// Token proibito rilevato.
        token: String,
        /// Contenuto (trim) della riga incriminata, a scopo diagnostico.
        snippet: String,
    },

    /// Una riga non vuota e non-commento non è nella forma `chiave = valore`.
    #[error("riga non conforme in {path}:{line}: atteso \"chiave = numero\", trovato {content:?}")]
    MalformedLine {
        /// Percorso del file coinvolto.
        path: PathBuf,
        /// Numero di riga (1-based).
        line: usize,
        /// Contenuto della riga.
        content: String,
    },

    /// Il valore associato a una chiave non è un numero (`0x…` o decimale).
    /// Il `Geode_Reference` è solo dato numerico: ogni valore non numerico è
    /// rifiutato (Req 4.2).
    #[error("valore non numerico in {path}:{line}: chiave {key:?} ha valore non numerico {value:?}")]
    NonNumeric {
        /// Percorso del file coinvolto.
        path: PathBuf,
        /// Numero di riga (1-based).
        line: usize,
        /// Chiave coinvolta.
        key: String,
        /// Valore grezzo non interpretabile come numero.
        value: String,
    },

    /// La stessa chiave compare più di una volta nel file.
    #[error("chiave duplicata in {path}:{line}: {key:?}")]
    DuplicateKey {
        /// Percorso del file coinvolto.
        path: PathBuf,
        /// Numero di riga (1-based) della seconda occorrenza.
        line: usize,
        /// Chiave duplicata.
        key: String,
    },
}

// ---------------------------------------------------------------------------
// load_geode_reference — il Geode_Firewall.
// ---------------------------------------------------------------------------

/// Carica e analizza una `Geode_Reference_Table` da disco, applicando il
/// **Geode_Firewall** (Req 4.2, 4.5).
///
/// Il percorso atteso è
/// `mod-index/catalog/geode-reference/{version}/{platform}.toml`; la coppia è
/// derivata dal percorso quando interpretabile (vedi
/// [`GeodeReferenceTable::pair`]).
///
/// Restituisce [`FirewallError::SourceViolation`] — **senza incorporare alcuna
/// parte del file** — se il contenuto include token di sorgente/header/
/// struttura (`#include`, `struct`, `class`, `{`/`}`, `;`, `.hpp`, `.cpp`).
pub fn load_geode_reference(path: &Path) -> Result<GeodeReferenceTable, FirewallError> {
    let content = std::fs::read_to_string(path).map_err(|source| FirewallError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    parse_geode_reference(&content, path)
}

/// Variante host-testabile di [`load_geode_reference`] che opera su contenuto in
/// memoria. `path` è usato solo per derivare la coppia e contestualizzare gli
/// errori (non viene letto).
///
/// L'ordine è **fail-closed**: prima il firewall scandisce l'**intero**
/// contenuto e, alla prima violazione, rifiuta senza costruire alcuna tabella;
/// solo se il file è integralmente pulito si procede al parsing numerico.
pub fn parse_geode_reference(
    content: &str,
    path: &Path,
) -> Result<GeodeReferenceTable, FirewallError> {
    // Fase 1 — Firewall: scan dell'intero file. Una sola violazione rifiuta
    // tutto, senza incorporare alcuna riga (Req 4.5).
    scan_for_source_tokens(content, path)?;

    // Fase 2 — Parsing numerico restrittivo: solo `chiave = numero` (Req 4.2).
    let pair = pair_from_path(path);
    let mut entries: BTreeMap<String, u64> = BTreeMap::new();

    for (idx, raw_line) in content.lines().enumerate() {
        let line_no = idx + 1;
        let line = strip_comment(raw_line).trim();
        if line.is_empty() {
            continue;
        }

        let (raw_key, raw_value) = line.split_once('=').ok_or_else(|| FirewallError::MalformedLine {
            path: path.to_path_buf(),
            line: line_no,
            content: line.to_owned(),
        })?;

        let key = unquote(raw_key.trim());
        let value = raw_value.trim();

        if key.is_empty() {
            return Err(FirewallError::MalformedLine {
                path: path.to_path_buf(),
                line: line_no,
                content: line.to_owned(),
            });
        }

        let address = parse_numeric(value).ok_or_else(|| FirewallError::NonNumeric {
            path: path.to_path_buf(),
            line: line_no,
            key: key.clone(),
            value: value.to_owned(),
        })?;

        if entries.insert(key.clone(), address).is_some() {
            return Err(FirewallError::DuplicateKey {
                path: path.to_path_buf(),
                line: line_no,
                key,
            });
        }
    }

    Ok(GeodeReferenceTable { pair, entries })
}

// ---------------------------------------------------------------------------
// Helper interni.
// ---------------------------------------------------------------------------

/// Scandisce l'intero contenuto alla ricerca di token di sorgente/header/
/// struttura. Alla **prima** violazione restituisce
/// [`FirewallError::SourceViolation`] (fail-closed, Req 4.5).
fn scan_for_source_tokens(content: &str, path: &Path) -> Result<(), FirewallError> {
    for (idx, line) in content.lines().enumerate() {
        let line_no = idx + 1;

        for token in SOURCE_TOKENS {
            if line.contains(token) {
                return Err(FirewallError::SourceViolation {
                    path: path.to_path_buf(),
                    line: line_no,
                    token: (*token).to_owned(),
                    snippet: line.trim().to_owned(),
                });
            }
        }

        for ch in SOURCE_CHARS {
            if line.contains(*ch) {
                return Err(FirewallError::SourceViolation {
                    path: path.to_path_buf(),
                    line: line_no,
                    token: ch.to_string(),
                    snippet: line.trim().to_owned(),
                });
            }
        }
    }
    Ok(())
}

/// Rimuove un eventuale commento `#…` dalla riga (solo dato numerico resta).
///
/// La scansione del firewall ha già rifiutato `#include`; qui un `#` residuo è
/// un commento legittimo e tutto ciò che lo segue viene ignorato.
fn strip_comment(line: &str) -> &str {
    match line.find('#') {
        Some(idx) => &line[..idx],
        None => line,
    }
}

/// Rimuove eventuali apici/virgolette di delimitazione da una chiave.
///
/// Le chiavi-simbolo contengono `::` e non sono bare-key TOML valide, quindi
/// possono comparire fra virgolette (`"MenuLayer::init"`); qui le normalizziamo.
fn unquote(value: &str) -> String {
    let bytes = value.as_bytes();
    if bytes.len() >= 2 {
        let first = bytes[0];
        let last = bytes[bytes.len() - 1];
        if (first == b'"' && last == b'"') || (first == b'\'' && last == b'\'') {
            return value[1..value.len() - 1].to_owned();
        }
    }
    value.to_owned()
}

/// Interpreta un valore come numero `u64`: esadecimale `0x…`/`0X…` oppure
/// decimale. Restituisce `None` su qualunque contenuto non numerico.
fn parse_numeric(value: &str) -> Option<u64> {
    if value.is_empty() {
        return None;
    }
    if let Some(hex) = value.strip_prefix("0x").or_else(|| value.strip_prefix("0X")) {
        u64::from_str_radix(hex, 16).ok()
    } else {
        value.parse::<u64>().ok()
    }
}

/// Deriva la coppia `(GD_Version, Target_Platform)` dal percorso
/// `…/geode-reference/{version}/{platform}.toml`, se interpretabile.
fn pair_from_path(path: &Path) -> Option<TargetPair> {
    let platform_id = path.file_stem()?.to_str()?;
    let platform = TargetPlatform::from_platform_id(platform_id)?;

    let version_str = path.parent()?.file_name()?.to_str()?;
    let gd = parse_gd_version_lenient(version_str)?;

    Some(TargetPair::new(gd, platform))
}

/// Interpreta `"<major>.<minor>"` come [`GdVersion`]; `None` se non conforme.
fn parse_gd_version_lenient(value: &str) -> Option<GdVersion> {
    let (major, minor) = value.split_once('.')?;
    Some(GdVersion::new(major.parse().ok()?, minor.parse().ok()?))
}

// ---------------------------------------------------------------------------
// CrossCheck — esito del confronto osservativo numerico (Req 4.1, 4.3, 4.4).
// ---------------------------------------------------------------------------

/// Esito di dominio dell'`Observational_Cross_Check`: il confronto **numerico
/// esatto bit-per-bit** tra l'indirizzo candidato e il `Geode_Reference` per la
/// stessa coppia (Req 4.1).
///
/// ## Relazione con [`CrossCheckOutcome`] della provenienza
///
/// Esiste un secondo tipo, [`super::provenance::CrossCheckOutcome`], con le
/// **stesse tre varianti**: non è un duplicato accidentale ma una separazione di
/// responsabilità deliberata.
///
/// - [`CrossCheck`] (qui) è l'**esito di dominio** restituito dall'operazione di
///   cross-check vera e propria ([`cross_check`]): è ciò che la logica della
///   pipeline produce e su cui ragiona (es. "devo forzare `Verified_Flag` a
///   `false`?").
/// - [`CrossCheckOutcome`] (in [`super::provenance`]) è l'esito **registrabile**
///   nel `Provenance_Record`, mappato alle stringhe canoniche del TOML del
///   catalogo (`"concordant"`/`"discordant"`/`"rejected"`).
///
/// I due tipi sono tenuti **coerenti** da una conversione 1-a-1 bidirezionale
/// ([`From`] in entrambe le direzioni) e da [`CrossCheck::to_outcome`]. Così
/// l'operazione di cross-check parla in termini di dominio e la registrazione in
/// provenienza riusa la mappatura canonica esistente, senza divergenze.
///
/// ## Mappatura verso la provenienza (Req 4.3, 4.4, 4.6)
///
/// - [`CrossCheck::Concordant`] → si registra `cross_check = "concordant"`
///   (Req 4.3); il `Verified_Flag` **non** è forzato a `false` dal cross-check
///   (resta soggetto al gate del prologo).
/// - [`CrossCheck::Discordant`] → si registra `cross_check = "discordant"` e si
///   forza `Verified_Flag = false` sull'offset (Req 4.4).
/// - [`CrossCheck::Rejected`] → violazione del `Geode_Firewall`: si registra
///   `cross_check = "rejected"` ("rifiutato per violazione") e si forza
///   `Verified_Flag = false` (Req 4.5). Questa variante **non** è mai prodotta da
///   [`cross_check`] (che opera su una tabella già accettata dal firewall): è il
///   livello di integrazione a costruirla quando [`load_geode_reference`]
///   fallisce con [`FirewallError`].
///
/// In **ogni** caso la provenienza documenta `cross_check_no_reuse = true`
/// ("solo riferimento numerico osservativo", Req 4.6): vedi
/// [`CrossCheck::no_reuse`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CrossCheck {
    /// Candidato e `Geode_Reference` numericamente **uguali** bit-per-bit
    /// (Req 4.1, 4.3).
    Concordant,
    /// Candidato e `Geode_Reference` numericamente **diversi** — oppure nessun
    /// riferimento confrontabile per la coppia (fail-closed). Forza
    /// `Verified_Flag = false` (Req 4.4).
    Discordant,
    /// Contenuto Geode **rifiutato** dal firewall per violazione (Req 4.5).
    Rejected,
}

impl CrossCheck {
    /// `true` se l'esito **forza** `Verified_Flag = false` sull'offset.
    ///
    /// Vale per [`CrossCheck::Discordant`] (Req 4.4) e [`CrossCheck::Rejected`]
    /// (Req 4.5). Un esito [`CrossCheck::Concordant`] **non** forza il flag a
    /// `false`: è un segnale positivo, ma la verifica resta subordinata al gate
    /// della `Prologue_Verification`.
    pub fn forces_verified_false(self) -> bool {
        matches!(self, CrossCheck::Discordant | CrossCheck::Rejected)
    }

    /// `true` solo per [`CrossCheck::Concordant`].
    pub fn is_concordant(self) -> bool {
        matches!(self, CrossCheck::Concordant)
    }

    /// Valore del flag `cross_check_no_reuse` da registrare in provenienza.
    ///
    /// È **sempre** `true` (Req 4.6): il `Geode_Reference` è usato come solo
    /// riferimento numerico osservativo, senza alcun riuso di codice, qualunque
    /// sia l'esito del confronto.
    pub fn no_reuse(self) -> bool {
        true
    }

    /// Converte l'esito di dominio nell'esito **registrabile** in provenienza
    /// ([`CrossCheckOutcome`]), preservando la corrispondenza 1-a-1.
    pub fn to_outcome(self) -> CrossCheckOutcome {
        CrossCheckOutcome::from(self)
    }
}

impl From<CrossCheck> for CrossCheckOutcome {
    fn from(value: CrossCheck) -> Self {
        match value {
            CrossCheck::Concordant => CrossCheckOutcome::Concordant,
            CrossCheck::Discordant => CrossCheckOutcome::Discordant,
            CrossCheck::Rejected => CrossCheckOutcome::Rejected,
        }
    }
}

impl From<CrossCheckOutcome> for CrossCheck {
    fn from(value: CrossCheckOutcome) -> Self {
        match value {
            CrossCheckOutcome::Concordant => CrossCheck::Concordant,
            CrossCheckOutcome::Discordant => CrossCheck::Discordant,
            CrossCheckOutcome::Rejected => CrossCheck::Rejected,
        }
    }
}

// ---------------------------------------------------------------------------
// cross_check — uguaglianza numerica esatta bit-per-bit (Req 4.1).
// ---------------------------------------------------------------------------

/// Confronto **numerico esatto bit-per-bit** tra due valori `u64` (Req 4.1).
///
/// Restituisce [`CrossCheck::Concordant`] **se e solo se** i due valori sono
/// identici bit-per-bit, altrimenti [`CrossCheck::Discordant`]. Non esiste alcun
/// trattamento speciale: `0` e il `Sentinel_Value` (`u64::MAX`) sono confrontati
/// per uguaglianza esatta come qualunque altro valore (concordi solo con loro
/// stessi).
///
/// È il **nucleo puro** del cross-check (Property 9), privo di qualsiasi
/// dipendenza dalla tabella o dalla coppia: [`cross_check`] vi si appoggia dopo
/// aver risolto il valore di riferimento.
pub fn cross_check_value(candidate_rva: u64, reference_value: u64) -> CrossCheck {
    if candidate_rva == reference_value {
        CrossCheck::Concordant
    } else {
        CrossCheck::Discordant
    }
}

/// Esegue l'`Observational_Cross_Check` dell'indirizzo candidato `candidate_rva`
/// contro il `Geode_Reference` del simbolo `symbol` nella tabella `reference`,
/// per la coppia `pair` (Req 4.1).
///
/// ## Nota sulla firma rispetto al design
///
/// La firma "abbreviata" del design è
/// `cross_check(candidate_rva, reference, pair) -> CrossCheck`. Poiché la
/// [`GeodeReferenceTable`] è indicizzata **per simbolo** (`chiave → indirizzo`),
/// il valore di riferimento da confrontare non è determinabile senza il simbolo:
/// la firma reale aggiunge perciò il parametro `symbol`. Il parametro `pair`
/// resta per **confermare** che la tabella sia quella della coppia attesa
/// (vedi sotto), coerentemente con l'intento del design "per la stessa coppia".
///
/// ## Semantica fail-closed
///
/// Il confronto è di pura uguaglianza esatta (Req 4.1), ma l'assenza di un
/// riferimento confrontabile **non** deve mai produrre un falso "concorde":
///
/// - se `reference.pair` è noto e **diverso** da `pair`, la tabella non è quella
///   della coppia richiesta → [`CrossCheck::Discordant`] (fail-closed, nessun
///   confronto cross-coppia);
/// - se la tabella **non contiene** `symbol`, non esiste un riferimento
///   numerico per quella coppia → [`CrossCheck::Discordant`] (fail-closed: un
///   riferimento mancante non concede mai la concordanza);
/// - altrimenti si delega a [`cross_check_value`] per l'uguaglianza esatta.
///
/// L'esito non viene **mai** [`CrossCheck::Rejected`] qui: il rifiuto per
/// violazione del firewall avviene a monte, in [`load_geode_reference`], che in
/// quel caso non produce affatto una tabella.
pub fn cross_check(
    candidate_rva: u64,
    reference: &GeodeReferenceTable,
    pair: TargetPair,
    symbol: &str,
) -> CrossCheck {
    // Fail-closed: una tabella di un'altra coppia non è confrontabile.
    if let Some(table_pair) = reference.pair {
        if table_pair != pair {
            return CrossCheck::Discordant;
        }
    }

    // Fail-closed: nessun riferimento numerico per il simbolo → mai concorde.
    match reference.get(symbol) {
        Some(reference_value) => cross_check_value(candidate_rva, reference_value),
        None => CrossCheck::Discordant,
    }
}


#[cfg(test)]
mod tests {
    use super::*;
    use crate::bindings::SENTINEL_VALUE;
    use std::path::Path;

    /// Percorso canonico tipico: `…/geode-reference/2.2081/macos-arm64.toml`.
    fn ref_path() -> PathBuf {
        Path::new("mod-index/catalog/geode-reference/2.2081/macos-arm64.toml").to_path_buf()
    }

    // -------------------------------------------------------------------
    // Accetta una tabella numerico-only pulita (Req 4.2).
    // -------------------------------------------------------------------

    #[test]
    fn accepts_clean_numeric_only_table() {
        let content = r#"
# Geode_Reference_Table — solo indirizzi numerici osservativi (nessun riuso di codice)
"MenuLayer::init"     = 0x316688
"PlayLayer::update"   = 0x2A1B40
"GameManager::sharedState" = 1234567
"#;
        let table = parse_geode_reference(content, &ref_path()).expect("tabella pulita accettata");

        assert_eq!(table.len(), 3);
        assert_eq!(table.get("MenuLayer::init"), Some(0x316688));
        assert_eq!(table.get("PlayLayer::update"), Some(0x2A1B40));
        assert_eq!(table.get("GameManager::sharedState"), Some(1234567));
        assert!(table.contains_key("MenuLayer::init"));
        assert_eq!(table.get("Nonexistent::symbol"), None);
    }

    #[test]
    fn parses_both_hex_and_decimal_values() {
        let content = r#"
"a::b" = 0xFF
"c::d" = 255
"#;
        let table = parse_geode_reference(content, &ref_path()).unwrap();
        // Stesso valore, espresso esadecimale e decimale.
        assert_eq!(table.get("a::b"), Some(255));
        assert_eq!(table.get("c::d"), Some(255));
    }

    #[test]
    fn derives_pair_from_path() {
        let content = "\"x::y\" = 0x10\n";
        let table = parse_geode_reference(content, &ref_path()).unwrap();
        let pair = table.pair.expect("coppia derivata dal percorso");
        assert_eq!(pair.gd, GdVersion::new(2, 2081));
        assert_eq!(pair.platform, TargetPlatform::MacosArm64);
    }

    #[test]
    fn empty_file_yields_empty_table() {
        let table = parse_geode_reference("\n\n   \n", &ref_path()).unwrap();
        assert!(table.is_empty());
        assert_eq!(table.len(), 0);
    }

    #[test]
    fn accepts_uppercase_hex_prefix() {
        let table = parse_geode_reference("\"k\" = 0X1A2B\n", &ref_path()).unwrap();
        assert_eq!(table.get("k"), Some(0x1A2B));
    }

    // -------------------------------------------------------------------
    // Firewall: rifiuta payload con token di sorgente/header/struttura,
    // SENZA incorporarne alcuna parte (Req 4.5).
    // -------------------------------------------------------------------

    #[test]
    fn rejects_include_directive() {
        let content = r#"
"MenuLayer::init" = 0x316688
#include <Geode/modify/MenuLayer.hpp>
"#;
        let err = parse_geode_reference(content, &ref_path()).unwrap_err();
        assert!(matches!(err, FirewallError::SourceViolation { .. }));
    }

    #[test]
    fn rejects_struct_keyword() {
        let content = r#"
"MenuLayer::init" = 0x316688
struct MenuLayer { int x; };
"#;
        let err = parse_geode_reference(content, &ref_path()).unwrap_err();
        assert!(matches!(err, FirewallError::SourceViolation { token, .. } if token == "struct"));
    }

    #[test]
    fn rejects_class_keyword() {
        let content = "class MenuLayer\n\"MenuLayer::init\" = 0x316688\n";
        let err = parse_geode_reference(content, &ref_path()).unwrap_err();
        assert!(matches!(err, FirewallError::SourceViolation { token, .. } if token == "class"));
    }

    #[test]
    fn rejects_opening_brace() {
        let content = "\"MenuLayer::init\" = 0x316688\nfoo { }\n";
        let err = parse_geode_reference(content, &ref_path()).unwrap_err();
        assert!(matches!(err, FirewallError::SourceViolation { .. }));
    }

    #[test]
    fn rejects_code_style_semicolon() {
        let content = "\"MenuLayer::init\" = 0x316688;\n";
        let err = parse_geode_reference(content, &ref_path()).unwrap_err();
        assert!(matches!(err, FirewallError::SourceViolation { token, .. } if token == ";"));
    }

    #[test]
    fn rejects_hpp_extension() {
        let content = "// from MenuLayer.hpp\n\"MenuLayer::init\" = 0x316688\n";
        let err = parse_geode_reference(content, &ref_path()).unwrap_err();
        assert!(matches!(err, FirewallError::SourceViolation { token, .. } if token == ".hpp"));
    }

    #[test]
    fn rejects_cpp_extension() {
        let content = "// from MenuLayer.cpp\n\"MenuLayer::init\" = 0x316688\n";
        let err = parse_geode_reference(content, &ref_path()).unwrap_err();
        assert!(matches!(err, FirewallError::SourceViolation { token, .. } if token == ".cpp"));
    }

    #[test]
    fn rejects_entire_file_without_partial_incorporation() {
        // Una sola riga di sorgente in mezzo a righe numeriche valide deve far
        // rifiutare l'INTERO file: nessuna parte viene incorporata (Req 4.5).
        let content = r#"
"A::a" = 0x1
"B::b" = 0x2
struct Sneaky { int field; };
"C::c" = 0x3
"#;
        let result = parse_geode_reference(content, &ref_path());
        // Il risultato è un errore: nessuna GeodeReferenceTable viene prodotta,
        // quindi nessuna delle righe numeriche valide è incorporata.
        assert!(result.is_err());
        match result {
            Err(FirewallError::SourceViolation { line, .. }) => {
                // La violazione è alla riga della struct (riga 4, 1-based).
                assert_eq!(line, 4);
            }
            other => panic!("attesa SourceViolation, trovato {other:?}"),
        }
    }

    // -------------------------------------------------------------------
    // Restrizione numerica: rifiuta righe non conformi / valori non numerici.
    // -------------------------------------------------------------------

    #[test]
    fn rejects_line_without_assignment() {
        let content = "\"MenuLayer::init\" 0x316688\n";
        let err = parse_geode_reference(content, &ref_path()).unwrap_err();
        assert!(matches!(err, FirewallError::MalformedLine { .. }));
    }

    #[test]
    fn rejects_non_numeric_value() {
        let content = "\"MenuLayer::init\" = not_a_number\n";
        let err = parse_geode_reference(content, &ref_path()).unwrap_err();
        assert!(matches!(err, FirewallError::NonNumeric { .. }));
    }

    #[test]
    fn rejects_duplicate_key() {
        let content = "\"dup\" = 0x1\n\"dup\" = 0x2\n";
        let err = parse_geode_reference(content, &ref_path()).unwrap_err();
        assert!(matches!(err, FirewallError::DuplicateKey { .. }));
    }

    #[test]
    fn rejects_empty_key() {
        let content = " = 0x10\n";
        let err = parse_geode_reference(content, &ref_path()).unwrap_err();
        assert!(matches!(err, FirewallError::MalformedLine { .. }));
    }

    #[test]
    fn comments_and_blank_lines_are_ignored() {
        let content = r#"
# questo è un commento
   # commento indentato

"only::entry" = 0x42
"#;
        let table = parse_geode_reference(content, &ref_path()).unwrap();
        assert_eq!(table.len(), 1);
        assert_eq!(table.get("only::entry"), Some(0x42));
    }

    #[test]
    fn iter_yields_entries_sorted_by_key() {
        let content = r#"
"z::z" = 0x3
"a::a" = 0x1
"m::m" = 0x2
"#;
        let table = parse_geode_reference(content, &ref_path()).unwrap();
        let keys: Vec<&str> = table.iter().map(|(k, _)| k).collect();
        assert_eq!(keys, vec!["a::a", "m::m", "z::z"]);
    }

    // -------------------------------------------------------------------
    // cross_check — uguaglianza numerica esatta bit-per-bit (Req 4.1).
    // -------------------------------------------------------------------

    fn pair_arm() -> TargetPair {
        TargetPair::new(GdVersion::new(2, 2081), TargetPlatform::MacosArm64)
    }

    fn pair_x64() -> TargetPair {
        TargetPair::new(GdVersion::new(2, 2081), TargetPlatform::MacosX64)
    }

    /// Tabella di riferimento per arm64 con alcune voci note.
    fn ref_table_arm() -> GeodeReferenceTable {
        parse_geode_reference(
            "\"MenuLayer::init\" = 0x316688\n\"Zero::sym\" = 0\n\"Sentinel::sym\" = 18446744073709551615\n",
            &ref_path(),
        )
        .unwrap()
    }

    #[test]
    fn cross_check_value_concordant_only_on_exact_equality() {
        assert_eq!(cross_check_value(0x316688, 0x316688), CrossCheck::Concordant);
        assert_eq!(cross_check_value(0x316688, 0x316689), CrossCheck::Discordant);
        // Differenza di un solo bit → discorde.
        assert_eq!(cross_check_value(0b1010, 0b1011), CrossCheck::Discordant);
    }

    #[test]
    fn cross_check_value_zero_and_sentinel_compared_by_exact_equality() {
        // 0 concorde solo con 0.
        assert_eq!(cross_check_value(0, 0), CrossCheck::Concordant);
        assert_eq!(cross_check_value(0, 1), CrossCheck::Discordant);
        // Sentinel (u64::MAX) concorde solo con sé stesso.
        assert_eq!(
            cross_check_value(SENTINEL_VALUE, SENTINEL_VALUE),
            CrossCheck::Concordant
        );
        assert_eq!(
            cross_check_value(SENTINEL_VALUE, SENTINEL_VALUE - 1),
            CrossCheck::Discordant
        );
        // 0 vs sentinel → discorde (estremi opposti).
        assert_eq!(cross_check_value(0, SENTINEL_VALUE), CrossCheck::Discordant);
    }

    #[test]
    fn cross_check_concordant_when_candidate_matches_reference() {
        let table = ref_table_arm();
        assert_eq!(
            cross_check(0x316688, &table, pair_arm(), "MenuLayer::init"),
            CrossCheck::Concordant
        );
    }

    #[test]
    fn cross_check_discordant_when_candidate_differs() {
        let table = ref_table_arm();
        assert_eq!(
            cross_check(0x316689, &table, pair_arm(), "MenuLayer::init"),
            CrossCheck::Discordant
        );
    }

    #[test]
    fn cross_check_zero_and_sentinel_entries_use_exact_equality() {
        let table = ref_table_arm();
        // Entry il cui riferimento è 0.
        assert_eq!(
            cross_check(0, &table, pair_arm(), "Zero::sym"),
            CrossCheck::Concordant
        );
        assert_eq!(
            cross_check(1, &table, pair_arm(), "Zero::sym"),
            CrossCheck::Discordant
        );
        // Entry il cui riferimento è il Sentinel_Value.
        assert_eq!(
            cross_check(SENTINEL_VALUE, &table, pair_arm(), "Sentinel::sym"),
            CrossCheck::Concordant
        );
        assert_eq!(
            cross_check(0x316688, &table, pair_arm(), "Sentinel::sym"),
            CrossCheck::Discordant
        );
    }

    #[test]
    fn cross_check_missing_reference_is_discordant_fail_closed() {
        let table = ref_table_arm();
        // Nessun riferimento per il simbolo → mai concorde (fail-closed).
        assert_eq!(
            cross_check(0x316688, &table, pair_arm(), "Unknown::symbol"),
            CrossCheck::Discordant
        );
    }

    #[test]
    fn cross_check_mismatched_pair_is_discordant_fail_closed() {
        // Tabella derivata per arm64, ma interrogata per x64 → discorde anche se
        // il valore numerico coinciderebbe (nessun confronto cross-coppia).
        let table = ref_table_arm();
        assert_eq!(table.pair, Some(pair_arm()));
        assert_eq!(
            cross_check(0x316688, &table, pair_x64(), "MenuLayer::init"),
            CrossCheck::Discordant
        );
    }

    #[test]
    fn cross_check_without_pair_context_still_compares_by_symbol() {
        // Tabella senza coppia nota (path non interpretabile): il confronto resta
        // possibile per simbolo, senza il controllo di coppia.
        let table = parse_geode_reference(
            "\"MenuLayer::init\" = 0x316688\n",
            Path::new("not-a-canonical-path.toml"),
        )
        .unwrap();
        assert_eq!(table.pair, None);
        assert_eq!(
            cross_check(0x316688, &table, pair_arm(), "MenuLayer::init"),
            CrossCheck::Concordant
        );
    }

    // -------------------------------------------------------------------
    // Riflessi su provenienza: forcing, no_reuse e mappatura (Req 4.3/4.4/4.6).
    // -------------------------------------------------------------------

    #[test]
    fn concordant_does_not_force_verified_false() {
        assert!(!CrossCheck::Concordant.forces_verified_false());
        assert!(CrossCheck::Concordant.is_concordant());
    }

    #[test]
    fn discordant_and_rejected_force_verified_false() {
        // Req 4.4 (discorde) e Req 4.5 (rifiutato per violazione).
        assert!(CrossCheck::Discordant.forces_verified_false());
        assert!(CrossCheck::Rejected.forces_verified_false());
        assert!(!CrossCheck::Discordant.is_concordant());
        assert!(!CrossCheck::Rejected.is_concordant());
    }

    #[test]
    fn no_reuse_is_always_true() {
        // Req 4.6: sempre "solo riferimento numerico osservativo".
        assert!(CrossCheck::Concordant.no_reuse());
        assert!(CrossCheck::Discordant.no_reuse());
        assert!(CrossCheck::Rejected.no_reuse());
    }

    #[test]
    fn cross_check_maps_to_provenance_outcome_one_to_one() {
        assert_eq!(
            CrossCheck::Concordant.to_outcome(),
            CrossCheckOutcome::Concordant
        );
        assert_eq!(
            CrossCheck::Discordant.to_outcome(),
            CrossCheckOutcome::Discordant
        );
        assert_eq!(CrossCheck::Rejected.to_outcome(), CrossCheckOutcome::Rejected);

        // Conversione bidirezionale coerente (round-trip).
        for cc in [
            CrossCheck::Concordant,
            CrossCheck::Discordant,
            CrossCheck::Rejected,
        ] {
            let outcome: CrossCheckOutcome = cc.into();
            let back: CrossCheck = outcome.into();
            assert_eq!(cc, back);
            // Coerenza con la stringa canonica del Provenance_Record.
            assert_eq!(outcome.as_str(), cc.to_outcome().as_str());
        }
    }
}
