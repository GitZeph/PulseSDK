//! Mascheramento consapevole dei confini di istruzione per `siggen` (Req 3.1, 3.4).
//!
//! Una `Byte_Signature` stabile deve tenere **fissi** i byte che identificano
//! un'istruzione (l'opcode e i campi non dipendenti dalla posizione) e rendere
//! **jolly** (`Wildcard`) i soli campi **volatili** — gli offset PC-relative e i
//! target di branch — che cambiano quando il codice viene rilocato tra un
//! aggiornamento del gioco e l'altro. Questo modulo decodifica una finestra di
//! byte con gli **stessi decoder** di `cli/src/extract/prologue.rs`
//! (`yaxpeax-arm` per arm64, `yaxpeax-x86` per x86-64) e produce, per ciascun
//! byte della finestra, un [`SigByte`] `Fixed` oppure `Wildcard`.
//!
//! ## Architettura dal formato del binario (Req 3.1)
//!
//! L'[`Architecture`] non è indovinata: è **derivata dal formato del binario**
//! via crate `object` ([`architecture_of`]). `Aarch64` → [`Architecture::Arm64`],
//! `X86_64` → [`Architecture::X86_64`]; qualunque altra architettura è un errore
//! onesto (fail-closed) — non si maschera ciò che non si sa decodificare.
//!
//! ## Criterio di mascheramento
//!
//! - **arm64**: istruzioni a lunghezza fissa di 4 byte. Se l'istruzione ha un
//!   operando PC-relative (`Operand::PCOffset`, cioè branch/`bl`/`adr`/`adrp` e
//!   load-literal) i **tre byte bassi** (little-endian) — che portano i bit
//!   dell'offset — diventano `Wildcard`, mentre il **byte più significativo**,
//!   che contiene l'opcode discriminante, resta `Fixed`. Le istruzioni senza
//!   operandi PC-relative restano interamente `Fixed`.
//! - **x86-64**: istruzioni a lunghezza variabile, decodificate in sequenza. Per
//!   i branch/call **relativi** (`jmp`/`call`/`jcc`/`loop`/`jrcxz` con operando
//!   immediato) il displacement relativo occupa i byte **in coda** all'istruzione
//!   (1 byte per `rel8`, 4 byte per `rel32`): questi diventano `Wildcard`, il
//!   prefisso opcode+ModRM resta `Fixed`. Gli immediati non di branch (es.
//!   `mov eax, imm32`) **non** sono mascherati: non dipendono dalla posizione e
//!   la loro fissità aumenta la specificità della firma.
//!
//! ## Fail-closed
//!
//! Il mascheramento è un'**euristica**: la correttezza finale è garantita a
//! valle da `siggen::generate` (task 7.4), che riscansiona l'intero binario ed
//! emette una firma **solo se** corrisponde a esattamente una posizione,
//! coincidente con l'offset richiesto. Sovra-fissare riduce la stabilità ma non
//! la correttezza; sotto-mascherare produce al più un errore onesto di
//! non-unicità. Coerentemente, [`mask_window`] **fallisce** (non tira a
//! indovinare) se un byte della finestra non decodifica in un'istruzione valida
//! e definita, così che il chiamante possa allargare la finestra o riportare un
//! errore.
//!
//! _Requisiti: 3.1, 3.4._

use anyhow::{anyhow, Context};

use object::{Architecture as ObjectArchitecture, File, Object};

use yaxpeax_arch::{Decoder, LengthedInstruction, U8Reader};
use yaxpeax_arm::armv8::a64::{
    InstDecoder as Arm64Decoder, Instruction as Arm64Instruction, Opcode as Arm64Opcode,
    Operand as Arm64Operand,
};
use yaxpeax_x86::long_mode::{
    InstDecoder as X86Decoder, Instruction as X86Instruction, Opcode as X86Opcode,
    Operand as X86Operand,
};

use crate::extract::Architecture;

use super::signature::SigByte;

