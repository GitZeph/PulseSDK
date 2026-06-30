//! `RttiParser` — recupero dell'identità di classe dai record `RTTI_Source` di
//! un `Source_Binary` Mach-O (Itanium/Clang ABI) o PE (MSVC ABI) (Req 3.1, 3.2,
//! 3.6).
//!
//! Le piattaforme Mach-O/PE sono **prive** dei nomi di GD nei simboli (a
//! differenza dell'ELF Android, master di nomi/firme), ma conservano i record
//! **RTTI** che identificano i **nomi delle classi**. Questo modulo li estrae in
//! una lista di [`RttiClass`] — l'identificatore di classe più il locatore del
//! descrittore RTTI necessario, nella Phase C successiva, ad associare la classe
//! alla sua vtable ([`crate::extract::vtable`]).
//!
//! ## Mach-O (Itanium / Clang C++ ABI)
//!
//! Le identità di classe provengono dai simboli `type_info`: ogni classe
//! polimorfica espone un simbolo `__ZTI<mangled>` (l'underscore extra è la
//! convenzione Mach-O: l'Itanium `_ZTI…` diventa `__ZTI…` nella tabella dei
//! simboli). Il nome di classe si recupera **demanglando** quel simbolo: il
//! demangler rende `_ZTI3Foo` come `"typeinfo for Foo"`, da cui si estrae
//! l'identificatore `Foo`. Il `descriptor_rva` è l'indirizzo del simbolo
//! `__ZTI…`, lo stesso valore a cui punta l'header RTTI di `__ZTV…` (vtable),
//! che la Phase C usa per confermare l'associazione vtable→classe.
//!
//! ## PE (MSVC C++ ABI) — best-effort documentato
//!
//! Le identità di classe provengono dai `TypeDescriptor` MSVC: una struttura in
//! sezione dati il cui campo nome è una stringa **decorata** che inizia con
//! `.?AV` (classe) o `.?AU` (struct), es. `.?AVMenuLayer@@`. Recuperiamo il nome
//! di classe **scandendo** le sezioni dati a sola lettura alla ricerca di queste
//! stringhe e **undecorandole** in modo conservativo (gestione dei namespace
//! annidati `@`; vedi [`undecorate_msvc_type_descriptor`]). Il `descriptor_rva`
//! è l'indirizzo **base** del `TypeDescriptor` (l'inizio della struttura, due
//! puntatori prima del campo nome), così che il `CompleteObjectLocator` della
//! Phase C — che riferisce il `TypeDescriptor` per RVA — possa risolvere la
//! classe.
//!
//! **Limitazioni note del best-effort PE (documentate, fail-closed):**
//!
//! - Sono undecorati **solo** i nomi semplici `.?AV…@@` / `.?AU…@@`, inclusi i
//!   namespace annidati (`.?AVCCNode@cocos2d@@` → `cocos2d::CCNode`). I nomi che
//!   contengono costrutti template o back-reference MSVC (carattere `?`) **non**
//!   vengono undecorati e l'ingresso è **escluso** anziché indovinato: mai un
//!   identificatore di classe errato (fail-closed).
//! - La scansione assume PE a 64 bit (`windows-x64`, l'unica `Target_Platform`
//!   PE dell'insieme supportato).
//!
//! ## Fail-closed (Req 3.2, 3.6)
//!
//! Un Mach-O/PE **leggibile** ma **privo** di record RTTI produce `Ok(vec![])`
//! senza errore: l'elaborazione prosegue (Req 3.2). Un binario atteso come
//! Mach-O/PE ma **non** leggibile come tale non produce alcun output, parziale o
//! completo, e restituisce un [`ExtractError::InvalidBinary`] che identifica il
//! binario (per identità tracciabile) e la causa (Req 3.6).
//!
//! _Requisiti: 3.1, 3.2, 3.6._

use std::collections::HashSet;

use cpp_demangle::{DemangleOptions, Symbol};
use object::{Object, ObjectSection, ObjectSymbol, SectionKind};

use super::binary::SourceBinary;
use super::ExtractError;

