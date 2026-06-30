//! `Validation_Stage` — applicazione lato Rust della semantica
//! *"resolved sse verificato"* sui `Binding_Set_File` `.pbind`.
//!
//! Questo stadio è la **controparte build-time** del predicato runtime
//! `address_is_verified` di `loader/bindings/binding_verifier.cpp`: una voce
//! `[function]` è **risolvibile** *se e solo se* — simultaneamente —
//!
//!   1. il suo `verified` è `true`;
//!   2. il suo offset è **diverso da zero**;
//!   3. il suo offset è **diverso** dal [`SENTINEL_VALUE`].
//!
//! È la **stessa** regola del `Binding_Verifier`, riusata e non ridefinita:
//! nessun fuzzy-match, nessuna sostituzione, valutazione sul singolo binding già
//! selezionato dalla coppia esatta. Il [`SENTINEL_VALUE`] (`u64::MAX`) è allineato
//! numericamente a `kPlaceholderSentinel = ~0` di `binding_verifier.hpp`.
//!
//! **Semantica positiva (Req 6.1, 6.5, 6.6):**
//!
//! - marca risolvibile una voce *se e solo se* le tre condizioni valgono insieme
//!   (Req 6.1);
//! - su validazione superata marca risolvibili **tutte e sole** le voci che
//!   soddisfano le tre condizioni (Req 6.5);
//! - una voce `verified = false` è **non risolvibile** *senza* rifiutare il file
//!   né segnalare errore (Req 6.6).
//!
//! **Rifiuti fail-closed (Req 6.2, 6.3, 6.4):** la validazione fallisce in
//! chiusura — non distribuisce nulla — appena rileva un'incoerenza che renderebbe
//! risolvibile un binding non sicuro:
//!
//! - **incoerenza strutturale (Req 6.2):** una voce che dichiara
//!   `verified = true` ma porta un offset `0` o uguale al [`SENTINEL_VALUE`] è
//!   internamente contraddittoria (afferma di essere verificata pur portando un
//!   offset che non è **mai** risolvibile). [`validate_set`] **rifiuta l'intero
//!   file** con [`ValidationError::InconsistentVerified`], identificando simbolo
//!   e coppia; nessuna voce del file è distribuita. Questo controllo è
//!   **puramente strutturale** e vive nel cuore di [`validate_set`] perché non
//!   richiede input esterni.
//! - **mismatch di prologo (Req 6.3):** i byte di prologo all'offset di una voce
//!   `verified = true` non corrispondono alla firma registrata. Il `PbindSet`
//!   parsato **non porta i byte di prologo** (la loro semantica vive in
//!   `prologue.rs`/`binding_verifier`), perciò questo controllo richiede un input
//!   aggiuntivo: [`validate_set_with_prologue_check`] accetta un predicato
//!   `prologue_ok` e, su una voce `verified = true` per cui il prologo non
//!   conferma, **rifiuta il file** con [`ValidationError::PrologueMismatch`]
//!   identificando il simbolo. La [`validate_set`] base resta stabile e applica
//!   solo i controlli che non richiedono input esterni (Req 6.2).
//! - **preservazione su fallimento (Req 6.4):** [`validate_set`] è **pura** (non
//!   scrive file): un `Err` significa *"non distribuire"*. Il confine di
//!   distribuzione è incapsulato in [`distribute_if_valid`], che valida **prima**
//!   e scrive il `.pbind` **solo** se valido, in modo **atomico** (temp nella
//!   stessa directory + `rename`, come il generatore): su rifiuto di validazione
//!   il `.pbind` precedentemente distribuito resta **invariato byte-per-byte** e
//!   l'errore segnala causa + simbolo.
//!
//! Resta fermo che `verified = false` con offset `0`/sentinel **non** è
//! un'incoerenza: è una voce semplicemente non risolvibile, accettata senza
//! errore (Req 6.6). Solo `verified = true` + `0`/sentinel rifiuta (Req 6.2).
//!
//! _Requisiti: 6.1, 6.2, 6.3, 6.4, 6.5, 6.6._

