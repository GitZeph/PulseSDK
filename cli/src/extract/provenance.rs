//! `ProvenanceTier` + `ExtractionProvenance` + `Geode_Concordance` (Phase F).
//!
//! Questo modulo realizza l'**onestà del catalogo** (Req 6, 7): ogni offset
//! emesso registra **con quale livello di evidenza** è stato ottenuto e l'intera
//! provenienza, e nessun offset è mai marcato `verified` senza che il suo esito
//! di prologo sia documentato. È **logica pura e deterministica**: non legge
//! file, non fa I/O, dipende solo dai suoi argomenti.
//!
//! Tre responsabilità complementari:
//!
//! 1. **Assegnazione del [`ProvenanceTier`]** ([`assign_tier`]) — totalità ed
//!    esclusività: a ogni offset `≠ SENTINEL_VALUE` esattamente un tier coerente
//!    con la sua origine; un offset sentinel non riceve alcun tier; **nessun**
//!    offset prodotto dall'automazione riceve `manual-prologue-confirmed`
//!    (Req 2.4, 3.5, 4.2, 5.6, 6.1, 6.6).
//! 2. **[`ExtractionProvenance`]** — estensione **additiva** del
//!    `[offset.provenance]` esistente con i campi di tiering, più il **gate di
//!    auditabilità** che rende **impossibile** emettere un offset `verified`
//!    senza un esito di prologo registrato (Req 1.7, 5.5, 6.2, 6.5, 9.3, 9.4).
//! 3. **[`GeodeConcordance`]** ([`geode_concordance`]) — il cross-check numerico
//!    **opzionale** instradato attraverso il **`Geode_Firewall` numerico-only
//!    RIUSATO** dalla pipeline, che **non** modifica mai il `Provenance_Tier`
//!    (Req 1.3, 1.4, 1.5, 1.6, 6.3).
//!
//! ## Mappatura **additiva** sul `Catalog_Entry` TOML esistente
//!
//! [`ExtractionProvenance`] **non** sostituisce lo schema core del catalogo
//! (`symbol`, `[signature]`, `[[offset]]` con `rva`/`verified`) che
//! `load_catalog` si aspetta: lo **estende** nella sola sezione opaca
//! `[offset.provenance]`. La riconciliazione con i campi del seed
//! `MenuLayer__init.toml` (`address_source`, `cross_check`,
//! `cross_check_no_reuse`, `prologue_method`, `prologue_outcome`) è:
//!
//! | Campo `[offset.provenance]` | Origine | Note |
//! | --- | --- | --- |
//! | `tier` | [`ProvenanceTier::as_str`] | nuovo campo additivo (Req 6.1) |
//! | `derivation_method` | [`ExtractionProvenance::derivation_method`] | nuovo additivo |
//! | `binary_identity` | [`ExtractionProvenance::identity_labels`] | nuovo additivo (Req 9.3) |
//! | `prologue_method` | [`ExtractionProvenance::prologue_method_label`] = `"auto-sanity"` | distinto da `"otool-manual"` del seed (Req 5.6) |
//! | `prologue_outcome` | [`ExtractionProvenance::prologue_outcome_label`] | stesso campo del seed (Req 5.5) |
//! | `cross_check` | [`ExtractionProvenance::geode_concordance_label`] | stesso oracolo numerico del seed (Req 6.3) |
//! | `cross_check_no_reuse` | [`ExtractionProvenance::geode_no_reuse`] = `true` | invariante condiviso (Req 1.7) |
//!
//! La scrittura vera e propria del TOML è del `CatalogEntryWriter` (Phase G):
//! qui produciamo solo i **valori canonici** che il writer mapperà
//! additivamente, senza toccare lo schema core.
//!
//! _Requisiti: 1.3, 1.4, 1.5, 1.6, 1.7, 2.4, 3.5, 4.2, 5.5, 5.6, 6.1, 6.2, 6.3,
//! 6.5, 6.6, 9.3, 9.4._

