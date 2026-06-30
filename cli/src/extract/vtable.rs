//! `VtableReconstructor` — ricostruzione delle vtable di un `Source_Binary`
//! Mach-O o PE e loro associazione alle classi `RTTI_Source` recuperate dal
//! [`crate::extract::rtti`] (Req 3.3, 3.4, 3.5, 3.7).
//!
//! L'output è una lista di [`ClassVtable`]: per ogni classe associata a una
//! vtable, i suoi [`VtableSlot`] ordinati per `index` crescente da 0 secondo la
//! posizione nella vtable, ciascuno con l'offset (non negativo) del metodo
//! virtuale che vi risiede (Req 3.3). Gli offset così derivati sono la base del
//! `Provenance_Tier` `rtti-vtable` (Req 3.5) — ma il tier vero e proprio è
//! assegnato nella Phase F (`provenance.rs`); qui produciamo **solo** le
//! `ClassVtable` corrette.
//!
//! ## Mach-O (Itanium / Clang ABI)
//!
//! Le vtable sono i simboli `__ZTV<class>`. Secondo l'Itanium C++ ABI il
//! contenuto inizia con un header di **2 puntatori** (offset-to-top + puntatore
//! al `type_info`); l'array dei puntatori a funzione comincia **dopo** l'header.
//! Ogni puntatore di dimensione-parola successivo è l'indirizzo di un metodo
//! virtuale → un [`VtableSlot`]. L'associazione vtable→classe avviene per
//! **nome di classe demanglato**: `__ZTV<class>` e `__ZTI<class>` condividono lo
//! stesso nome (`vtable for X` ↔ `typeinfo for X`).
//!
//! ## PE (MSVC ABI) — best-effort documentato
//!
//! La vftable MSVC è localizzata tramite il `RTTICompleteObjectLocator` (COL)
//! che la **precede**: appena prima del primo puntatore a funzione della vftable
//! c'è un puntatore meta al COL, e il COL riferisce per RVA il `TypeDescriptor`
//! (quindi la classe). Localizziamo i COL (firma x64 con auto-riferimento
//! `pSelf`), risolviamo `COL → TypeDescriptor → classe`, troviamo il puntatore
//! meta che riferisce il COL e leggiamo gli slot a partire dalla parola
//! successiva.
//!
//! **Limitazioni note del best-effort PE (documentate, fail-closed):**
//!
//! - Assume PE a 64 bit (`windows-x64`, l'unica `Target_Platform` PE supportata)
//!   con COL in forma RVA (`signature == 1`) auto-referenziante (`pSelf`).
//! - Il confine della vftable è determinato euristicamente: lo slot si chiude al
//!   primo puntatore nullo, al primo puntatore **fuori** da una sezione di
//!   codice, o all'incontro di un altro puntatore meta-COL. Un confine ambiguo
//!   chiude semplicemente la vtable: mai uno slot inventato.
//! - Non è verificato contro un PE reale (il generatore di fixture binarie
//!   sintetiche è il task 3.3, opzionale e non implementato qui); è una
//!   ricostruzione conservativa e fail-closed.
//!
//! ## Regole comuni (fail-closed, exclude-and-continue)
//!
//! - Una vtable **associabile** a una classe RTTI produce una [`ClassVtable`]
//!   con i suoi slot (Req 3.3, 3.5).
//! - Una vtable **senza** slot non produce alcun offset; la causa è registrata e
//!   l'elaborazione prosegue (Req 3.4). La `ClassVtable` resta presente con
//!   `slots` vuoto, così l'identità di classe è preservata senza emettere
//!   offset.
//! - Una vtable **non** associabile ad alcuna classe RTTI è **interamente**
//!   esclusa (tutti i suoi offset scartati); la causa è registrata e
//!   l'elaborazione delle altre prosegue (Req 3.7).
//! - Un Mach-O/PE non leggibile come formato atteso non produce alcun output e
//!   restituisce [`ExtractError::InvalidBinary`] (Req 3.6, difesa in profondità).
//!
//! _Requisiti: 3.3, 3.4, 3.5, 3.7._

