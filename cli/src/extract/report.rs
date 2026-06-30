//! `ExtractReport` — confine onesto di copertura e report per tier (Req 7).
//!
//! L'estrattore deriva offset **solo** per l'**Automatable_Subset**: i metodi
//! virtuali recuperabili via `RTTI_Source`/`Vtable` e le funzioni dotate di
//! simbolo recuperabili via `Android_Symbol_Source` (Req 7.1). Per le funzioni
//! non recuperabili in nessuno dei due modi **non** emette alcun offset
//! (Req 7.2), e **non** dichiara alcuna percentuale o conteggio di copertura per
//! ciò che resta fuori dall'Automatable_Subset (Req 7.4).
//!
//! [`ExtractReport`] è la fotografia onesta di **cosa è stato effettivamente
//! emesso**: il totale degli offset e la loro suddivisione per [`ProvenanceTier`]
//! (Req 7.3). È deliberatamente **privo** di qualunque campo di "copertura
//! percentuale" o "totale funzioni del gioco": dichiarare una copertura
//! sull'universo delle funzioni di GD significherebbe promettere ciò che
//! l'automazione non può raggiungere (Req 7.4). Il report parla **solo** degli
//! offset emessi, non del denominatore sconosciuto.
//!
//! ## Determinismo
//!
//! La suddivisione per tier usa una [`BTreeMap`] ordinata sulla chiave
//! [`ProvenanceTier`] (che deriva `Ord`): l'iterazione è in ordine totale
//! stabile, indipendente dall'ordine di registrazione, per report riproducibili
//! in code review (coerente con Req 9.1/9.2).
//!
//! _Requisiti: 7.1, 7.2, 7.3, 7.4._

use std::collections::BTreeMap;

use super::provenance::ProvenanceTier;

/// Report di fine elaborazione del `Binding_Extractor`: il totale degli offset
/// emessi e la loro suddivisione per [`ProvenanceTier`] (Req 7.3).
///
/// **Confine onesto** (Req 7.4): contiene **solo** conteggi di offset
/// effettivamente emessi. Non espone alcuna percentuale di copertura né un
/// totale di funzioni del gioco, perché l'automazione copre solo
/// l'**Automatable_Subset** (Req 7.1) e una percentuale sull'universo completo
/// sarebbe una promessa non mantenibile.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ExtractReport {
    /// Numero totale di offset emessi (somma dei conteggi per tier) (Req 7.3).
    pub total_offsets: usize,
    /// Suddivisione `tier → numero di offset emessi con quel tier` (Req 7.3),
    /// ordinata per `ProvenanceTier` (iterazione deterministica).
    pub by_tier: BTreeMap<ProvenanceTier, usize>,
}

impl ExtractReport {
    /// Crea un report vuoto (nessun offset emesso).
    pub fn new() -> Self {
        Self::default()
    }

    /// Registra un offset emesso con il `tier` indicato: incrementa il totale e
    /// il conteggio del tier (Req 7.3).
    ///
    /// Accetta **solo** un offset dotato di tier: un offset pari al
    /// `Sentinel_Value` non riceve alcun tier (Req 6.6) e quindi **non** è
    /// contato qui — coerente con il fatto che il report conta gli offset
    /// **emessi e risolvibili**, non i segnaposto.
    pub fn record(&mut self, tier: ProvenanceTier) {
        self.total_offsets += 1;
        *self.by_tier.entry(tier).or_insert(0) += 1;
    }

    /// Costruisce un report a partire da un iteratore dei tier degli offset
    /// emessi (gli offset sentinel, privi di tier, sono già esclusi a monte:
    /// [`super::provenance::assign_tier`] restituisce `None`, Req 6.6).
    pub fn from_tiers(tiers: impl IntoIterator<Item = ProvenanceTier>) -> Self {
        let mut report = Self::new();
        for tier in tiers {
            report.record(tier);
        }
        report
    }

    /// Numero di offset emessi con uno specifico `tier`.
    pub fn count(&self, tier: ProvenanceTier) -> usize {
        self.by_tier.get(&tier).copied().unwrap_or(0)
    }

    /// Numero di offset emessi nell'**Automatable_Subset** (tier `symbol-table`,
    /// `rtti-vtable`, `cross-derived`) — cioè quelli prodotti dall'automazione
    /// (Req 7.1). Esclude il tier gold `manual-prologue-confirmed` (il seed).
    pub fn automatable_subset_total(&self) -> usize {
        self.by_tier
            .iter()
            .filter(|(tier, _)| tier.is_automatable_subset())
            .map(|(_, count)| *count)
            .sum()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn empty_report_has_no_offsets() {
        let report = ExtractReport::new();
        assert_eq!(report.total_offsets, 0);
        assert!(report.by_tier.is_empty());
        assert_eq!(report.automatable_subset_total(), 0);
    }

    #[test]
    fn records_total_and_per_tier_breakdown() {
        // Req 7.3: totale + suddivisione per tier.
        let report = ExtractReport::from_tiers([
            ProvenanceTier::SymbolTable,
            ProvenanceTier::SymbolTable,
            ProvenanceTier::RttiVtable,
            ProvenanceTier::CrossDerived,
            ProvenanceTier::ManualPrologueConfirmed,
        ]);

        assert_eq!(report.total_offsets, 5);
        assert_eq!(report.count(ProvenanceTier::SymbolTable), 2);
        assert_eq!(report.count(ProvenanceTier::RttiVtable), 1);
        assert_eq!(report.count(ProvenanceTier::CrossDerived), 1);
        assert_eq!(report.count(ProvenanceTier::ManualPrologueConfirmed), 1);
    }

    #[test]
    fn automatable_subset_excludes_manual_tier() {
        // Req 7.1: l'Automatable_Subset esclude il tier gold (seed manuale).
        let report = ExtractReport::from_tiers([
            ProvenanceTier::SymbolTable,
            ProvenanceTier::RttiVtable,
            ProvenanceTier::CrossDerived,
            ProvenanceTier::ManualPrologueConfirmed,
        ]);
        assert_eq!(report.total_offsets, 4);
        assert_eq!(report.automatable_subset_total(), 3);
    }

    #[test]
    fn by_tier_iterates_in_deterministic_order() {
        // BTreeMap ⇒ ordine totale stabile sul ProvenanceTier (Req 9.1/9.2).
        let report = ExtractReport::from_tiers([
            ProvenanceTier::CrossDerived,
            ProvenanceTier::SymbolTable,
            ProvenanceTier::ManualPrologueConfirmed,
            ProvenanceTier::RttiVtable,
        ]);
        let order: Vec<ProvenanceTier> = report.by_tier.keys().copied().collect();
        assert_eq!(
            order,
            vec![
                ProvenanceTier::ManualPrologueConfirmed,
                ProvenanceTier::SymbolTable,
                ProvenanceTier::RttiVtable,
                ProvenanceTier::CrossDerived,
            ]
        );
    }
}
