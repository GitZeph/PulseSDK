//! `Provenance_Store` — store dei `Provenance_Record` e registrazione degli
//! esiti di auditabilità (Req 5.5, 10).
//!
//! Questo modulo **non ridefinisce** il record di provenienza: il
//! [`ProvenanceRecord`] canonico vive in [`super::catalog`] perché è la forma
//! **inline nel TOML del catalogo** (sezione `[offset.provenance]`) consumata
//! dal parser di `OffsetRecord`. Qui lo **re-esportiamo** e implementiamo la
//! **logica di store**: mantenere un record per ogni offset di ogni
//! `Catalog_Entry`, registrare gli esiti di `Observational_Cross_Check` e
//! `Prologue_Verification` (con il **metodo** usato), pilotare la transizione
//! `Verified_Flag → true` registrando fonte+esiti **prima** di esporre l'offset
//! come distribuibile, e rileggere un record completo senza ricomputazione.
//!
//! ## Decisione di design — un solo tipo `ProvenanceRecord`
//!
//! Il record resta quello di [`super::catalog::ProvenanceRecord`], con i campi
//! di esito mantenuti come [`Option<String>`] per restare **serde-compatibile**
//! con il TOML del catalogo. Per evitare di duplicare tipi e rompere il parsing,
//! questo modulo fornisce **setter tipizzati** ([`CrossCheckOutcome`],
//! `PrologueMethod`, `PrologueOutcome`) che scrivono nel record le **stringhe
//! canoniche** (es. `PrologueMethod::as_str()`). Così la logica del Provenance_
//! Store è tipizzata, ma la rappresentazione su disco resta invariata.
//!
//! ## Regole implementate
//!
//! - **Un record per offset di ogni Catalog_Entry** (Req 10.1): lo
//!   [`ProvenanceStore`] è indicizzato per `(SymbolId, TargetPair)` e
//!   [`ProvenanceStore::from_catalog`] crea un record per ogni `OffsetRecord`.
//! - **Esito della Prologue_Verification, sia successo sia fallimento, con il
//!   metodo** (Req 5.5): [`ProvenanceStore::record_prologue`] registra metodo +
//!   esito qualunque sia l'esito.
//! - **Transizione `Verified_Flag → true`** (Req 10.2):
//!   [`ProvenanceStore::record_verified_transition`] registra fonte
//!   dell'`Address_Data`, esito del prologo ed esito del cross-check **prima**
//!   di considerare l'offset distribuibile.
//! - **Lettura di un record distribuito** (Req 10.4): [`ProvenanceStore::get`]
//!   restituisce il record completo **senza** rieseguire né il cross-check né il
//!   prologo — è una pura lettura dallo store.
//!
//! - **Gate di completezza della provenienza** (Req 10.3, 10.5):
//!   [`gate_verified`] *impedisce* il passaggio `Verified_Flag → true` quando il
//!   record non documenta **sia** l'esito dell'`Observational_Cross_Check`
//!   **sia** l'esito della `Prologue_Verification`, segnalando un
//!   [`AuditabilityError`] con simbolo+coppia; [`gate_distributable`] tratta un
//!   offset già `verified = true` ma **privo dell'esito del prologo** come **non
//!   distribuibile**, senza alterare l'`Address_Data` registrato. La variante
//!   **gated** [`ProvenanceStore::try_verified_transition`] applica il gate prima
//!   di consentire la transizione. Entrambi i gate poggiano sui predicati
//!   [`has_cross_check_outcome`]/[`has_prologue_outcome`].
//!
//! _Requisiti: 5.5, 10.1, 10.2, 10.3, 10.4, 10.5._

use std::collections::BTreeMap;

use super::catalog::BindingCatalog;
use super::prologue::{PrologueMethod, PrologueOutcome, PrologueVerification};
use super::{SymbolId, TargetPair};

// Re-export del record canonico: un solo tipo `ProvenanceRecord` in tutta la
// pipeline, definito in `catalog` per la compatibilità serde col TOML.
pub use super::catalog::ProvenanceRecord;

// ---------------------------------------------------------------------------
// CrossCheckOutcome — esito tipizzato dell'Observational_Cross_Check.
// ---------------------------------------------------------------------------

