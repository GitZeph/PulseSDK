//! `Prologue_Verification` — verifica del prologo prima di marcare un binding
//! come verificato (Req 5).
//!
//! Questo modulo è la **controparte build-time** del runtime `IPrologueVerifier`
//! di `loader/bindings/binding_verifier.hpp`: stessa idea (il prologo all'offset
//! candidato deve essere conforme alla firma registrata), eseguita **una volta**
//! in pipeline e poi cristallizzata nel campo `verified` del `.pbind`.
//!
//! Esistono due modalità ([`PrologueMethod`], Req 5.5):
//!
//! - **[`PrologueMethod::OtoolManual`]** — il Contributor disassembla l'offset
//!   con `otool -arch arm64 -tv` sul binario reale (procedura Phase E),
//!   confronta il prologo con la firma attesa e registra l'esito. **Non è
//!   automatizzabile in CI**: è coperta dal task manuale 19.1. Quando i byte
//!   osservati vengono comunque forniti (es. catturati dall'output di `otool`),
//!   si applica **lo stesso** confronto deterministico della variante
//!   automatica; la differenza è soltanto il **metodo** registrato nel
//!   `Provenance_Record`.
//! - **[`PrologueMethod::AutomaticBytes`]** — confronto programmatico,
//!   **host-testabile**: si confrontano *N* byte di prologo all'offset, con *N*
//!   pari alla lunghezza in byte della firma registrata, considerando "match"
//!   **solo** se **tutti** i byte sono identici (Req 5.1).
//!
//! ## Da dove vengono gli *N* byte attesi (decisione di design)
//!
//! La variante `AutomaticBytes` è una funzione **pura e host-testabile**: non
//! legge il binario reale (quello è il compito della variante manuale). Per
//! renderla deterministica e testabile, gli **byte attesi** sono derivati dalla
//! **firma registrata** della [`CatalogEntry`] tramite
//! [`expected_prologue_bytes`]: la firma ha una **forma canonica** testuale
//! (`<return>(<param0>,<param1>,…)`, es. `"bool(MenuLayer*)"`) e i suoi byte
//! UTF-8 sono gli "byte di prologo attesi". *N* è quindi la **lunghezza in byte
//! della firma registrata** ([`signature_byte_length`]), esattamente come
//! richiede il Req 5.1. La funzione confronta i `prologue_bytes` forniti (i byte
//! candidati all'offset) contro questi byte attesi derivati dalla firma.
//!
//! Questo modello mantiene la regola **"match sse tutti gli N byte identici"**
//! verificabile in CI, lasciando alla variante manuale (`OtoolManual`, Req 5.5)
//! la lettura dei byte reali dal binario.
//!
//! ## Transizione del `Verified_Flag` (Req 5.2, 5.4)
//!
//! Il `Verified_Flag` di un offset diventa `true` **solo** se:
//!
//! 1. il prologo è "match" ([`PrologueOutcome::Match`]), **e**
//! 2. l'offset candidato è `> 0`, **e**
//! 3. l'offset candidato è `!= SENTINEL_VALUE`.
//!
//! In ogni altro caso il flag **resta `false`** (fail-closed):
//!
//! - prologo non conforme → [`PrologueOutcome::Mismatch`], flag `false` + errore
//!   con simbolo/coppia/causa (Req 5.3);
//! - offset `0`, offset `SENTINEL_VALUE`, o byte illeggibili
//!   ([`PrologueOutcome::Unreadable`]) → flag `false` + errore con la condizione
//!   rilevata (Req 5.6).
//!
//! _Requisiti: 5.1, 5.2, 5.3, 5.4, 5.6._

use std::fmt;

use super::catalog::CatalogEntry;
use super::{Signature, SymbolId, TargetPair, SENTINEL_VALUE};

// ---------------------------------------------------------------------------
// Tipi pubblici.
// ---------------------------------------------------------------------------

/// Metodo con cui è stata eseguita la `Prologue_Verification` (Req 5.5).
///
/// Il metodo viene registrato nel `Provenance_Record` insieme all'esito, sia in
/// caso di successo sia di fallimento.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PrologueMethod {
    /// Disassemblamento manuale via `otool` sul binario reale (Phase E, task
    /// 19.1). **Non automatizzabile in CI.**
    OtoolManual,
    /// Confronto programmatico dei byte di prologo (host-testabile).
    AutomaticBytes,
}

