//! `ItaniumDemangler` — decodifica di un [`RawSymbol`] Itanium C++ in un
//! [`DerivedSymbol`] (classe, metodo, `Derived_Signature` this-first, RVA),
//! la base del `Provenance_Tier` `symbol-table` (Req 2.2, 2.3, 2.4, 2.5).
//!
//! ## Strategia di decodifica (e perché parsiamo la stringa demanglata)
//!
//! La crate [`cpp_demangle`] espone un'API che parsa un nome mangled in un AST,
//! ma quell'AST è **privato**: i suoi nodi (classe, metodo, tipi dei parametri)
//! non sono navigabili pubblicamente. La sola superficie pubblica è il
//! **rendering testuale** del nome demanglato. Perciò estraiamo gli identificatori
//! e la firma **parsando l'output testuale** del demangler, sfruttando però le
//! sue opzioni per separare in modo affidabile le componenti:
//!
//! - `demangle_with_options(no_params + no_return_type)` → il **nome qualificato**
//!   puro, es. `MenuLayer::init`, `cocos2d::CCNode::addChild`;
//! - `demangle_with_options(no_return_type)` → nome **+ lista parametri**,
//!   es. `MenuLayer::init()`, `cocos2d::CCNode::addChild(cocos2d::CCNode*, int)`;
//! - `demangle_with_options(default)` → eventuale **tipo di ritorno** in testa
//!   (presente solo per i template Itanium, vedi sotto).
//!
//! Usare il nome-solo come **prefisso** del nome-con-parametri rende l'estrazione
//! della lista parametri robusta anche con operatori come `operator()`, perché
//! il prefisso include già le parentesi dell'operatore.
//!
//! ## Convenzione this-first (Req 2.3)
//!
//! Per i metodi **membro** la `Derived_Signature` è espressa con il puntatore
//! alla classe ricevente (`Class*`) come **primo** parametro, esattamente come
//! nel `Binding_Catalog` (es. `MenuLayer::init` → params `["MenuLayer*"]`). Le
//! funzioni libere (senza qualificazione di classe) non ricevono alcun `this`.
//!
//! ### Limitazione nota — namespace vs classe
//!
//! L'Itanium C++ ABI **non** distingue, nel nome mangled, una qualificazione di
//! *namespace* da una di *classe*: `N3foo3barE` può essere il metodo `bar` della
//! classe `foo` **oppure** la funzione libera `bar` nel namespace `foo`. Dalla
//! sola stringa demanglata questa ambiguità è irrisolvibile. Adottiamo la
//! convenzione del catalogo: **ogni nome qualificato è trattato come metodo
//! membro** (this-first), coerente con la realtà dei simboli di RobTop (dominati
//! dai metodi di classe). Una funzione libera in un namespace verrebbe quindi
//! trattata come membro: è un best-effort documentato, non una garanzia.
//!
//! ## Limitazione nota — tipo di ritorno (Req 2.2)
//!
//! Il mangling Itanium codifica **sempre** i tipi dei parametri, ma il tipo di
//! ritorno **solo** per le funzioni template. Per i metodi non-template il
//! ritorno **non è recuperabile** dal solo nome mangled: in quel caso usiamo il
//! segnaposto opaco [`UNKNOWN_RETURN_TYPE`] anziché **inventare** un tipo errato.
//! Quando il ritorno *è* codificato (template), lo recuperiamo fedelmente. Questa
//! limitazione è rilevante per il cross-check successivo e va tenuta presente
//! (es. il seed `MenuLayer::init` ha ritorno `bool` noto **manualmente**, non
//! derivabile qui).
//!
//! _Requisiti: 2.2, 2.3, 2.4, 2.5._

use cpp_demangle::{DemangleOptions, Symbol};

use super::elf::RawSymbol;
use super::Signature;

/// Segnaposto opaco per un tipo di ritorno non recuperabile dal nome mangled
/// (metodi non-template; vedi documentazione del modulo).
///
/// È deliberatamente un token **non** confondibile con un tipo C++ reale, così
/// da non fabbricare un ritorno errato (Req 2.2).
pub const UNKNOWN_RETURN_TYPE: &str = "?";