use std::path::{Path, PathBuf};

use crate::bindings::{Signature, SymbolId, TargetPair, SENTINEL_VALUE};

// ---------------------------------------------------------------------------
// Modello di un `.pbind` PARSATO (mirror del C++ BindingSet/FunctionBinding).
// ---------------------------------------------------------------------------

/// Una singola voce `[function]` parsata da un `.pbind`.
///
/// Rispecchia `pulse::loader::bindings::FunctionBinding`: simbolo, offset RVA,
/// firma e il flag `verified` (che lato C++ alimenta il campo `resolved`
/// pre-verifica). L'offset è un `u64`: l'assenza/placeholder è rappresentata dal
/// [`SENTINEL_VALUE`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PbindFunction {
    /// Identificatore univoco del simbolo, es. `"MenuLayer::init"`.
    pub symbol: SymbolId,
    /// Offset RVA della funzione; `0` o [`SENTINEL_VALUE`] ⇒ mai risolvibile.
    pub offset: u64,
    /// Firma registrata della funzione (`return` + `params`).
    pub signature: Signature,
    /// `verified` dichiarato dal `.pbind`: necessario (ma non sufficiente) alla
    /// risolvibilità.
    pub verified: bool,
}

impl PbindFunction {
    /// Costruisce una `PbindFunction` dai suoi campi.
    pub fn new(symbol: SymbolId, offset: u64, signature: Signature, verified: bool) -> Self {
        Self {
            symbol,
            offset,
            signature,
            verified,
        }
    }
}

/// Un `Binding_Set_File` `.pbind` parsato: la coppia d'intestazione più la lista
/// ordinata delle voci `[function]`.
///
/// Rispecchia `pulse::loader::bindings::BindingSet`: una `key` (qui [`TargetPair`])
/// più l'insieme dei `FunctionBinding`. Questo è il modello Rust su cui opera la
/// validazione; un parser testuale completo dei `.pbind` lato Rust è una
/// preoccupazione separata (il `pbind_format` C++ resta la fonte canonica).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PbindSet {
    /// Coppia `(GD_Version, Target_Platform)` del set.
    pub pair: TargetPair,
    /// Voci `[function]` del set.
    pub functions: Vec<PbindFunction>,
}

impl PbindSet {
    /// Costruisce un `PbindSet` dalla coppia e dalle sue funzioni.
    pub fn new(pair: TargetPair, functions: Vec<PbindFunction>) -> Self {
        Self { pair, functions }
    }
}

// ---------------------------------------------------------------------------
// Esito di risolvibilità per voce e report di validazione.
// ---------------------------------------------------------------------------

/// Esito di risolvibilità di una singola voce `[function]`.
///
/// Specchia l'esito binario di `address_is_verified`: `Yes` ⇔ tutte e tre le
/// condizioni del criterio 1 valgono simultaneamente.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Resolvable {
    /// La voce è risolvibile: `verified == true` ∧ offset `!= 0` ∧ offset
    /// `!= SENTINEL_VALUE`.
    Yes,
    /// La voce **non** è risolvibile (almeno una delle tre condizioni è falsa).
    No,
}

impl Resolvable {
    /// `true` se l'esito è [`Resolvable::Yes`].
    pub fn is_resolvable(self) -> bool {
        matches!(self, Resolvable::Yes)
    }
}

/// Report prodotto da [`validate_set`] su una validazione **superata**.
///
/// Registra, **per ogni** voce e nello stesso ordine del `PbindSet` d'ingresso,
/// se è risolvibile, così che i chiamanti conoscano esattamente l'**insieme
/// risolvibile** (Req 6.5).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ValidationReport {
    /// Una entry per ciascuna voce `[function]`, in ordine: `(simbolo, esito)`.
    pub resolvable: Vec<(SymbolId, Resolvable)>,
}