use crate::bindings::crosscheck::{cross_check_value, CrossCheck, GeodeReferenceTable};

use super::binary::BinaryIdentity;
use super::prologue::{PrologueOutcome, SkipReason};
use super::{SymbolId, TargetPair, SENTINEL_VALUE};

// ===========================================================================
// 11.1 — ProvenanceTier + Derivation + assign_tier
// ===========================================================================

/// Classificazione del livello di evidenza con cui un offset è stato ottenuto
/// (Req 6.1). L'ordinamento (`Ord`) è stabile e va dal tier di maggiore evidenza
/// (`ManualPrologueConfirmed`, gold) a quello derivato, così da fornire una
/// chiave totale e deterministica al report per-tier (Req 7.3).
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum ProvenanceTier {
    /// Gold: conferma manuale del prologo stile Phase E. **Non assegnabile
    /// dall'automazione** (Req 5.6, 6.4); riservato al seed `MenuLayer::init`.
    ManualPrologueConfirmed,
    /// Offset da un `Mangled_Symbol` Android dotato di RVA definito (Req 2.4).
    SymbolTable,
    /// Offset da un `Vtable_Slot` di una vtable associata a una classe RTTI
    /// (Req 3.5).
    RttiVtable,
    /// Offset mappato cross-platform da un match unico (Req 4.2).
    CrossDerived,
}

impl ProvenanceTier {
    /// Stringa canonica del tier, identica al valore registrato nel
    /// `[offset.provenance]` del catalogo TOML.
    pub fn as_str(self) -> &'static str {
        match self {
            ProvenanceTier::ManualPrologueConfirmed => "manual-prologue-confirmed",
            ProvenanceTier::SymbolTable => "symbol-table",
            ProvenanceTier::RttiVtable => "rtti-vtable",
            ProvenanceTier::CrossDerived => "cross-derived",
        }
    }

    /// `true` se il tier appartiene all'**Automatable_Subset** (Req 7.1): i tier
    /// prodotti dall'automazione dai `Source_Binary` — `symbol-table`,
    /// `rtti-vtable`, `cross-derived`. Il tier gold `manual-prologue-confirmed`
    /// **non** è automatizzabile (è il seed, conferma manuale Phase E).
    pub fn is_automatable_subset(self) -> bool {
        matches!(
            self,
            ProvenanceTier::SymbolTable | ProvenanceTier::RttiVtable | ProvenanceTier::CrossDerived
        )
    }
}

impl std::fmt::Display for ProvenanceTier {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

/// Origine di un offset prodotto dall'**automazione**, con il valore di offset
/// (RVA) derivato. È la base da cui il [`ProvenanceTier`] **segue** in modo
/// totale ed esclusivo (Req 6.1).
///
/// Cruciale per Req 5.6: questo `enum` **non ha** alcuna variante manuale, quindi
/// è strutturalmente impossibile che [`assign_tier`] assegni
/// `manual-prologue-confirmed` a un offset prodotto dall'automazione.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Derivation {
    /// Da un `Mangled_Symbol` Android demanglato con RVA definito → tier
    /// `symbol-table` (Req 2.4).
    SymbolTable {
        /// RVA definito nell'ELF Android.
        rva: u64,
    },
    /// Da un `Vtable_Slot` di una vtable associata a una classe RTTI → tier
    /// `rtti-vtable` (Req 3.5).
    RttiVtable {
        /// RVA dello slot di vtable.
        rva: u64,
    },
    /// Da un match cross-platform unico (simbolo Android ↔ slot Mach-O/PE) →
    /// tier `cross-derived` (Req 4.2).
    CrossDerived {
        /// RVA derivato dallo slot abbinato.
        rva: u64,
    },
}

impl Derivation {
    /// L'offset (RVA) derivato, qualunque sia l'origine.
    pub fn rva(self) -> u64 {
        match self {
            Derivation::SymbolTable { rva }
            | Derivation::RttiVtable { rva }
            | Derivation::CrossDerived { rva } => rva,
        }
    }

