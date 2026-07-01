//! `pulse siggen` — generazione fail-closed di `Byte_Signature` (Req 3).
//!
//! Il modulo `siggen/` implementa la pipeline di generazione di una firma di
//! byte AOB (array-of-bytes con wildcard) stabile a partire da un `Offset` del
//! `GD_Binary`. La logica è divisa in file dedicati e host-testabili:
//!
//! - [`signature`] — il modello [`signature::ByteSignature`] / [`signature::SigByte`]
//!   e il round-trip AOB (render → parse) (Req 3.1);
//! - `scan.rs` — la scansione AOB sull'intero binario (oracolo condiviso, Req 3.4);
//! - `mask.rs` — il mascheramento consapevole dei confini di istruzione (Req 3.1, 3.4);
//! - questo `mod.rs` — l'orchestrazione `generate` con verifica fail-closed
//!   (Req 3.4, 3.5, 3.6).
//!
//! _Requisiti: 3.1._

pub mod scan;
pub mod mask;
pub mod signature;

use anyhow::{anyhow, bail, Context};

use crate::extract::Architecture;

use mask::{architecture_of, mask_window};
use scan::scan_all;
use signature::ByteSignature;

/// Larghezza massima (in byte) della finestra esplorata per generare una firma
/// a corrispondenza unica. Oltre questo limite si rinuncia con un errore onesto
/// (fail-closed): una firma stabile utile è ampiamente contenuta entro questo
/// numero di byte, e continuare all'infinito nasconderebbe la non-unicità.
const MAX_WINDOW_BYTES: usize = 128;

/// Genera una [`ByteSignature`] **verificata** per l'`offset` indicato del
/// `binary`, oppure fallisce senza emettere alcuna firma (fail-closed, Req 3.4,
/// 3.5, 3.6).
///
/// Pipeline:
/// 1. **Bounds check** — l'`offset` deve ricadere in `0 ..= len-1`; fuori dai
///    limiti o binario vuoto/illeggibile ⇒ errore, **nessuna firma** (Req 3.5).
/// 2. **Architettura** — derivata dal formato del binario via
///    [`mask::architecture_of`]; formato non supportato ⇒ errore onesto.
/// 3. **Finestra crescente + mascheramento** — a partire dall'`offset` si estrae
///    una finestra che parte piccola (una istruzione) e si **allarga** un'unità
///    per volta (4 byte su arm64, 1 byte su x86-64 saltando i confini non
///    validi), mascherando i byte volatili con [`mask::mask_window`].
/// 4. **Verifica fail-closed** — per ogni finestra ben formata si riscansiona
///    l'**intero** binario con [`scan::scan_all`] ed **emette la firma solo se**
///    combacia con **esattamente una** posizione, **coincidente** con l'`offset`
///    richiesto (Req 3.4). Si restituisce la prima firma univoca trovata.
/// 5. **Nessuna unicità** — se, esaurito l'allargamento fino a
///    [`MAX_WINDOW_BYTES`] (o alla fine del binario), nessuna finestra produce
///    una corrispondenza unica coincidente, il comando **fallisce** con un
///    errore onesto senza emettere alcuna firma non verificata (Req 3.6).
///
/// La firma restituita è sempre non vuota e, per costruzione, combacia con
/// l'`offset` richiesto: i byte `Fixed` provengono dai byte a quell'offset e i
/// `Wildcard` combaciano con qualunque byte, quindi l'`offset` è sempre tra le
/// corrispondenze; la garanzia aggiuntiva è che sia l'**unica**.
pub fn generate(binary: &[u8], offset: u64) -> anyhow::Result<ByteSignature> {
    let len = binary.len();
    if len == 0 {
        bail!("GD_Binary vuoto o illeggibile: impossibile generare una Byte_Signature");
    }

    // Bounds check: l'intervallo valido è 0 ..= len-1 (Req 3.2, 3.5).
    let start = usize::try_from(offset).map_err(|_| {
        anyhow!("offset {offset} fuori dai limiti del GD_Binary ({len} byte)")
    })?;
    if start >= len {
        bail!(
            "offset {offset} fuori dai limiti del GD_Binary: l'intervallo valido è 0..={}",
            len - 1
        );
    }

    // Architettura derivata dal formato del binario (fail-closed su formati
    // non supportati o binari non analizzabili).
    let arch = architecture_of(binary).context(
        "impossibile derivare l'architettura del GD_Binary per la generazione della firma",
    )?;

    // Passo di allargamento della finestra: arm64 ha istruzioni a 4 byte fissi,
    // x86-64 è a lunghezza variabile e va allargata byte per byte (le finestre
    // che cadono a metà di un'istruzione sono scartate da `mask_window`).
    let step = match arch {
        Architecture::Arm64 => 4usize,
        Architecture::X86_64 => 1usize,
    };

    let remaining = len - start;
    let max_window = remaining.min(MAX_WINDOW_BYTES);

    let mut window_len = step;
    while window_len <= max_window {
        let window = &binary[start..start + window_len];
        // Una finestra che non decodifica in istruzioni valide e complete
        // (es. termina a metà di un'istruzione x86-64) non è mascherabile in
        // modo provato: si prova con la finestra successiva.
        if let Ok(sig_bytes) = mask_window(window, arch) {
            let sig = ByteSignature::new(sig_bytes);
            let matches = scan_all(binary, &sig);
            // Verifica fail-closed: esattamente un match, coincidente con
            // l'offset richiesto (Req 3.4).
            if matches.len() == 1 && matches[0] == offset {
                return Ok(sig);
            }
        }
        window_len += step;
    }

    // Nessuna finestra entro il limite produce una corrispondenza unica: errore
    // onesto, nessuna firma non verificata emessa (Req 3.6, fail-closed).
    Err(anyhow!(
        "impossibile generare una Byte_Signature a corrispondenza unica per l'offset {offset}: \
         nessuna finestra fino a {max_window} byte combacia con esattamente una posizione del \
         GD_Binary (fail-closed: nessuna firma non verificata emessa)"
    ))
}