impl ValidationReport {
    /// Iteratore sui soli simboli marcati risolvibili (`Resolvable::Yes`).
    pub fn resolvable_symbols(&self) -> impl Iterator<Item = &SymbolId> {
        self.resolvable
            .iter()
            .filter_map(|(symbol, state)| state.is_resolvable().then_some(symbol))
    }

    /// Numero di voci risolvibili nel report.
    pub fn resolvable_count(&self) -> usize {
        self.resolvable
            .iter()
            .filter(|(_, state)| state.is_resolvable())
            .count()
    }
}

// ---------------------------------------------------------------------------
// Errore di validazione (fail-closed): rifiuto dell'intero Binding_Set_File.
// ---------------------------------------------------------------------------

/// Errore di validazione di un `Binding_Set_File` (fail-closed).
///
/// Ogni variante implica il **rifiuto dell'intero file**: nessuna voce viene
/// distribuita (Req 6.2, 6.3) e qualsiasi `.pbind` precedentemente distribuito
/// resta invariato (Req 6.4, gestito da [`distribute_if_valid`]). L'enum è
/// `#[non_exhaustive]` per poter crescere senza rompere i chiamanti.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
#[non_exhaustive]
pub enum ValidationError {
    /// Incoerenza strutturale (Req 6.2): la voce dichiara `verified = true` ma
    /// porta un offset `0` o uguale al [`SENTINEL_VALUE`] — un offset che non è
    /// **mai** risolvibile. Il file è rifiutato per intero.
    #[error(
        "incoerenza: il simbolo '{symbol}' della coppia {pair} dichiara verified=true \
         ma porta un offset non risolvibile (0 o sentinel); l'intero Binding_Set_File è rifiutato"
    )]
    InconsistentVerified {
        /// Simbolo incoerente.
        symbol: SymbolId,
        /// Coppia `(GD_Version, Target_Platform)` del file rifiutato.
        pair: TargetPair,
    },

    /// Mismatch di prologo (Req 6.3): i byte di prologo all'offset di una voce
    /// `verified = true` non corrispondono alla firma registrata. Il file è
    /// rifiutato per intero con un errore di firma non corrispondente.
    #[error(
        "firma non corrispondente: il prologo del simbolo '{symbol}' (coppia {pair}) non \
         corrisponde alla firma registrata; l'intero Binding_Set_File è rifiutato"
    )]
    PrologueMismatch {
        /// Simbolo il cui prologo non conferma la firma.
        symbol: SymbolId,
        /// Coppia `(GD_Version, Target_Platform)` del file rifiutato.
        pair: TargetPair,
    },
}

// ---------------------------------------------------------------------------
// Predicato "resolved sse verificato" e validate_set.
// ---------------------------------------------------------------------------

/// Predicato puro *"resolved sse verificato"*, immagine Rust di
/// `address_is_verified` (`loader/bindings/binding_verifier.cpp`).
///
/// Restituisce `true` **se e solo se**, simultaneamente: `func.verified` è
/// `true`, `func.offset != 0`, e `func.offset != SENTINEL_VALUE` (Req 6.1).
#[must_use]
pub fn is_resolvable(func: &PbindFunction) -> bool {
    func.verified && func.offset != 0 && func.offset != SENTINEL_VALUE
}

