//! `CrossPlatformMatcher` — abbina un metodo identificato dal
//! [`DerivedSymbol`] dell'Android_Symbol_Source allo slot di vtable
//! corrispondente di un `Source_Binary` Mach-O/PE, **per coppia**, così che da
//! nome e firma first-party (Android) si derivi l'offset di quel metodo su
//! ciascuna piattaforma (Req 4.1, 4.2, 4.3, 4.4, 4.5, 4.6).
//!
//! Questo modulo è **logica pura e deterministica**: non legge file, non fa I/O
//! e dipende esclusivamente dai suoi argomenti.
//!
//! ## Su cosa si basa l'abbinamento (e perché non può basarsi sul nome)
//!
//! Il design fissa l'euristica di `Cross_Platform_Match` come **identità di
//! classe (RTTI)** + **posizione ordinale nella vtable**. Funziona perché
//! l'ordine dei metodi virtuali è stabile fra le build della stessa versione di
//! GD per una data ABI; la posizione ordinale del metodo virtuale è ciò che lega
//! il nome Android allo slot Mach-O/PE.
//!
//! Il punto di **onestà progettuale** è strutturale e va dichiarato senza
//! ambiguità: un [`DerivedSymbol`] porta `class`, `method` e `signature`, ma un
//! [`VtableSlot`] porta **solo** `{ index, rva }` — la vtable ricostruita dai
//! binari di destinazione **non conosce i nomi dei metodi**. Di conseguenza
//! "il metodo corrisponde a esattamente uno slot secondo l'ordine di vtable"
//! (Req 4.1) **non** è decidibile per nome: l'unica base sana è la **posizione
//! ordinale**. Ma la posizione ordinale di un metodo nella vtable della sua
//! classe è un'informazione che proviene dal **lato Android** (l'ordine dei
//! metodi virtuali della classe), non dal singolo simbolo isolato.
//!
//! Per questo l'API è a **due livelli**, entrambi fedeli al design e
//! fail-closed:
//!
//! - [`match_class`] è la funzione **design-fedele e raccomandata**: riceve i
//!   metodi virtuali di **una** classe **già ordinati secondo l'ordine di vtable
//!   Android** e assegna a ciascun metodo lo slot della stessa posizione ordinale
//!   (`metodo in posizione i` ↔ `slot con index == i`). È qui che la
//!   corrispondenza ordinale nome↔slot è determinata in modo sano, perché
//!   l'ordinale arriva dall'ordinamento Android fornito dal chiamante.
//! - [`match_symbol`] è il **wrapper sottile** con la firma del design
//!   (`symbol`, `vtables`): un singolo simbolo isolato **non** porta con sé il
//!   proprio ordinale, perciò il wrapper **non inventa** una corrispondenza
//!   nome↔slot che il dato non può sostenere. Si limita a ciò che è
//!   determinabile dal solo simbolo + classe RTTI: considera candidati **tutti**
//!   gli slot delle vtable la cui classe coincide, e si impegna (`Matched`)
//!   **solo** quando esiste **esattamente uno** slot possibile per quella
//!   classe; se ce ne sono più d'uno l'ordinale non è univocamente determinato e
//!   l'esito è [`MatchOutcome::Ambiguous`] (Req 4.6); se non ce n'è nessuno è
//!   [`MatchOutcome::NoMatch`] (Req 4.4). È volutamente **più conservativo** di
//!   [`match_class`].
//!
//! In entrambi i casi vale l'iff del Req 4.1: `Matched` **se e solo se**
//! l'identità di classe del simbolo coincide con quella del `RTTI_Source` di una
//! vtable **e** il metodo corrisponde a **esattamente uno** `Vtable_Slot`.
//!
//! ## Preservazione dell'identità Android (Req 4.3)
//!
//! L'abbinamento riguarda **solo** la derivazione dell'offset (dallo slot
//! `rva`). Nome (`SymbolId`) e `Derived_Signature` restano **sempre** quelli del
//! [`DerivedSymbol`] dell'Android: questo modulo non legge mai nome o firma dal
//! Mach-O/PE (gli slot non li contengono affatto). [`match_class`] restituisce
//! il `DerivedSymbol` Android intatto accanto a ciascun esito, rendendo la
//! preservazione esplicita.
//!
//! ## Nessuna propagazione cross-coppia (Req 4.5)
//!
//! Il matcher vede **solo** le `vtables` che gli vengono passate — quelle del
//! `Source_Binary` Mach-O/PE della coppia in elaborazione. Non ha alcun accesso
//! agli offset o alle vtable di altre coppie, quindi l'offset di una coppia è,
//! per costruzione, derivato esclusivamente dai binari di quella coppia: la
//! non-propagazione cross-coppia è soddisfatta strutturalmente.
//!
//! ## Sentinel / verified (Req 4.4, 4.6)
//!
//! Su `NoMatch`/`Ambiguous` non esiste alcun offset risolvibile: a valle
//! l'offset sarà rappresentato con il [`SENTINEL_VALUE`] e `verified = false`,
//! con la causa registrata (simbolo + coppia, e per l'ambiguità il numero di
//! candidati). L'**emissione** di sentinel/`verified` e la registrazione nel
//! `Provenance_Record` avvengono nelle fasi successive (prologo / provenienza /
//! writer); qui l'esito [`MatchOutcome`] trasporta già tutto il necessario
//! (lo slot abbinato oppure il conteggio dei candidati). Il tier `cross-derived`
//! del match unico (Req 4.2) è assegnato nella Phase F (`provenance.rs`): qui
//! **non** lo costruiamo.
//!
//! _Requisiti: 4.1, 4.2, 4.3, 4.4, 4.5, 4.6._

