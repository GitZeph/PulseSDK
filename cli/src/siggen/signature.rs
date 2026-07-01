//! Modello della `Byte_Signature` e round-trip AOB (Req 3.1).
//!
//! Una [`ByteSignature`] è una sequenza di [`SigByte`], ciascuno un byte fisso
//! (`Fixed`) oppure un jolly (`Wildcard`). La sua forma testuale è un pattern
//! **array-of-bytes (AOB)**: token separati da spazio, ogni byte fisso reso in
//! esadecimale a **2 cifre** (maiuscolo, coerente con lo stile `0x{..:X}` del
//! resto della CLI) e il jolly reso come `?`, ad esempio `48 8B ? ? 89`.
//!
//! [`ByteSignature::render`] produce questa forma; [`ByteSignature::parse`] è il
//! suo inverso: **tollerante alla spaziatura** (spazi multipli, tab, a capo,
//! spazi in testa/coda) ma **rifiuta** ogni token non valido — un byte che non
//! sia esattamente 2 cifre esadecimali o il jolly `?`. Il vincolo delle 2 cifre
//! rende `render`→`parse` un'identità.
//!
//! _Requisiti: 3.1._

use std::fmt;

/// Un elemento di una [`ByteSignature`]: un byte fisso o un jolly (Req 3.1).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SigByte {
    /// Un byte concreto che deve corrispondere esattamente.
    Fixed(u8),
    /// Un jolly che corrisponde a qualunque byte (reso come `?`).
    Wildcard,
}

/// Una firma di byte AOB: sequenza ordinata di [`SigByte`] (Req 3.1).
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ByteSignature {
    /// Gli elementi della firma, nell'ordine in cui compaiono nel binario.
    pub bytes: Vec<SigByte>,
}

/// Errore fail-closed del parsing AOB: un token non è né un byte esadecimale a
/// 2 cifre né il jolly `?` (Req 3.1). Nessuna firma parziale è prodotta.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AobParseError {
    /// Il token invalido incontrato (così com'è nella stringa d'ingresso).
    pub token: String,
}

impl fmt::Display for AobParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "token AOB non valido: {:?}; atteso un byte esadecimale a 2 cifre \
             (es. `48`) o il jolly `?`",
            self.token
        )
    }
}

impl std::error::Error for AobParseError {}

impl ByteSignature {
    /// Costruisce una firma dai suoi elementi.
    pub fn new(bytes: Vec<SigByte>) -> Self {
        Self { bytes }
    }

    /// Numero di elementi (byte fissi + jolly) nella firma.
    pub fn len(&self) -> usize {
        self.bytes.len()
    }

    /// `true` se la firma non contiene alcun elemento.
    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }

    /// Rende la firma come pattern AOB: token separati da singolo spazio, byte
    /// fissi in esadecimale a 2 cifre maiuscole, jolly come `?` (Req 3.1).
    pub fn render(&self) -> String {
        let mut out = String::with_capacity(self.bytes.len() * 3);
        for (i, b) in self.bytes.iter().enumerate() {
            if i > 0 {
                out.push(' ');
            }
            match b {
                SigByte::Fixed(byte) => {
                    // Byte esadecimale a 2 cifre, sempre padded (es. `0F`).
                    out.push_str(&format!("{byte:02X}"));
                }
                SigByte::Wildcard => out.push('?'),
            }
        }
        out
    }

    /// Interpreta un pattern AOB, inverso di [`render`](Self::render).
    ///
    /// Tollerante alla spaziatura: qualunque combinazione di spazi bianchi
    /// separa i token e gli spazi in testa/coda sono ignorati. Rifiuta con
    /// [`AobParseError`] ogni token che non sia un byte esadecimale a 2 cifre o
    /// il jolly `?`, senza produrre alcuna firma parziale (fail-closed, Req 3.1).
    pub fn parse(text: &str) -> Result<Self, AobParseError> {
        let mut bytes = Vec::new();
        for token in text.split_whitespace() {
            if token == "?" {
                bytes.push(SigByte::Wildcard);
                continue;
            }
            // Esattamente 2 cifre esadecimali: né 1 né 3+, così render→parse è
            // un'identità e i token ambigui sono rifiutati.
            if token.len() == 2 && token.bytes().all(|c| c.is_ascii_hexdigit()) {
                let value = u8::from_str_radix(token, 16).map_err(|_| AobParseError {
                    token: token.to_owned(),
                })?;
                bytes.push(SigByte::Fixed(value));
                continue;
            }
            return Err(AobParseError {
                token: token.to_owned(),
            });
        }
        Ok(Self { bytes })
    }
}

impl fmt::Display for ByteSignature {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.render())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn render_produces_two_digit_hex_and_wildcard() {
        let sig = ByteSignature::new(vec![
            SigByte::Fixed(0x48),
            SigByte::Fixed(0x8B),
            SigByte::Wildcard,
            SigByte::Wildcard,
            SigByte::Fixed(0x89),
        ]);
        assert_eq!(sig.render(), "48 8B ? ? 89");
    }

    #[test]
    fn render_pads_single_digit_bytes() {
        let sig = ByteSignature::new(vec![SigByte::Fixed(0x0F), SigByte::Fixed(0x00)]);
        assert_eq!(sig.render(), "0F 00");
    }

    #[test]
    fn empty_signature_renders_empty() {
        assert_eq!(ByteSignature::default().render(), "");
        assert_eq!(ByteSignature::parse("").unwrap(), ByteSignature::default());
        assert_eq!(
            ByteSignature::parse("   \t\n ").unwrap(),
            ByteSignature::default()
        );
    }

    #[test]
    fn parse_is_inverse_of_render() {
        let sig = ByteSignature::new(vec![
            SigByte::Fixed(0x00),
            SigByte::Wildcard,
            SigByte::Fixed(0xFF),
            SigByte::Fixed(0x8B),
        ]);
        let rendered = sig.render();
        assert_eq!(ByteSignature::parse(&rendered).unwrap(), sig);
    }

    #[test]
    fn parse_tolerates_irregular_whitespace() {
        let expected = ByteSignature::new(vec![
            SigByte::Fixed(0x48),
            SigByte::Wildcard,
            SigByte::Fixed(0x89),
        ]);
        assert_eq!(
            ByteSignature::parse("  48\t?\n  89  ").unwrap(),
            expected
        );
    }

    #[test]
    fn parse_accepts_lowercase_hex() {
        assert_eq!(
            ByteSignature::parse("ab cd").unwrap(),
            ByteSignature::new(vec![SigByte::Fixed(0xAB), SigByte::Fixed(0xCD)])
        );
    }

    #[test]
    fn parse_rejects_single_digit_token() {
        let err = ByteSignature::parse("4 8B").unwrap_err();
        assert_eq!(err.token, "4");
    }

    #[test]
    fn parse_rejects_three_digit_token() {
        assert!(ByteSignature::parse("488 B").is_err());
    }

    #[test]
    fn parse_rejects_non_hex_token() {
        assert_eq!(ByteSignature::parse("ZZ").unwrap_err().token, "ZZ");
        assert_eq!(ByteSignature::parse("0x 48").unwrap_err().token, "0x");
    }

    #[test]
    fn parse_rejects_double_wildcard_token() {
        // Solo `?` singolo è un jolly; `??` non è un token valido.
        assert_eq!(ByteSignature::parse("?? 48").unwrap_err().token, "??");
    }
}