    /// Etichetta canonica del **metodo di derivazione** registrata nel
    /// `[offset.provenance]` (`derivation_method`, Req 6.2). Distingue le tre
    /// vie di derivazione automatica.
    pub fn method_label(self) -> &'static str {
        match self {
            Derivation::SymbolTable { .. } => "android-symbol-table",
            Derivation::RttiVtable { .. } => "rtti-vtable-slot",
            Derivation::CrossDerived { .. } => "android-symbol->macho-pe-vtable-slot",
        }
    }

    /// Il [`ProvenanceTier`] che **segue** dall'origine (ignorando il sentinel).
    fn origin_tier(self) -> ProvenanceTier {
        match self {
            Derivation::SymbolTable { .. } => ProvenanceTier::SymbolTable,
            Derivation::RttiVtable { .. } => ProvenanceTier::RttiVtable,
            Derivation::CrossDerived { .. } => ProvenanceTier::CrossDerived,
        }
    }
}

/// Assegna a un offset derivato **esattamente un** [`ProvenanceTier`] coerente
/// con la sua origine, oppure `None` se l'offset è il sentinel (Req 6.1, 6.6).
///
/// Regole (totalità ed esclusività):
///
/// - **Sentinel ⇒ nessun tier** (Req 6.6): se l'RVA della `derivation` è pari al
///   [`SENTINEL_VALUE`], oppure il prologo è stato saltato proprio perché
///   l'offset è il sentinel ([`SkipReason::SentinelOffset`]), restituisce `None`.
/// - **Altrimenti ⇒ esattamente un tier dall'origine** (Req 6.1): `symbol-table`
///   per [`Derivation::SymbolTable`] (Req 2.4), `rtti-vtable` per
///   [`Derivation::RttiVtable`] (Req 3.5), `cross-derived` per
///   [`Derivation::CrossDerived`] (Req 4.2).
/// - **Mai `manual-prologue-confirmed`** (Req 5.6): garantito a livello di tipo,
///   perché [`Derivation`] non ha alcuna variante manuale.
///
/// Il parametro `prologue` **non** influenza il tier scelto: l'esito del
/// `Prologue_Sanity_Check` governa il `Verified_Flag` (vedi [`ExtractionProvenance`]
/// e il gate del modulo `prologue`), non la classificazione di provenienza
/// (Req 6.3 nello spirito: gli attributi aggiuntivi non spostano il tier). È
/// accettato solo per riconoscere il caso sentinel-by-skip in difesa in
/// profondità.
pub fn assign_tier(derivation: &Derivation, prologue: &PrologueOutcome) -> Option<ProvenanceTier> {
    let is_sentinel = derivation.rva() == SENTINEL_VALUE
        || matches!(
            prologue,
            PrologueOutcome::Skipped {
                reason: SkipReason::SentinelOffset
            }
        );
    if is_sentinel {
        // Req 6.6: un offset pari al Sentinel_Value non riceve alcun tier.
        return None;
    }
    // Req 6.1: esattamente un tier, coerente con l'origine. Req 5.6: mai gold.
    Some(derivation.origin_tier())
}

// ===========================================================================
// 11.3 — Geode_Concordance via il Geode_Firewall RIUSATO
// ===========================================================================

/// Esito del cross-check numerico **opzionale** di un offset derivato contro la
/// `Geode_Reference` (Req 6.3). È un **attributo aggiuntivo** del
/// [`ProvenanceTier`], **mai** una fonte di nomi/firme/offset e **mai** un
/// modificatore del tier.
///
/// Non esiste alcuna variante "rejected": il rifiuto per violazione del firewall
/// avviene **a monte**, al caricamento della tabella ([`crate::extract::ExtractError::GeodeFirewall`]),
/// dove la `GeodeReferenceTable` non viene nemmeno prodotta. Qui si opera **solo**
/// su una tabella già accettata dal firewall (numerico-only per costruzione).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GeodeConcordance {
    /// Il valore numerico dell'offset coincide con l'indirizzo della
    /// `Geode_Reference` per quel simbolo (Req 6.3).
    Concordant,
    /// Il valore numerico dell'offset **non** coincide con l'indirizzo di
    /// riferimento (Req 6.3).
    Discordant,
}

