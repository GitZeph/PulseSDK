//! `Surface_Validator` — gate di **completezza della provenienza** per coppia
//! (Req 8.1, 8.2).
//!
//! La superficie espone un `API_Element` come **risolvibile** per una coppia
//! `(GD_Version, Target_Platform)` **se e solo se** il binding sottostante è
//! verificato **e** la sua provenienza è completa. Questo modulo realizza il
//! secondo controllo **riusando** il gate della `bindings-pipeline` —
//! [`gate_distributable`](crate::bindings::provenance::gate_distributable) — e il
//! suo errore [`AuditabilityError`](crate::bindings::provenance::AuditabilityError):
//! la superficie **non re-implementa** la nozione di provenienza completa, la
//! **riusa**.
//!
//! ## Cosa fa (per ogni `API_Element`, per ogni coppia)
//!
//! Per ogni elemento della [`SurfaceIr`], cercata la sua `Catalog_Entry` nel
//! [`BindingCatalog`] (per `SymbolId` canonico), si scorre **ogni** offset della
//! voce — cioè ogni coppia — e si classifica la risolvibilità leggendo
//! **esclusivamente** il binding di quella coppia (nessuna derivazione
//! cross-coppia):
//!
//! - **`verified = true` + provenienza completa** → l'elemento è **risolvibile**
//!   per quella coppia e gli si associa un riferimento al `Provenance_Record`
//!   ([`ProvenanceRef`] con `complete = true`, Req 8.1);
//! - **`verified = true` ma provenienza incompleta** → l'elemento è **declassato
//!   a non risolvibile** per quella coppia e si registra un
//!   [`AuditabilityError`] che identifica **simbolo + coppia** (Req 8.2). È
//!   esattamente l'esito di [`gate_distributable(true, record)`](crate::bindings::provenance::gate_distributable);
//! - **`verified = false`** → l'elemento è semplicemente **non risolvibile** per
//!   quella coppia, **senza** errore: è il fail-closed ereditato (un binding non
//!   verificato non è mai esposto), e [`gate_distributable(false, …)`](crate::bindings::provenance::gate_distributable)
//!   non impone nulla.
//!
//! In breve, per ciascuna coppia:
//!
//! ```text
//! risolvibile  ⟺  verified == true  ∧  gate_distributable(verified, prov).is_ok()
//! errore audit ⟺  verified == true  ∧  gate_distributable(verified, prov).is_err()
//! ```
//!
//! ## Confine con il runtime (task 13.x)
//!
//! La risolvibilità per coppia è **pienamente popolata a runtime** dal loader
//! (task 13.x), che conosce la coppia del `RuntimeContext`. A **build-time** il
//! validator non ha un `RuntimeContext`: valida quindi l'elemento **contro i dati
//! di offset/provenienza del catalogo** per **tutte** le coppie note della
//! `Catalog_Entry`, anticipando il medesimo verdetto che il runtime
//! confermerebbe per la coppia attiva. Il gate riusato è lo **stesso**, perciò il
//! verdetto è coerente fra build-time e runtime.
//!
//! ## Disciplina fail-closed
//!
//! Il validator **non produce artefatti**: classifica e riporta. Un
//! [`ValidationOutcome`] con [`ValidationOutcome::auditability_errors`] non vuoto
//! segnala al chiamante che la superficie contiene elementi declassati e che la
//! provenienza va corretta prima di considerare quelle coppie risolvibili.
//!
//! _Requisiti: 8.1, 8.2._

use crate::bindings::catalog::{BindingCatalog, CatalogEntry};
use crate::bindings::provenance::gate_distributable;

use super::ir::{ProvenanceRef, SurfaceIr};
use super::{AuditabilityError, SurfaceError, SymbolId, TargetPair};

/// Classificazione di un `API_Element` per una specifica coppia `(GD_Version,
/// Target_Platform)`, prodotta dal [`Surface_Validator`](validate_surface).
///
/// È il verdetto **per coppia** richiesto da Req 7/8: `resolvable` riflette il
/// gate riusato e — quando risolvibile — `provenance` porta il riferimento al
/// `Provenance_Record` completo (Req 8.1).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ElementClassification {
    /// Simbolo canonico dell'elemento classificato.
    pub symbol: SymbolId,
    /// Coppia `(GD_Version, Target_Platform)` a cui si riferisce il verdetto.
    pub pair: TargetPair,
    /// `true` sse il binding di `pair` è verificato **e** la provenienza è
    /// completa (Req 8.1). Un elemento `verified = true` ma con provenienza
    /// incompleta è **declassato** a `false` (Req 8.2).
    pub resolvable: bool,
    /// Riferimento di provenienza per la coppia: `Some(complete = true)` quando
    /// l'elemento è risolvibile (Req 8.1); `None` quando non risolvibile.
    pub provenance: Option<ProvenanceRef>,
}