use std::collections::{BTreeSet, HashMap, HashSet};

use object::{Object, ObjectSection, ObjectSymbol, SectionKind};

use super::binary::SourceBinary;
use super::rtti::{
    class_from_typeinfo_name, class_from_vtable, pointer_size, undecorate_msvc_type_descriptor,
    RttiClass,
};
use super::ExtractError;

/// Limite di sicurezza sul numero di slot letti da una singola vtable, a difesa
/// da confini mal determinati su binari inattesi (fail-closed: chiude la vtable
/// anziché leggere all'infinito).
const MAX_VTABLE_SLOTS: u32 = 100_000;

/// Un singolo slot ordinato di una vtable: la posizione del metodo virtuale e
/// l'offset (non negativo) che vi risiede (Req 3.3).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VtableSlot {
    /// Indice ordinale crescente da 0 secondo la posizione nella vtable (Req 3.3).
    pub index: u32,
    /// Offset (RVA) del metodo virtuale di quello slot; sempre non negativo
    /// (è un `u64`) (Req 3.3).
    pub rva: u64,
}

/// La vtable ricostruita di una classe associata a un record `RTTI_Source`
/// (Req 3.3, 3.5).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ClassVtable {
    /// Identità di classe dal `RTTI_Source` associato (Req 3.1, 3.5).
    pub class: String,
    /// Slot ordinati per `index` crescente da 0, ordine preservato (Req 3.3).
    pub slots: Vec<VtableSlot>,
}

/// Ricostruisce le vtable di un `Source_Binary` Mach-O/PE e le associa alle
/// classi `RTTI_Source` fornite in `rtti` (Req 3.3, 3.5, 3.7).
///
/// Restituisce le [`ClassVtable`] delle sole vtable **associabili** a una classe
/// RTTI nota. Le vtable non associabili sono escluse (Req 3.7); quelle senza
/// slot non producono offset (Req 3.4). Fallisce in chiusura con
/// [`ExtractError::InvalidBinary`] se il binario non è leggibile come formato
/// atteso (Req 3.6).
pub fn reconstruct_vtables(
    bin: &SourceBinary,
    rtti: &[RttiClass],
) -> Result<Vec<ClassVtable>, ExtractError> {
    // Difesa in profondità (Req 3.6).
    let file = object::File::parse(bin.bytes()).map_err(|err| ExtractError::InvalidBinary {
        identity: bin.identity.traceable_id(),
        expected: bin.format,
        reason: err.to_string(),
    })?;

    let identity = bin.identity.traceable_id();

    match file.format() {
        object::BinaryFormat::MachO => Ok(reconstruct_macho(&file, rtti, &identity)),
        object::BinaryFormat::Pe => Ok(reconstruct_pe(&file, rtti, &identity)),
        other => Err(ExtractError::InvalidBinary {
            identity,
            expected: bin.format,
            reason: format!(
                "formato {other:?} non è un Source_Binary RTTI/vtable atteso (Mach-O o PE)"
            ),
        }),
    }
}

// ---------------------------------------------------------------------------
// Mach-O (Itanium / Clang ABI)
// ---------------------------------------------------------------------------

