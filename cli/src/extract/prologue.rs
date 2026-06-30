//! `PrologueSanityChecker` — controllo automatico di plausibilità del prologo di
//! una funzione all'offset derivato, per le `Architecture` arm64 e x86-64
//! (Req 5.1, 5.2, 5.3, 5.4, 12.1, 12.3).
//!
//! ## Cosa dimostra (e cosa no)
//!
//! Il `Prologue_Sanity_Check` disassembla una **finestra di lunghezza fissa
//! ≥ 16 byte** a partire dall'offset e ne conferma la plausibilità come **entry
//! di funzione** per l'architettura bersaglio. È un controllo **necessario ma
//! non sufficiente**: dimostra che all'offset c'è *una* funzione plausibile, non
//! che sia *la funzione giusta*. Per questo l'esito automatico è sempre
//! **distinto** dalla conferma manuale del prologo (Phase E / `otool`) e
//! **nessun** offset può essere promosso al `Provenance_Tier`
//! `manual-prologue-confirmed` sulla sola base di questo controllo (Req 5.6).
//! Quel tier è di competenza della Phase F (manuale) e qui **non** viene mai
//! prodotto: questo modulo espone solo il **gate** verso il `Verified_Flag`,
//! non un'assegnazione di tier.
//!
//! ## Criterio di plausibilità (Req 5.1, 5.3)
//!
//! Per un offset `> 0` e `≠ SENTINEL_VALUE`, la finestra è classificata
//! [`PrologueOutcome::PlausibleEntry`] **se e solo se**:
//!
//! 1. l'**intera** finestra ≥ 16 byte decodifica in una sequenza di istruzioni
//!    **valide e definite** per l'`Architecture` (nessun opcode illegale /
//!    `Invalid` e nessun errore di decodifica); **e**
//! 2. la **prima** istruzione decodificata **non** è un'istruzione di ritorno o
//!    di trap.
//!
//! Altrimenti la finestra è [`PrologueOutcome::NotPlausible`] con la causa
//! (Req 5.3).
//!
//! ### Per architettura
//!
//! - **arm64**: istruzioni a lunghezza fissa di 4 byte ⇒ la finestra di 16 byte
//!   è esattamente **4 istruzioni**; tutte devono decodificare e nessuna deve
//!   avere opcode `Invalid`. Poiché il decoder AArch64 di `yaxpeax-arm` riporta
//!   `well_defined() == true` indiscriminatamente, la validità è derivata dal
//!   `Result` di decodifica **e** dal confronto esplicito con `Opcode::Invalid`.
//! - **x86-64**: istruzioni a lunghezza variabile ⇒ si decodifica in sequenza
//!   finché la lunghezza cumulata copre **≥ 16 byte**, trattando un errore di
//!   decodifica (o un opcode `Invalid`) entro la finestra come
//!   [`PrologueOutcome::NotPlausible`]. L'ultima istruzione può estendersi oltre
//!   il 16° byte: per consentirne la decodifica si forniscono al decoder i byte
//!   contigui disponibili nella stessa sezione (fino alla massima lunghezza di
//!   un'istruzione x86, 15 byte), pur **richiedendo** che siano leggibili almeno
//!   i 16 byte della finestra (Req 5.4).
//!
//! ## Casi `Skipped` (senza disassemblare, Req 5.4)
//!
//! Prima di qualunque decodifica, il checker distingue tre condizioni che
//! impediscono il controllo e mantengono il `Verified_Flag` a `false`:
//!
//! - `offset == 0` → [`SkipReason::ZeroOffset`];
//! - `offset == SENTINEL_VALUE` → [`SkipReason::SentinelOffset`];
//! - la finestra ≥ 16 byte **non** è interamente leggibile →
//!   [`SkipReason::UnreadablePrologue`].
//!
//! ## Determinismo
//!
//! Gli stessi byte e la stessa `Architecture` producono **sempre** lo stesso
//! [`PrologueOutcome`]: i decoder sono puri e deterministici e la finestra è di
//! lunghezza fissa.
//!
//! _Requisiti: 5.1, 5.2, 5.3, 5.4, 12.1, 12.3._

use object::{Object, ObjectSection};

use yaxpeax_arch::{Decoder, LengthedInstruction, U8Reader};
use yaxpeax_arm::armv8::a64::{InstDecoder as Arm64Decoder, Opcode as Arm64Opcode};
use yaxpeax_x86::long_mode::{InstDecoder as X86Decoder, Opcode as X86Opcode};