/// Un simbolo decodificato dal `Mangled_Symbol` Android: identità canonica
/// (classe + metodo), `Derived_Signature` this-first e RVA definito (Req 2.2,
/// 2.3, 2.4).
///
/// Un `DerivedSymbol` con RVA definito è la base del `Provenance_Tier`
/// `symbol-table` (Req 2.4); l'assegnazione del tier vera e propria vive nella
/// Phase F (`provenance.rs`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DerivedSymbol {
    /// Identificatore di classe (vuoto per le funzioni libere) (Req 2.2).
    pub class: String,
    /// Identificatore di metodo / funzione (Req 2.2).
    pub method: String,
    /// `Derived_Signature` riusata dalla pipeline; this-first per i membri
    /// (Req 2.3).
    pub signature: Signature,
    /// RVA definito nell'ELF Android (Req 2.4).
    pub rva: u64,
}

impl DerivedSymbol {
    /// `SymbolId` canonico `"Class::method"` (o solo `method` per le funzioni
    /// libere), identico alla chiave del `Binding_Catalog`.
    pub fn symbol_id(&self) -> String {
        if self.class.is_empty() {
            self.method.clone()
        } else {
            format!("{}::{}", self.class, self.method)
        }
    }
}

/// Errore di decodifica di un `Mangled_Symbol`.
///
/// Segue la convenzione `thiserror` del resto dell'estrattore. Ogni variante
/// trasporta il `Mangled_Symbol` grezzo, così che il **chiamante** possa
/// escludere il simbolo e registrarne il mangled + la causa senza interrompere
/// l'elaborazione degli altri (Req 2.5, 2.6).
#[derive(Debug, thiserror::Error)]
pub enum DemangleError {
    /// Il `Mangled_Symbol` è decodificabile ma **privo** di un indirizzo
    /// definito (simbolo non definito o importato): escluso, **nessun** tier
    /// `symbol-table` (Req 2.6).
    #[error("simbolo senza indirizzo definito {mangled:?}: non definito o importato")]
    NoAddress {
        /// `Mangled_Symbol` grezzo.
        mangled: String,
    },

    /// Il `Mangled_Symbol` non è analizzabile dal demangler Itanium (Req 2.5).
    #[error("Mangled_Symbol non decodificabile {mangled:?}: {reason}")]
    Parse {
        /// `Mangled_Symbol` grezzo.
        mangled: String,
        /// Causa della mancata decodifica.
        reason: String,
    },

    /// Il `Mangled_Symbol` è stato parsato ma non se ne è potuta derivare una
    /// struttura classe/metodo/firma utilizzabile (Req 2.5).
    #[error("Mangled_Symbol non interpretabile come funzione {mangled:?}: {reason}")]
    Unsupported {
        /// `Mangled_Symbol` grezzo.
        mangled: String,
        /// Causa dell'impossibilità di estrarre la struttura.
        reason: String,
    },
}