impl GeodeConcordance {
    /// Stringa canonica registrata in `[offset.provenance]` (`cross_check`),
    /// coerente col vocabolario del seed (`"concordant"`/`"discordant"`).
    pub fn as_str(self) -> &'static str {
        match self {
            GeodeConcordance::Concordant => "concordant",
            GeodeConcordance::Discordant => "discordant",
        }
    }
}

/// Cross-check numerico **opzionale** dell'offset `rva` contro la
/// `Geode_Reference` del simbolo, instradato attraverso il **`Geode_Firewall`
/// numerico-only RIUSATO** dalla pipeline (Req 1.4, 6.3).
///
/// La `reference` è una [`GeodeReferenceTable`] **già caricata dal firewall**
/// (`crate::bindings::crosscheck::load_geode_reference`): per costruzione
/// contiene **solo** coppie `(chiave, indirizzo numerico)` — qualunque
/// sorgente/header/struttura sarebbe stata rifiutata al caricamento, senza
/// produrre alcuna tabella (Req 1.4, 1.5). Di conseguenza nessun valore qui
/// usato può dipendere da dati Geode non numerici (Req 1.6): l'unico dato letto
/// è un `u64` di indirizzo.
///
/// Esiti:
///
/// - **`None`** se la tabella **non** contiene un riferimento per `symbol`
///   (incluso il caso di `Geode_Reference` assente del tutto): l'estrazione
///   prosegue senza errore e senza attributo di concordanza (Req 1.3).
/// - **`Some(Concordant)`** se il valore numerico di `rva` coincide **esattamente**
///   con l'indirizzo di riferimento; **`Some(Discordant)`** altrimenti (Req 6.3).
///
/// Il confronto numerico esatto è delegato al **nucleo riusato**
/// [`cross_check_value`] del `crosscheck` della pipeline: nessun secondo
/// comparatore è introdotto qui. Questa funzione **non** restituisce né calcola
/// il [`ProvenanceTier`]: la concordanza è un attributo aggiuntivo e non sposta
/// mai il tier (Req 6.3).
pub fn geode_concordance(
    rva: u64,
    reference: &GeodeReferenceTable,
    symbol: &SymbolId,
) -> Option<GeodeConcordance> {
    // Req 1.3: nessun riferimento per il simbolo (o tabella assente) ⇒ nessun
    // cross-check, nessun errore, l'estrazione prosegue.
    let reference_value = reference.get(symbol.as_str())?;

    // Confronto numerico esatto col nucleo RIUSATO della pipeline (Req 6.3).
    // `cross_check_value` restituisce solo Concordant/Discordant (mai Rejected:
    // il rifiuto del firewall è già avvenuto al caricamento della tabella).
    Some(match cross_check_value(rva, reference_value) {
        CrossCheck::Concordant => GeodeConcordance::Concordant,
        CrossCheck::Discordant => GeodeConcordance::Discordant,
        CrossCheck::Rejected => GeodeConcordance::Discordant,
    })
}

// ===========================================================================
// 11.2 — ExtractionProvenance (record completo) + gate di auditabilità
// ===========================================================================

