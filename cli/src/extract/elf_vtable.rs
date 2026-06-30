//! `ElfVtableReconstructor` — ricostruzione delle vtable Itanium dell'ELF
//! Android (`_ZTV<class>`) per recuperare, **dal lato Android e in modo
//! puramente first-party**, l'ordine dei metodi virtuali di ciascuna classe
//! (Req 3.1, 3.3, 4.1, 9.4).
//!
//! ## Perché questo modulo esiste (il crux d'integrazione di Phase D/G)
//!
//! Il `Cross_Platform_Match` ordinale di
//! [`match_class`](crate::extract::matcher::match_class) /
//! [`match_method_at`](crate::extract::matcher::match_method_at) richiede, per
//! ciascuna classe, l'ordine dei suoi metodi virtuali **secondo la posizione di
//! vtable** — ma la tabella dei simboli ELF (`crate::extract::elf`) **non**
//! codifica quella posizione: elenca i simboli, non l'ordinamento virtuale.
//!
//! La vtable Android lo codifica però **direttamente**. Secondo l'Itanium C++
//! ABI il simbolo `_ZTV<class>` (vtable for X) ha, dopo un header di **2
//! puntatori** (offset-to-top + puntatore al `type_info`), un array di
//! **puntatori a funzione**: gli RVA dei metodi virtuali nell'**ordine di
//! vtable**. E ogni [`DerivedSymbol`] demanglato porta già il **proprio** `rva`
//! (Req 2.4). Quindi, mappando l'RVA di **ciascuno slot** della vtable al
//! `DerivedSymbol` con quell'RVA, si recupera la sequenza ordinata dei metodi
//! virtuali della classe — esclusivamente dal binario first-party. È questo
//! ordinamento sano che rende la cross-derivazione macOS/Windows **reale** (e
//! host-testabile) invece di fail-closed.
//!
//! ## Rappresentazione dei "buchi" (integrità della posizione ordinale)
//!
//! Uno slot il cui RVA **non** mappa ad alcun `DerivedSymbol` noto è un **buco**
//! ([`VtableEntry::Hole`]): lo slot esiste (è un vero metodo virtuale), ma non ne
//! conosciamo nome/firma first-party. **Non** inventiamo un metodo. Cruciale: il
//! buco **occupa comunque la sua posizione ordinale**, perché la vtable Mach-O/PE
//! di destinazione ha lo **stesso** ordinamento Itanium e lo slot `index == i`
//! del target è lo **stesso** metodo virtuale dello slot Android in posizione
//! `i`. Per questo l'ordine per classe è un `Vec<`[`VtableEntry`]`>` che preserva
//! le posizioni dei buchi: collassare i buchi disallineerebbe gli ordinali e
//! produrrebbe offset cross-derived **errati**. Il chiamante deve quindi usare
//! l'**indice assoluto** nella sequenza come ordinale (vedi
//! [`match_method_at`](crate::extract::matcher::match_method_at)), saltando i
//! buchi solo in fase di **emissione** (nessun nome da emettere) ma **non** in
//! fase di conteggio degli ordinali.
//!
//! ## La sottigliezza delle rilocazioni ELF (onestà per la Phase H reale)
//!
//! In un `.so` Android (PIE) gli slot di una vtable vivono tipicamente in
//! `.data.rel.ro` e sono soggetti a **rilocazioni**: il valore del puntatore
//! **su disco** può essere `0`, con una voce di rilocazione (es.
//! `R_AARCH64_RELATIVE` in `.rela.dyn`) che fornisce l'RVA reale a tempo di
//! caricamento. Leggere il solo valore su disco mancherebbe quindi questi slot.
//!
//! Cosa **gestiamo** in questo task:
//!
//! 1. **Percorso a valore diretto (sempre corretto):** se lo slot contiene su
//!    disco un RVA diretto (vtable non rilocata, o fixture sintetica), lo
//!    leggiamo e lo risolviamo correttamente.
//! 2. **Rilocazioni risolvibili (best-effort):** interroghiamo
//!    [`Object::dynamic_relocations`] (le rilocazioni dinamiche `.rela.dyn` di un
//!    `.so` collegato) **e** le rilocazioni applicate alle sezioni
//!    ([`ObjectSection::relocations`], utili per oggetti/fixture), costruendo una
//!    mappa `VA-dello-slot → RVA-risolto`. Risolviamo le rilocazioni di tipo
//!    **relativo** (l'addend porta l'RVA target, con image base 0 per un `.so`) e
//!    quelle **basate su simbolo** (`indirizzo del simbolo + addend`).
//!
//! Cosa **rinviamo** esplicitamente (documentato, fail-closed):
//!
//! - Tipi di rilocazione che non sappiamo risolvere con certezza (né relativi né
//!   basati su simbolo a noi noti) **non** vengono indovinati: lo slot resta
//!   `0`/non risolto e diventa un **buco**, mai un metodo inventato.
//! - Nessun valore di slot è mai promosso a metodo se il suo RVA risolto non
//!   coincide **esattamente** con l'RVA di un `DerivedSymbol` noto.
//!
//! Questa è la zona di rischio "funziona su fixture sintetiche ma va curata sul
//! GD reale": il percorso a valore diretto è corretto ovunque; la risoluzione
//! delle rilocazioni dinamiche è best-effort e va riconfermata sul GD reale
//! (Phase H), ma essendo fail-closed non può **mai** produrre un ordinamento
//! errato — al più lascia dei buchi.
//!
//! ## Fail-closed (Req 9.4, 10.3)
//!
//! - Un `Source_Binary` atteso come ELF ma non leggibile come ELF valido non
//!   produce alcun output e restituisce [`ExtractError::InvalidBinary`].
//! - Una singola vtable non ricostruibile è **esclusa** con causa registrata,
//!   senza interrompere le altre.
//! - **Nessun** byte del contenuto del binario è re-emesso: l'output sono solo
//!   identità di classe, ordinali e i `DerivedSymbol` già derivati (Req 10.3).
//!
//! _Requisiti: 3.1, 3.3, 4.1, 9.4._