impl PrologueMethod {
    /// Identificatore testuale stabile usato nel `Provenance_Record`
    /// (`prologue_method`), coerente con i valori del catalogo TOML.
    pub fn as_str(self) -> &'static str {
        match self {
            PrologueMethod::OtoolManual => "otool-manual",
            PrologueMethod::AutomaticBytes => "automatic-bytes",
        }
    }
}

impl fmt::Display for PrologueMethod {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

/// Esito del confronto del prologo (Req 5.1, 5.6).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PrologueOutcome {
    /// Tutti gli *N* byte di prologo coincidono con la firma registrata.
    Match,
    /// I byte di prologo **non** coincidono (in tutto o in parte) con la firma.
    Mismatch,
    /// I byte di prologo all'offset candidato non sono leggibili (Req 5.6).
    Unreadable,
}

impl PrologueOutcome {
    /// Identificatore testuale stabile usato nel `Provenance_Record`
    /// (`prologue_outcome`).
    pub fn as_str(self) -> &'static str {
        match self {
            PrologueOutcome::Match => "match",
            PrologueOutcome::Mismatch => "mismatch",
            PrologueOutcome::Unreadable => "unreadable",
        }
    }
}

impl fmt::Display for PrologueOutcome {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

/// Condizione bloccante rilevata che impedisce la verifica (Req 5.6).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PrologueCondition {
    /// L'offset candidato è pari a zero.
    OffsetZero,
    /// L'offset candidato è pari al [`SENTINEL_VALUE`].
    OffsetSentinel,
    /// I byte di prologo all'offset candidato non sono leggibili.
    UnreadableBytes,
}

impl PrologueCondition {
    /// Descrizione testuale della condizione rilevata, per l'errore (Req 5.6).
    pub fn as_str(self) -> &'static str {
        match self {
            PrologueCondition::OffsetZero => "offset pari a zero",
            PrologueCondition::OffsetSentinel => "offset pari al Sentinel_Value",
            PrologueCondition::UnreadableBytes => "byte di prologo illeggibili",
        }
    }
}

impl fmt::Display for PrologueCondition {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

/// Errore della `Prologue_Verification`: identifica sempre **simbolo** e
/// **coppia** `(GD_Version, Target_Platform)`, oltre alla causa (Req 5.3, 5.6).
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum PrologueError {
    /// Il prologo non corrisponde alla firma registrata (Req 5.3): il
    /// `Verified_Flag` resta `false`.
    #[error("prologo non conforme per {symbol} su {pair}: {cause}")]
    Mismatch {
        /// Simbolo coinvolto.
        symbol: SymbolId,
        /// Coppia `(GD_Version, Target_Platform)` coinvolta.
        pair: TargetPair,
        /// Causa della mancata corrispondenza.
        cause: String,
    },

    /// Offset `0`, offset `SENTINEL_VALUE`, o byte illeggibili (Req 5.6): il
    /// `Verified_Flag` resta `false`.
    #[error("prologo non verificabile per {symbol} su {pair}: {condition}")]
    Condition {
        /// Simbolo coinvolto.
        symbol: SymbolId,
        /// Coppia `(GD_Version, Target_Platform)` coinvolta.
        pair: TargetPair,
        /// Condizione bloccante rilevata.
        condition: PrologueCondition,
    },
}

/// Esito completo di una `Prologue_Verification`: l'[`PrologueOutcome`], il
/// [`PrologueMethod`] usato e il `Verified_Flag` risultante.
///
/// È il valore di ritorno "ricco" di [`evaluate_prologue`], pensato per essere
/// registrato nel `Provenance_Record` (metodo + esito, Req 5.5) e per pilotare
/// la transizione del flag (Req 5.2, 5.4).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PrologueVerification {
    /// Esito del confronto del prologo.
    pub outcome: PrologueOutcome,
    /// Metodo usato per la verifica (registrato in provenienza, Req 5.5).
    pub method: PrologueMethod,
    /// `Verified_Flag` risultante: `true` **solo** se prologo "match" e offset
    /// valido (`> 0` e `!= SENTINEL_VALUE`).
    pub verified: bool,
}