/// Valida un `Binding_Set_File` parsato e produce un [`ValidationReport`] che,
/// per ogni voce `[function]`, indica se è risolvibile secondo la semantica
/// *"resolved sse verificato"* (Req 6.1).
///
/// Su validazione superata sono marcate risolvibili **tutte e sole** le voci che
/// soddisfano simultaneamente le tre condizioni (Req 6.5); una voce
/// `verified = false` è marcata non risolvibile **senza** rifiutare il file né
/// segnalare errore (Req 6.6).
///
/// **Rifiuto fail-closed strutturale (Req 6.2):** se **una qualsiasi** voce
/// dichiara `verified = true` ma porta un offset `0` o uguale al
/// [`SENTINEL_VALUE`], l'intero file è **rifiutato** con
/// [`ValidationError::InconsistentVerified`] (simbolo + coppia) e **nessuna**
/// voce è distribuita. Il controllo è puramente strutturale: non richiede i byte
/// di prologo, perciò vive qui nel cuore della validazione. Il mismatch di
/// prologo (Req 6.3) richiede invece un input aggiuntivo ed è applicato da
/// [`validate_set_with_prologue_check`].
pub fn validate_set(parsed: &PbindSet) -> Result<ValidationReport, ValidationError> {
    // Rifiuto fail-closed (Req 6.2): una voce verified=true con offset
    // 0/sentinel è internamente contraddittoria. Si rifiuta l'INTERO file alla
    // prima incoerenza, prima di produrre qualunque report (nulla è distribuito).
    for func in &parsed.functions {
        if func.verified && (func.offset == 0 || func.offset == SENTINEL_VALUE) {
            return Err(ValidationError::InconsistentVerified {
                symbol: func.symbol.clone(),
                pair: parsed.pair,
            });
        }
    }

    let resolvable = parsed
        .functions
        .iter()
        .map(|func| {
            let state = if is_resolvable(func) {
                Resolvable::Yes
            } else {
                Resolvable::No
            };
            (func.symbol.clone(), state)
        })
        .collect();

    Ok(ValidationReport { resolvable })
}

/// Come [`validate_set`], ma applica anche la **verifica del prologo** (Req 6.3)
/// su ogni voce `verified = true`.
///
/// Il `PbindSet` parsato non porta i byte di prologo (vivono in
/// `prologue.rs`/`binding_verifier`), perciò il controllo è esposto come
/// predicato `prologue_ok`: per una voce `func`, `prologue_ok(func)` è `true`
/// quando i byte di prologo all'offset di `func` corrispondono alla firma
/// registrata. La funzione:
///
/// 1. applica prima i controlli strutturali di [`validate_set`] (Req 6.2);
/// 2. poi, per **ogni** voce `verified = true`, se `prologue_ok(func)` è `false`
///    **rifiuta l'intero file** con [`ValidationError::PrologueMismatch`]
///    (simbolo + coppia), senza distribuire alcuna voce (Req 6.3).
///
/// Le voci `verified = false` non sono sottoposte al controllo di prologo: sono
/// già non risolvibili e non installano hook (Req 6.6).
pub fn validate_set_with_prologue_check(
    parsed: &PbindSet,
    prologue_ok: impl Fn(&PbindFunction) -> bool,
) -> Result<ValidationReport, ValidationError> {
    // 1) Controlli strutturali (Req 6.2): rifiuta verified=true + 0/sentinel.
    let report = validate_set(parsed)?;

    // 2) Conformità del prologo (Req 6.3): su una voce verified=true il cui
    //    prologo non conferma la firma, rifiuta l'INTERO file.
    for func in &parsed.functions {
        if func.verified && !prologue_ok(func) {
            return Err(ValidationError::PrologueMismatch {
                symbol: func.symbol.clone(),
                pair: parsed.pair,
            });
        }
    }

    Ok(report)
}

// ---------------------------------------------------------------------------
// Confine di distribuzione: preservazione del .pbind precedente (Req 6.4).
// ---------------------------------------------------------------------------