/// Decodifica un [`RawSymbol`] Itanium C++ in un [`DerivedSymbol`] con firma
/// this-first (Req 2.2, 2.3).
///
/// Restituisce `Err` (che il chiamante esclude e registra senza abortire gli
/// altri, Req 2.5) quando:
///
/// - il simbolo è privo di indirizzo definito ([`DemangleError::NoAddress`],
///   Req 2.6);
/// - il nome non è decodificabile dal demangler ([`DemangleError::Parse`]);
/// - non se ne può estrarre la struttura di funzione
///   ([`DemangleError::Unsupported`]).
pub fn demangle(sym: &RawSymbol) -> Result<DerivedSymbol, DemangleError> {
    // Un offset `symbol-table` richiede un RVA definito (Req 2.4, 2.6).
    let rva = sym.rva.ok_or_else(|| DemangleError::NoAddress {
        mangled: sym.mangled.clone(),
    })?;

    let parsed = Symbol::new(sym.mangled.as_bytes()).map_err(|err| DemangleError::Parse {
        mangled: sym.mangled.clone(),
        reason: err.to_string(),
    })?;

    // Nome qualificato puro (senza parametri né ritorno): es. `MenuLayer::init`.
    let name_only = parsed
        .demangle_with_options(&DemangleOptions::new().no_params().no_return_type())
        .map_err(|err| DemangleError::Parse {
            mangled: sym.mangled.clone(),
            reason: format!("rendering del nome fallito: {err}"),
        })?;

    // Nome + lista parametri (senza ritorno): es. `MenuLayer::init()`.
    let with_params = parsed
        .demangle_with_options(&DemangleOptions::new().no_return_type())
        .map_err(|err| DemangleError::Parse {
            mangled: sym.mangled.clone(),
            reason: format!("rendering dei parametri fallito: {err}"),
        })?;

    // Rendering completo (con eventuale ritorno per i template).
    let with_return = parsed
        .demangle_with_options(&DemangleOptions::new())
        .map_err(|err| DemangleError::Parse {
            mangled: sym.mangled.clone(),
            reason: format!("rendering completo fallito: {err}"),
        })?;

    // 1. Separa classe e metodo dal nome qualificato (split su `::` di livello 0).
    let (class, method) = split_class_method(&name_only).ok_or_else(|| {
        DemangleError::Unsupported {
            mangled: sym.mangled.clone(),
            reason: format!("nome demanglato privo di identificatore: {name_only:?}"),
        }
    })?;

    // 2. Estrai la lista parametri dal nome-con-parametri usando il nome-solo
    //    come prefisso (robusto anche con `operator()`).
    let param_list = extract_param_list(&name_only, &with_params).ok_or_else(|| {
        DemangleError::Unsupported {
            mangled: sym.mangled.clone(),
            reason: format!("lista parametri non individuabile in {with_params:?}"),
        }
    })?;

    let mut params = split_top_level(param_list, ',');
    // Itanium: nessun parametro ⇒ `()`; `void` esplicito ⇒ nessun parametro.
    if params.len() == 1 && (params[0].is_empty() || params[0] == "void") {
        params.clear();
    }

    // 3. Convenzione this-first: i metodi membro ricevono `Class*` come primo
    //    parametro (Req 2.3). Le funzioni libere no.
    if !class.is_empty() {
        params.insert(0, format!("{class}*"));
    }

    // 4. Tipo di ritorno: recuperabile solo per i template (prefisso in testa);
    //    altrimenti segnaposto opaco, senza fabbricare (Req 2.2).
    let return_type = recover_return_type(&with_return, &with_params);

    Ok(DerivedSymbol {
        class,
        method,
        signature: Signature::new(return_type, params),
        rva,
    })
}

/// Separa un nome qualificato in `(class, method)` sullo `::` di **livello 0**
/// (ignorando `::` annidati in argomenti template o parentesi).
///
/// - `"MenuLayer::init"` → `("MenuLayer", "init")`
/// - `"cocos2d::CCNode::addChild"` → `("cocos2d::CCNode", "addChild")`
/// - `"globalFn"` → `("", "globalFn")` (funzione libera)
///
/// Restituisce `None` se il metodo risultante è vuoto.
fn split_class_method(name: &str) -> Option<(String, String)> {
    let sep = last_top_level_scope(name);
    let (class, method) = match sep {
        Some(idx) => (name[..idx].to_owned(), name[idx + 2..].to_owned()),
        None => (String::new(), name.to_owned()),
    };
    if method.is_empty() {
        return None;
    }
    Some((class, method))
}

/// Indice di byte dell'ultimo `::` di **livello 0** in `name`, o `None` se non
/// presente a livello 0. La profondità è contata su `<>`, `()`, `[]`, `{}`.
fn last_top_level_scope(name: &str) -> Option<usize> {
    let bytes = name.as_bytes();
    let mut depth: i32 = 0;
    let mut last: Option<usize> = None;
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'<' | b'(' | b'[' | b'{' => depth += 1,
            b'>' | b')' | b']' | b'}' => {
                if depth > 0 {
                    depth -= 1;
                }
            }
            b':' if depth == 0 && i + 1 < bytes.len() && bytes[i + 1] == b':' => {
                last = Some(i);
                i += 2;
                continue;
            }
            _ => {}
        }
        i += 1;
    }
    last
}