// ---------------------------------------------------------------------------
// Derivazione dei byte attesi dalla firma registrata.
// ---------------------------------------------------------------------------

/// Forma canonica testuale di una [`Signature`]: `<return>(<param0>,<param1>,…)`.
///
/// Esempio: `MenuLayer::init` → `Signature { return_type: "bool",
/// param_types: ["MenuLayer*"] }` → `"bool(MenuLayer*)"`. È deterministica e
/// stabile, così *N* (la sua lunghezza in byte) è ben definito.
pub fn canonical_signature(signature: &Signature) -> String {
    format!("{}({})", signature.return_type, signature.param_types.join(","))
}

/// Byte di prologo **attesi** derivati dalla firma registrata: i byte UTF-8
/// della forma canonica della firma ([`canonical_signature`]).
///
/// Sono il termine di confronto della variante `AutomaticBytes`: i byte
/// candidati all'offset devono coincidere con questi per dare "match" (Req 5.1).
pub fn expected_prologue_bytes(signature: &Signature) -> Vec<u8> {
    canonical_signature(signature).into_bytes()
}

/// *N* = lunghezza in byte della firma registrata (Req 5.1): il numero di byte
/// di prologo da confrontare all'offset candidato.
pub fn signature_byte_length(signature: &Signature) -> usize {
    expected_prologue_bytes(signature).len()
}

// ---------------------------------------------------------------------------
// Confronto del prologo e transizione del Verified_Flag.
// ---------------------------------------------------------------------------

/// Confronta i byte di prologo candidati contro i byte attesi derivati dalla
/// firma registrata della [`CatalogEntry`] (Req 5.1).
///
/// Restituisce:
///
/// - [`PrologueOutcome::Unreadable`] se `prologue_bytes` è `None` (byte non
///   leggibili, Req 5.6);
/// - [`PrologueOutcome::Match`] **se e solo se** i primi *N* byte candidati
///   (con *N* = [`signature_byte_length`]) sono **tutti** identici ai byte
///   attesi — il che richiede che siano disponibili almeno *N* byte;
/// - [`PrologueOutcome::Mismatch`] in ogni altro caso (anche con byte
///   insufficienti o prefisso parzialmente uguale).
///
/// Questa funzione rispetta la firma illustrata nel design
/// (`verify_prologue(entry, pair, method, prologue_bytes) -> PrologueOutcome`):
/// `pair` e `method` non influenzano il **confronto** dei byte (che è identico
/// per entrambi i metodi), ma fanno parte del contratto pubblico e sono usati a
/// monte/valle per registrare la provenienza (Req 5.5) e per la transizione del
/// flag (vedi [`evaluate_prologue`]).
pub fn verify_prologue(
    entry: &CatalogEntry,
    _pair: TargetPair,
    _method: PrologueMethod,
    prologue_bytes: Option<&[u8]>,
) -> PrologueOutcome {
    let candidate = match prologue_bytes {
        // Byte illeggibili → Unreadable (Req 5.6).
        None => return PrologueOutcome::Unreadable,
        Some(bytes) => bytes,
    };

    let expected = expected_prologue_bytes(&entry.signature);
    let n = expected.len();

    // "Match" SOLO se sono disponibili almeno N byte e tutti i primi N byte
    // coincidono con la firma registrata (Req 5.1).
    if candidate.len() >= n && candidate[..n] == expected[..] {
        PrologueOutcome::Match
    } else {
        PrologueOutcome::Mismatch
    }
}

/// Determina se il `Verified_Flag` deve passare a `true` (Req 5.2, 5.4).
///
/// Ritorna `true` **se e solo se** il prologo è "match" **e** l'offset candidato
/// `rva` è `> 0` **e** `!= SENTINEL_VALUE`. In ogni altro caso ritorna `false`
/// (fail-closed): un offset non può mai diventare verificato senza aver superato
/// la `Prologue_Verification` (Req 5.4).
pub fn should_verify(outcome: PrologueOutcome, rva: u64) -> bool {
    matches!(outcome, PrologueOutcome::Match) && rva > 0 && rva != SENTINEL_VALUE
}