use std::collections::{BTreeMap, HashMap};

use object::{Object, ObjectSection, ObjectSymbol, RelocationTarget};

use super::binary::SourceBinary;
use super::demangle::DerivedSymbol;
use super::rtti::{class_from_vtable, pointer_size};
use super::{BinaryFormat, ExtractError};

/// Limite di sicurezza sul numero di slot letti da una singola vtable, a difesa
/// da confini mal determinati su binari inattesi (fail-closed).
const MAX_VTABLE_SLOTS: usize = 100_000;

/// Una posizione nella vtable Android, nell'ordine di vtable (Req 3.3).
///
/// La posizione (l'**indice** nella `Vec<VtableEntry>` della classe) è
/// l'ordinale da usare nel `Cross_Platform_Match`; i buchi **occupano** la loro
/// posizione per preservare l'allineamento ordinale con la vtable di
/// destinazione (vedi documentazione del modulo).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum VtableEntry {
    /// Lo slot mappa a un `DerivedSymbol` noto: è quel metodo virtuale, in questa
    /// posizione di vtable.
    Method(DerivedSymbol),
    /// Lo slot esiste ma il suo RVA risolto non mappa ad alcun `DerivedSymbol`
    /// noto (o non è risolvibile): **buco**, nessun metodo inventato. Conserva
    /// l'RVA risolto (o `0` se non risolto) a fini diagnostici.
    Hole {
        /// RVA risolto dello slot, o `0` se non risolvibile (rilocazione non
        /// gestita).
        rva: u64,
    },
}