/// `Provenance_Record` **esteso** dell'estrattore: estensione **additiva** del
/// `[offset.provenance]` esistente, **mai** una sostituzione dello schema core
/// (Req 6.2, 5.5, 9.3, 9.4).
///
/// Documenta, per ogni offset emesso: il [`ProvenanceTier`], il **metodo di
/// derivazione**, la [`BinaryIdentity`] (con `gd_version`, `platform`, hash del
/// contenuto) di **ciascun** `Source_Binary` coinvolto (Req 9.3, 9.4) e l'esito
/// del `Prologue_Sanity_Check` **sia in successo sia in fallimento** (Req 5.5);
/// più l'eventuale [`GeodeConcordance`] opzionale (Req 6.3).
///
/// ## Gate di auditabilità (Req 6.5)
///
/// Il campo [`ExtractionProvenance::prologue_outcome`] è
/// [`Option<PrologueOutcome>`]: `None` rappresenta un record **privo** dell'esito
/// del prologo. La funzione [`emit_offset`] è l'**unico** punto in cui un offset
/// riceve `verified = true`, e **non** può produrre un offset verificato quando
/// `prologue_outcome` è `None`: in tal caso forza `verified = false` ed emette un
/// [`AuditabilityError`] con simbolo + coppia. Così è strutturalmente impossibile
/// emettere un offset verificato senza esito del prologo documentato.
///
/// ## `geode_no_reuse`
///
/// È **sempre** `true` ("solo riferimento numerico osservativo", Req 1.7): il
/// costruttore non offre alcun modo di impostarlo a `false`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExtractionProvenance {
    /// Il `Provenance_Tier` dell'offset (Req 6.1).
    pub tier: ProvenanceTier,
    /// Metodo di derivazione testuale (Req 6.2), es. `"android-symbol-table"`.
    pub derivation_method: String,
    /// `Binary_Identity` di **ciascun** `Source_Binary` coinvolto (Req 6.2, 9.3,
    /// 9.4): per il `symbol-table` un solo ELF; per il `cross-derived` l'ELF
    /// Android **e** il Mach-O/PE di destinazione.
    pub binary_identity: Vec<BinaryIdentity>,
    /// Esito del `Prologue_Sanity_Check`, registrato sia in successo sia in
    /// fallimento (Req 5.5). `None` ⇒ esito **non** documentato: il gate di
    /// auditabilità impedisce di emettere l'offset come `verified` (Req 6.5).
    pub prologue_outcome: Option<PrologueOutcome>,
    /// Esito opzionale del cross-check numerico di `Geode_Concordance` (Req 6.3).
    pub geode_concordance: Option<GeodeConcordance>,
    /// Invariante: la `Geode_Concordance`, quando presente, è stata usata come
    /// **solo riferimento numerico osservativo, senza riuso di codice**: sempre
    /// `true` (Req 1.7).
    pub geode_no_reuse: bool,
}

impl ExtractionProvenance {
    /// Costruisce un `ExtractionProvenance` completo da una [`Derivation`], il
    /// tier assegnato, le identità dei binari coinvolti, l'esito del prologo e
    /// l'eventuale concordanza Geode (Req 6.2, 5.5, 9.3, 9.4).
    ///
    /// `geode_no_reuse` è impostato a `true` per costruzione (Req 1.7): non
    /// esiste alcun modo di renderlo `false`.
    pub fn new(
        tier: ProvenanceTier,
        derivation: &Derivation,
        binary_identity: Vec<BinaryIdentity>,
        prologue_outcome: Option<PrologueOutcome>,
        geode_concordance: Option<GeodeConcordance>,
    ) -> Self {
        Self {
            tier,
            derivation_method: derivation.method_label().to_owned(),
            binary_identity,
            prologue_outcome,
            geode_concordance,
            // Req 1.7: sempre true, per costruzione.
            geode_no_reuse: true,
        }
    }

    /// `true` se il record documenta l'esito del `Prologue_Sanity_Check`.
    /// Fondamento del gate di auditabilità (Req 6.5).
    pub fn has_prologue_outcome(&self) -> bool {
        self.prologue_outcome.is_some()
    }