use super::demangle::DerivedSymbol;
use super::vtable::{ClassVtable, VtableSlot};
use super::SENTINEL_VALUE;

/// Esito di un `Cross_Platform_Match` per un singolo metodo su una coppia
/// `(GD_Version, Target_Platform)`.
///
/// È una **funzione** dell'input (Req 4.1, Property 4): per gli stessi
/// argomenti l'esito è deterministico.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MatchOutcome {
    /// Match **unico**: il metodo corrisponde a esattamente un [`VtableSlot`]
    /// (classe coincidente + posizione ordinale univoca). L'offset del metodo
    /// per la coppia si deriva da `slot.rva` e il tier sarà `cross-derived`
    /// (assegnato nella Phase F) (Req 4.1, 4.2).
    Matched {
        /// Lo slot abbinato; `slot.rva` è l'offset derivato per la coppia.
        slot: VtableSlot,
    },
    /// Nessuno slot candidato: il metodo non è abbinabile su questa coppia
    /// (offset `Sentinel_Value`, `verified = false` a valle) (Req 4.4).
    NoMatch,
    /// Più di uno slot candidato: la corrispondenza ordinale non è univocamente
    /// determinata, quindi il matcher **rifiuta di indovinare** (offset
    /// `Sentinel_Value`, `verified = false` a valle), registrando il numero di
    /// candidati (Req 4.6, 12.4).
    Ambiguous {
        /// Numero di `Vtable_Slot` candidati (> 1).
        candidates: usize,
    },
}

impl MatchOutcome {
    /// `true` se l'esito è un match unico risolvibile.
    pub fn is_matched(&self) -> bool {
        matches!(self, MatchOutcome::Matched { .. })
    }

    /// Lo slot abbinato, se l'esito è [`MatchOutcome::Matched`].
    pub fn matched_slot(&self) -> Option<&VtableSlot> {
        match self {
            MatchOutcome::Matched { slot } => Some(slot),
            _ => None,
        }
    }

