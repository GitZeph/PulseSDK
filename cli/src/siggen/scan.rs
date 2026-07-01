//! Scansione AOB dell'intero binario — oracolo condiviso (Req 3.4).
//!
//! [`scan_all`] cerca **tutte** le posizioni in cui una [`ByteSignature`]
//! combacia con i byte di un binario: un byte [`SigByte::Fixed`] deve
//! corrispondere esattamente, un [`SigByte::Wildcard`] combacia con qualunque
//! byte. La funzione è **pura** e **deterministica** — dato lo stesso binario e
//! la stessa firma restituisce sempre gli stessi offset, nello stesso ordine
//! crescente — ed è l'**unico oracolo** condiviso sia dalla pipeline `generate`
//! di `siggen` (verifica fail-closed dell'unicità) sia da `check-offsets`
//! (classificazione degli `Offset_Verdict`).
//!
//! Una firma **vuota** non ha alcun contenuto da cercare: `scan_all` restituisce
//! un elenco vuoto (nessuna corrispondenza) invece di combaciare degenerando su
//! ogni posizione, coerentemente con la semantica fail-closed a monte.
//!
//! _Requisiti: 3.4._

use super::signature::{ByteSignature, SigByte};

/// Restituisce **tutti** gli offset (in byte, crescenti) in cui `sig` combacia
/// con `binary`; il jolly combacia con qualunque byte (Req 3.4).
///
/// Funzione pura e deterministica: nessuno stato, nessun effetto collaterale.
/// Una firma vuota o più lunga del binario non produce alcuna corrispondenza.
pub fn scan_all(binary: &[u8], sig: &ByteSignature) -> Vec<u64> {
    let pattern = &sig.bytes;
    // Firma vuota o troppo lunga: nessuna finestra da confrontare.
    if pattern.is_empty() || pattern.len() > binary.len() {
        return Vec::new();
    }

    let mut matches = Vec::new();
    // Ultima posizione di partenza per cui la finestra sta interamente dentro.
    let last_start = binary.len() - pattern.len();
    for start in 0..=last_start {
        if window_matches(&binary[start..start + pattern.len()], pattern) {
            matches.push(start as u64);
        }
    }
    matches
}

/// `true` se ogni elemento della firma combacia con il byte corrispondente
/// della finestra (i jolly combaciano sempre). `window` e `pattern` hanno la
/// stessa lunghezza per costruzione del chiamante.
fn window_matches(window: &[u8], pattern: &[SigByte]) -> bool {
    window
        .iter()
        .zip(pattern.iter())
        .all(|(&byte, sig_byte)| match sig_byte {
            SigByte::Fixed(expected) => byte == *expected,
            SigByte::Wildcard => true,
        })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sig(bytes: &[SigByte]) -> ByteSignature {
        ByteSignature::new(bytes.to_vec())
    }

    #[test]
    fn finds_single_exact_match() {
        let binary = [0x00, 0x48, 0x8B, 0x89, 0xFF];
        let s = sig(&[SigByte::Fixed(0x48), SigByte::Fixed(0x8B)]);
        assert_eq!(scan_all(&binary, &s), vec![1]);
    }

    #[test]
    fn wildcard_matches_any_byte() {
        let binary = [0x48, 0x00, 0x89, 0x48, 0xFF, 0x89];
        let s = sig(&[SigByte::Fixed(0x48), SigByte::Wildcard, SigByte::Fixed(0x89)]);
        assert_eq!(scan_all(&binary, &s), vec![0, 3]);
    }

    #[test]
    fn finds_all_matches_in_order() {
        let binary = [0xAA, 0xAA, 0xAA, 0xAA];
        let s = sig(&[SigByte::Fixed(0xAA), SigByte::Fixed(0xAA)]);
        assert_eq!(scan_all(&binary, &s), vec![0, 1, 2]);
    }

    #[test]
    fn matches_at_start_and_end() {
        let binary = [0x90, 0x01, 0x02, 0x90];
        let s = sig(&[SigByte::Fixed(0x90)]);
        assert_eq!(scan_all(&binary, &s), vec![0, 3]);
    }

    #[test]
    fn no_match_returns_empty() {
        let binary = [0x01, 0x02, 0x03];
        let s = sig(&[SigByte::Fixed(0xFF)]);
        assert!(scan_all(&binary, &s).is_empty());
    }

    #[test]
    fn all_wildcards_matches_every_window() {
        let binary = [0x01, 0x02, 0x03, 0x04];
        let s = sig(&[SigByte::Wildcard, SigByte::Wildcard]);
        assert_eq!(scan_all(&binary, &s), vec![0, 1, 2]);
    }

    #[test]
    fn empty_signature_yields_no_matches() {
        let binary = [0x01, 0x02, 0x03];
        assert!(scan_all(&binary, &ByteSignature::default()).is_empty());
    }

    #[test]
    fn signature_longer_than_binary_yields_no_matches() {
        let binary = [0x01, 0x02];
        let s = sig(&[SigByte::Fixed(0x01), SigByte::Fixed(0x02), SigByte::Fixed(0x03)]);
        assert!(scan_all(&binary, &s).is_empty());
    }

    #[test]
    fn deterministic_across_calls() {
        let binary = [0x48, 0x8B, 0x48, 0x8B, 0x48, 0x8B];
        let s = sig(&[SigByte::Fixed(0x48), SigByte::Fixed(0x8B)]);
        assert_eq!(scan_all(&binary, &s), scan_all(&binary, &s));
    }

    #[test]
    fn empty_binary_yields_no_matches() {
        let s = sig(&[SigByte::Fixed(0x00)]);
        assert!(scan_all(&[], &s).is_empty());
    }
}