/// Ricostruisce le vtable `__ZTV…` di un Mach-O associandole alle classi RTTI
/// per nome di classe demanglato.
fn reconstruct_macho(file: &object::File, rtti: &[RttiClass], identity: &str) -> Vec<ClassVtable> {
    let ptr_size = pointer_size(file) as u64;

    // Indice delle classi RTTI note per associazione fail-closed (Req 3.7).
    let known: HashSet<&str> = rtti.iter().map(|c| c.class.as_str()).collect();

    // Indirizzi di tutti i simboli definiti, ordinati: servono a delimitare la
    // fine di una vtable (il simbolo successivo).
    let mut symbol_addrs: Vec<u64> = file
        .symbols()
        .chain(file.dynamic_symbols())
        .filter(|s| !s.is_undefined())
        .map(|s| s.address())
        .collect();
    symbol_addrs.sort_unstable();
    symbol_addrs.dedup();

    let mut out: Vec<ClassVtable> = Vec::new();
    let mut seen: HashSet<String> = HashSet::new();

    for symbol in file.symbols().chain(file.dynamic_symbols()) {
        let name = match symbol.name() {
            Ok(name) if !name.is_empty() => name,
            _ => continue,
        };
        let class = match class_from_vtable(name) {
            Some(class) => class,
            None => continue,
        };
        if !seen.insert(class.clone()) {
            continue;
        }

        // Req 3.7: vtable non associabile a una classe RTTI ⇒ esclusa.
        if !known.contains(class.as_str()) {
            eprintln!(
                "[extract:vtable] {identity}: vtable per `{class}` esclusa — \
                 nessuna classe RTTI associata (Req 3.7)"
            );
            continue;
        }

        let start = symbol.address();
        let bound = vtable_upper_bound(file, start, &symbol_addrs);

        // L'array dei puntatori a funzione inizia dopo l'header di 2 puntatori
        // (offset-to-top + puntatore al type_info) dell'Itanium ABI.
        let data_start = start.saturating_add(2 * ptr_size);
        let slots = read_slots(file, data_start, bound, ptr_size);

        if slots.is_empty() {
            // Req 3.4: vtable senza slot ⇒ nessun offset, causa registrata.
            eprintln!(
                "[extract:vtable] {identity}: vtable per `{class}` senza slot — \
                 nessun offset prodotto (Req 3.4)"
            );
        }

        out.push(ClassVtable { class, slots });
    }

    // Recupero STRUTTURALE per i Mach-O **stripati** (release reali di GD):
    // i simboli `__ZTV`/`__ZTI` delle classi di GD sono assenti (solo quelli
    // della libc++/cxxabi sopravvivono), quindi il percorso per-simbolo qui
    // sopra non recupera **alcuna** classe di GD. Le **strutture** RTTI Itanium
    // (oggetti `type_info` + vtable) sono però ancora nei dati: le ricostruiamo
    // scandendo i puntatori (vedi `reconstruct_macho_structural`), in modo
    // fail-closed. Le classi già recuperate per-simbolo hanno precedenza.
    let structural = reconstruct_macho_structural(file, ptr_size, identity, &seen);
    out.extend(structural);

    out
}