/// Esito della validazione della superficie: le classificazioni per coppia di
/// ogni `API_Element` e gli errori di auditabilità raccolti.
///
/// Deriva solo `Debug`: trasporta [`SurfaceError`] (non `Clone`/`PartialEq`) per
/// le conversioni; le classificazioni e gli [`AuditabilityError`] (che invece
/// sono `Clone`/`PartialEq`) restano ispezionabili direttamente nei test.
#[derive(Debug)]
pub struct ValidationOutcome {
    /// Verdetto per ogni `(API_Element, coppia)`, in ordine di scoperta
    /// (elementi nell'ordine della IR, coppie nell'ordine degli offset).
    pub classifications: Vec<ElementClassification>,
    /// Errori di auditabilità (simbolo + coppia) per gli elementi `verified =
    /// true` declassati per provenienza incompleta (Req 8.2), in ordine di
    /// scoperta.
    pub auditability_errors: Vec<AuditabilityError>,
}

impl ValidationOutcome {
    /// `true` se nessun elemento è stato declassato per auditabilità: ogni
    /// elemento verificato ha provenienza completa (Req 8.1).
    pub fn is_clean(&self) -> bool {
        self.auditability_errors.is_empty()
    }

    /// Le classificazioni risolvibili (Req 8.1), comode per i report.
    pub fn resolvable(&self) -> impl Iterator<Item = &ElementClassification> {
        self.classifications.iter().filter(|c| c.resolvable)
    }

    /// Gli errori di auditabilità mappati nella variante
    /// [`SurfaceError::Auditability`], per la propagazione fail-closed verso il
    /// chiamante (es. il sottocomando `pulse surface validate`, task 11.1).
    ///
    /// [`AuditabilityError`] è `Clone`, quindi i verdetti restano disponibili in
    /// [`ValidationOutcome::auditability_errors`] anche dopo questa conversione.
    pub fn to_surface_errors(&self) -> Vec<SurfaceError> {
        self.auditability_errors
            .iter()
            .cloned()
            .map(SurfaceError::Auditability)
            .collect()
    }
}

/// Cerca la [`CatalogEntry`] per `SymbolId` (corrispondenza **esatta**), come nel
/// compiler: scansione lineare coerente con la risoluzione del loader.
fn find_entry<'a>(catalog: &'a BindingCatalog, symbol: &SymbolId) -> Option<&'a CatalogEntry> {
    catalog.entries.iter().find(|entry| &entry.symbol == symbol)
}