/// Errore del confine di distribuzione di [`distribute_if_valid`].
///
/// Separa il **rifiuto di validazione** (fail-closed, Req 6.2/6.3 — il `.pbind`
/// precedente resta intatto per Req 6.4) da un eventuale errore di **I/O** della
/// scrittura. In entrambi i casi la scrittura è atomica, quindi un fallimento
/// non lascia mai un file parziale né altera il `.pbind` precedente.
#[derive(Debug, thiserror::Error)]
#[non_exhaustive]
pub enum DistributeError {
    /// La validazione ha rifiutato il file: **non** è stato distribuito nulla e
    /// il `.pbind` precedente resta invariato byte-per-byte (Req 6.4). L'errore
    /// interno identifica causa + simbolo.
    #[error("validazione fallita, distribuzione annullata: {0}")]
    Rejected(#[from] ValidationError),

    /// Errore di I/O durante la scrittura atomica del nuovo `.pbind`. Grazie
    /// alla semantica temp+`rename`, il `.pbind` precedente resta intatto e non
    /// resta alcun file parziale.
    #[error("errore di I/O nella scrittura di {path}: {source}")]
    Io {
        /// Percorso di destinazione del `.pbind`.
        path: PathBuf,
        /// Causa di basso livello.
        source: std::io::Error,
    },
}

/// Valida `parsed` e, **solo se** valido, distribuisce `content` su `dest_path`
/// in modo **atomico**, preservando su fallimento il `.pbind` precedente
/// (Req 6.4).
///
/// Disciplina fail-closed:
///
/// - se [`validate_set_with_prologue_check`] rifiuta il file (Req 6.2/6.3), la
///   funzione ritorna [`DistributeError::Rejected`] **senza toccare**
///   `dest_path`: qualsiasi `.pbind` precedentemente distribuito per quella
///   coppia resta **invariato byte-per-byte** e l'errore segnala causa + simbolo
///   (Req 6.4);
/// - se la validazione passa, `content` è scritto in un file temporaneo nella
///   **stessa directory** di `dest_path` e promosso con [`std::fs::rename`]
///   (atomico sullo stesso filesystem, come il generatore): un consumatore
///   osserva sempre il `.pbind` vecchio completo **o** il nuovo, mai uno
///   parziale. Su errore di I/O la destinazione resta intatta e il temporaneo è
///   rimosso.
///
/// La directory genitore di `dest_path` deve esistere (responsabilità del
/// chiamante, come nel generatore).
pub fn distribute_if_valid(
    parsed: &PbindSet,
    prologue_ok: impl Fn(&PbindFunction) -> bool,
    dest_path: &Path,
    content: &[u8],
) -> Result<ValidationReport, DistributeError> {
    // Gate di validazione: su rifiuto NON si tocca la destinazione (Req 6.4).
    let report = validate_set_with_prologue_check(parsed, prologue_ok)?;

    // Validazione superata: scrittura atomica (temp + rename) nella stessa dir.
    atomic_write(dest_path, content).map_err(|source| DistributeError::Io {
        path: dest_path.to_path_buf(),
        source,
    })?;

    Ok(report)
}

/// Scrive `content` in `path` in modo **atomico** rispetto al filesystem,
/// rispecchiando la disciplina di `generator::atomic_write`: il contenuto è
/// prima riversato in un file temporaneo nella **stessa directory** della
/// destinazione, poi promosso con [`std::fs::rename`]. Sullo stesso filesystem
/// il `rename` è atomico e sostituisce integralmente il file preesistente.
///
/// Su errore la destinazione **non** viene mai toccata (il `.pbind` precedente
/// resta intatto byte-per-byte) e il temporaneo è rimosso (nessun residuo).
fn atomic_write(path: &Path, content: &[u8]) -> std::io::Result<()> {
    use std::io::Write as _;
    use std::sync::atomic::{AtomicU64, Ordering};

    static COUNTER: AtomicU64 = AtomicU64::new(0);

    let parent = path.parent().unwrap_or_else(|| Path::new("."));
    let unique = COUNTER.fetch_add(1, Ordering::Relaxed);
    let file_name = path
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| "pbind".to_owned());
    let tmp_path = parent.join(format!(".{file_name}.{}.{unique}.tmp", std::process::id()));

    // 1) Scrive il temporaneo; su errore lo rimuove e la destinazione resta intatta.
    if let Err(source) = (|| -> std::io::Result<()> {
        let mut file = std::fs::File::create(&tmp_path)?;
        file.write_all(content)?;
        file.sync_all()
    })() {
        let _ = std::fs::remove_file(&tmp_path);
        return Err(source);
    }