use super::binary::SourceBinary;
use super::{Architecture, SymbolId, TargetPair, SENTINEL_VALUE};

/// Lunghezza **fissa** della finestra di prologo disassemblata, in byte: almeno
/// 16 come richiesto da Req 5.1. Per arm64 sono esattamente 4 istruzioni a 4
/// byte; per x86-64 è il numero minimo di byte che la sequenza di istruzioni
/// deve coprire.
pub const PROLOGUE_WINDOW_LEN: u64 = 16;

/// Lunghezza massima di una singola istruzione x86-64 (byte): serve a fornire al
/// decoder i byte contigui sufficienti a decodificare l'istruzione che eventualmente
/// si estende oltre il 16° byte della finestra, senza ampliare il requisito di
/// leggibilità (che resta la finestra di 16 byte, Req 5.4).
const X86_MAX_INSN_LEN: u64 = 15;

/// Esito del `Prologue_Sanity_Check` su un offset derivato (Req 5.1, 5.3, 5.4).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PrologueOutcome {
    /// La finestra ≥ 16 byte decodifica interamente in istruzioni valide e la
    /// prima istruzione non è ret/trap: entry di funzione plausibile
    /// (Req 5.1, 5.2).
    PlausibleEntry,
    /// La finestra contiene un opcode illegale/non definito oppure la prima
    /// istruzione è di ritorno o di trap: non plausibile (Req 5.3).
    NotPlausible {
        /// Causa leggibile della non plausibilità.
        reason: String,
    },
    /// Il controllo è stato saltato **senza** disassemblare per una delle
    /// condizioni distinte di [`SkipReason`] (Req 5.4).
    Skipped {
        /// Condizione che ha impedito il controllo.
        reason: SkipReason,
    },
}

/// Le tre condizioni distinte che fanno **saltare** il `Prologue_Sanity_Check`
/// senza disassemblare, mantenendo il `Verified_Flag` a `false` (Req 5.4).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SkipReason {
    /// L'offset è pari a zero.
    ZeroOffset,
    /// L'offset è uguale al `Sentinel_Value`.
    SentinelOffset,
    /// La finestra di prologo ≥ 16 byte a partire dall'offset non è interamente
    /// leggibile dal `Source_Binary`.
    UnreadablePrologue,
}

impl SkipReason {
    /// Descrizione testuale stabile della condizione di skip.
    pub fn description(self) -> &'static str {
        match self {
            SkipReason::ZeroOffset => "offset nullo (== 0)",
            SkipReason::SentinelOffset => "offset pari al Sentinel_Value",
            SkipReason::UnreadablePrologue => {
                "finestra di prologo di almeno 16 byte non interamente leggibile"
            }
        }
    }
}

/// Esegue il `Prologue_Sanity_Check` per un offset derivato del `Source_Binary`,
/// disassemblando una finestra di lunghezza fissa [`PROLOGUE_WINDOW_LEN`] byte a
/// partire dall'offset e classificandola per l'`Architecture` `arch`
/// (Req 5.1, 5.3, 5.4).
///
/// I casi [`PrologueOutcome::Skipped`] sono rilevati **prima** di qualunque
/// decodifica e con causa distinta (Req 5.4):
///
/// - `offset == 0` → [`SkipReason::ZeroOffset`];
/// - `offset == SENTINEL_VALUE` → [`SkipReason::SentinelOffset`];
/// - finestra ≥ 16 byte non interamente leggibile → [`SkipReason::UnreadablePrologue`].
///
/// Altrimenti restituisce [`PrologueOutcome::PlausibleEntry`] **se e solo se**
/// l'intera finestra decodifica in istruzioni valide/definite e la prima non è
/// ret/trap; in caso contrario [`PrologueOutcome::NotPlausible`] con la causa.
///
/// È una **funzione pura** dei byte del binario, dell'offset e
/// dell'architettura: stessi input ⇒ stesso esito (determinismo).
pub fn check_prologue(bin: &SourceBinary, offset: u64, arch: Architecture) -> PrologueOutcome {
    // Req 5.4: i casi di skip sono valutati PRIMA di disassemblare, distinti.
    if offset == 0 {
        return PrologueOutcome::Skipped {
            reason: SkipReason::ZeroOffset,
        };
    }
    if offset == SENTINEL_VALUE {
        return PrologueOutcome::Skipped {
            reason: SkipReason::SentinelOffset,
        };
    }

    // Risolve i byte del binario per la VA dell'offset, come fa il lettore di
    // vtable: individua la sezione che contiene la finestra. Richiede ALMENO i
    // 16 byte della finestra (Req 5.4); per x86-64 fornisce i byte contigui
    // extra disponibili (fino a X86_MAX_INSN_LEN) per l'eventuale istruzione che
    // oltrepassa il 16° byte.
    let max_len = match arch {
        Architecture::Arm64 => PROLOGUE_WINDOW_LEN,
        Architecture::X86_64 => PROLOGUE_WINDOW_LEN + X86_MAX_INSN_LEN,
    };
    let window = match read_prologue_window(bin, offset, PROLOGUE_WINDOW_LEN, max_len) {
        Some(window) => window,
        None => {
            return PrologueOutcome::Skipped {
                reason: SkipReason::UnreadablePrologue,
            };
        }
    };

    match arch {
        Architecture::Arm64 => check_prologue_arm64(&window),
        Architecture::X86_64 => check_prologue_x86_64(&window),
    }
}