/// Valida la [`SurfaceIr`] contro il [`BindingCatalog`], applicando il **gate di
/// completezza della provenienza** per ogni `(API_Element, coppia)` (Req 8.1,
/// 8.2).
///
/// Per ogni elemento della IR cerca la `Catalog_Entry` corrispondente; per ogni
/// suo offset (coppia) chiama
/// [`gate_distributable(verified, &provenance)`](crate::bindings::provenance::gate_distributable)
/// e ne deriva il verdetto:
///
/// - `verified && gate.is_ok()` → **risolvibile** con [`ProvenanceRef`] completo;
/// - `verified && gate.is_err()` → **declassato** a non risolvibile + errore di
///   auditabilità con simbolo+coppia;
/// - `!verified` → **non risolvibile** senza errore.
///
/// Un elemento privo di `Catalog_Entry` non genera classificazioni (la sua
/// esclusione è già di competenza del `Surface_Linter`, Req 1.5).
pub fn validate_surface(ir: &SurfaceIr, catalog: &BindingCatalog) -> ValidationOutcome {
    let mut classifications: Vec<ElementClassification> = Vec::new();
    let mut auditability_errors: Vec<AuditabilityError> = Vec::new();

    for element in &ir.elements {
        let symbol = &element.symbol.canonical;
        let Some(entry) = find_entry(catalog, symbol) else {
            // Senza Catalog_Entry non c'è offset/provenienza da validare: la
            // segnalazione del simbolo mancante è del linter (Req 1.5), non qui.
            continue;
        };

        for offset in &entry.offsets {
            let pair = offset.pair;
            let verified = offset.verified;

            // RIUSO del gate della pipeline: nessuna re-implementazione della
            // completezza. Per verified = false il gate è sempre Ok, ma
            // l'elemento resta comunque non risolvibile (fail-closed).
            match gate_distributable(verified, &offset.provenance) {
                Ok(()) if verified => {
                    // Req 8.1: risolvibile ⇒ riferimento al Provenance_Record
                    // completo per quella coppia.
                    classifications.push(ElementClassification {
                        symbol: symbol.clone(),
                        pair,
                        resolvable: true,
                        provenance: Some(ProvenanceRef {
                            pair,
                            complete: true,
                        }),
                    });
                }
                Ok(()) => {
                    // verified = false: non risolvibile, senza errore (Req 8.2,
                    // fail-closed). Nessun riferimento di provenienza.
                    classifications.push(ElementClassification {
                        symbol: symbol.clone(),
                        pair,
                        resolvable: false,
                        provenance: None,
                    });
                }
                Err(error) => {
                    // Req 8.2: verified = true ma provenienza incompleta ⇒
                    // DECLASSATO a non risolvibile + errore di auditabilità che
                    // identifica simbolo + coppia.
                    auditability_errors.push(error);
                    classifications.push(ElementClassification {
                        symbol: symbol.clone(),
                        pair,
                        resolvable: false,
                        provenance: None,
                    });
                }
            }
        }
    }

    ValidationOutcome {
        classifications,
        auditability_errors,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bindings::catalog::{CatalogEntry, OffsetRecord, ProvenanceRecord};
    use crate::bindings::provenance::MissingProvenance;
    use crate::bindings::{GdVersion, Signature, TargetPlatform};
    use crate::surface::ir::{ApiElement, CanonicalSignature, ClassBinding, SurfaceIr};
    use crate::surface::SurfaceSymbol;

    const RVA_VALID: u64 = 0x316688;

    fn symbol() -> SymbolId {
        SymbolId::new("MenuLayer::init")
    }

    fn pair_arm() -> TargetPair {
        TargetPair::new(GdVersion::new(2, 2081), TargetPlatform::MacosArm64)
    }

    /// `Provenance_Record` completo (cross-check + prologo documentati).
    fn complete_provenance(pair: TargetPair) -> ProvenanceRecord {
        ProvenanceRecord {
            symbol: symbol(),
            pair,
            address_source: Some("contributor:tomas".to_owned()),
            cross_check: Some("concordant".to_owned()),
            cross_check_no_reuse: true,
            prologue_method: Some("otool-manual".to_owned()),
            prologue_outcome: Some("match".to_owned()),
        }
    }

    /// `Provenance_Record` incompleto: manca l'esito del prologo (ciò che
    /// `gate_distributable` controlla).
    fn incomplete_provenance(pair: TargetPair) -> ProvenanceRecord {
        ProvenanceRecord {
            symbol: symbol(),
            pair,
            address_source: Some("contributor:tomas".to_owned()),
            cross_check: Some("concordant".to_owned()),
            cross_check_no_reuse: true,
            prologue_method: None,
            prologue_outcome: None,
        }
    }

    /// Costruisce una `SurfaceIr` con un solo elemento `MenuLayer::init`.
    fn ir_with_menulayer_init() -> SurfaceIr {
        SurfaceIr {
            classes: vec![ClassBinding {
                name: "MenuLayer".to_owned(),
                methods: vec![symbol()],
            }],
            elements: vec![ApiElement {
                symbol: SurfaceSymbol::from_canonical(symbol()).unwrap(),
                class_name: "MenuLayer".to_owned(),
                signature: CanonicalSignature {
                    return_gd: "bool".to_owned(),
                    param_gds: vec!["MenuLayer*".to_owned()],
                },
                priority: 100,
                is_hook_point: true,
                resolvability: Vec::new(),
                provenance: Vec::new(),
            }],
            type_overrides: Vec::new(),
        }
    }

    /// Catalogo con un singolo offset (coppia/verified/provenienza forniti).
    fn catalog_with_offset(
        verified: bool,
        rva: Option<u64>,
        provenance: ProvenanceRecord,
    ) -> BindingCatalog {
        BindingCatalog {
            entries: vec![CatalogEntry {
                symbol: symbol(),
                signature: Signature::new("bool", vec!["MenuLayer*".to_owned()]),
                offsets: vec![OffsetRecord {
                    pair: pair_arm(),
                    rva,
                    verified,
                    provenance,
                }],
            }],
        }
    }

    #[test]
    fn complete_provenance_makes_element_resolvable_with_reference() {
        // Req 8.1: verified + provenienza completa ⇒ risolvibile con riferimento
        // al Provenance_Record completo per quella coppia.
        let catalog = catalog_with_offset(true, Some(RVA_VALID), complete_provenance(pair_arm()));
        let ir = ir_with_menulayer_init();

        let outcome = validate_surface(&ir, &catalog);

        assert!(outcome.is_clean(), "nessun errore di auditabilità atteso");
        assert_eq!(outcome.classifications.len(), 1);

        let c = &outcome.classifications[0];
        assert_eq!(c.symbol, symbol());
        assert_eq!(c.pair, pair_arm());
        assert!(c.resolvable, "deve essere risolvibile");
        let prov = c.provenance.as_ref().expect("riferimento di provenienza");
        assert_eq!(prov.pair, pair_arm());
        assert!(prov.complete, "il Provenance_Record è completo");
    }

    #[test]
    fn verified_but_incomplete_provenance_is_demoted_with_auditability_error() {
        // Req 8.2: verified = true ma provenienza incompleta ⇒ declassato a non
        // risolvibile + errore di auditabilità che nomina simbolo + coppia.
        let catalog =
            catalog_with_offset(true, Some(RVA_VALID), incomplete_provenance(pair_arm()));
        let ir = ir_with_menulayer_init();

        let outcome = validate_surface(&ir, &catalog);

        // Declassato: non risolvibile e senza riferimento di provenienza.
        assert_eq!(outcome.classifications.len(), 1);
        let c = &outcome.classifications[0];
        assert!(!c.resolvable, "deve essere declassato a non risolvibile");
        assert!(c.provenance.is_none());

        // Errore di auditabilità con simbolo + coppia.
        assert!(!outcome.is_clean());
        assert_eq!(outcome.auditability_errors.len(), 1);
        let err = &outcome.auditability_errors[0];
        assert_eq!(err.symbol, symbol());
        assert_eq!(err.pair, pair_arm());
        assert_eq!(err.missing, MissingProvenance::Prologue);

        // E la conversione nella variante SurfaceError::Auditability è disponibile.
        let surface_errors = outcome.to_surface_errors();
        assert_eq!(surface_errors.len(), 1);
        assert!(matches!(
            surface_errors[0],
            SurfaceError::Auditability(_)
        ));
    }

    #[test]
    fn unverified_element_is_non_resolvable_without_error() {
        // verified = false ⇒ semplicemente non risolvibile, nessun errore.
        let catalog = catalog_with_offset(
            false,
            None,
            ProvenanceRecord::empty(symbol(), pair_arm()),
        );
        let ir = ir_with_menulayer_init();

        let outcome = validate_surface(&ir, &catalog);

        assert!(outcome.is_clean(), "verified=false non genera errori");
        assert_eq!(outcome.classifications.len(), 1);
        let c = &outcome.classifications[0];
        assert!(!c.resolvable);
        assert!(c.provenance.is_none());
        assert!(outcome.auditability_errors.is_empty());
    }

    #[test]
    fn classifies_each_pair_independently() {
        // Req 7.2/8: la risolvibilità è derivata SOLO dal binding di ciascuna
        // coppia, senza propagazione cross-coppia.
        let pair_x64 = TargetPair::new(GdVersion::new(2, 2081), TargetPlatform::MacosX64);
        let catalog = BindingCatalog {
            entries: vec![CatalogEntry {
                symbol: symbol(),
                signature: Signature::new("bool", vec!["MenuLayer*".to_owned()]),
                offsets: vec![
                    // arm64: verificato + provenienza completa ⇒ risolvibile.
                    OffsetRecord {
                        pair: pair_arm(),
                        rva: Some(RVA_VALID),
                        verified: true,
                        provenance: complete_provenance(pair_arm()),
                    },
                    // x64: non verificato ⇒ non risolvibile, senza errore.
                    OffsetRecord {
                        pair: pair_x64,
                        rva: None,
                        verified: false,
                        provenance: ProvenanceRecord::empty(symbol(), pair_x64),
                    },
                ],
            }],
        };
        let ir = ir_with_menulayer_init();

        let outcome = validate_surface(&ir, &catalog);
        assert_eq!(outcome.classifications.len(), 2);

        let arm = outcome
            .classifications
            .iter()
            .find(|c| c.pair == pair_arm())
            .unwrap();
        let x64 = outcome
            .classifications
            .iter()
            .find(|c| c.pair == pair_x64)
            .unwrap();

        assert!(arm.resolvable, "arm64 risolvibile");
        assert!(!x64.resolvable, "x64 non risolvibile");
        assert!(outcome.is_clean(), "nessun verified+incompleto, nessun errore");
    }

    #[test]
    fn element_without_catalog_entry_yields_no_classification() {
        // Un elemento privo di Catalog_Entry non è validato qui (è il linter a
        // segnalarne l'assenza, Req 1.5).
        let catalog = BindingCatalog {
            entries: Vec::new(),
        };
        let ir = ir_with_menulayer_init();

        let outcome = validate_surface(&ir, &catalog);
        assert!(outcome.classifications.is_empty());
        assert!(outcome.is_clean());
    }
}