    /// L'offset derivato per la coppia: `slot.rva` per un match unico, altrimenti
    /// il [`SENTINEL_VALUE`] (Req 4.4, 4.6).
    ///
    /// È un **ponte di comodità** verso le fasi successive, che useranno questo
    /// valore (e imporranno `verified = false` sul sentinel). Non assegna alcun
    /// tier né `verified`: quella logica vive nel prologo/provenienza/writer.
    pub fn offset_or_sentinel(&self) -> u64 {
        match self {
            MatchOutcome::Matched { slot } => slot.rva,
            MatchOutcome::NoMatch | MatchOutcome::Ambiguous { .. } => SENTINEL_VALUE,
        }
    }
}

/// L'esito di [`match_class`] per un singolo metodo: il [`DerivedSymbol`]
/// dell'Android **preservato intatto** (Req 4.3) accanto al suo [`MatchOutcome`].
///
/// Il chiamante deriva l'offset da `outcome` (dallo slot per un match unico),
/// ma nome e firma restano **sempre** quelli di `symbol`: questo tipo rende la
/// preservazione dell'identità Android parte esplicita del contratto.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MethodMatch {
    /// Il `DerivedSymbol` dell'Android, invariato: fonte canonica di
    /// `SymbolId` e `Derived_Signature` (Req 4.3).
    pub symbol: DerivedSymbol,
    /// L'esito dell'abbinamento per questo metodo sulla coppia.
    pub outcome: MatchOutcome,
}

/// Abbina i **metodi virtuali di una classe** (già ordinati secondo l'ordine di
/// vtable del lato Android) agli slot delle `vtables` di destinazione, per
/// posizione ordinale (Req 4.1). **Funzione design-fedele e raccomandata.**
///
/// `class_methods` è il contratto chiave: deve contenere i [`DerivedSymbol`]
/// **della stessa classe**, **ordinati** secondo la posizione che i metodi
/// virtuali occupano nella vtable Android. È da qui che proviene la
/// corrispondenza ordinale nome↔slot — informazione che il singolo slot
/// Mach-O/PE (privo di nomi) non può fornire. Il metodo in posizione `i` è
/// candidato allo slot con `index == i`.
///
/// Per ciascun metodo l'esito è (iff del Req 4.1):
///
/// - [`MatchOutcome::Matched`] **se e solo se** esiste **esattamente uno** slot,
///   fra le vtable la cui classe coincide con quella del metodo, con
///   `index == i`;
/// - [`MatchOutcome::NoMatch`] se non esiste alcuno slot a quella posizione per
///   quella classe (Req 4.4);
/// - [`MatchOutcome::Ambiguous`] se ne esiste più d'uno — caso che nasce quando
///   più vtable condividono la stessa identità di classe, o quando una vtable
///   presenta `index` duplicati: in tutti questi casi la posizione ordinale non
///   è univoca e il matcher **non indovina** (Req 4.6, 12.4).
///
/// L'esito è **deterministico**: dipende solo dall'ordine di `class_methods` e
/// dal contenuto di `vtables`, scanditi in ordine. Il `DerivedSymbol` Android è
/// restituito intatto in ogni [`MethodMatch`] (Req 4.3).
pub fn match_class(class_methods: &[DerivedSymbol], vtables: &[ClassVtable]) -> Vec<MethodMatch> {
    class_methods
        .iter()
        .enumerate()
        .map(|(ordinal, symbol)| MethodMatch {
            symbol: symbol.clone(),
            outcome: match_method_at(symbol, ordinal, vtables),
        })
        .collect()
}