impl VtableEntry {
    /// Il `DerivedSymbol` se questa posizione è un metodo noto, altrimenti
    /// `None` (buco).
    pub fn method(&self) -> Option<&DerivedSymbol> {
        match self {
            VtableEntry::Method(sym) => Some(sym),
            VtableEntry::Hole { .. } => None,
        }
    }
}

/// L'ordinamento dei metodi virtuali per classe, recuperato dalle vtable
/// Itanium dell'ELF Android (Req 3.3, 4.1).
///
/// Per ciascuna classe conserva la sequenza ordinata delle sue posizioni di
/// vtable ([`VtableEntry`]) **inclusi i buchi**, così che l'indice nella
/// sequenza sia l'ordinale di vtable da usare nel `Cross_Platform_Match`.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct AndroidVtableOrder {
    /// `class → sequenza ordinata di posizioni di vtable` (chiavi ordinate per
    /// determinismo).
    by_class: BTreeMap<String, Vec<VtableEntry>>,
}

impl AndroidVtableOrder {
    /// La sequenza ordinata di posizioni di vtable per `class`, o `None` se per
    /// quella classe non è stata ricostruita alcuna vtable Android (in tal caso
    /// il chiamante resta **fail-closed** per i suoi metodi).
    ///
    /// L'**indice** di ciascun elemento nella slice restituita è l'ordinale di
    /// vtable: i buchi occupano la loro posizione e **non** vanno collassati.
    pub fn order_for(&self, class: &str) -> Option<&[VtableEntry]> {
        self.by_class.get(class).map(|v| v.as_slice())
    }

    /// Iteratore deterministico sulle classi con un ordine di vtable
    /// ricostruito.
    pub fn classes(&self) -> impl Iterator<Item = &String> {
        self.by_class.keys()
    }

    /// Numero di classi con un ordine di vtable ricostruito.
    pub fn class_count(&self) -> usize {
        self.by_class.len()
    }

    /// `true` se non è stata ricostruita alcuna vtable Android.
    pub fn is_empty(&self) -> bool {
        self.by_class.is_empty()
    }

    /// Costruisce la mappa `SymbolId → ordinale di vtable` per i soli metodi
    /// **risolti** (non buchi), in modo **conservativo e fail-closed**: se uno
    /// stesso `SymbolId` comparisse a più di un ordinale (vtable malformata o
    /// duplicati), è marcato ambiguo (`None`) e il chiamante lo tratta come non
    /// recuperabile, senza indovinare.
    ///
    /// È il ponte d'integrazione verso il `Cross_Platform_Match`: dato un
    /// `DerivedSymbol`, fornisce l'ordinale assoluto (posizione di vtable
    /// Android) da passare a
    /// [`match_method_at`](crate::extract::matcher::match_method_at).
    pub fn symbol_ordinals(&self) -> HashMap<String, Option<usize>> {
        let mut map: HashMap<String, Option<usize>> = HashMap::new();
        for entries in self.by_class.values() {
            for (ordinal, entry) in entries.iter().enumerate() {
                if let VtableEntry::Method(sym) = entry {
                    let id = sym.symbol_id();
                    match map.get(&id) {
                        // Già visto allo stesso ordinale: idempotente.
                        Some(Some(existing)) if *existing == ordinal => {}
                        // Già visto altrove (o già ambiguo): marca ambiguo.
                        Some(_) => {
                            map.insert(id, None);
                        }
                        None => {
                            map.insert(id, Some(ordinal));
                        }
                    }
                }
            }
        }
        map
    }
}