/// Esecuzione completa della `Prologue_Verification` per un offset di una
/// [`CatalogEntry`] su una coppia `(GD_Version, Target_Platform)`.
///
/// Trova l'`OffsetRecord` della coppia nel catalogo (l'offset assente è trattato
/// come [`SENTINEL_VALUE`], fail-closed), confronta il prologo, applica la regola
/// di transizione del `Verified_Flag` e produce un risultato auditabile.
///
/// Esiti (tutti registrabili nel `Provenance_Record`, Req 5.5):
///
/// - **Successo** → `Ok(PrologueVerification { outcome: Match, verified: true })`
///   quando il prologo è "match" e l'offset è `> 0` e `!= SENTINEL_VALUE`
///   (Req 5.2, 5.4).
/// - **Offset `0` / `SENTINEL_VALUE` / byte illeggibili** →
///   `Err(PrologueError::Condition { .. })`, flag `false` (Req 5.6). Queste
///   condizioni sono valutate **prima** del confronto dei byte.
/// - **Prologo non conforme** → `Err(PrologueError::Mismatch { .. })`, flag
///   `false`, con simbolo/coppia/causa (Req 5.3).
///
/// L'offset valido ma con prologo "match" è l'**unico** caso in cui il flag
/// diventa `true`: la funzione non imposta mai `verified = true` senza che il
/// prologo abbia superato il confronto (Req 5.4).
pub fn evaluate_prologue(
    entry: &CatalogEntry,
    pair: TargetPair,
    method: PrologueMethod,
    prologue_bytes: Option<&[u8]>,
) -> Result<PrologueVerification, PrologueError> {
    // Offset effettivo della coppia: l'assenza dell'`OffsetRecord` è trattata
    // come sentinel logico (fail-closed).
    let rva = entry
        .offsets
        .iter()
        .find(|o| o.pair == pair)
        .map(|o| o.effective_rva())
        .unwrap_or(SENTINEL_VALUE);

    // Req 5.6 — condizioni bloccanti sull'offset, valutate prima del confronto.
    if rva == 0 {
        return Err(PrologueError::Condition {
            symbol: entry.symbol.clone(),
            pair,
            condition: PrologueCondition::OffsetZero,
        });
    }
    if rva == SENTINEL_VALUE {
        return Err(PrologueError::Condition {
            symbol: entry.symbol.clone(),
            pair,
            condition: PrologueCondition::OffsetSentinel,
        });
    }

    // Confronto del prologo (Req 5.1).
    let outcome = verify_prologue(entry, pair, method, prologue_bytes);

    match outcome {
        // Req 5.6 — byte illeggibili.
        PrologueOutcome::Unreadable => Err(PrologueError::Condition {
            symbol: entry.symbol.clone(),
            pair,
            condition: PrologueCondition::UnreadableBytes,
        }),
        // Req 5.3 — prologo non conforme: flag resta false, errore con causa.
        PrologueOutcome::Mismatch => Err(PrologueError::Mismatch {
            symbol: entry.symbol.clone(),
            pair,
            cause: format!(
                "i byte di prologo non corrispondono alla firma registrata {:?} ({} byte attesi)",
                canonical_signature(&entry.signature),
                signature_byte_length(&entry.signature),
            ),
        }),
        // Req 5.2, 5.4 — prologo conforme e offset valido: flag a true.
        PrologueOutcome::Match => Ok(PrologueVerification {
            outcome: PrologueOutcome::Match,
            method,
            // rva qui è già garantito > 0 e != SENTINEL_VALUE.
            verified: should_verify(PrologueOutcome::Match, rva),
        }),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bindings::catalog::{OffsetRecord, ProvenanceRecord};
    use crate::bindings::{GdVersion, TargetPlatform};

    const RVA_VALID: u64 = 0x316688;

    fn pair_arm() -> TargetPair {
        TargetPair::new(GdVersion::new(2, 2081), TargetPlatform::MacosArm64)
    }

    /// Costruisce una `CatalogEntry` (`MenuLayer::init`, `bool(MenuLayer*)`) con
    /// un singolo offset sulla coppia indicata e l'RVA dato (`None` ⇒ sentinel).
    fn entry_with_offset(rva: Option<u64>) -> CatalogEntry {
        let pair = pair_arm();
        CatalogEntry {
            symbol: SymbolId::new("MenuLayer::init"),
            signature: Signature::new("bool", vec!["MenuLayer*".to_owned()]),
            offsets: vec![OffsetRecord {
                pair,
                rva,
                verified: false,
                provenance: ProvenanceRecord::empty(SymbolId::new("MenuLayer::init"), pair),
            }],
        }
    }

    fn expected_bytes() -> Vec<u8> {
        // "bool(MenuLayer*)"
        expected_prologue_bytes(&Signature::new("bool", vec!["MenuLayer*".to_owned()]))
    }

    #[test]
    fn canonical_signature_is_return_open_params_close() {
        let sig = Signature::new("bool", vec!["MenuLayer*".to_owned()]);
        assert_eq!(canonical_signature(&sig), "bool(MenuLayer*)");

        let sig2 = Signature::new("void", vec!["PlayLayer*".to_owned(), "float".to_owned()]);
        assert_eq!(canonical_signature(&sig2), "void(PlayLayer*,float)");

        let sig3 = Signature::new("void", vec![]);
        assert_eq!(canonical_signature(&sig3), "void()");
    }

    #[test]
    fn signature_byte_length_matches_canonical_utf8_len() {
        let sig = Signature::new("bool", vec!["MenuLayer*".to_owned()]);
        assert_eq!(signature_byte_length(&sig), "bool(MenuLayer*)".len());
        assert_eq!(signature_byte_length(&sig), 16);
    }

    #[test]
    fn all_bytes_match_yields_match_outcome() {
        let entry = entry_with_offset(Some(RVA_VALID));
        let bytes = expected_bytes();
        let outcome = verify_prologue(
            &entry,
            pair_arm(),
            PrologueMethod::AutomaticBytes,
            Some(&bytes),
        );
        assert_eq!(outcome, PrologueOutcome::Match);
    }

    #[test]
    fn match_with_valid_offset_sets_verified_true() {
        let entry = entry_with_offset(Some(RVA_VALID));
        let bytes = expected_bytes();
        let result = evaluate_prologue(
            &entry,
            pair_arm(),
            PrologueMethod::AutomaticBytes,
            Some(&bytes),
        )
        .expect("prologo conforme e offset valido");
        assert_eq!(result.outcome, PrologueOutcome::Match);
        assert!(result.verified, "Verified_Flag deve diventare true (Req 5.2)");
        assert_eq!(result.method, PrologueMethod::AutomaticBytes);
    }

    #[test]
    fn extra_trailing_bytes_still_match_first_n() {
        // Più di N byte disponibili: contano solo i primi N (Req 5.1).
        let entry = entry_with_offset(Some(RVA_VALID));
        let mut bytes = expected_bytes();
        bytes.extend_from_slice(&[0xAA, 0xBB, 0xCC]);
        let outcome = verify_prologue(
            &entry,
            pair_arm(),
            PrologueMethod::AutomaticBytes,
            Some(&bytes),
        );
        assert_eq!(outcome, PrologueOutcome::Match);
    }

    #[test]
    fn partial_mismatch_yields_mismatch_and_verified_false() {
        let entry = entry_with_offset(Some(RVA_VALID));
        let mut bytes = expected_bytes();
        // Altera l'ultimo byte: prefisso parzialmente uguale, non "match".
        let last = bytes.len() - 1;
        bytes[last] ^= 0xFF;
        let outcome = verify_prologue(
            &entry,
            pair_arm(),
            PrologueMethod::AutomaticBytes,
            Some(&bytes),
        );
        assert_eq!(outcome, PrologueOutcome::Mismatch);

        let err = evaluate_prologue(
            &entry,
            pair_arm(),
            PrologueMethod::AutomaticBytes,
            Some(&bytes),
        )
        .unwrap_err();
        match err {
            PrologueError::Mismatch { symbol, pair, .. } => {
                assert_eq!(symbol, SymbolId::new("MenuLayer::init"));
                assert_eq!(pair, pair_arm());
            }
            other => panic!("atteso Mismatch, trovato {other:?}"),
        }
    }

    #[test]
    fn too_few_bytes_yields_mismatch() {
        let entry = entry_with_offset(Some(RVA_VALID));
        let bytes = vec![b'b', b'o']; // meno di N byte
        let outcome = verify_prologue(
            &entry,
            pair_arm(),
            PrologueMethod::AutomaticBytes,
            Some(&bytes),
        );
        assert_eq!(outcome, PrologueOutcome::Mismatch);
    }

    #[test]
    fn offset_zero_keeps_verified_false_with_error() {
        let entry = entry_with_offset(Some(0));
        let bytes = expected_bytes();
        let err = evaluate_prologue(
            &entry,
            pair_arm(),
            PrologueMethod::AutomaticBytes,
            Some(&bytes),
        )
        .unwrap_err();
        match err {
            PrologueError::Condition {
                symbol,
                pair,
                condition,
            } => {
                assert_eq!(symbol, SymbolId::new("MenuLayer::init"));
                assert_eq!(pair, pair_arm());
                assert_eq!(condition, PrologueCondition::OffsetZero);
            }
            other => panic!("attesa Condition OffsetZero, trovata {other:?}"),
        }
        // should_verify non concede mai true con offset 0.
        assert!(!should_verify(PrologueOutcome::Match, 0));
    }

    #[test]
    fn offset_sentinel_keeps_verified_false_with_error() {
        let entry = entry_with_offset(Some(SENTINEL_VALUE));
        let bytes = expected_bytes();
        let err = evaluate_prologue(
            &entry,
            pair_arm(),
            PrologueMethod::AutomaticBytes,
            Some(&bytes),
        )
        .unwrap_err();
        assert!(matches!(
            err,
            PrologueError::Condition {
                condition: PrologueCondition::OffsetSentinel,
                ..
            }
        ));
        assert!(!should_verify(PrologueOutcome::Match, SENTINEL_VALUE));
    }

    #[test]
    fn missing_offset_record_is_treated_as_sentinel() {
        // Offset rva = None ⇒ sentinel logico (effective_rva == SENTINEL_VALUE).
        let entry = entry_with_offset(None);
        let bytes = expected_bytes();
        let err = evaluate_prologue(
            &entry,
            pair_arm(),
            PrologueMethod::AutomaticBytes,
            Some(&bytes),
        )
        .unwrap_err();
        assert!(matches!(
            err,
            PrologueError::Condition {
                condition: PrologueCondition::OffsetSentinel,
                ..
            }
        ));
    }

    #[test]
    fn none_bytes_yields_unreadable_and_error() {
        let entry = entry_with_offset(Some(RVA_VALID));
        let outcome = verify_prologue(&entry, pair_arm(), PrologueMethod::AutomaticBytes, None);
        assert_eq!(outcome, PrologueOutcome::Unreadable);

        let err = evaluate_prologue(&entry, pair_arm(), PrologueMethod::AutomaticBytes, None)
            .unwrap_err();
        assert!(matches!(
            err,
            PrologueError::Condition {
                condition: PrologueCondition::UnreadableBytes,
                ..
            }
        ));
    }

    #[test]
    fn should_verify_only_true_for_match_and_valid_offset() {
        assert!(should_verify(PrologueOutcome::Match, RVA_VALID));
        assert!(should_verify(PrologueOutcome::Match, 1));
        assert!(!should_verify(PrologueOutcome::Match, 0));
        assert!(!should_verify(PrologueOutcome::Match, SENTINEL_VALUE));
        assert!(!should_verify(PrologueOutcome::Mismatch, RVA_VALID));
        assert!(!should_verify(PrologueOutcome::Unreadable, RVA_VALID));
    }

    #[test]
    fn otool_manual_uses_same_byte_comparison_records_method() {
        // La variante manuale, quando i byte osservati sono forniti, applica lo
        // stesso confronto; la differenza è solo il metodo registrato (Req 5.5).
        let entry = entry_with_offset(Some(RVA_VALID));
        let bytes = expected_bytes();
        let result =
            evaluate_prologue(&entry, pair_arm(), PrologueMethod::OtoolManual, Some(&bytes))
                .expect("prologo conforme");
        assert_eq!(result.method, PrologueMethod::OtoolManual);
        assert!(result.verified);
        assert_eq!(PrologueMethod::OtoolManual.as_str(), "otool-manual");
    }
}