/// Risolve i byte del `Source_Binary` per la VA `va`, individuando la sezione
/// che la contiene, e restituisce uno slice lungo fra `min_len` e `max_len`
/// byte (limitato alla fine della sezione). Restituisce `None` se sono leggibili
/// **meno** di `min_len` byte contigui a partire da `va` (Req 5.4).
fn read_prologue_window(
    bin: &SourceBinary,
    va: u64,
    min_len: u64,
    max_len: u64,
) -> Option<Vec<u8>> {
    // Difesa in profondità: il binario è già stato validato in apertura, ma il
    // checker non si fida. Un re-parse fallito ⇒ prologo non leggibile.
    let file = object::File::parse(bin.bytes()).ok()?;

    for section in file.sections() {
        let base = section.address();
        let end = base.saturating_add(section.size());
        if va < base || va >= end {
            continue;
        }
        // Byte disponibili dalla VA alla fine della sezione.
        let available = end - va;
        if available < min_len {
            // La finestra minima non è interamente contenuta nella sezione.
            return None;
        }
        let data = section.data().ok()?;
        let start = (va - base) as usize;
        if start >= data.len() {
            return None;
        }
        let want = available.min(max_len) as usize;
        // I byte effettivamente materializzati nella sezione possono essere meno
        // della dimensione virtuale (es. .bss): richiediamo almeno `min_len`
        // byte reali a partire da `start`.
        if start + (min_len as usize) > data.len() {
            return None;
        }
        let upper = (start + want).min(data.len());
        return Some(data[start..upper].to_vec());
    }
    None
}

// ---------------------------------------------------------------------------
// arm64 (AArch64)
// ---------------------------------------------------------------------------

/// Classifica la finestra di prologo per arm64: 4 istruzioni a 4 byte (16 byte
/// totali). Tutte devono decodificare in opcode validi/definiti e la prima non
/// deve essere ret/trap (Req 5.1, 5.3).
fn check_prologue_arm64(window: &[u8]) -> PrologueOutcome {
    let decoder = Arm64Decoder::default();
    let mut reader = U8Reader::new(window);

    let total = (PROLOGUE_WINDOW_LEN / 4) as usize; // 4 istruzioni
    for index in 0..total {
        match decoder.decode(&mut reader) {
            Ok(inst) => {
                // Il decoder AArch64 di yaxpeax-arm riporta `well_defined()`
                // sempre `true`: la validità è derivata dal confronto esplicito
                // con `Opcode::Invalid` (Req 5.1, 5.3).
                if inst.opcode == Arm64Opcode::Invalid {
                    return PrologueOutcome::NotPlausible {
                        reason: format!(
                            "istruzione arm64 #{index} non definita (opcode illegale)"
                        ),
                    };
                }
                // La PRIMA istruzione non deve essere ret/trap (Req 5.1).
                if index == 0 {
                    if let Some(kind) = arm64_ret_or_trap(inst.opcode) {
                        return PrologueOutcome::NotPlausible {
                            reason: format!(
                                "la prima istruzione arm64 è {kind} ({:?}), non un prologo di funzione",
                                inst.opcode
                            ),
                        };
                    }
                }
            }
            Err(err) => {
                return PrologueOutcome::NotPlausible {
                    reason: format!(
                        "decodifica arm64 fallita all'istruzione #{index}: {err}"
                    ),
                };
            }
        }
    }

    PrologueOutcome::PlausibleEntry
}