/// Identità di una classe recuperata da un record `RTTI_Source` (Req 3.1).
///
/// Trasporta l'identificatore di classe demanglato/undecorato più il locatore
/// del descrittore RTTI (`descriptor_rva`) usato dalla Phase C per associare la
/// classe alla sua vtable:
///
/// - Mach-O: indirizzo del simbolo `__ZTI<class>` (`type_info`), a cui punta
///   l'header RTTI della vtable `__ZTV<class>`.
/// - PE: indirizzo **base** del `TypeDescriptor` MSVC, riferito per RVA dal
///   `RTTICompleteObjectLocator` che precede la vftable.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RttiClass {
    /// Identificatore di classe (es. `"MenuLayer"`, `"cocos2d::CCNode"`),
    /// recuperato esclusivamente dal `Source_Binary` (Req 3.1).
    pub class: String,
    /// Indirizzo (VA) del descrittore RTTI: `type_info` (Mach-O) o
    /// `TypeDescriptor` base (PE). È il locatore d'associazione vtable→classe
    /// usato dalla Phase C.
    pub descriptor_rva: u64,
}

/// Estrae l'identità di classe da ciascun record `RTTI_Source` di un
/// `Source_Binary` Mach-O o PE (Req 3.1).
///
/// Restituisce `Ok(vec![])` (senza errore) per un binario leggibile **privo** di
/// record RTTI (Req 3.2). Restituisce [`ExtractError::InvalidBinary`], senza
/// alcun output parziale, se il binario non è leggibile come formato atteso
/// (Req 3.6).
///
/// L'ordine del risultato è quello di prima apparizione dei record (stabile e
/// deterministico); l'ordine totale finale è comunque imposto a valle dal
/// writer. I duplicati di nome di classe sono fusi sulla prima occorrenza.
pub fn parse_rtti(bin: &SourceBinary) -> Result<Vec<RttiClass>, ExtractError> {
    // Difesa in profondità (Req 3.6): re-parse dei byte e conferma del formato.
    // `open_source_binary` valida già il formato in apertura, ma il parser RTTI
    // non si fida e fallisce in chiusura.
    let file = object::File::parse(bin.bytes()).map_err(|err| ExtractError::InvalidBinary {
        identity: bin.identity.traceable_id(),
        expected: bin.format,
        reason: err.to_string(),
    })?;

    match file.format() {
        object::BinaryFormat::MachO => Ok(parse_macho_rtti(&file)),
        object::BinaryFormat::Pe => Ok(parse_pe_rtti(&file)),
        other => Err(ExtractError::InvalidBinary {
            identity: bin.identity.traceable_id(),
            expected: bin.format,
            reason: format!(
                "formato {other:?} non è un Source_Binary RTTI atteso (Mach-O o PE)"
            ),
        }),
    }
}

// ---------------------------------------------------------------------------
// Mach-O (Itanium / Clang ABI)
// ---------------------------------------------------------------------------

/// Estrae le identità di classe dai simboli `type_info` (`__ZTI…`) di un Mach-O.
fn parse_macho_rtti(file: &object::File) -> Vec<RttiClass> {
    let mut classes: Vec<RttiClass> = Vec::new();
    let mut seen: HashSet<String> = HashSet::new();

    for symbol in file.symbols().chain(file.dynamic_symbols()) {
        let name = match symbol.name() {
            Ok(name) if !name.is_empty() => name,
            _ => continue,
        };
        if let Some(class) = class_from_special(name, "typeinfo for ") {
            if seen.insert(class.clone()) {
                classes.push(RttiClass {
                    class,
                    descriptor_rva: symbol.address(),
                });
            }
        }
    }

    classes
}

/// Demangla un simbolo Itanium "speciale" (Mach-O) e, se il rendering inizia con
/// `prefix` (es. `"typeinfo for "` o `"vtable for "`), restituisce la parte
/// restante come identificatore di classe.
///
/// Gestisce l'underscore extra della convenzione Mach-O (`__ZTI…` → `_ZTI…`).
pub(crate) fn class_from_special(raw_name: &str, prefix: &str) -> Option<String> {
    let demangled = demangle_itanium(raw_name)?;
    demangled
        .strip_prefix(prefix)
        .map(|class| class.trim().to_owned())
        .filter(|class| !class.is_empty())
}

