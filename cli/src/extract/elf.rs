//! `ElfSymbolSource` — lettura dei `Mangled_Symbol` C++ dalla tabella dei
//! simboli dell'ELF Android (`libcocos2dcpp.so`), la **lista master** di nomi e
//! firme first-party del Binding_Extractor (Req 2.1, 2.6, 2.7).
//!
//! Questo modulo è il primo stadio della catena `ELF → demangle`: estrae i
//! [`RawSymbol`] grezzi (stringa mangled + RVA eventuale) dalla tabella dei
//! simboli, **senza** decodificarli. La decodifica in
//! [`crate::extract::demangle::DerivedSymbol`] (classe/metodo/firma) avviene nel
//! modulo [`crate::extract::demangle`].
//!
//! Politica di estrazione:
//!
//! - Sono considerati **solo** i simboli di **tipo funzione**
//!   ([`object::SymbolKind::Text`]) il cui nome è un `Mangled_Symbol` C++
//!   Itanium, riconosciuto dal prefisso standard `_Z` dell'Itanium C++ ABI
//!   (Req 2.1). I simboli C non-mangled, i dati e gli altri tipi sono ignorati a
//!   monte: ciò che ha prefisso `_Z` ma non è decodificabile sarà comunque
//!   escluso e registrato a valle dal [`crate::extract::demangle`] (Req 2.5).
//! - Un simbolo **definito** (con un indirizzo nell'immagine) riceve
//!   `rva = Some(addr)`; un simbolo **non definito o importato**
//!   (`is_undefined()`) riceve `rva = None` e sarà escluso a valle **senza**
//!   tier `symbol-table` (Req 2.6).
//! - Vengono lette **sia** la tabella dei simboli statica (`.symtab`) **sia**
//!   quella dinamica (`.dynsym`): per un `.so` Android stripato gli export
//!   vivono in `.dynsym`. I duplicati di nome sono fusi preferendo l'occorrenza
//!   **definita**, con ordine di prima apparizione preservato (determinismo).
//! - **Fail-closed**: un `Source_Binary` atteso come ELF ma non leggibile come
//!   ELF valido non produce **alcun** output e restituisce un
//!   [`ExtractError::InvalidBinary`] che identifica il binario (per identità
//!   tracciabile) e la causa (Req 2.7). `open_source_binary` valida già il
//!   formato in apertura; questo controllo è una difesa in profondità.
//!
//! _Requisiti: 2.1, 2.6, 2.7._

use std::collections::HashMap;

use object::{Object, ObjectSymbol, SymbolKind};

use super::binary::SourceBinary;
use super::{BinaryFormat, ExtractError};

/// Prefisso standard dei simboli C++ mangled secondo l'Itanium C++ ABI.
///
/// Ogni `Mangled_Symbol` C++ Itanium inizia con `_Z`; filtrare su questo
/// prefisso seleziona i simboli C++ ed evita di trattare i simboli C
/// non-mangled (es. `malloc`) come candidati al demangling (Req 2.1).
const ITANIUM_MANGLE_PREFIX: &str = "_Z";

/// Un simbolo **grezzo** letto dalla tabella dei simboli dell'ELF, prima del
/// demangling.
///
/// `mangled` è la stringa del `Mangled_Symbol` così come compare nella tabella;
/// `rva` è l'indirizzo definito nell'immagine (`Some`) oppure `None` per i
/// simboli non definiti o importati, che saranno esclusi a valle senza tier
/// `symbol-table` (Req 2.6).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RawSymbol {
    /// Stringa del `Mangled_Symbol` C++ Itanium (prefisso `_Z`).
    pub mangled: String,
    /// RVA definito nell'ELF (`Some`) oppure `None` se non definito/importato
    /// (Req 2.6).
    pub rva: Option<u64>,
}

/// Legge la tabella dei simboli dell'ELF ed estrae **ogni** `Mangled_Symbol`
/// C++ di tipo funzione (Req 2.1).
///
/// Restituisce i simboli sia **definiti** (`rva = Some`) sia **non definiti /
/// importati** (`rva = None`, esclusi a valle senza tier `symbol-table`,
/// Req 2.6). I duplicati di nome fra `.symtab` e `.dynsym` sono fusi preferendo
/// l'occorrenza definita.
///
/// **Fail-closed**: se il `Source_Binary` non è leggibile come ELF valido,
/// restituisce [`ExtractError::InvalidBinary`] senza produrre alcun output
/// (Req 2.7).
pub fn read_elf_symbols(bin: &SourceBinary) -> Result<Vec<RawSymbol>, ExtractError> {
    // Difesa in profondità: re-parse dei byte come binario `object` e conferma
    // che si tratti di un ELF valido (Req 2.7). `open_source_binary` ha già
    // validato il formato in apertura, ma `read_elf_symbols` non si fida.
    let file = object::File::parse(bin.bytes()).map_err(|err| ExtractError::InvalidBinary {
        identity: bin.identity.traceable_id(),
        expected: BinaryFormat::Elf,
        reason: err.to_string(),
    })?;

    if file.format() != object::BinaryFormat::Elf {
        return Err(ExtractError::InvalidBinary {
            identity: bin.identity.traceable_id(),
            expected: BinaryFormat::Elf,
            reason: format!(
                "formato rilevato {:?}, atteso un ELF valido",
                file.format()
            ),
        });
    }

    // Accumula i simboli di tipo funzione con prefisso Itanium, fondendo i
    // duplicati di nome (preferendo la variante definita). `order` conserva
    // l'ordine di prima apparizione per un risultato deterministico (l'ordine
    // totale finale è comunque imposto dal writer per `SymbolId`, Req 9.2).
    let mut symbols: Vec<RawSymbol> = Vec::new();
    let mut index_by_name: HashMap<String, usize> = HashMap::new();

    // `.symtab` (statica) seguita da `.dynsym` (dinamica): per i `.so` Android
    // stripati gli export vivono nella tabella dinamica.
    for symbol in file.symbols().chain(file.dynamic_symbols()) {
        // Solo simboli di tipo funzione (codice) (Req 2.1).
        if symbol.kind() != SymbolKind::Text {
            continue;
        }

        // Nome leggibile e non vuoto.
        let name = match symbol.name() {
            Ok(name) if !name.is_empty() => name,
            _ => continue,
        };

        // Solo `Mangled_Symbol` C++ Itanium (prefisso `_Z`) (Req 2.1).
        if !name.starts_with(ITANIUM_MANGLE_PREFIX) {
            continue;
        }

        // Simbolo definito ⇒ RVA presente; non definito/importato ⇒ None
        // (escluso a valle senza tier `symbol-table`, Req 2.6).
        let rva = if symbol.is_undefined() {
            None
        } else {
            Some(symbol.address())
        };

        match index_by_name.get(name).copied() {
            // Nome già visto: se il precedente era senza indirizzo e questo è
            // definito, promuovi all'occorrenza definita.
            Some(existing) => {
                if symbols[existing].rva.is_none() {
                    if let Some(addr) = rva {
                        symbols[existing].rva = Some(addr);
                    }
                }
                // Altrimenti il duplicato è ridondante: mantieni la prima
                // occorrenza definita.
            }
            None => {
                index_by_name.insert(name.to_owned(), symbols.len());
                symbols.push(RawSymbol {
                    mangled: name.to_owned(),
                    rva,
                });
            }
        }
    }

    Ok(symbols)
}