    /// Etichetta canonica del **metodo di prologo** registrata in
    /// `[offset.provenance]` (`prologue_method`): sempre `"auto-sanity"` per
    /// l'automazione, **distinto** dal `"otool-manual"` della conferma manuale
    /// (Req 5.6).
    pub fn prologue_method_label(&self) -> &'static str {
        "auto-sanity"
    }

    /// Etichetta canonica dell'**esito del prologo** registrata in
    /// `[offset.provenance]` (`prologue_outcome`, Req 5.5), o `None` se l'esito
    /// non è documentato.
    pub fn prologue_outcome_label(&self) -> Option<&'static str> {
        self.prologue_outcome.as_ref().map(|outcome| match outcome {
            PrologueOutcome::PlausibleEntry => "plausible-entry",
            PrologueOutcome::NotPlausible { .. } => "not-plausible",
            PrologueOutcome::Skipped { .. } => "skipped",
        })
    }

    /// Etichetta canonica della `Geode_Concordance` registrata in
    /// `[offset.provenance]` (`cross_check`, Req 6.3), o `None` se non eseguita.
    pub fn geode_concordance_label(&self) -> Option<&'static str> {
        self.geode_concordance.map(GeodeConcordance::as_str)
    }

    /// Etichette tracciabili delle `Binary_Identity` coinvolte, da scrivere nel
    /// campo additivo `binary_identity` di `[offset.provenance]` (Req 9.3): una
    /// `"<platform>:<hash-prefix>"` per ciascun `Source_Binary`.
    pub fn identity_labels(&self) -> Vec<String> {
        self.binary_identity
            .iter()
            .map(BinaryIdentity::traceable_id)
            .collect()
    }
}

/// Errore di **auditabilità** del gate di completezza della provenienza
/// dell'estrattore (Req 6.5).
///
/// Segnalato quando si richiede l'emissione di un offset con `verified = true`
/// ma il suo [`ExtractionProvenance`] è **privo** dell'esito del
/// `Prologue_Sanity_Check`. In tal caso l'offset è emesso con `verified = false`
/// (vedi [`emit_offset`]) e questo errore identifica **sempre** il simbolo e la
/// coppia `(GD_Version, Target_Platform)`. Coerente con lo stile fail-closed
/// degli altri errori della pipeline (cfr.
/// `crate::bindings::provenance::AuditabilityError`).
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
#[error(
    "auditabilità incompleta per il simbolo {symbol} sulla coppia {pair}: \
     il Provenance_Record non documenta l'esito del Prologue_Sanity_Check; \
     offset emesso con verified = false"
)]
pub struct AuditabilityError {
    /// Simbolo dell'offset privo di esito del prologo.
    pub symbol: SymbolId,
    /// Coppia `(GD_Version, Target_Platform)` interessata.
    pub pair: TargetPair,
}

/// Un offset emesso dall'estrattore, prodotto **esclusivamente** dal gate di
/// auditabilità [`emit_offset`].
///
/// Il campo [`EmittedOffset::verified`] è `true` **solo** se il gate lo consente:
/// per costruzione non può essere `true` senza un esito di prologo documentato
/// nella `provenance` (Req 6.5).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EmittedOffset {
    /// Simbolo dell'offset.
    pub symbol: SymbolId,
    /// Coppia `(GD_Version, Target_Platform)` da cui l'offset è derivato.
    pub pair: TargetPair,
    /// Valore di offset (RVA) emesso, **preservato senza modifiche** (Req 5.3).
    pub offset: u64,
    /// `Verified_Flag` risultante dal gate: mai `true` senza esito di prologo
    /// documentato (Req 6.5).
    pub verified: bool,
    /// Provenienza estesa dell'offset (Req 6.2).
    pub provenance: ExtractionProvenance,
}