/// Ricostruisce le vtable Itanium `_ZTV<class>` dell'ELF Android e recupera, per
/// ciascuna classe, l'ordine dei metodi virtuali risolvendo l'RVA di ogni slot
/// al [`DerivedSymbol`] con quell'RVA (Req 3.1, 3.3, 4.1).
///
/// `symbols` è la lista master dei `DerivedSymbol` demanglati (da
/// [`crate::extract::elf::read_elf_symbols`] + [`crate::extract::demangle`]); i
/// loro `rva` sono la chiave di risoluzione degli slot.
///
/// Fallisce in chiusura con [`ExtractError::InvalidBinary`] se `elf` non è
/// leggibile come ELF valido (Req 9.4); le singole vtable non ricostruibili
/// sono escluse con causa registrata, senza abortire le altre.
pub fn reconstruct_android_vtables(
    elf: &SourceBinary,
    symbols: &[DerivedSymbol],
) -> Result<AndroidVtableOrder, ExtractError> {
    // Difesa in profondità (Req 9.4): re-parse e conferma del formato ELF.
    let file = object::File::parse(elf.bytes()).map_err(|err| ExtractError::InvalidBinary {
        identity: elf.identity.traceable_id(),
        expected: BinaryFormat::Elf,
        reason: err.to_string(),
    })?;

    if file.format() != object::BinaryFormat::Elf {
        return Err(ExtractError::InvalidBinary {
            identity: elf.identity.traceable_id(),
            expected: BinaryFormat::Elf,
            reason: format!("formato rilevato {:?}, atteso un ELF valido", file.format()),
        });
    }

    let identity = elf.identity.traceable_id();
    let ptr_size = pointer_size(&file) as u64;

    // 1. Mappa RVA → DerivedSymbol (first-wins, deterministico): più simboli con
    //    lo stesso RVA (alias) sono risolti conservativamente al primo incontrato
    //    nell'ordine della lista master.
    let mut rva_to_symbol: HashMap<u64, &DerivedSymbol> = HashMap::new();
    for sym in symbols {
        rva_to_symbol.entry(sym.rva).or_insert(sym);
    }

    // 2. Mappa delle rilocazioni VA-dello-slot → RVA-risolto (best-effort).
    let reloc_map = build_reloc_map(&file);

    // 3. Indirizzi dei simboli per delimitare il confine di una vtable quando il
    //    simbolo non espone una dimensione.
    let mut symbol_addrs: Vec<u64> = file
        .symbols()
        .chain(file.dynamic_symbols())
        .filter(|s| !s.is_undefined())
        .map(|s| s.address())
        .collect();
    symbol_addrs.sort_unstable();
    symbol_addrs.dedup();

    let mut by_class: BTreeMap<String, Vec<VtableEntry>> = BTreeMap::new();

    for symbol in file.symbols().chain(file.dynamic_symbols()) {
        let name = match symbol.name() {
            Ok(name) if !name.is_empty() => name,
            _ => continue,
        };
        // Il nome di classe della vtable si recupera demanglando `_ZTV<class>`.
        // `cpp_demangle` rende la vtable come `{vtable(X)}` (non `vtable for X`):
        // `class_from_vtable` accetta entrambe le rese (vedi `rtti`).
        let class = match class_from_vtable(name) {
            Some(class) => class,
            None => continue,
        };
        if by_class.contains_key(&class) {
            continue; // una sola vtable per classe (first-wins, deterministico).
        }

        let start = symbol.address();
        let bound = vtable_upper_bound(&file, start, symbol.size(), &symbol_addrs);

        // L'array dei puntatori a funzione inizia dopo l'header di 2 puntatori
        // (offset-to-top + puntatore al type_info) dell'Itanium ABI.
        let data_start = start.saturating_add(2 * ptr_size);
        if data_start >= bound {
            // Vtable senza slot di metodo (solo header o confine degenerato):
            // esclusa con causa, prosegue (fail-closed).
            eprintln!(
                "[extract:elf_vtable] {identity}: vtable Android per `{class}` senza slot di \
                 metodo — esclusa (fail-closed)"
            );
            continue;
        }

        let entries = read_method_slots(
            &file,
            &reloc_map,
            &rva_to_symbol,
            data_start,
            bound,
            ptr_size,
        );

        if entries.is_empty() {
            eprintln!(
                "[extract:elf_vtable] {identity}: vtable Android per `{class}` non ricostruibile \
                 (nessuno slot leggibile) — esclusa (fail-closed)"
            );
            continue;
        }

        // Diagnostica onesta: conteggio dei buchi (slot non risolti a un
        // DerivedSymbol noto), mai trasformati in metodi.
        let holes = entries
            .iter()
            .filter(|e| matches!(e, VtableEntry::Hole { .. }))
            .count();
        if holes > 0 {
            eprintln!(
                "[extract:elf_vtable] {identity}: vtable Android per `{class}` ricostruita con \
                 {holes} buco/buchi su {} slot (slot il cui RVA non mappa ad alcun simbolo noto \
                 o non risolvibile via rilocazione) — posizioni preservate, nessun metodo \
                 inventato",
                entries.len()
            );
        }

        by_class.insert(class, entries);
    }

    Ok(AndroidVtableOrder { by_class })
}