/// Recupero **strutturale** delle vtable Itanium di un Mach-O **stripato** dei
/// simboli `__ZTV`/`__ZTI` delle classi di GD (il caso reale: una release di GD
/// conserva i soli simboli RTTI della libc++/cxxabi).
///
/// Sui binari reali questo è **indispensabile**: senza i simboli `__ZTV` il
/// percorso per-nome non trova alcuna vtable di GD e la cross-derivazione resta
/// a 0. Le **strutture** RTTI sono però intatte nei dati (`__const`), e su un
/// Mach-O arm64 con `LC_DYLD_INFO_ONLY` i puntatori su disco sono già le VA al
/// base preferito (nessun chained-fixup da decodificare). Ricostruiamo quindi la
/// catena Itanium **per struttura**:
///
/// 1. **Stringa nome del `type_info`** (es. `"9MenuLayer"`): un puntatore-dato vi
///    punta dal campo nome (offset `+ptr`) di un oggetto `type_info`. La classe
///    si recupera da [`class_from_typeinfo_name`] (identica al lato Android).
/// 2. **Oggetto `type_info`** `T`: base = (locazione del puntatore al nome) −
///    `ptr`.
/// 3. **Vtable primaria**: una locazione `V` con `*(V) == T` (puntatore al
///    `type_info` nell'header della vtable), `*(V−ptr) == 0` (offset-to-top
///    della sotto-oggetto primario) e `*(V+ptr)` che ricade nel **codice**
///    (primo metodo virtuale). L'array dei puntatori a funzione inizia a `V+ptr`.
///
/// **Fail-closed e disambiguazione:** se per una classe esistono **più** vtable
/// primarie candidate distinte (ambiguità da basi virtuali / costruzione), la
/// classe è **esclusa** (mai indovinare). La fine della vtable è il primo
/// puntatore non-codice (l'offset-to-top negativo del sotto-oggetto successivo
/// chiude naturalmente l'array, senza sconfinare nelle vtable secondarie).
/// Nessuno slot è mai inventato; la conferma del prologo a valle resta il
/// guardiano finale.
fn reconstruct_macho_structural(
    file: &object::File,
    ptr_size: u64,
    identity: &str,
    seen: &HashSet<String>,
) -> Vec<ClassVtable> {
    // Solo 64-bit (tutte le Target_Platform Mach-O supportate lo sono).
    if ptr_size != 8 {
        return Vec::new();
    }

    // Indice `valore-puntatore → locazioni (VA)` su tutte le sezioni dati
    // (esclusa la sezione di codice): un'unica passata, poi sole ricerche.
    let ptr_index = build_pointer_index(file, ptr_size);

    // 1+2. Scopri gli oggetti `type_info` per classe: per ogni valore-puntatore
    //      che punta a una stringa-nome di tipo demanglabile, le sue locazioni
    //      sono campi-nome di `type_info`; la base del `type_info` è loc − ptr.
    let mut typeinfos_by_class: HashMap<String, Vec<u64>> = HashMap::new();
    for (&value, locations) in &ptr_index {
        // Pre-filtro economico: i nomi di tipo Itanium di classe iniziano con
        // una cifra (lunghezza, classe semplice) o `N` (qualificato/nidificato).
        match peek_byte(file, value) {
            Some(b) if b.is_ascii_digit() || b == b'N' => {}
            _ => continue,
        }
        let name = match read_cstr_at(file, value) {
            Some(name) => name,
            None => continue,
        };
        let class = match class_from_typeinfo_name(&name) {
            Some(class) => class,
            None => continue,
        };
        for &loc in locations {
            if let Some(ti) = loc.checked_sub(ptr_size) {
                typeinfos_by_class.entry(class.clone()).or_default().push(ti);
            }
        }
    }

    // 3. Per ciascuna classe, individua la vtable primaria **unica**.
    let mut out: Vec<ClassVtable> = Vec::new();
    let mut classes: Vec<&String> = typeinfos_by_class.keys().collect();
    classes.sort(); // determinismo
    for class in classes {
        if seen.contains(class) {
            continue; // il percorso per-simbolo ha precedenza.
        }
        let mut funcptr_starts: BTreeSet<u64> = BTreeSet::new();
        for &ti in &typeinfos_by_class[class] {
            let Some(locations) = ptr_index.get(&ti) else {
                continue;
            };
            for &v in locations {
                // Header della vtable: offset-to-top a V−ptr, type_info a V.
                let Some(otop_addr) = v.checked_sub(ptr_size) else {
                    continue;
                };
                let otop = read_ptr(file, otop_addr, ptr_size);
                let first_fn = read_ptr(file, v.saturating_add(ptr_size), ptr_size);
                let first_in_code = first_fn.map_or(false, |f| va_in_code(file, f));
                if otop == Some(0) && first_in_code {
                    funcptr_starts.insert(v.saturating_add(ptr_size));
                }
            }
        }

        match funcptr_starts.len() {
            0 => { /* nessuna vtable primaria individuata: fail-closed, niente. */ }
            1 => {
                let start = *funcptr_starts.iter().next().expect("len==1");
                let bound = section_end_of(file, start).unwrap_or(u64::MAX);
                let slots = read_slots_in_code(file, start, bound, ptr_size);
                if !slots.is_empty() {
                    out.push(ClassVtable {
                        class: class.clone(),
                        slots,
                    });
                }
            }
            n => {
                eprintln!(
                    "[extract:vtable] {identity}: vtable strutturale per `{class}` ambigua \
                     ({n} candidate primarie distinte) — esclusa (fail-closed)"
                );
            }
        }
    }

    out
}