/// Esito **tipizzato** dell'`Observational_Cross_Check`, mappato a/da le
/// stringhe canoniche del campo `cross_check` del [`ProvenanceRecord`].
///
/// Lo `enum CrossCheck` "di dominio" del cross-check vero e proprio è del task
/// successivo (5.2) e vive in [`super::crosscheck`]; qui rappresentiamo solo
/// l'**esito registrabile in provenienza**, con i valori canonici usati anche
/// nel TOML del catalogo (`"concordant"`, `"discordant"`, `"rejected"`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CrossCheckOutcome {
    /// Indirizzo candidato e `Geode_Reference` numericamente uguali (Req 4.3).
    Concordant,
    /// Indirizzo candidato e `Geode_Reference` numericamente diversi (Req 4.4).
    Discordant,
    /// Contenuto rifiutato per violazione del `Geode_Firewall` (Req 4.5).
    Rejected,
}

impl CrossCheckOutcome {
    /// Stringa canonica registrata nel `Provenance_Record` (`cross_check`).
    pub fn as_str(self) -> &'static str {
        match self {
            CrossCheckOutcome::Concordant => "concordant",
            CrossCheckOutcome::Discordant => "discordant",
            CrossCheckOutcome::Rejected => "rejected",
        }
    }

    /// Inverso di [`CrossCheckOutcome::as_str`]: dalla stringa canonica
    /// all'esito tipizzato. `None` per valori non riconosciuti.
    pub fn from_str(value: &str) -> Option<Self> {
        match value {
            "concordant" => Some(CrossCheckOutcome::Concordant),
            "discordant" => Some(CrossCheckOutcome::Discordant),
            "rejected" => Some(CrossCheckOutcome::Rejected),
            _ => None,
        }
    }
}

// ---------------------------------------------------------------------------
// Predicati di completezza a livello di record (fondamenta del gate 7.4).
// ---------------------------------------------------------------------------

/// `true` se il record documenta l'esito dell'`Observational_Cross_Check`.
///
/// Fondamenta del gate di completezza (Req 10.5, task 7.4): qui è solo un
/// predicato di lettura, non impone alcun vincolo.
pub fn has_cross_check_outcome(record: &ProvenanceRecord) -> bool {
    record.cross_check.is_some()
}

/// `true` se il record documenta l'esito della `Prologue_Verification`.
///
/// Fondamenta del gate di completezza (Req 10.3, task 7.4).
pub fn has_prologue_outcome(record: &ProvenanceRecord) -> bool {
    record.prologue_outcome.is_some()
}

// ---------------------------------------------------------------------------
// Gate di completezza della provenienza (Req 10.3, 10.5).
// ---------------------------------------------------------------------------

/// Esito di provenienza **mancante** che rende un offset non auditabile.
///
/// Distingue *cosa* manca nel `Provenance_Record` perché il messaggio d'errore
/// sia diagnostico: solo il cross-check, solo il prologo, o entrambi.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MissingProvenance {
    /// Manca l'esito dell'`Observational_Cross_Check` (Req 10.5).
    CrossCheck,
    /// Manca l'esito della `Prologue_Verification` (Req 10.3, 10.5).
    Prologue,
    /// Mancano **entrambi** gli esiti.
    Both,
}

impl std::fmt::Display for MissingProvenance {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let text = match self {
            MissingProvenance::CrossCheck => "l'esito dell'Observational_Cross_Check",
            MissingProvenance::Prologue => "l'esito della Prologue_Verification",
            MissingProvenance::Both => {
                "sia l'esito dell'Observational_Cross_Check sia quello della Prologue_Verification"
            }
        };
        f.write_str(text)
    }
}

/// Errore di **auditabilità** del gate di completezza della provenienza.
///
/// Segnalato quando il `Provenance_Record` non documenta gli esiti necessari per
/// considerare un offset distribuibile o per consentirne la transizione a
/// `verified = true`. Identifica **sempre** il simbolo e la coppia
/// `(GD_Version, Target_Platform)` interessati (Req 10.3, 10.5), oltre a *cosa*
/// manca. Coerente con lo stile fail-closed degli altri errori della pipeline
/// (vedi `catalog::CatalogError`): l'offset associato resta **non
/// distribuibile**.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
#[error(
    "auditabilità incompleta per il simbolo {symbol} sulla coppia {pair}: \
     il Provenance_Record non documenta {missing}; offset non distribuibile"
)]
pub struct AuditabilityError {
    /// Simbolo dell'offset privo di provenienza completa.
    pub symbol: SymbolId,
    /// Coppia `(GD_Version, Target_Platform)` interessata.
    pub pair: TargetPair,
    /// Esito (o esiti) di provenienza mancante.
    pub missing: MissingProvenance,
}