/// Confine superiore (esclusivo) di una vtable che inizia a `start`.
///
/// Preferisce la **dimensione** del simbolo ELF (`st_size`, quando `> 0`), molto
/// più precisa; altrimenti ripiega sul minimo fra l'indirizzo del simbolo
/// successivo e la fine della sezione che contiene `start`.
fn vtable_upper_bound(
    file: &object::File,
    start: u64,
    sym_size: u64,
    symbol_addrs: &[u64],
) -> u64 {
    let section_end = section_end_of(file, start).unwrap_or(u64::MAX);
    if sym_size > 0 {
        return start.saturating_add(sym_size).min(section_end);
    }
    let next_symbol = symbol_addrs
        .iter()
        .copied()
        .find(|&addr| addr > start)
        .unwrap_or(u64::MAX);
    section_end.min(next_symbol)
}

/// Fine (esclusiva, VA) della sezione che contiene `addr`, se individuata.
fn section_end_of(file: &object::File, addr: u64) -> Option<u64> {
    for section in file.sections() {
        let base = section.address();
        let end = base.saturating_add(section.size());
        if addr >= base && addr < end {
            return Some(end);
        }
    }
    None
}

/// Legge gli slot dei metodi virtuali da `data_start` fino a `bound`
/// (esclusivo), risolvendo ciascuno slot a un [`VtableEntry`].
///
/// Per ogni slot, il valore del puntatore a funzione è ottenuto **prima** dalla
/// mappa delle rilocazioni (slot rilocato in `.data.rel.ro`), **poi** dal valore
/// diretto su disco. L'RVA così ottenuto è mappato a un [`DerivedSymbol`] noto:
/// un match diventa [`VtableEntry::Method`], altrimenti un [`VtableEntry::Hole`]
/// (posizione preservata, nessun metodo inventato).
fn read_method_slots(
    file: &object::File,
    reloc_map: &HashMap<u64, u64>,
    rva_to_symbol: &HashMap<u64, &DerivedSymbol>,
    data_start: u64,
    bound: u64,
    ptr_size: u64,
) -> Vec<VtableEntry> {
    let mut entries: Vec<VtableEntry> = Vec::new();
    let mut addr = data_start;
    while addr.saturating_add(ptr_size) <= bound && entries.len() < MAX_VTABLE_SLOTS {
        // Valore dello slot: rilocazione (se presente) poi valore diretto.
        let value = match reloc_map.get(&addr).copied() {
            Some(resolved) => resolved,
            None => read_ptr(file, addr, ptr_size).unwrap_or(0),
        };

        match rva_to_symbol.get(&value) {
            Some(sym) => entries.push(VtableEntry::Method((*sym).clone())),
            None => entries.push(VtableEntry::Hole { rva: value }),
        }

        addr = addr.saturating_add(ptr_size);
    }
    entries
}