/// Recupera l'identità di classe dal simbolo di **vtable** Itanium
/// `_ZTV<class>` (Mach-O: `__ZTV<class>`), in modo robusto rispetto al
/// rendering del demangler.
///
/// A differenza del `type_info` (`_ZTI…`), che [`cpp_demangle`] rende come
/// `"typeinfo for X"` (gestito da [`class_from_special`] con prefisso
/// `"typeinfo for "`), la **vtable** `_ZTV…` è resa da `cpp_demangle` nella
/// forma `"{vtable(X)}"` — **non** `"vtable for X"` come farebbe `c++filt`.
/// Affidarsi al prefisso `"vtable for "` falliva quindi su **ogni** vtable
/// reale (0 classi recuperate). Questa funzione accetta **entrambe** le rese:
///
/// - `cpp_demangle`: `"{vtable(MenuLayer)}"` → `"MenuLayer"`,
///   `"{vtable(cocos2d::CCNode)}"` → `"cocos2d::CCNode"`;
/// - `c++filt`/Itanium classico: `"vtable for MenuLayer"` → `"MenuLayer"`.
///
/// È **fail-closed**: richiede che il simbolo grezzo sia effettivamente una
/// vtable Itanium (prefisso `_ZTV` / `__ZTV`), così da non confondere VTT
/// (`_ZTT`) o construction-vtable (`_ZTC`) con una vtable; restituisce `None`
/// per qualunque nome che non sia decodificabile come vtable.
pub(crate) fn class_from_vtable(raw_name: &str) -> Option<String> {
    // Solo simboli di vtable Itanium: `_ZTV<class>` (ELF) o `__ZTV<class>`
    // (convenzione Mach-O). Esclude VTT (`_ZTT`)/construction-vtable (`_ZTC`).
    if !(raw_name.starts_with("_ZTV") || raw_name.starts_with("__ZTV")) {
        return None;
    }
    let demangled = demangle_itanium(raw_name)?;

    // Resa `cpp_demangle`: `{vtable(<class>)}`.
    if let Some(rest) = demangled.strip_prefix("{vtable(") {
        if let Some(class) = rest.strip_suffix(")}") {
            let class = class.trim();
            if !class.is_empty() {
                return Some(class.to_owned());
            }
        }
        return None;
    }

    // Resa classica `c++filt`/Itanium: `vtable for <class>`.
    demangled
        .strip_prefix("vtable for ")
        .map(|class| class.trim().to_owned())
        .filter(|class| !class.is_empty())
}

/// Recupera l'identità di classe dalla **stringa nome del `type_info`** Itanium
/// (il valore restituito da `std::type_info::name()`), cioè il nome di tipo
/// mangled **senza** il prefisso `_ZTI`/`_ZTS` — es. `"9MenuLayer"`,
/// `"N7cocos2d6CCNodeE"`.
///
/// È il ponte per il **recupero strutturale** delle vtable su un Mach-O
/// **stripato** dei simboli `__ZTI`/`__ZTV` delle classi di GD (vedi
/// [`crate::extract::vtable`]): lì le classi non hanno simboli, ma gli **oggetti
/// `type_info`** sono ancora presenti nei dati e puntano a queste stringhe nome.
/// Ricostruendo il simbolo `_ZTI<name>` e demanglandolo (`"typeinfo for X"`) si
/// recupera l'identità di classe **identica** a quella del lato Android, così la
/// corrispondenza per nome di classe nel `Cross_Platform_Match` regge.
///
/// Fail-closed: restituisce `None` per qualunque stringa che non sia un nome di
/// tipo Itanium demanglabile a una classe.
pub(crate) fn class_from_typeinfo_name(type_name: &str) -> Option<String> {
    if type_name.is_empty() {
        return None;
    }
    let mangled = format!("_ZTI{type_name}");
    class_from_special(&mangled, "typeinfo for ")
}

/// Demangla un nome di simbolo Mach-O/Itanium con le opzioni di default.
///
/// Strippa l'underscore di guida della convenzione Mach-O (`__Z…` → `_Z…`) e
/// accetta sia i nomi già in forma `_Z…`. Restituisce `None` per i nomi non
/// Itanium o non decodificabili (fail-soft: il chiamante li ignora).
fn demangle_itanium(raw_name: &str) -> Option<String> {
    let itanium: &str = if raw_name.starts_with("__Z") {
        &raw_name[1..]
    } else if raw_name.starts_with("_Z") {
        raw_name
    } else {
        return None;
    };
    let symbol = Symbol::new(itanium.as_bytes()).ok()?;
    symbol.demangle_with_options(&DemangleOptions::new()).ok()
}

// ---------------------------------------------------------------------------
// PE (MSVC ABI) — best-effort documentato
// ---------------------------------------------------------------------------

/// Prefisso comune dei nomi decorati dei `TypeDescriptor` MSVC (il quarto
/// carattere è il codice di tipo: `V` classe, `U` struct, …).
const MSVC_TYPE_DESCRIPTOR_PREFIX: &[u8] = b".?A";