/// Esito dell'abbinamento per un metodo di cui è **noto** l'ordinale `ordinal`
/// nella vtable della sua classe (Req 4.1). È il **predicato puro** che
/// implementa l'iff: classe coincidente **e** esattamente uno slot alla
/// posizione ordinale.
///
/// Conta gli slot candidati = slot con `index == ordinal` fra **tutte** le
/// vtable la cui `class` coincide con `symbol.class`. Un simbolo di funzione
/// libera (`class` vuoto) non è un metodo virtuale e non ha vtable: nessun
/// candidato ⇒ [`MatchOutcome::NoMatch`].
pub fn match_method_at(
    symbol: &DerivedSymbol,
    ordinal: usize,
    vtables: &[ClassVtable],
) -> MatchOutcome {
    // Le funzioni libere non hanno vtable: non sono abbinabili cross-platform
    // (il loro offset, se esiste, viene dal tier `symbol-table` dell'Android).
    if symbol.class.is_empty() {
        return MatchOutcome::NoMatch;
    }

    let mut matched: Option<&VtableSlot> = None;
    let mut candidates = 0usize;

    // Identità di classe (Req 4.1, primo congiunto) + posizione ordinale
    // (secondo congiunto): scansione in ordine, nessun hashing ⇒ deterministico.
    for vtable in vtables {
        if vtable.class != symbol.class {
            continue;
        }
        for slot in &vtable.slots {
            if slot.index as usize == ordinal {
                candidates += 1;
                if matched.is_none() {
                    matched = Some(slot);
                }
            }
        }
    }

    classify(matched, candidates)
}

/// Abbina un **singolo** [`DerivedSymbol`] alle `vtables`, con la firma del
/// design (Req 4.1). **Wrapper sottile, fail-closed.**
///
/// Un singolo simbolo isolato **non** porta con sé il proprio ordinale di
/// vtable, e uno [`VtableSlot`] non porta nomi: perciò questo wrapper **non
/// inventa** una corrispondenza nome↔slot non sostenibile dal dato. Considera
/// candidati **tutti** gli slot delle vtable la cui classe coincide con
/// `symbol.class` e applica l'iff del Req 4.1 sull'unicità dello slot:
///
/// - nessun candidato (classe assente fra le vtable, o vtable senza slot) ⇒
///   [`MatchOutcome::NoMatch`] (Req 4.4);
/// - **esattamente uno** slot possibile per quella classe ⇒
///   [`MatchOutcome::Matched`]: è l'unica corrispondenza possibile, quindi
///   univoca senza bisogno dell'ordinale (Req 4.1);
/// - più di uno ⇒ [`MatchOutcome::Ambiguous`] con il numero di candidati,
///   perché senza l'ordinale Android la posizione non è univocamente
///   determinata e il matcher rifiuta di indovinare (Req 4.6, 12.4).
///
/// Per l'abbinamento preciso per posizione di un'intera classe usa
/// [`match_class`], che dispone dell'ordinamento Android dei metodi virtuali.
/// Questo wrapper è deterministico e volutamente più conservativo.
pub fn match_symbol(symbol: &DerivedSymbol, vtables: &[ClassVtable]) -> MatchOutcome {
    if symbol.class.is_empty() {
        return MatchOutcome::NoMatch;
    }

    let mut matched: Option<&VtableSlot> = None;
    let mut candidates = 0usize;

    for vtable in vtables {
        if vtable.class != symbol.class {
            continue;
        }
        for slot in &vtable.slots {
            candidates += 1;
            if matched.is_none() {
                matched = Some(slot);
            }
        }
    }

    classify(matched, candidates)
}

/// Classifica `(slot candidato, numero di candidati)` nell'iff del Req 4.1:
/// 0 ⇒ `NoMatch`, 1 ⇒ `Matched`, >1 ⇒ `Ambiguous`.
fn classify(matched: Option<&VtableSlot>, candidates: usize) -> MatchOutcome {
    match candidates {
        0 => MatchOutcome::NoMatch,
        1 => MatchOutcome::Matched {
            // `candidates == 1` garantisce che `matched` sia `Some`.
            slot: matched.expect("un candidato unico implica uno slot abbinato").clone(),
        },
        n => MatchOutcome::Ambiguous { candidates: n },
    }
}