/// Larghezza in byte di un'istruzione arm64 (AArch64): fissa a 4.
const ARM64_INSN_LEN: usize = 4;

/// Deriva l'[`Architecture`] di un binario **dal suo formato**, via crate
/// `object` (Req 3.1).
///
/// Riconosce esclusivamente le architetture che i decoder condivisi con
/// `extract/prologue.rs` sanno trattare: `Aarch64` → [`Architecture::Arm64`],
/// `X86_64` → [`Architecture::X86_64`]. Qualunque altra architettura (o un
/// binario non analizzabile) è un **errore onesto**: non si maschera ciò che non
/// si sa decodificare (fail-closed).
pub fn architecture_of(binary: &[u8]) -> anyhow::Result<Architecture> {
    let file = File::parse(binary)
        .context("impossibile analizzare il GD_Binary per derivarne l'architettura")?;
    match file.architecture() {
        ObjectArchitecture::Aarch64 => Ok(Architecture::Arm64),
        ObjectArchitecture::X86_64 => Ok(Architecture::X86_64),
        other => Err(anyhow!(
            "architettura non supportata dal masker siggen: {other:?} \
             (attese arm64 o x86-64)"
        )),
    }
}

/// Maschera una finestra di byte per l'`Architecture` indicata, restituendo un
/// [`SigByte`] per **ciascun byte decodificato** della finestra (Req 3.1, 3.4).
///
/// I byte volatili (offset PC-relative / target di branch) diventano
/// [`SigByte::Wildcard`]; gli opcode e i campi non dipendenti dalla posizione
/// restano [`SigByte::Fixed`]. Le istruzioni sono decodificate con gli **stessi
/// decoder** di `extract/prologue.rs`.
///
/// Fallisce (fail-closed) se un byte della finestra non decodifica in
/// un'istruzione valida e definita, o — per x86-64 — se un'istruzione
/// oltrepassa la fine della finestra fornita.
pub fn mask_window(window: &[u8], arch: Architecture) -> anyhow::Result<Vec<SigByte>> {
    match arch {
        Architecture::Arm64 => mask_window_arm64(window),
        Architecture::X86_64 => mask_window_x86_64(window),
    }
}

// ---------------------------------------------------------------------------
// arm64 (AArch64)
// ---------------------------------------------------------------------------

/// Maschera una finestra arm64: 4 byte per istruzione. La finestra deve essere
/// un multiplo di 4 byte non vuoto.
fn mask_window_arm64(window: &[u8]) -> anyhow::Result<Vec<SigByte>> {
    if window.is_empty() || window.len() % ARM64_INSN_LEN != 0 {
        return Err(anyhow!(
            "finestra arm64 non valida: {} byte (atteso multiplo non nullo di {ARM64_INSN_LEN})",
            window.len()
        ));
    }

    let decoder = Arm64Decoder::default();
    let mut reader = U8Reader::new(window);
    let mut out = Vec::with_capacity(window.len());

    let count = window.len() / ARM64_INSN_LEN;
    for index in 0..count {
        let base = index * ARM64_INSN_LEN;
        let inst = decoder
            .decode(&mut reader)
            .map_err(|err| anyhow!("decodifica arm64 fallita all'istruzione #{index}: {err}"))?;
        if inst.opcode == Arm64Opcode::Invalid {
            return Err(anyhow!(
                "istruzione arm64 #{index} non definita (opcode illegale): impossibile mascherare"
            ));
        }

        let volatile = arm64_is_pc_relative(&inst);
        for byte_index in 0..ARM64_INSN_LEN {
            // Little-endian: i byte 0..2 portano i bit bassi dell'offset
            // PC-relative; il byte 3 (più significativo) contiene l'opcode
            // discriminante e resta sempre fisso.
            if volatile && byte_index < ARM64_INSN_LEN - 1 {
                out.push(SigByte::Wildcard);
            } else {
                out.push(SigByte::Fixed(window[base + byte_index]));
            }
        }
    }

    Ok(out)
}