/// Costruisce l'indice `valore-puntatore → locazioni (VA)` scandendo a passo di
/// parola (8 byte, allineato) **tutte** le sezioni dati con un indirizzo
/// (escluso il codice): è la base O(1) per risalire da una stringa-nome al suo
/// `type_info` e da un `type_info` alla sua vtable.
fn build_pointer_index(file: &object::File, ptr_size: u64) -> HashMap<u64, Vec<u64>> {
    let mut index: HashMap<u64, Vec<u64>> = HashMap::new();
    let step = ptr_size as usize;
    for section in file.sections() {
        // Il codice non contiene i puntatori-dato RTTI; saltarlo riduce i falsi
        // positivi e il costo.
        if section.kind() == SectionKind::Text {
            continue;
        }
        let base = section.address();
        if base == 0 {
            continue;
        }
        let data = match section.data() {
            Ok(data) => data,
            Err(_) => continue,
        };
        let mut off = 0usize;
        while off + step <= data.len() {
            let value = read_ptr_le(&data[off..], ptr_size);
            if value != 0 {
                index
                    .entry(value)
                    .or_default()
                    .push(base.saturating_add(off as u64));
            }
            off += step;
        }
    }
    index
}

/// Legge un singolo byte alla VA `addr`, o `None` se non leggibile.
fn peek_byte(file: &object::File, addr: u64) -> Option<u8> {
    read_bytes_at(file, addr, 1).map(|b| b[0])
}