/// Se l'opcode arm64 è un'istruzione di ritorno o di trap, restituisce la sua
/// categoria leggibile; altrimenti `None`.
fn arm64_ret_or_trap(opcode: Arm64Opcode) -> Option<&'static str> {
    match opcode {
        // Ritorni.
        Arm64Opcode::RET
        | Arm64Opcode::RETAA
        | Arm64Opcode::RETAB
        | Arm64Opcode::ERET
        | Arm64Opcode::ERETAA
        | Arm64Opcode::ERETAB => Some("un'istruzione di ritorno"),
        // Trap / debug / eccezioni che non aprono mai un prologo di funzione.
        Arm64Opcode::BRK
        | Arm64Opcode::HLT
        | Arm64Opcode::UDF
        | Arm64Opcode::DCPS1
        | Arm64Opcode::DCPS2
        | Arm64Opcode::DCPS3 => Some("un'istruzione di trap"),
        _ => None,
    }
}

// ---------------------------------------------------------------------------
// x86-64
// ---------------------------------------------------------------------------

/// Classifica la finestra di prologo per x86-64: decodifica in sequenza finché
/// la lunghezza cumulata copre ≥ 16 byte. Ogni istruzione deve decodificare in
/// un opcode valido/definito; la prima non deve essere ret/trap (Req 5.1, 5.3).
fn check_prologue_x86_64(window: &[u8]) -> PrologueOutcome {
    let decoder = X86Decoder::default();
    let mut reader = U8Reader::new(window);

    let mut consumed: u64 = 0;
    let mut index: usize = 0;
    while consumed < PROLOGUE_WINDOW_LEN {
        match decoder.decode(&mut reader) {
            Ok(inst) => {
                let opcode = inst.opcode();
                if opcode == X86Opcode::Invalid {
                    return PrologueOutcome::NotPlausible {
                        reason: format!(
                            "istruzione x86-64 #{index} non definita (opcode illegale)"
                        ),
                    };
                }
                if index == 0 {
                    if let Some(kind) = x86_ret_or_trap(opcode) {
                        return PrologueOutcome::NotPlausible {
                            reason: format!(
                                "la prima istruzione x86-64 è {kind} ({opcode:?}), non un prologo di funzione"
                            ),
                        };
                    }
                }
                let len = inst.len().to_const();
                if len == 0 {
                    // Difesa: un'istruzione a lunghezza zero non deve mai
                    // accadere; chiude la finestra come non plausibile anziché
                    // ciclare all'infinito (fail-closed).
                    return PrologueOutcome::NotPlausible {
                        reason: format!(
                            "istruzione x86-64 #{index} con lunghezza nulla (decodifica incoerente)"
                        ),
                    };
                }
                consumed = consumed.saturating_add(len);
                index += 1;
            }
            Err(err) => {
                return PrologueOutcome::NotPlausible {
                    reason: format!(
                        "decodifica x86-64 fallita all'istruzione #{index} dopo {consumed} byte: {err}"
                    ),
                };
            }
        }
    }

    PrologueOutcome::PlausibleEntry
}

/// Se l'opcode x86-64 è un'istruzione di ritorno o di trap, restituisce la sua
/// categoria leggibile; altrimenti `None`.
fn x86_ret_or_trap(opcode: X86Opcode) -> Option<&'static str> {
    match opcode {
        // Ritorni (near/far/interrupt).
        X86Opcode::RETURN
        | X86Opcode::RETF
        | X86Opcode::IRET
        | X86Opcode::IRETD
        | X86Opcode::IRETQ
        | X86Opcode::SYSRET => Some("un'istruzione di ritorno"),
        // Trap / eccezioni che non aprono mai un prologo di funzione.
        X86Opcode::INT
        | X86Opcode::INTO
        | X86Opcode::UD0
        | X86Opcode::UD1
        | X86Opcode::UD2
        | X86Opcode::HLT => Some("un'istruzione di trap"),
        _ => None,
    }
}