/// `true` se l'istruzione arm64 ha un operando PC-relative (`Operand::PCOffset`),
/// cioè un target di branch o un riferimento `adr`/`adrp`/load-literal: i suoi
/// bit di offset sono volatili tra un aggiornamento e l'altro.
fn arm64_is_pc_relative(inst: &Arm64Instruction) -> bool {
    inst.operands
        .iter()
        .any(|operand| matches!(operand, Arm64Operand::PCOffset(_)))
}

// ---------------------------------------------------------------------------
// x86-64
// ---------------------------------------------------------------------------

/// Maschera una finestra x86-64 decodificando le istruzioni in sequenza fino a
/// coprire l'intera finestra. Ogni istruzione deve decodificare in un opcode
/// valido/definito e restare **entro** la finestra fornita (fail-closed).
fn mask_window_x86_64(window: &[u8]) -> anyhow::Result<Vec<SigByte>> {
    if window.is_empty() {
        return Err(anyhow!("finestra x86-64 vuota: nulla da mascherare"));
    }

    let decoder = X86Decoder::default();
    let mut reader = U8Reader::new(window);
    let mut out = Vec::with_capacity(window.len());

    let mut consumed = 0usize;
    while consumed < window.len() {
        let inst = decoder.decode(&mut reader).map_err(|err| {
            anyhow!("decodifica x86-64 fallita dopo {consumed} byte: {err}")
        })?;
        if inst.opcode() == X86Opcode::Invalid {
            return Err(anyhow!(
                "istruzione x86-64 non definita (opcode illegale) all'offset {consumed} della finestra"
            ));
        }

        let len = inst.len().to_const() as usize;
        if len == 0 {
            return Err(anyhow!(
                "istruzione x86-64 con lunghezza nulla all'offset {consumed} (decodifica incoerente)"
            ));
        }
        if consumed + len > window.len() {
            // L'istruzione oltrepassa i byte forniti: non possiamo mascherarla
            // in modo provato. Fail-closed: è compito del chiamante fornire una
            // finestra sufficiente.
            return Err(anyhow!(
                "istruzione x86-64 all'offset {consumed} oltrepassa la finestra di {} byte",
                window.len()
            ));
        }

        // Numero di byte volatili in coda all'istruzione: il displacement
        // relativo dei branch/call (0 se l'istruzione non è un branch relativo).
        let tail = x86_relative_tail_len(&inst);
        let first_wildcard = len - tail;
        for byte_index in 0..len {
            if tail > 0 && byte_index >= first_wildcard {
                out.push(SigByte::Wildcard);
            } else {
                out.push(SigByte::Fixed(window[consumed + byte_index]));
            }
        }

        consumed += len;
    }

    Ok(out)
}

/// Lunghezza in byte del displacement relativo **in coda** a un'istruzione
/// x86-64 di branch/call relativo, oppure `0` se l'istruzione non è un branch
/// relativo o non ha un operando immediato.
///
/// Il displacement relativo (`rel8`/`rel32`) è sempre l'ultimo campo
/// dell'istruzione: `rel8` ⇒ 1 byte, `rel32` ⇒ 4 byte. I `jmp`/`call`
/// **indiretti** (registro/memoria) non hanno operando immediato ⇒ `0`, e gli
/// immediati non di branch (es. `mov`) non sono considerati volatili.
fn x86_relative_tail_len(inst: &X86Instruction) -> usize {
    if !x86_is_relative_branch(inst.opcode()) {
        return 0;
    }
    for i in 0..inst.operand_count() {
        match inst.operand(i) {
            X86Operand::ImmediateI8 { .. } | X86Operand::ImmediateU8 { .. } => return 1,
            X86Operand::ImmediateI32 { .. } | X86Operand::ImmediateU32 { .. } => return 4,
            _ => {}
        }
    }
    0
}