/// **Gate di auditabilità** — l'**unico** modo di emettere un offset con il suo
/// `Verified_Flag`, che rende **impossibile** un offset verificato senza esito
/// di prologo documentato (Req 6.5).
///
/// Dato un `requested_verified` (l'esito che il gate del prologo a monte
/// vorrebbe, tipicamente `crate::extract::prologue::verified_flag`):
///
/// - se `requested_verified == true` **ma** `provenance.prologue_outcome` è
///   `None` → emette l'offset con `verified = false` e restituisce un
///   [`AuditabilityError`] (simbolo + coppia) (Req 6.5);
/// - altrimenti → emette l'offset con `verified == requested_verified` e nessun
///   errore.
///
/// L'offset è **sempre** emesso (anche in caso di errore di auditabilità: con
/// `verified = false`), preservato senza modifiche (Req 5.3). Poiché
/// [`EmittedOffset`] è costruibile **solo** qui, non esiste alcun percorso per
/// ottenere `verified = true` aggirando questo controllo.
pub fn emit_offset(
    symbol: SymbolId,
    pair: TargetPair,
    offset: u64,
    requested_verified: bool,
    provenance: ExtractionProvenance,
) -> (EmittedOffset, Option<AuditabilityError>) {
    let (verified, error) = if requested_verified && !provenance.has_prologue_outcome() {
        // Req 6.5: non si può emettere verified senza esito del prologo.
        (
            false,
            Some(AuditabilityError {
                symbol: symbol.clone(),
                pair,
            }),
        )
    } else {
        (requested_verified, None)
    };

    (
        EmittedOffset {
            symbol,
            pair,
            offset,
            verified,
            provenance,
        },
        error,
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bindings::{GdVersion, TargetPlatform};
    use crate::extract::binary::BinaryIdentity;
    use crate::extract::prologue::{PrologueOutcome, SkipReason};

    fn pair() -> TargetPair {
        TargetPair::new(GdVersion::new(2, 2081), TargetPlatform::MacosArm64)
    }

    fn identity() -> BinaryIdentity {
        BinaryIdentity {
            gd_version: GdVersion::new(2, 2081),
            platform: TargetPlatform::MacosArm64,
            content_hash: [0xAB; 32],
        }
    }

    fn plausible() -> PrologueOutcome {
        PrologueOutcome::PlausibleEntry
    }

    // ---- 11.1 assign_tier ------------------------------------------------

    #[test]
    fn assigns_exactly_one_tier_per_origin() {
        assert_eq!(
            assign_tier(&Derivation::SymbolTable { rva: 0x1000 }, &plausible()),
            Some(ProvenanceTier::SymbolTable)
        );
        assert_eq!(
            assign_tier(&Derivation::RttiVtable { rva: 0x2000 }, &plausible()),
            Some(ProvenanceTier::RttiVtable)
        );
        assert_eq!(
            assign_tier(&Derivation::CrossDerived { rva: 0x3000 }, &plausible()),
            Some(ProvenanceTier::CrossDerived)
        );
    }

    #[test]
    fn sentinel_offset_gets_no_tier() {
        // Req 6.6: sentinel ⇒ None, qualunque origine.
        assert_eq!(
            assign_tier(
                &Derivation::SymbolTable {
                    rva: SENTINEL_VALUE
                },
                &plausible()
            ),
            None
        );
        // Anche via skip-by-sentinel (difesa in profondità).
        assert_eq!(
            assign_tier(
                &Derivation::CrossDerived { rva: 0x10 },
                &PrologueOutcome::Skipped {
                    reason: SkipReason::SentinelOffset
                }
            ),
            None
        );
    }

    #[test]
    fn automation_never_yields_manual_tier() {
        // Req 5.6: nessuna Derivation produce mai manual-prologue-confirmed.
        for d in [
            Derivation::SymbolTable { rva: 1 },
            Derivation::RttiVtable { rva: 2 },
            Derivation::CrossDerived { rva: 3 },
        ] {
            let tier = assign_tier(&d, &plausible()).unwrap();
            assert_ne!(tier, ProvenanceTier::ManualPrologueConfirmed);
        }
    }

    #[test]
    fn prologue_outcome_does_not_change_tier() {
        // Req 6.3 (spirito): l'esito del prologo non sposta il tier.
        let not_plausible = PrologueOutcome::NotPlausible {
            reason: "test".to_owned(),
        };
        assert_eq!(
            assign_tier(&Derivation::SymbolTable { rva: 0x1000 }, &not_plausible),
            Some(ProvenanceTier::SymbolTable)
        );
    }

    // ---- 11.2 ExtractionProvenance + gate --------------------------------

    #[test]
    fn provenance_is_additive_and_no_reuse_true() {
        let d = Derivation::CrossDerived { rva: 0x2A1B40 };
        let prov = ExtractionProvenance::new(
            ProvenanceTier::CrossDerived,
            &d,
            vec![identity(), identity()],
            Some(plausible()),
            Some(GeodeConcordance::Concordant),
        );
        assert!(prov.geode_no_reuse); // Req 1.7
        assert_eq!(prov.derivation_method, "android-symbol->macho-pe-vtable-slot");
        assert_eq!(prov.binary_identity.len(), 2); // Req 9.3
        assert_eq!(prov.prologue_method_label(), "auto-sanity"); // Req 5.6
        assert_eq!(prov.prologue_outcome_label(), Some("plausible-entry"));
        assert_eq!(prov.geode_concordance_label(), Some("concordant"));
    }

    #[test]
    fn gate_emits_verified_when_prologue_documented() {
        let d = Derivation::SymbolTable { rva: 0x1000 };
        let prov = ExtractionProvenance::new(
            ProvenanceTier::SymbolTable,
            &d,
            vec![identity()],
            Some(plausible()),
            None,
        );
        let (emitted, err) =
            emit_offset(SymbolId::new("MenuLayer::init"), pair(), 0x1000, true, prov);
        assert!(emitted.verified);
        assert!(err.is_none());
    }

    #[test]
    fn gate_refuses_verified_without_prologue_outcome() {
        // Req 6.5: verified richiesto ma prologo assente ⇒ verified=false + errore.
        let d = Derivation::SymbolTable { rva: 0x1000 };
        let prov = ExtractionProvenance::new(
            ProvenanceTier::SymbolTable,
            &d,
            vec![identity()],
            None, // esito del prologo non documentato
            None,
        );
        let (emitted, err) =
            emit_offset(SymbolId::new("MenuLayer::init"), pair(), 0x1000, true, prov);
        assert!(!emitted.verified);
        let err = err.expect("atteso AuditabilityError");
        assert_eq!(err.symbol, SymbolId::new("MenuLayer::init"));
        assert_eq!(err.pair, pair());
    }

    #[test]
    fn gate_passthrough_when_not_requesting_verified() {
        let d = Derivation::RttiVtable { rva: 0x2000 };
        let prov =
            ExtractionProvenance::new(ProvenanceTier::RttiVtable, &d, vec![identity()], None, None);
        let (emitted, err) =
            emit_offset(SymbolId::new("Foo::bar"), pair(), 0x2000, false, prov);
        assert!(!emitted.verified);
        assert!(err.is_none()); // nessun errore: non si chiedeva verified
    }

    // ---- 11.3 geode_concordance ------------------------------------------

    #[test]
    fn geode_concordance_concordant_and_discordant() {
        let table = crate::bindings::crosscheck::parse_geode_reference(
            "\"MenuLayer::init\" = 0x316688\n",
            std::path::Path::new("mod-index/catalog/geode-reference/2.2081/macos-arm64.toml"),
        )
        .unwrap();
        let sym = SymbolId::new("MenuLayer::init");
        assert_eq!(
            geode_concordance(0x316688, &table, &sym),
            Some(GeodeConcordance::Concordant)
        );
        assert_eq!(
            geode_concordance(0x999999, &table, &sym),
            Some(GeodeConcordance::Discordant)
        );
    }

    #[test]
    fn geode_concordance_absent_reference_is_none() {
        // Req 1.3: nessun riferimento per il simbolo ⇒ None, nessun errore.
        let table = crate::bindings::crosscheck::parse_geode_reference(
            "\"Other::sym\" = 0x10\n",
            std::path::Path::new("mod-index/catalog/geode-reference/2.2081/macos-arm64.toml"),
        )
        .unwrap();
        assert_eq!(
            geode_concordance(0x316688, &table, &SymbolId::new("MenuLayer::init")),
            None
        );
    }
}