    // 2) Promuove atomicamente il temporaneo a destinazione finale.
    if let Err(source) = std::fs::rename(&tmp_path, path) {
        let _ = std::fs::remove_file(&tmp_path);
        return Err(source);
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bindings::{GdVersion, TargetPlatform};

    /// Coppia di riferimento per i test (Prioritized_Target).
    fn pair() -> TargetPair {
        TargetPair::new(GdVersion::new(2, 2081), TargetPlatform::MacosArm64)
    }

    /// Firma fittizia coerente con `MenuLayer::init`.
    fn sig() -> Signature {
        Signature::new("bool", vec!["MenuLayer*".to_owned()])
    }

    fn func(symbol: &str, offset: u64, verified: bool) -> PbindFunction {
        PbindFunction::new(SymbolId::new(symbol), offset, sig(), verified)
    }

    /// `verified = true` + offset valido ⇒ risolvibile.
    #[test]
    fn verified_with_valid_offset_is_resolvable() {
        let f = func("MenuLayer::init", 0x316688, true);
        assert!(is_resolvable(&f));

        let report = validate_set(&PbindSet::new(pair(), vec![f])).unwrap();
        assert_eq!(report.resolvable[0].1, Resolvable::Yes);
        assert_eq!(report.resolvable_count(), 1);
    }

    /// `verified = false` ⇒ non risolvibile, senza errore né rifiuto (Req 6.6).
    #[test]
    fn unverified_is_not_resolvable_without_error() {
        let f = func("PlayLayer::update", 0x4000, false);
        assert!(!is_resolvable(&f));

        let report = validate_set(&PbindSet::new(pair(), vec![f]))
            .expect("una voce verified=false non deve produrre errore (Req 6.6)");
        assert_eq!(report.resolvable[0].1, Resolvable::No);
        assert_eq!(report.resolvable_count(), 0);
    }

    /// `verified = true` ma offset `0` ⇒ **rifiuto** dell'intero file (Req 6.2).
    /// Aggiornato dal task 13.2: ciò che 13.1 trattava in modo conservativo come
    /// "non risolvibile" è ora un'incoerenza fail-closed.
    #[test]
    fn verified_with_zero_offset_is_rejected() {
        let f = func("A::b", 0, true);
        // Il predicato runtime resta `false` (offset non risolvibile)...
        assert!(!is_resolvable(&f));

        // ...ma la validazione fail-closed rifiuta l'intero file (Req 6.2).
        let err = validate_set(&PbindSet::new(pair(), vec![f])).unwrap_err();
        assert_eq!(
            err,
            ValidationError::InconsistentVerified {
                symbol: SymbolId::new("A::b"),
                pair: pair(),
            }
        );
    }

    /// `verified = true` ma offset == `SENTINEL_VALUE` ⇒ rifiuto (Req 6.2).
    #[test]
    fn verified_with_sentinel_offset_is_rejected() {
        let f = func("A::c", SENTINEL_VALUE, true);
        assert!(!is_resolvable(&f));

        let err = validate_set(&PbindSet::new(pair(), vec![f])).unwrap_err();
        assert_eq!(
            err,
            ValidationError::InconsistentVerified {
                symbol: SymbolId::new("A::c"),
                pair: pair(),
            }
        );
    }

    /// `verified = false` + offset `0`/sentinel **non** è un'incoerenza: la voce
    /// è semplicemente non risolvibile, accettata senza errore (Req 6.6).
    #[test]
    fn unverified_with_zero_or_sentinel_offset_is_accepted_non_resolvable() {
        let zero = func("Zero::off", 0, false);
        let sentinel = func("Sentinel::off", SENTINEL_VALUE, false);

        let report = validate_set(&PbindSet::new(pair(), vec![zero, sentinel]))
            .expect("verified=false + 0/sentinel non è un'incoerenza (Req 6.6)");
        assert_eq!(report.resolvable[0].1, Resolvable::No);
        assert_eq!(report.resolvable[1].1, Resolvable::No);
        assert_eq!(report.resolvable_count(), 0);
    }

    /// L'incoerenza di **una sola** voce rifiuta l'intero set: nessuna voce
    /// risolvibile viene distribuita (Req 6.2).
    #[test]
    fn one_inconsistent_entry_rejects_the_whole_file() {
        let functions = vec![
            func("Good::ok", 0x1000, true),  // di per sé risolvibile
            func("Bad::sentinel", SENTINEL_VALUE, true), // incoerente
        ];
        let err = validate_set(&PbindSet::new(pair(), functions)).unwrap_err();
        assert_eq!(
            err,
            ValidationError::InconsistentVerified {
                symbol: SymbolId::new("Bad::sentinel"),
                pair: pair(),
            }
        );
    }

    /// Mismatch di prologo su una voce `verified = true` ⇒ rifiuto del file con
    /// errore di firma non corrispondente (Req 6.3).
    #[test]
    fn prologue_mismatch_on_verified_entry_is_rejected() {
        let f = func("MenuLayer::init", 0x316688, true);
        let set = PbindSet::new(pair(), vec![f]);

        // `prologue_ok` falso ⇒ firma non corrispondente ⇒ rifiuto.
        let err = validate_set_with_prologue_check(&set, |_| false).unwrap_err();
        assert_eq!(
            err,
            ValidationError::PrologueMismatch {
                symbol: SymbolId::new("MenuLayer::init"),
                pair: pair(),
            }
        );
    }

    /// Con prologo conforme su tutte le voci `verified = true`, la validazione
    /// passa e marca correttamente l'insieme risolvibile (Req 6.3 + 6.5).
    #[test]
    fn prologue_ok_on_verified_entries_validates() {
        let functions = vec![
            func("Verified::ok", 0x1000, true),
            func("Unverified::no", 0x2000, false),
        ];
        let set = PbindSet::new(pair(), functions);

        // Il prologo è interrogato solo sulle voci verified=true.
        let report = validate_set_with_prologue_check(&set, |f| {
            assert!(f.verified, "il prologo non deve essere interrogato su verified=false");
            true
        })
        .expect("prologo conforme ⇒ validazione superata");
        let resolvable: Vec<&str> = report.resolvable_symbols().map(|s| s.as_str()).collect();
        assert_eq!(resolvable, vec!["Verified::ok"]);
    }

    /// `validate_set_with_prologue_check` applica **prima** il controllo
    /// strutturale (Req 6.2): una voce verified=true + 0/sentinel è rifiutata
    /// come incoerenza, non come mismatch di prologo, anche con `prologue_ok` che
    /// ritornerebbe `true`.
    #[test]
    fn structural_check_precedes_prologue_check() {
        let f = func("A::b", 0, true);
        let set = PbindSet::new(pair(), vec![f]);

        let err = validate_set_with_prologue_check(&set, |_| true).unwrap_err();
        assert_eq!(
            err,
            ValidationError::InconsistentVerified {
                symbol: SymbolId::new("A::b"),
                pair: pair(),
            }
        );
    }

    /// L'insieme risolvibile è **esattamente** quello delle voci che soddisfano
    /// tutte e tre le condizioni (Req 6.5). Le voci `0`/sentinel qui sono
    /// `verified = false` (non incoerenti): restano non risolvibili senza
    /// rifiutare il file (Req 6.6).
    #[test]
    fn resolvable_set_is_exactly_the_entries_meeting_all_three_conditions() {
        let functions = vec![
            func("Verified::ok", 0x1000, true),       // Yes
            func("Unverified::no", 0x2000, false),    // No: verified=false
            func("Zero::off", 0, false),              // No: offset 0 (verified=false)
            func("Sentinel::off", SENTINEL_VALUE, false), // No: sentinel (verified=false)
            func("Another::ok", 0x3000, true),        // Yes
        ];
        let report = validate_set(&PbindSet::new(pair(), functions)).unwrap();

        let resolvable: Vec<&str> = report.resolvable_symbols().map(|s| s.as_str()).collect();
        assert_eq!(resolvable, vec!["Verified::ok", "Another::ok"]);
        assert_eq!(report.resolvable_count(), 2);
    }

    /// Un set vuoto valida con un report vuoto (nessun errore).
    #[test]
    fn empty_set_validates_to_empty_report() {
        let report = validate_set(&PbindSet::new(pair(), vec![])).unwrap();
        assert!(report.resolvable.is_empty());
        assert_eq!(report.resolvable_count(), 0);
    }

    // -----------------------------------------------------------------------
    // Preservazione su fallimento (Req 6.4): confine di distribuzione.
    // -----------------------------------------------------------------------

    /// Directory temporanea auto-pulente per i test di distribuzione.
    struct TempDir {
        root: std::path::PathBuf,
    }

    impl TempDir {
        fn new() -> Self {
            use std::sync::atomic::{AtomicU32, Ordering};
            static N: AtomicU32 = AtomicU32::new(0);
            let unique = N.fetch_add(1, Ordering::Relaxed);
            let root = std::env::temp_dir().join(format!(
                "pulse-validation-test-{}-{unique}",
                std::process::id()
            ));
            std::fs::create_dir_all(&root).unwrap();
            Self { root }
        }
    }

    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.root);
        }
    }

    /// Su validazione superata, `distribute_if_valid` scrive il nuovo `.pbind`.
    #[test]
    fn distribute_writes_when_valid() {
        let tmp = TempDir::new();
        let dest = tmp.root.join("macos-arm64.pbind");
        let set = PbindSet::new(pair(), vec![func("MenuLayer::init", 0x316688, true)]);

        let report = distribute_if_valid(&set, |_| true, &dest, b"nuovo-contenuto")
            .expect("validazione superata ⇒ distribuzione riuscita");
        assert_eq!(report.resolvable_count(), 1);
        assert_eq!(std::fs::read(&dest).unwrap(), b"nuovo-contenuto");
    }

    /// Su rifiuto di validazione (Req 6.2), il `.pbind` precedente resta
    /// **invariato byte-per-byte** e nulla è distribuito (Req 6.4).
    #[test]
    fn distribute_preserves_previous_file_on_validation_failure() {
        let tmp = TempDir::new();
        let dest = tmp.root.join("macos-arm64.pbind");
        let previous = b"contenuto-precedente-distribuito";
        std::fs::write(&dest, previous).unwrap();

        // Voce incoerente (verified=true + sentinel) ⇒ rifiuto fail-closed.
        let set = PbindSet::new(pair(), vec![func("Bad::x", SENTINEL_VALUE, true)]);
        let err = distribute_if_valid(&set, |_| true, &dest, b"NON-DEVE-ESSERE-SCRITTO")
            .unwrap_err();

        match err {
            DistributeError::Rejected(ValidationError::InconsistentVerified { symbol, pair: p }) => {
                assert_eq!(symbol, SymbolId::new("Bad::x"));
                assert_eq!(p, pair());
            }
            other => panic!("atteso Rejected(InconsistentVerified), trovato {other:?}"),
        }

        // Il file precedente è intatto byte-per-byte (Req 6.4).
        assert_eq!(std::fs::read(&dest).unwrap(), previous);
    }

    /// Anche un mismatch di prologo (Req 6.3) preserva il `.pbind` precedente.
    #[test]
    fn distribute_preserves_previous_file_on_prologue_mismatch() {
        let tmp = TempDir::new();
        let dest = tmp.root.join("macos-arm64.pbind");
        let previous = b"vecchio-pbind";
        std::fs::write(&dest, previous).unwrap();

        let set = PbindSet::new(pair(), vec![func("MenuLayer::init", 0x316688, true)]);
        let err = distribute_if_valid(&set, |_| false, &dest, b"nuovo").unwrap_err();

        match err {
            DistributeError::Rejected(ValidationError::PrologueMismatch { symbol, .. }) => {
                assert_eq!(symbol, SymbolId::new("MenuLayer::init"));
            }
            other => panic!("atteso Rejected(PrologueMismatch), trovato {other:?}"),
        }
        assert_eq!(std::fs::read(&dest).unwrap(), previous);
    }
}