// ---------------------------------------------------------------------------
// Gate prologo → Verified_Flag (fail-closed, Req 5.2, 12.1, 12.3)
// ---------------------------------------------------------------------------

/// Calcola il `Verified_Flag` di un offset a partire dall'esito del
/// `Prologue_Sanity_Check`, **fail-closed** (Req 5.2, 12.3).
///
/// Restituisce `true` **se e solo se**:
///
/// 1. il prologo è [`PrologueOutcome::PlausibleEntry`]; **e**
/// 2. l'offset è `> 0`; **e**
/// 3. l'offset è `≠ SENTINEL_VALUE`.
///
/// In ogni altro caso restituisce `false`. Le condizioni `> 0` e `≠ Sentinel`
/// sono verificate qui in modo **indipendente** dall'esito del prologo
/// (difesa in profondità): anche se a monte fossero incoerenti, il gate resta
/// chiuso. Questo gate **non** assegna alcun `Provenance_Tier` e **non** può
/// promuovere a `manual-prologue-confirmed`: è necessario ma non sufficiente
/// (Req 5.6).
pub fn verified_flag(offset: u64, outcome: &PrologueOutcome) -> bool {
    matches!(outcome, PrologueOutcome::PlausibleEntry) && offset > 0 && offset != SENTINEL_VALUE
}

/// Decisione registrabile del gate prologo per un singolo offset di un simbolo
/// su una coppia `(GD_Version, Target_Platform)`.
///
/// Trasporta l'esito del `Prologue_Sanity_Check`, il `Verified_Flag` risultante
/// (calcolato da [`verified_flag`]) e l'offset **derivato preservato senza
/// modifiche** (Req 5.3): il gate non altera mai l'offset, decide solo se può
/// essere marcato `verified`. La [`PrologueDecision::cause`] fornisce la causa
/// auditabile con simbolo + coppia quando l'offset **non** è verificato
/// (Req 5.2, 12.1, 12.3).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PrologueDecision {
    /// Simbolo a cui appartiene l'offset.
    pub symbol: SymbolId,
    /// Coppia `(GD_Version, Target_Platform)` dell'offset.
    pub pair: TargetPair,
    /// Offset derivato, **preservato invariato** (Req 5.3).
    pub offset: u64,
    /// Esito del `Prologue_Sanity_Check`.
    pub outcome: PrologueOutcome,
    /// `Verified_Flag` risultante dal gate fail-closed (Req 5.2, 12.3).
    pub verified: bool,
}

impl PrologueDecision {
    /// Costruisce la decisione applicando il gate fail-closed [`verified_flag`]
    /// all'esito del prologo, preservando l'offset derivato (Req 5.2, 5.3, 12.3).
    pub fn evaluate(
        symbol: SymbolId,
        pair: TargetPair,
        offset: u64,
        outcome: PrologueOutcome,
    ) -> Self {
        let verified = verified_flag(offset, &outcome);
        Self {
            symbol,
            pair,
            offset,
            outcome,
            verified,
        }
    }

    /// Causa auditabile registrabile quando l'offset **non** è verificato, che
    /// identifica il simbolo, la coppia e il motivo (Req 5.2, 12.1, 12.3).
    /// Restituisce `None` quando l'offset è verificato (nessuna causa da
    /// registrare).
    pub fn cause(&self) -> Option<String> {
        if self.verified {
            return None;
        }
        let reason = match &self.outcome {
            PrologueOutcome::PlausibleEntry => {
                // Prologo plausibile ma offset non eleggibile (== 0 o Sentinel):
                // il gate resta chiuso (Req 12.3).
                if self.offset == 0 {
                    "prologo plausibile ma offset nullo (== 0): non verificabile".to_owned()
                } else if self.offset == SENTINEL_VALUE {
                    "prologo plausibile ma offset pari al Sentinel_Value: non verificabile"
                        .to_owned()
                } else {
                    "offset non eleggibile alla verifica".to_owned()
                }
            }
            PrologueOutcome::NotPlausible { reason } => {
                format!("prologo non plausibile: {reason}")
            }
            PrologueOutcome::Skipped { reason } => {
                format!("controllo del prologo saltato: {}", reason.description())
            }
        };
        Some(format!(
            "simbolo `{}`, coppia {}: {reason}",
            self.symbol.0, self.pair
        ))
    }
}