/// Confine superiore (esclusivo) di una vtable che inizia a `start`: il minimo
/// fra l'indirizzo del simbolo successivo e la fine della sezione che contiene
/// `start`.
fn vtable_upper_bound(file: &object::File, start: u64, symbol_addrs: &[u64]) -> u64 {
    let section_end = section_end_of(file, start).unwrap_or(u64::MAX);
    // Primo indirizzo di simbolo strettamente maggiore di `start`.
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

// ---------------------------------------------------------------------------
// PE (MSVC ABI) — best-effort documentato
// ---------------------------------------------------------------------------

/// Ricostruisce le vftable MSVC di un PE x64 via `COL → TypeDescriptor → classe`
/// e puntatore meta-COL (best-effort, fail-closed).
fn reconstruct_pe(file: &object::File, rtti: &[RttiClass], identity: &str) -> Vec<ClassVtable> {
    let ptr_size = pointer_size(file) as u64;
    let image_base = file.relative_address_base();

    let known: HashSet<&str> = rtti.iter().map(|c| c.class.as_str()).collect();

    // 1. Localizza i COL e risolvi la classe di ciascuno: mappa COL-VA → classe.
    let col_to_class = locate_pe_cols(file, image_base);

    // 2. Per ogni COL associabile a una classe RTTI nota, trova il puntatore
    //    meta che lo riferisce e leggi gli slot della vftable che segue.
    let mut out: Vec<ClassVtable> = Vec::new();
    let mut seen: HashSet<String> = HashSet::new();

    // Ordine deterministico: per VA del COL crescente.
    let mut cols: Vec<(u64, String)> = col_to_class.into_iter().collect();
    cols.sort_by_key(|(va, _)| *va);

    for (col_va, class) in cols {
        if !seen.insert(class.clone()) {
            continue;
        }
        if !known.contains(class.as_str()) {
            // Req 3.7: classe non confermata dai record RTTI ⇒ esclusa.
            eprintln!(
                "[extract:vtable] {identity}: vftable per `{class}` esclusa — \
                 nessuna classe RTTI associata (Req 3.7)"
            );
            continue;
        }

        match find_pe_vftable(file, col_va, ptr_size) {
            Some(vftable_start) => {
                let bound = section_end_of(file, vftable_start).unwrap_or(u64::MAX);
                let slots = read_slots_in_code(file, vftable_start, bound, ptr_size);
                if slots.is_empty() {
                    // Req 3.4.
                    eprintln!(
                        "[extract:vtable] {identity}: vftable per `{class}` senza slot — \
                         nessun offset prodotto (Req 3.4)"
                    );
                }
                out.push(ClassVtable { class, slots });
            }
            None => {
                // Nessun puntatore meta-COL individuato ⇒ vftable non
                // localizzabile: fail-closed, nessun offset (Req 3.7).
                eprintln!(
                    "[extract:vtable] {identity}: vftable per `{class}` non localizzabile — \
                     puntatore meta-COL non trovato (Req 3.7)"
                );
            }
        }
    }

    out
}

/// Localizza i `RTTICompleteObjectLocator` (COL) x64 di un PE e ne risolve la
/// classe via `pTypeDescriptor`. Restituisce una mappa `COL-VA → classe`.
///
/// Un COL x64 ha `signature == 1` e un campo auto-referenziante `pSelf` (RVA
/// dello stesso COL): questo auto-riferimento è il filtro che distingue un COL
/// da byte arbitrari, in modo robusto e fail-closed.
fn locate_pe_cols(file: &object::File, image_base: u64) -> HashMap<u64, String> {
    // Layout COL x64 (offset in byte):
    //   0x00 u32 signature (== 1)
    //   0x04 u32 offset
    //   0x08 u32 cdOffset
    //   0x0C u32 pTypeDescriptor (RVA)
    //   0x10 u32 pClassDescriptor (RVA)
    //   0x14 u32 pSelf (RVA del COL stesso)
    const COL_SIZE: usize = 0x18;
    const OFF_TYPE_DESCRIPTOR: usize = 0x0C;
    const OFF_SELF: usize = 0x14;

    let mut cols: HashMap<u64, String> = HashMap::new();

    for section in file.sections() {
        if !matches!(section.kind(), SectionKind::Data | SectionKind::ReadOnlyData) {
            continue;
        }
        let data = match section.data() {
            Ok(data) => data,
            Err(_) => continue,
        };
        let base = section.address();

        // I COL sono allineati a 4 byte; scandiamo a passo 4.
        let mut off = 0usize;
        while off + COL_SIZE <= data.len() {
            let signature = read_u32_le(&data[off..]);
            if signature == 1 {
                let col_va = base + off as u64;
                let self_rva = read_u32_le(&data[off + OFF_SELF..]) as u64;
                // L'auto-riferimento deve combaciare con la VA del COL.
                if image_base + self_rva == col_va {
                    let td_rva = read_u32_le(&data[off + OFF_TYPE_DESCRIPTOR..]) as u64;
                    let td_va = image_base + td_rva;
                    if let Some(class) = pe_class_from_type_descriptor(file, td_va) {
                        cols.insert(col_va, class);
                    }
                }
            }
            off += 4;
        }
    }

    cols
}

/// Legge il nome di un `TypeDescriptor` MSVC alla VA `td_va` e lo undecora.
///
/// Il `TypeDescriptor` ha il campo nome (stringa null-terminata) a offset di 2
/// puntatori dall'inizio della struttura.
fn pe_class_from_type_descriptor(file: &object::File, td_va: u64) -> Option<String> {
    let ptr_size = pointer_size(file) as u64;
    let name_va = td_va.checked_add(2 * ptr_size)?;
    let name = read_cstr_at(file, name_va)?;
    undecorate_msvc_type_descriptor(&name)
}

/// Trova l'inizio della vftable associata a un COL: cerca il puntatore **meta**
/// (un puntatore di dimensione-parola al COL) nelle sezioni dati; la vftable
/// inizia alla parola immediatamente successiva.
fn find_pe_vftable(file: &object::File, col_va: u64, ptr_size: u64) -> Option<u64> {
    for section in file.sections() {
        if !matches!(section.kind(), SectionKind::Data | SectionKind::ReadOnlyData) {
            continue;
        }
        let data = match section.data() {
            Ok(data) => data,
            Err(_) => continue,
        };
        let base = section.address();

        let step = ptr_size as usize;
        let mut off = 0usize;
        while off + step <= data.len() {
            let value = read_ptr_le(&data[off..], ptr_size);
            if value == col_va {
                // La vftable inizia subito dopo il puntatore meta.
                return Some(base + off as u64 + ptr_size);
            }
            off += step;
        }
    }
    None
}

// ---------------------------------------------------------------------------
// Lettura degli slot
// ---------------------------------------------------------------------------

/// Legge gli slot (puntatori a funzione) da `start` fino a `bound` (esclusivo),
/// fermandosi al primo puntatore nullo o non leggibile. Ogni slot riceve un
/// `index` crescente da 0 (Req 3.3).
fn read_slots(file: &object::File, start: u64, bound: u64, ptr_size: u64) -> Vec<VtableSlot> {
    let mut slots = Vec::new();
    let mut addr = start;
    let mut index: u32 = 0;
    while addr.saturating_add(ptr_size) <= bound && index < MAX_VTABLE_SLOTS {
        let value = match read_ptr(file, addr, ptr_size) {
            Some(value) => value,
            None => break,
        };
        if value == 0 {
            break;
        }
        slots.push(VtableSlot { index, rva: value });
        index += 1;
        addr += ptr_size;
    }
    slots
}

/// Come [`read_slots`], ma accetta solo puntatori che ricadono in una sezione di
/// **codice** (`Text`): è il criterio di chiusura della vftable MSVC, dove gli
/// slot sono indirizzi di funzione (best-effort, fail-closed).
fn read_slots_in_code(
    file: &object::File,
    start: u64,
    bound: u64,
    ptr_size: u64,
) -> Vec<VtableSlot> {
    let mut slots = Vec::new();
    let mut addr = start;
    let mut index: u32 = 0;
    while addr.saturating_add(ptr_size) <= bound && index < MAX_VTABLE_SLOTS {
        let value = match read_ptr(file, addr, ptr_size) {
            Some(value) => value,
            None => break,
        };
        if value == 0 || !va_in_code(file, value) {
            break;
        }
        slots.push(VtableSlot { index, rva: value });
        index += 1;
        addr += ptr_size;
    }
    slots
}

/// `true` se la VA `addr` ricade in una sezione di codice (`Text`).
fn va_in_code(file: &object::File, addr: u64) -> bool {
    for section in file.sections() {
        if section.kind() != SectionKind::Text {
            continue;
        }
        let base = section.address();
        let end = base.saturating_add(section.size());
        if addr >= base && addr < end {
            return true;
        }
    }
    false
}

/// Legge un puntatore di dimensione-parola (4 o 8 byte, little-endian) alla VA
/// `addr`, individuando la sezione che la contiene. Restituisce `None` se la VA
/// non è interamente leggibile.
fn read_ptr(file: &object::File, addr: u64, ptr_size: u64) -> Option<u64> {
    let bytes = read_bytes_at(file, addr, ptr_size as usize)?;
    Some(read_ptr_le(bytes, ptr_size))
}

/// Legge i byte alla VA `addr` per `len` byte, individuando la sezione che li
/// contiene. Restituisce `None` se l'intervallo non è interamente contenuto in
/// una sezione leggibile.
fn read_bytes_at<'data>(
    file: &object::File<'data>,
    addr: u64,
    len: usize,
) -> Option<&'data [u8]> {
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