/// **Gate di completezza** che condiziona il passaggio `Verified_Flag → true`
/// (Req 10.5).
///
/// **Impedisce** la transizione restituendo un [`AuditabilityError`] se il
/// `Provenance_Record` non documenta **sia** l'esito dell'`Observational_Cross_
/// Check` **sia** l'esito della `Prologue_Verification`. Restituisce `Ok(())`
/// **solo** quando entrambi gli esiti sono presenti: è la condizione che il
/// chiamante deve superare *prima* di impostare il flag a vero.
///
/// È una **pura lettura** del record (`&ProvenanceRecord`): per costruzione non
/// può alterare l'`Address_Data` né alcun altro campo.
pub fn gate_verified(record: &ProvenanceRecord) -> Result<(), AuditabilityError> {
    let missing = match (
        has_cross_check_outcome(record),
        has_prologue_outcome(record),
    ) {
        (true, true) => return Ok(()),
        (false, true) => MissingProvenance::CrossCheck,
        (true, false) => MissingProvenance::Prologue,
        (false, false) => MissingProvenance::Both,
    };
    Err(AuditabilityError {
        symbol: record.symbol.clone(),
        pair: record.pair,
        missing,
    })
}

/// Gate di **distribuibilità** per un offset già `verified = true` (Req 10.3).
///
/// Se `verified` è vero e il `Provenance_Record` è **privo dell'esito della
/// `Prologue_Verification`**, segnala un [`AuditabilityError`] e l'offset va
/// trattato come **non distribuibile**. Per `verified = false` non impone nulla
/// (l'offset è già non risolvibile per fail-closed).
///
/// Come [`gate_verified`], opera su `&ProvenanceRecord` in sola lettura: in caso
/// di fallimento **non altera l'`Address_Data` registrato** (Req 10.3).
pub fn gate_distributable(
    verified: bool,
    record: &ProvenanceRecord,
) -> Result<(), AuditabilityError> {
    if verified && !has_prologue_outcome(record) {
        return Err(AuditabilityError {
            symbol: record.symbol.clone(),
            pair: record.pair,
            missing: MissingProvenance::Prologue,
        });
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// ProvenanceStore — un record per offset di ogni Catalog_Entry (Req 10.1).
// ---------------------------------------------------------------------------

/// Store dei `Provenance_Record`, indicizzato per `(SymbolId, TargetPair)`.
///
/// Mantiene **un record per ogni offset di ogni `Catalog_Entry`** (Req 10.1).
/// L'ordinamento per chiave (`BTreeMap`) garantisce iterazione deterministica,
/// utile a diff e report riproducibili.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ProvenanceStore {
    records: BTreeMap<(SymbolId, TargetPair), ProvenanceRecord>,
}

impl ProvenanceStore {
    /// Crea uno store vuoto.
    pub fn new() -> Self {
        Self::default()
    }

    /// Costruisce lo store da un [`BindingCatalog`], creando **un record per
    /// ogni offset di ogni `Catalog_Entry`** (Req 10.1).
    ///
    /// I record inline già presenti nel TOML (`[offset.provenance]`) vengono
    /// preservati così come sono: lo store è la loro vista in memoria, non li
    /// ricalcola.
    pub fn from_catalog(catalog: &BindingCatalog) -> Self {
        let mut store = Self::new();
        for entry in &catalog.entries {
            for offset in &entry.offsets {
                store.records.insert(
                    (entry.symbol.clone(), offset.pair),
                    offset.provenance.clone(),
                );
            }
        }
        store
    }

    /// Numero di record nello store (uno per offset registrato).
    pub fn len(&self) -> usize {
        self.records.len()
    }

    /// `true` se lo store non contiene alcun record.
    pub fn is_empty(&self) -> bool {
        self.records.is_empty()
    }

    /// Itera i record `((symbol, pair), record)` in ordine di chiave.
    pub fn iter(&self) -> impl Iterator<Item = (&(SymbolId, TargetPair), &ProvenanceRecord)> {
        self.records.iter()
    }

    /// **Lettura** del `Provenance_Record` di un offset (Req 10.4).
    ///
    /// Restituisce il record **completo** così com'è memorizzato, **senza
    /// rieseguire** l'`Observational_Cross_Check` né la `Prologue_Verification`:
    /// è una pura lettura dallo store, non innesca alcuna ricomputazione.
    pub fn get(&self, symbol: &SymbolId, pair: TargetPair) -> Option<&ProvenanceRecord> {
        self.records.get(&(symbol.clone(), pair))
    }

    /// Restituisce (creandolo vuoto se assente) il record mutabile per la
    /// coppia `(symbol, pair)`. Garantisce l'invariante "un record per offset".
    fn entry_mut(&mut self, symbol: &SymbolId, pair: TargetPair) -> &mut ProvenanceRecord {
        self.records
            .entry((symbol.clone(), pair))
            .or_insert_with(|| ProvenanceRecord::empty(symbol.clone(), pair))
    }

    /// Registra la **fonte dell'`Address_Data`** nel record dell'offset
    /// (Req 2.2, 10.1). Crea il record (vuoto) se non esiste ancora.
    pub fn record_address_source(
        &mut self,
        symbol: &SymbolId,
        pair: TargetPair,
        source: impl Into<String>,
    ) -> &ProvenanceRecord {
        let record = self.entry_mut(symbol, pair);
        record.address_source = Some(source.into());
        record
    }

    /// Registra l'**esito dell'`Observational_Cross_Check`** (Req 4.3/4.4) e il
    /// flag `cross_check_no_reuse` (Req 4.6) nel record dell'offset.
    pub fn record_cross_check(
        &mut self,
        symbol: &SymbolId,
        pair: TargetPair,
        outcome: CrossCheckOutcome,
        no_reuse: bool,
    ) -> &ProvenanceRecord {
        let record = self.entry_mut(symbol, pair);
        record.cross_check = Some(outcome.as_str().to_owned());
        record.cross_check_no_reuse = no_reuse;
        record
    }

    /// Registra l'**esito della `Prologue_Verification` con il metodo usato**,
    /// **sia in successo sia in fallimento** (Req 5.5).
    ///
    /// Scrive nel record le stringhe canoniche di [`PrologueMethod::as_str`] e
    /// [`PrologueOutcome::as_str`]; vale per `Match`, `Mismatch` e `Unreadable`.
    pub fn record_prologue(
        &mut self,
        symbol: &SymbolId,
        pair: TargetPair,
        method: PrologueMethod,
        outcome: PrologueOutcome,
    ) -> &ProvenanceRecord {
        let record = self.entry_mut(symbol, pair);
        record.prologue_method = Some(method.as_str().to_owned());
        record.prologue_outcome = Some(outcome.as_str().to_owned());
        record
    }

    /// Convenienza: registra l'esito di una [`PrologueVerification`] riuscita
    /// (metodo + esito), tipicamente subito dopo `evaluate_prologue` (Req 5.5).
    pub fn record_prologue_verification(
        &mut self,
        symbol: &SymbolId,
        pair: TargetPair,
        verification: &PrologueVerification,
    ) -> &ProvenanceRecord {
        self.record_prologue(symbol, pair, verification.method, verification.outcome)
    }

    /// Registra la **transizione `Verified_Flag → true`** (Req 10.2).
    ///
    /// Alla transizione, **prima** di esporre l'offset come distribuibile,
    /// registra nel `Provenance_Record`:
    ///
    /// 1. la **fonte** dell'`Address_Data`,
    /// 2. l'**esito della `Prologue_Verification`** (con il metodo, dalla
    ///    [`PrologueVerification`] che ha appena reso `verified == true`),
    /// 3. l'**esito dell'`Observational_Cross_Check`** (con il flag
    ///    `no_reuse`, Req 4.6).
    ///
    /// Restituisce il record completo: poiché tutti e tre gli esiti sono scritti
    /// **prima** del ritorno, ogni lettura successiva (Req 10.4) o controllo di
    /// distribuibilità vede un record già completo. La transizione presuppone un
    /// esito di prologo "match" (`verification.verified == true`): questo è
    /// l'unico caso in cui il `Verified_Flag` passa a vero (Req 5.2, 5.4).
    pub fn record_verified_transition(
        &mut self,
        symbol: &SymbolId,
        pair: TargetPair,
        address_source: impl Into<String>,
        cross_check: CrossCheckOutcome,
        cross_check_no_reuse: bool,
        verification: &PrologueVerification,
    ) -> &ProvenanceRecord {
        let record = self.entry_mut(symbol, pair);
        // Tutti e tre gli esiti registrati PRIMA di esporre come distribuibile.
        record.address_source = Some(address_source.into());
        record.cross_check = Some(cross_check.as_str().to_owned());
        record.cross_check_no_reuse = cross_check_no_reuse;
        record.prologue_method = Some(verification.method.as_str().to_owned());
        record.prologue_outcome = Some(verification.outcome.as_str().to_owned());
        record
    }

    /// Variante **gated** della transizione `Verified_Flag → true` (Req 10.5).
    ///
    /// A differenza di [`ProvenanceStore::record_verified_transition`] — che
    /// **scrive** fonte ed esiti a partire dagli argomenti, e per costruzione
    /// produce sempre un record completo — questa variante presuppone che gli
    /// esiti siano **già stati registrati** nel record (es. via
    /// [`ProvenanceStore::record_cross_check`] e
    /// [`ProvenanceStore::record_prologue`]) e applica il **gate di completezza**
    /// [`gate_verified`] **prima** di consentire la transizione:
    ///
    /// - se il record documenta **sia** il cross-check **sia** il prologo →
    ///   `Ok(record)`: la transizione è consentita e il chiamante può esporre
    ///   l'offset come distribuibile;
    /// - altrimenti → `Err(AuditabilityError)` con simbolo+coppia e l'esito
    ///   mancante; **nessuna mutazione** viene applicata, quindi l'`Address_Data`
    ///   registrato resta invariato (Req 10.3).
    ///
    /// Un offset privo di record è equiparato a provenienza del tutto assente
    /// ([`MissingProvenance::Both`]).
    pub fn try_verified_transition(
        &mut self,
        symbol: &SymbolId,
        pair: TargetPair,
    ) -> Result<&ProvenanceRecord, AuditabilityError> {
        // Sola lettura: il gate non muta nulla (Address_Data invariato, Req 10.3).
        match self.get(symbol, pair) {
            Some(record) => gate_verified(record)?,
            None => {
                return Err(AuditabilityError {
                    symbol: symbol.clone(),
                    pair,
                    missing: MissingProvenance::Both,
                })
            }
        }
        // Il gate è superato: restituisci il record (ora noto completo).
        Ok(self
            .get(symbol, pair)
            .expect("record presente: appena letto sopra"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bindings::catalog::{CatalogEntry, OffsetRecord};
    use crate::bindings::prologue::{evaluate_prologue, expected_prologue_bytes};
    use crate::bindings::{GdVersion, Signature, TargetPlatform};

    const RVA_VALID: u64 = 0x316688;

    fn symbol() -> SymbolId {
        SymbolId::new("MenuLayer::init")
    }

    fn signature() -> Signature {
        Signature::new("bool", vec!["MenuLayer*".to_owned()])
    }

    fn pair_arm() -> TargetPair {
        TargetPair::new(GdVersion::new(2, 2081), TargetPlatform::MacosArm64)
    }

    fn pair_x64() -> TargetPair {
        TargetPair::new(GdVersion::new(2, 2081), TargetPlatform::MacosX64)
    }

    /// `CatalogEntry` con due offset: arm64 con RVA valido + provenienza inline,
    /// x64 senza RVA (sentinel) con provenienza vuota.
    fn entry_two_offsets() -> CatalogEntry {
        let inline_prov = ProvenanceRecord {
            symbol: symbol(),
            pair: pair_arm(),
            address_source: Some("contributor:tomas".to_owned()),
            cross_check: Some("concordant".to_owned()),
            cross_check_no_reuse: true,
            prologue_method: Some("otool-manual".to_owned()),
            prologue_outcome: Some("match".to_owned()),
        };
        CatalogEntry {
            symbol: symbol(),
            signature: signature(),
            offsets: vec![
                OffsetRecord {
                    pair: pair_arm(),
                    rva: Some(RVA_VALID),
                    verified: true,
                    provenance: inline_prov,
                },
                OffsetRecord {
                    pair: pair_x64(),
                    rva: None,
                    verified: false,
                    provenance: ProvenanceRecord::empty(symbol(), pair_x64()),
                },
            ],
        }
    }

    // -----------------------------------------------------------------------
    // Req 10.1 — un record per ogni offset di ogni Catalog_Entry.
    // -----------------------------------------------------------------------

    #[test]
    fn from_catalog_keeps_one_record_per_offset() {
        let catalog = BindingCatalog {
            entries: vec![entry_two_offsets()],
        };
        let store = ProvenanceStore::from_catalog(&catalog);

        // Due offset → due record.
        assert_eq!(store.len(), 2);
        assert!(store.get(&symbol(), pair_arm()).is_some());
        assert!(store.get(&symbol(), pair_x64()).is_some());
    }

    #[test]
    fn cross_check_outcome_round_trips_canonical_strings() {
        for outcome in [
            CrossCheckOutcome::Concordant,
            CrossCheckOutcome::Discordant,
            CrossCheckOutcome::Rejected,
        ] {
            assert_eq!(CrossCheckOutcome::from_str(outcome.as_str()), Some(outcome));
        }
        assert_eq!(CrossCheckOutcome::from_str("garbage"), None);
    }

    // -----------------------------------------------------------------------
    // Req 5.5 — esito del prologo registrato sia in successo sia in fallimento,
    // con il metodo usato.
    // -----------------------------------------------------------------------

    #[test]
    fn records_prologue_success_with_method() {
        let mut store = ProvenanceStore::new();
        store.record_prologue(
            &symbol(),
            pair_arm(),
            PrologueMethod::AutomaticBytes,
            PrologueOutcome::Match,
        );

        let record = store.get(&symbol(), pair_arm()).expect("record presente");
        assert_eq!(record.prologue_method.as_deref(), Some("automatic-bytes"));
        assert_eq!(record.prologue_outcome.as_deref(), Some("match"));
    }

    #[test]
    fn records_prologue_failure_with_method() {
        let mut store = ProvenanceStore::new();
        // Fallimento: il metodo è comunque registrato (Req 5.5).
        store.record_prologue(
            &symbol(),
            pair_arm(),
            PrologueMethod::OtoolManual,
            PrologueOutcome::Mismatch,
        );

        let record = store.get(&symbol(), pair_arm()).expect("record presente");
        assert_eq!(record.prologue_method.as_deref(), Some("otool-manual"));
        assert_eq!(record.prologue_outcome.as_deref(), Some("mismatch"));

        // Anche un esito Unreadable è registrato con il suo metodo.
        store.record_prologue(
            &symbol(),
            pair_x64(),
            PrologueMethod::AutomaticBytes,
            PrologueOutcome::Unreadable,
        );
        let rec_x64 = store.get(&symbol(), pair_x64()).expect("record presente");
        assert_eq!(rec_x64.prologue_outcome.as_deref(), Some("unreadable"));
    }

    #[test]
    fn records_prologue_verification_result_from_evaluate() {
        // Integra evaluate_prologue: l'esito riuscito viene registrato (Req 5.5).
        let entry = entry_two_offsets();
        let bytes = expected_prologue_bytes(&signature());
        let verification = evaluate_prologue(
            &entry,
            pair_arm(),
            PrologueMethod::AutomaticBytes,
            Some(&bytes),
        )
        .expect("prologo conforme");

        let mut store = ProvenanceStore::new();
        store.record_prologue_verification(&symbol(), pair_arm(), &verification);

        let record = store.get(&symbol(), pair_arm()).unwrap();
        assert_eq!(record.prologue_outcome.as_deref(), Some("match"));
        assert_eq!(record.prologue_method.as_deref(), Some("automatic-bytes"));
    }

    // -----------------------------------------------------------------------
    // Req 10.2 — la transizione Verified_Flag → true registra tutti e tre gli
    // esiti (fonte, prologo, cross-check) PRIMA di esporre come distribuibile.
    // -----------------------------------------------------------------------

    #[test]
    fn verified_transition_records_all_three_before_distributable() {
        let entry = entry_two_offsets();
        let bytes = expected_prologue_bytes(&signature());
        let verification = evaluate_prologue(
            &entry,
            pair_arm(),
            PrologueMethod::AutomaticBytes,
            Some(&bytes),
        )
        .expect("prologo conforme → verified true");
        assert!(verification.verified);

        let mut store = ProvenanceStore::new();
        let record = store.record_verified_transition(
            &symbol(),
            pair_arm(),
            "contributor:tomas",
            CrossCheckOutcome::Concordant,
            true,
            &verification,
        );

        // Tutti e tre gli esiti presenti nel record restituito dalla transizione
        // (cioè PRIMA di qualunque passo di distribuzione a valle).
        assert_eq!(record.address_source.as_deref(), Some("contributor:tomas"));
        assert_eq!(record.cross_check.as_deref(), Some("concordant"));
        assert!(record.cross_check_no_reuse);
        assert_eq!(record.prologue_outcome.as_deref(), Some("match"));
        assert_eq!(record.prologue_method.as_deref(), Some("automatic-bytes"));

        // Fondamenta del gate (7.4): il record è completo.
        assert!(has_cross_check_outcome(record));
        assert!(has_prologue_outcome(record));
    }

    // -----------------------------------------------------------------------
    // Req 10.4 — la lettura di un record distribuito lo restituisce completo
    // senza rieseguire cross-check/prologo.
    // -----------------------------------------------------------------------

    #[test]
    fn reading_distributed_record_returns_it_complete_without_recompute() {
        // Lo store è costruito dal catalogo (provenienza inline già completa per
        // l'offset arm64 distribuito): la lettura restituisce il record così
        // com'è, senza alcuna ricomputazione di cross-check/prologo.
        let catalog = BindingCatalog {
            entries: vec![entry_two_offsets()],
        };
        let store = ProvenanceStore::from_catalog(&catalog);

        let record = store
            .get(&symbol(), pair_arm())
            .expect("record dell'offset distribuito");

        // Completo: fonte, cross-check ed esito del prologo come da TOML inline.
        assert_eq!(record.address_source.as_deref(), Some("contributor:tomas"));
        assert_eq!(record.cross_check.as_deref(), Some("concordant"));
        assert!(record.cross_check_no_reuse);
        assert_eq!(record.prologue_method.as_deref(), Some("otool-manual"));
        assert_eq!(record.prologue_outcome.as_deref(), Some("match"));
        assert_eq!(record.symbol, symbol());
        assert_eq!(record.pair, pair_arm());

        // Una seconda lettura è identica e idempotente (nessun side effect).
        let again = store.get(&symbol(), pair_arm()).unwrap();
        assert_eq!(record, again);
    }

    #[test]
    fn records_address_source_and_cross_check_individually() {
        let mut store = ProvenanceStore::new();
        store.record_address_source(&symbol(), pair_arm(), "contributor:tomas");
        store.record_cross_check(&symbol(), pair_arm(), CrossCheckOutcome::Discordant, true);

        let record = store.get(&symbol(), pair_arm()).unwrap();
        assert_eq!(record.address_source.as_deref(), Some("contributor:tomas"));
        assert_eq!(record.cross_check.as_deref(), Some("discordant"));
        assert!(record.cross_check_no_reuse);
        // Il prologo non è ancora stato registrato.
        assert!(!has_prologue_outcome(record));
    }

    #[test]
    fn get_on_unknown_offset_returns_none() {
        let store = ProvenanceStore::new();
        assert!(store.get(&symbol(), pair_arm()).is_none());
    }

    // -----------------------------------------------------------------------
    // Req 10.5 — gate di completezza: il passaggio Verified_Flag → true è
    // impedito se il record non documenta SIA il cross-check SIA il prologo.
    // -----------------------------------------------------------------------

    #[test]
    fn gate_refuses_verified_when_cross_check_missing() {
        // Solo prologo documentato: manca il cross-check → gate rifiuta.
        let mut record = ProvenanceRecord::empty(symbol(), pair_arm());
        record.prologue_method = Some("otool-manual".to_owned());
        record.prologue_outcome = Some("match".to_owned());

        let err = gate_verified(&record).expect_err("gate deve rifiutare");
        assert_eq!(err.symbol, symbol());
        assert_eq!(err.pair, pair_arm());
        assert_eq!(err.missing, MissingProvenance::CrossCheck);
    }

    #[test]
    fn gate_refuses_verified_when_prologue_missing() {
        // Solo cross-check documentato: manca il prologo → gate rifiuta.
        let mut record = ProvenanceRecord::empty(symbol(), pair_arm());
        record.cross_check = Some("concordant".to_owned());
        record.cross_check_no_reuse = true;

        let err = gate_verified(&record).expect_err("gate deve rifiutare");
        assert_eq!(err.symbol, symbol());
        assert_eq!(err.pair, pair_arm());
        assert_eq!(err.missing, MissingProvenance::Prologue);
    }

    #[test]
    fn gate_refuses_verified_when_both_missing() {
        let record = ProvenanceRecord::empty(symbol(), pair_arm());
        let err = gate_verified(&record).expect_err("gate deve rifiutare");
        assert_eq!(err.missing, MissingProvenance::Both);
    }

    #[test]
    fn gate_passes_verified_when_both_outcomes_present() {
        // Entrambi gli esiti documentati → gate consente la transizione.
        let mut record = ProvenanceRecord::empty(symbol(), pair_arm());
        record.cross_check = Some("concordant".to_owned());
        record.prologue_outcome = Some("match".to_owned());

        assert!(gate_verified(&record).is_ok());
    }

    #[test]
    fn gated_transition_via_store_refuses_until_complete() {
        let mut store = ProvenanceStore::new();

        // Offset sconosciuto → trattato come provenienza del tutto assente.
        let err = store
            .try_verified_transition(&symbol(), pair_arm())
            .expect_err("nessun record → rifiuto");
        assert_eq!(err.missing, MissingProvenance::Both);

        // Registrato solo il cross-check → manca ancora il prologo.
        store.record_cross_check(&symbol(), pair_arm(), CrossCheckOutcome::Concordant, true);
        let err = store
            .try_verified_transition(&symbol(), pair_arm())
            .expect_err("manca il prologo → rifiuto");
        assert_eq!(err.missing, MissingProvenance::Prologue);

        // Registrato anche il prologo → gate superato.
        store.record_prologue(
            &symbol(),
            pair_arm(),
            PrologueMethod::OtoolManual,
            PrologueOutcome::Match,
        );
        let record = store
            .try_verified_transition(&symbol(), pair_arm())
            .expect("entrambi gli esiti presenti → transizione consentita");
        assert!(has_cross_check_outcome(record));
        assert!(has_prologue_outcome(record));
    }

    // -----------------------------------------------------------------------
    // Req 10.3 — un offset verified = true privo dell'esito del prologo è
    // trattato come non distribuibile, senza alterare l'Address_Data.
    // -----------------------------------------------------------------------

    #[test]
    fn verified_offset_missing_prologue_is_not_distributable_and_address_data_unchanged() {
        // verified = true ma esito del prologo assente (solo cross-check + fonte).
        let mut record = ProvenanceRecord::empty(symbol(), pair_arm());
        record.address_source = Some("contributor:tomas".to_owned());
        record.cross_check = Some("concordant".to_owned());

        // Snapshot dell'Address_Data prima del gate.
        let address_before = record.address_source.clone();

        let err = gate_distributable(true, &record)
            .expect_err("verified=true senza prologo → errore di auditabilità");
        // Errore con simbolo + coppia (Req 10.3).
        assert_eq!(err.symbol, symbol());
        assert_eq!(err.pair, pair_arm());
        assert_eq!(err.missing, MissingProvenance::Prologue);

        // Address_Data invariato: il gate non muta il record (Req 10.3).
        assert_eq!(record.address_source, address_before);
        assert_eq!(record.address_source.as_deref(), Some("contributor:tomas"));
    }

    #[test]
    fn verified_offset_with_prologue_is_distributable() {
        let mut record = ProvenanceRecord::empty(symbol(), pair_arm());
        record.address_source = Some("contributor:tomas".to_owned());
        record.cross_check = Some("concordant".to_owned());
        record.prologue_outcome = Some("match".to_owned());

        assert!(gate_distributable(true, &record).is_ok());
    }

    #[test]
    fn unverified_offset_missing_prologue_is_not_an_audit_error() {
        // verified = false: il gate di distribuibilità non impone nulla.
        let record = ProvenanceRecord::empty(symbol(), pair_arm());
        assert!(gate_distributable(false, &record).is_ok());
    }

    #[test]
    fn record_verified_transition_satisfies_the_gate() {
        // La transizione "scrivente" produce sempre un record completo: il gate
        // applicato a posteriori deve passare (coerenza tra i due percorsi).
        let entry = entry_two_offsets();
        let bytes = expected_prologue_bytes(&signature());
        let verification = evaluate_prologue(
            &entry,
            pair_arm(),
            PrologueMethod::AutomaticBytes,
            Some(&bytes),
        )
        .expect("prologo conforme");

        let mut store = ProvenanceStore::new();
        let record = store
            .record_verified_transition(
                &symbol(),
                pair_arm(),
                "contributor:tomas",
                CrossCheckOutcome::Concordant,
                true,
                &verification,
            )
            .clone();

        assert!(gate_verified(&record).is_ok());
        assert!(gate_distributable(true, &record).is_ok());
    }
}