/// Estrae il contenuto della lista parametri da `with_params`, usando
/// `name_only` come prefisso.
///
/// `with_params` ha la forma `"<name_only>(<params>)[ qualificatori]"`. Si
/// individua la prima parentesi dopo il prefisso e la sua chiusura bilanciata.
/// Restituisce il contenuto fra le parentesi (può essere vuoto). Se il prefisso
/// non combacia, si ripiega sull'ultimo gruppo di parentesi di livello 0.
fn extract_param_list<'a>(name_only: &str, with_params: &'a str) -> Option<&'a str> {
    if let Some(rest) = with_params.strip_prefix(name_only) {
        let rest = rest.trim_start();
        if rest.starts_with('(') {
            return balanced_paren_content(rest);
        }
    }
    // Fallback: ultimo gruppo di parentesi bilanciate di livello 0.
    last_top_level_paren_content(with_params)
}

/// Dato un testo che **inizia** con `(`, restituisce il contenuto fino alla
/// parentesi chiusa bilanciata corrispondente.
fn balanced_paren_content(s: &str) -> Option<&str> {
    let bytes = s.as_bytes();
    if bytes.first() != Some(&b'(') {
        return None;
    }
    let mut depth: i32 = 0;
    for (i, &b) in bytes.iter().enumerate() {
        match b {
            b'(' => depth += 1,
            b')' => {
                depth -= 1;
                if depth == 0 {
                    return Some(&s[1..i]);
                }
            }
            _ => {}
        }
    }
    None
}

/// Restituisce il contenuto dell'**ultimo** gruppo di parentesi di livello 0 in
/// `s` (fallback quando il prefisso del nome non combacia).
fn last_top_level_paren_content(s: &str) -> Option<&str> {
    let bytes = s.as_bytes();
    let mut depth: i32 = 0;
    let mut open_at: Option<usize> = None;
    let mut last: Option<(usize, usize)> = None;
    for (i, &b) in bytes.iter().enumerate() {
        match b {
            b'(' => {
                if depth == 0 {
                    open_at = Some(i);
                }
                depth += 1;
            }
            b')' => {
                depth -= 1;
                if depth == 0 {
                    if let Some(start) = open_at.take() {
                        last = Some((start + 1, i));
                    }
                }
            }
            _ => {}
        }
    }
    last.map(|(start, end)| &s[start..end])
}

/// Divide `s` sui separatori `sep` di **livello 0** (ignorando quelli annidati
/// in `<>`, `()`, `[]`, `{}`), restituendo i segmenti già trimmati.
///
/// Una stringa vuota produce `[""]` (un singolo segmento vuoto), così che il
/// chiamante possa riconoscere la lista parametri vuota.
fn split_top_level(s: &str, sep: char) -> Vec<String> {
    let sep = sep as u8;
    let bytes = s.as_bytes();
    let mut depth: i32 = 0;
    let mut segments: Vec<String> = Vec::new();
    let mut start = 0;
    for (i, &b) in bytes.iter().enumerate() {
        match b {
            b'<' | b'(' | b'[' | b'{' => depth += 1,
            b'>' | b')' | b']' | b'}' => {
                if depth > 0 {
                    depth -= 1;
                }
            }
            _ if b == sep && depth == 0 => {
                segments.push(s[start..i].trim().to_owned());
                start = i + 1;
            }
            _ => {}
        }
    }
    segments.push(s[start..].trim().to_owned());
    segments
}

/// Recupera il tipo di ritorno **solo** quando è codificato (funzioni template):
/// in tal caso `with_return` ha la forma `"<ret> <with_params>"`, quindi termina
/// con `with_params` ed espone il ritorno come prefisso. Altrimenti restituisce
/// il segnaposto opaco [`UNKNOWN_RETURN_TYPE`], senza fabbricare (Req 2.2).
fn recover_return_type(with_return: &str, with_params: &str) -> String {
    if with_return != with_params {
        if let Some(prefix) = with_return.strip_suffix(with_params) {
            let ret = prefix.trim();
            if !ret.is_empty() {
                return ret.to_owned();
            }
        }
    }
    UNKNOWN_RETURN_TYPE.to_owned()
}