/// Legge una stringa C (null-terminata, UTF-8) alla VA `addr`.
fn read_cstr_at(file: &object::File, addr: u64) -> Option<String> {
    for section in file.sections() {
        let base = section.address();
        let end = base.saturating_add(section.size());
        if addr >= base && addr < end {
            let data = section.data().ok()?;
            let start = (addr - base) as usize;
            if start >= data.len() {
                return None;
            }
            let mut j = start;
            while j < data.len() && data[j] != 0 {
                j += 1;
            }
            return std::str::from_utf8(&data[start..j]).ok().map(|s| s.to_owned());
        }
    }
    None
}

/// Interpreta `bytes` come un puntatore little-endian di `ptr_size` byte.
fn read_ptr_le(bytes: &[u8], ptr_size: u64) -> u64 {
    if ptr_size == 8 {
        let mut buf = [0u8; 8];
        buf.copy_from_slice(&bytes[..8]);
        u64::from_le_bytes(buf)
    } else {
        read_u32_le(bytes) as u64
    }
}

/// Legge un `u32` little-endian dall'inizio di `bytes`.
fn read_u32_le(bytes: &[u8]) -> u32 {
    let mut buf = [0u8; 4];
    buf.copy_from_slice(&bytes[..4]);
    u32::from_le_bytes(buf)
}