/// Costruisce la mappa `VA-dello-slot → RVA-risolto` dalle rilocazioni del
/// binario (best-effort, fail-closed; vedi documentazione del modulo).
///
/// Unisce le rilocazioni **dinamiche** (`.rela.dyn` di un `.so` collegato, via
/// [`Object::dynamic_relocations`]) e quelle **applicate alle sezioni** (utili
/// per oggetti/fixture, via [`ObjectSection::relocations`]). I tipi non
/// risolvibili sono semplicemente omessi (lo slot resterà un buco).
fn build_reloc_map(file: &object::File) -> HashMap<u64, u64> {
    let mut map: HashMap<u64, u64> = HashMap::new();

    // Rilocazioni dinamiche (.rela.dyn): la sorgente reale degli slot di vtable
    // di un `.so` Android (PIE) in `.data.rel.ro`.
    if let Some(relocs) = file.dynamic_relocations() {
        for (va, reloc) in relocs {
            if let Some(resolved) = resolve_reloc(file, &reloc) {
                map.entry(va).or_insert(resolved);
            }
        }
    }

    // Rilocazioni applicate alle sezioni (oggetti rilocabili / fixture
    // sintetiche).
    for section in file.sections() {
        let base = section.address();
        for (off, reloc) in section.relocations() {
            if let Some(resolved) = resolve_reloc(file, &reloc) {
                map.entry(base.saturating_add(off)).or_insert(resolved);
            }
        }
    }

    map
}

/// Risolve una rilocazione all'RVA che deposita nello slot, **solo** per i casi
/// che sappiamo trattare con certezza (fail-closed: gli altri tornano `None` e
/// lasciano un buco).
///
/// - Target **simbolo**: `indirizzo del simbolo + addend`.
/// - Target **assoluto/relativo** (es. `R_AARCH64_RELATIVE`): l'addend porta
///   l'RVA target (image base `0` per un `.so`).
fn resolve_reloc(file: &object::File, reloc: &object::Relocation) -> Option<u64> {
    let addend = reloc.addend();
    match reloc.target() {
        RelocationTarget::Symbol(idx) => {
            let sym = file.symbol_by_index(idx).ok()?;
            let base = sym.address();
            Some((base as i64).wrapping_add(addend) as u64)
        }
        // `R_*_RELATIVE` e affini: object normalizza il target ad `Absolute`,
        // con l'RVA target nell'addend (base immagine 0 per un `.so`).
        RelocationTarget::Absolute => {
            if addend > 0 {
                Some(addend as u64)
            } else {
                None
            }
        }
        // Tipi che non sappiamo risolvere con certezza: rinviati (buco).
        _ => None,
    }
}

/// Legge un puntatore di dimensione-parola (4 o 8 byte, little-endian) alla VA
/// `addr`, individuando la sezione che la contiene. Restituisce `None` se la VA
/// non è interamente leggibile.
fn read_ptr(file: &object::File, addr: u64, ptr_size: u64) -> Option<u64> {
    let bytes = read_bytes_at(file, addr, ptr_size as usize)?;
    if ptr_size == 8 {
        let mut buf = [0u8; 8];
        buf.copy_from_slice(&bytes[..8]);
        Some(u64::from_le_bytes(buf))
    } else {
        let mut buf = [0u8; 4];
        buf.copy_from_slice(&bytes[..4]);
        Some(u32::from_le_bytes(buf) as u64)
    }
}

/// Legge i byte alla VA `addr` per `len` byte, individuando la sezione che li
/// contiene. Restituisce `None` se l'intervallo non è interamente contenuto in
/// una sezione leggibile.
fn read_bytes_at<'data>(file: &object::File<'data>, addr: u64, len: usize) -> Option<&'data [u8]> {
    for section in file.sections() {
        let base = section.address();
        let end = base.saturating_add(section.size());
        if addr >= base && addr.saturating_add(len as u64) <= end {
            let data = section.data().ok()?;
            let offset = (addr - base) as usize;
            if offset + len <= data.len() {
                return Some(&data[offset..offset + len]);
            }
        }
    }
    None
}