/// `true` se l'opcode x86-64 è un branch/call che usa un displacement
/// **relativo** (rel8/rel32): salti condizionati, `jmp`/`call` near relativi,
/// `loop*` e `jrcxz`. Esclude le forme *far* (`callf`/`jmpf`) e gli indiretti,
/// che non portano un displacement relativo in coda.
fn x86_is_relative_branch(opcode: X86Opcode) -> bool {
    matches!(
        opcode,
        X86Opcode::JMP
            | X86Opcode::CALL
            | X86Opcode::JO
            | X86Opcode::JNO
            | X86Opcode::JB
            | X86Opcode::JNB
            | X86Opcode::JZ
            | X86Opcode::JNZ
            | X86Opcode::JA
            | X86Opcode::JNA
            | X86Opcode::JS
            | X86Opcode::JNS
            | X86Opcode::JP
            | X86Opcode::JNP
            | X86Opcode::JL
            | X86Opcode::JGE
            | X86Opcode::JLE
            | X86Opcode::JG
            | X86Opcode::LOOP
            | X86Opcode::LOOPZ
            | X86Opcode::LOOPNZ
            | X86Opcode::JRCXZ
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Estrae i soli valori `Fixed`, con `None` per i `Wildcard`, per confronti
    /// leggibili nei test.
    fn shape(sig: &[SigByte]) -> Vec<Option<u8>> {
        sig.iter()
            .map(|b| match b {
                SigByte::Fixed(v) => Some(*v),
                SigByte::Wildcard => None,
            })
            .collect()
    }

    #[test]
    fn arm64_non_relative_instruction_is_fully_fixed() {
        // `nop` = 0xD503201F, little-endian: 1F 20 03 D5. Nessun operando
        // PC-relative ⇒ tutti i byte fissi.
        let window = [0x1F, 0x20, 0x03, 0xD5];
        let sig = mask_window(&window, Architecture::Arm64).unwrap();
        assert_eq!(
            shape(&sig),
            vec![Some(0x1F), Some(0x20), Some(0x03), Some(0xD5)]
        );
    }

    #[test]
    fn arm64_branch_masks_low_three_bytes_keeps_opcode_byte() {
        // `bl #0` = 0x94000000, little-endian: 00 00 00 94. Operando PC-relative
        // ⇒ i tre byte bassi diventano wildcard, il byte alto (opcode) resta.
        let window = [0x00, 0x00, 0x00, 0x94];
        let sig = mask_window(&window, Architecture::Arm64).unwrap();
        assert_eq!(shape(&sig), vec![None, None, None, Some(0x94)]);
    }

    #[test]
    fn arm64_rejects_non_multiple_of_four() {
        assert!(mask_window(&[0x00, 0x00, 0x00], Architecture::Arm64).is_err());
    }

    #[test]
    fn x86_relative_call_masks_rel32_tail() {
        // `call rel32` = E8 00 00 00 00. Opcode E8 fisso, rel32 (4 byte) wildcard.
        let window = [0xE8, 0x11, 0x22, 0x33, 0x44];
        let sig = mask_window(&window, Architecture::X86_64).unwrap();
        assert_eq!(shape(&sig), vec![Some(0xE8), None, None, None, None]);
    }

    #[test]
    fn x86_short_jump_masks_rel8_tail() {
        // `jmp rel8` = EB 05. Opcode EB fisso, rel8 (1 byte) wildcard.
        let window = [0xEB, 0x05];
        let sig = mask_window(&window, Architecture::X86_64).unwrap();
        assert_eq!(shape(&sig), vec![Some(0xEB), None]);
    }

    #[test]
    fn x86_non_branch_immediate_is_not_masked() {
        // `mov eax, 0x11223344` = B8 44 33 22 11. Immediato non di branch ⇒ tutti
        // i byte restano fissi (non dipende dalla posizione).
        let window = [0xB8, 0x44, 0x33, 0x22, 0x11];
        let sig = mask_window(&window, Architecture::X86_64).unwrap();
        assert_eq!(
            shape(&sig),
            vec![Some(0xB8), Some(0x44), Some(0x33), Some(0x22), Some(0x11)]
        );
    }
}