/// Scandisce le sezioni dati a sola lettura di un PE alla ricerca dei nomi dei
/// `TypeDescriptor` MSVC e ne estrae le identità di classe (best-effort).
fn parse_pe_rtti(file: &object::File) -> Vec<RttiClass> {
    let ptr_size = pointer_size(file) as u64;
    let mut classes: Vec<RttiClass> = Vec::new();
    let mut seen: HashSet<String> = HashSet::new();

    for section in file.sections() {
        // I `TypeDescriptor` vivono nelle sezioni dati (`.data`/`.rdata`).
        if !matches!(section.kind(), SectionKind::Data | SectionKind::ReadOnlyData) {
            continue;
        }
        let data = match section.data() {
            Ok(data) => data,
            Err(_) => continue,
        };
        let base = section.address();

        let mut i = 0usize;
        while i + MSVC_TYPE_DESCRIPTOR_PREFIX.len() < data.len() {
            if &data[i..i + MSVC_TYPE_DESCRIPTOR_PREFIX.len()] == MSVC_TYPE_DESCRIPTOR_PREFIX {
                // Stringa decorata null-terminata.
                let name_start = i;
                let mut j = i;
                while j < data.len() && data[j] != 0 {
                    j += 1;
                }
                if j < data.len() {
                    if let Ok(name) = std::str::from_utf8(&data[name_start..j]) {
                        if let Some(class) = undecorate_msvc_type_descriptor(name) {
                            if seen.insert(class.clone()) {
                                // Il campo nome è a offset 2 puntatori dall'inizio
                                // del `TypeDescriptor`; il `descriptor_rva` è la
                                // base della struttura.
                                let name_addr = base + name_start as u64;
                                let descriptor_rva =
                                    name_addr.saturating_sub(2 * ptr_size);
                                classes.push(RttiClass {
                                    class,
                                    descriptor_rva,
                                });
                            }
                        }
                    }
                }
                i = j + 1;
            } else {
                i += 1;
            }
        }
    }

    classes
}

/// Undecora in modo **conservativo** il nome di un `TypeDescriptor` MSVC nella
/// forma `.?AV<name>@@` (classe) o `.?AU<name>@@` (struct), inclusi i namespace
/// annidati separati da `@` e in ordine inverso.
///
/// - `.?AVMenuLayer@@` → `"MenuLayer"`
/// - `.?AVCCNode@cocos2d@@` → `"cocos2d::CCNode"`
///
/// Restituisce `None` (escludendo l'ingresso, fail-closed) quando il nome
/// contiene costrutti che non sappiamo undecorare con certezza — template o
/// back-reference MSVC (carattere `?`) o componenti con caratteri non
/// identificativi — anziché indovinare un nome di classe potenzialmente errato.
pub(crate) fn undecorate_msvc_type_descriptor(name: &str) -> Option<String> {
    // Prefisso `.?A` + un carattere di codice tipo (V=classe, U=struct,
    // T=union, W=enum).
    let rest = name.strip_prefix(".?A")?;
    let type_code = rest.chars().next()?;
    if !matches!(type_code, 'V' | 'U' | 'T' | 'W') {
        return None;
    }
    let body = &rest[type_code.len_utf8()..];

    // Il nome decorato termina con `@@`.
    let trimmed = body.strip_suffix("@@")?;
    if trimmed.is_empty() {
        return None;
    }

    // Conservativo: rifiuta template e back-reference (`?`) — non li undecoriamo.
    if trimmed.contains('?') {
        return None;
    }

    // Le componenti sono separate da `@` e in ordine inverso (innermost-first).
    let components: Vec<&str> = trimmed.split('@').filter(|c| !c.is_empty()).collect();
    if components.is_empty() {
        return None;
    }
    // Ogni componente deve essere un identificatore semplice.
    for component in &components {
        if !component
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '_')
        {
            return None;
        }
    }

    Some(
        components
            .iter()
            .rev()
            .copied()
            .collect::<Vec<_>>()
            .join("::"),
    )
}

/// Dimensione di un puntatore (byte) per il `Source_Binary`: 8 per i binari a 64
/// bit (tutte le `Target_Platform` supportate), 4 altrimenti.
pub(crate) fn pointer_size(file: &object::File) -> usize {
    if file.is_64() {
        8
    } else {
        4
    }
}
