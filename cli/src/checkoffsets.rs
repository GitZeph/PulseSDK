//! `pulse check-offsets` — classificazione degli `Offset_Verdict` (Req 4).
//!
//! Questo modulo contiene la **logica pura e host-testabile** di
//! `check-offsets`: data una `Offset_Declaration` (nome + offset dichiarato +
//! AOB attesa opzionale) e i byte del `GD_Binary`, produce un `Offset_Verdict`
//! mutuamente esclusivo. L'orchestrazione con effetti (lettura del manifest e
//! del binario, exit code) vive nel gestore `run_check_offsets` in `cli.rs`.
//!
//! **Oracolo del "contenuto atteso".** Il perno della classificazione è
//! [`crate::siggen::scan::scan_all`], lo stesso oracolo che `siggen` usa per la
//! verifica fail-closed: si scansiona l'**intero** binario cercando l'AOB
//! dichiarata (`signature`) e si osservano gli offset che combaciano.
//!
//! **Regole di classificazione (mutuamente esclusive, Req 4.3, 4.4, 4.5):**
//!
//! 1. i match includono **esattamente** l'offset dichiarato ⇒ [`Verdict::Valid`]
//!    (`Valid` **SOLO SE** il contenuto atteso è all'offset dichiarato —
//!    fail-closed);
//! 2. nessun match all'offset dichiarato ma **un unico** match altrove ⇒
//!    [`Verdict::Shifted`] con `detected` e `delta = detected - declared`;
//! 3. nessun match e firma ben formata ⇒ [`Verdict::Invalid`];
//! 4. `signature` **assente** o vuota, offset dichiarato non interpretabile,
//!    oppure scansione **ambigua** (>1 match e nessuno all'offset dichiarato) ⇒
//!    [`Verdict::Undeterminable`] — **mai** `Valid` (fail-closed, Req 4.5).
//!
//! [`check_all`] elabora **ogni** dichiarazione nell'ordine di dichiarazione,
//! producendo **esattamente un** verdetto per dichiarazione e **proseguendo**
//! anche sulle non determinabili (Req 4.1).
//!
//! _Requisiti: 4.1, 4.3, 4.4, 4.5._

use crate::manifest::OffsetDeclaration;
use crate::siggen::scan::scan_all;
use crate::siggen::signature::ByteSignature;

/// Verdetto mutuamente esclusivo su una `Offset_Declaration` (Req 4.3).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Verdict {
    /// Il contenuto atteso combacia con l'offset dichiarato (Req 4.3, `Valid`
    /// **SOLO SE**, fail-closed).
    Valid,
    /// Lo stesso contenuto atteso è stato trovato a un **unico** offset diverso
    /// da quello dichiarato; `delta = detected - declared` (Req 4.4).
    Shifted { detected: u64, delta: i64 },
    /// Il contenuto atteso è ben formato ma non compare da nessuna parte nel
    /// binario (Req 4.3).
    Invalid,
    /// Non provabile: contenuto atteso assente/vuoto, offset non interpretabile
    /// o scansione ambigua. **Mai** `Valid` (fail-closed, Req 4.5).
    Undeterminable { reason: String },
}

/// Il verdetto su una singola `Offset_Declaration`, con il nome dichiarato e
/// l'offset dichiarato interpretato (0 se non interpretabile — vedi `verdict`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OffsetVerdict {
    /// Nome leggibile della dichiarazione (es. `MenuLayer::init`).
    pub name: String,
    /// Offset dichiarato interpretato; su offset non interpretabile è `0` e il
    /// `verdict` è [`Verdict::Undeterminable`].
    pub declared: u64,
    /// L'esito della classificazione.
    pub verdict: Verdict,
}

/// Classifica una singola `Offset_Declaration` contro `binary` (Req 4.3, 4.4,
/// 4.5). Funzione **pura**: nessun I/O, nessun effetto collaterale.
///
/// La `signature` della dichiarazione è il **contenuto atteso**; senza di essa
/// (assente o vuota) non c'è nulla da provare e il verdetto è
/// [`Verdict::Undeterminable`] — mai `Valid`.
pub fn classify(binary: &[u8], decl: &OffsetDeclaration) -> OffsetVerdict {
    // 1. Interpretazione dell'offset dichiarato (decimale o 0x…, coerente con
    //    siggen). Un offset non interpretabile non è provabile ⇒ Undeterminable.
    let declared = match crate::cli::parse_offset(&decl.offset) {
        Ok(value) => value,
        Err(err) => {
            return OffsetVerdict {
                name: decl.name.clone(),
                declared: 0,
                verdict: Verdict::Undeterminable {
                    reason: format!("offset dichiarato non interpretabile: {err}"),
                },
            };
        }
    };

    // 2. Contenuto atteso: senza AOB memorizzata non esiste nulla da provare
    //    (fail-closed, Req 4.5) ⇒ Undeterminable, mai Valid.
    let signature_text = match &decl.signature {
        Some(text) => text,
        None => {
            return OffsetVerdict {
                name: decl.name.clone(),
                declared,
                verdict: Verdict::Undeterminable {
                    reason: "nessuna Byte_Signature attesa dichiarata (campo `signature` assente)"
                        .to_string(),
                },
            };
        }
    };

    // AOB malformata: nessuna scansione affidabile ⇒ Undeterminable.
    let signature = match ByteSignature::parse(signature_text) {
        Ok(sig) => sig,
        Err(err) => {
            return OffsetVerdict {
                name: decl.name.clone(),
                declared,
                verdict: Verdict::Undeterminable {
                    reason: format!("Byte_Signature attesa non valida: {err}"),
                },
            };
        }
    };

    // AOB vuota: presente ma senza contenuto da cercare ⇒ non calcolabile.
    if signature.is_empty() {
        return OffsetVerdict {
            name: decl.name.clone(),
            declared,
            verdict: Verdict::Undeterminable {
                reason: "Byte_Signature attesa vuota: nessun contenuto da verificare".to_string(),
            },
        };
    }

    // 3. Scansione dell'intero binario con l'oracolo condiviso (Req 4.3).
    let matches = scan_all(binary, &signature);

    let verdict = if matches.contains(&declared) {
        // Il contenuto atteso è all'offset dichiarato ⇒ Valid (SOLO SE).
        Verdict::Valid
    } else if matches.is_empty() {
        // Firma ben formata ma nessuna corrispondenza ⇒ Invalid.
        Verdict::Invalid
    } else if matches.len() == 1 {
        // Unico match altrove ⇒ Shifted con delta = detected - declared.
        let detected = matches[0];
        Verdict::Shifted {
            detected,
            delta: detected as i64 - declared as i64,
        }
    } else {
        // >1 match e nessuno all'offset dichiarato ⇒ ambiguo ⇒ Undeterminable.
        Verdict::Undeterminable {
            reason: format!(
                "scansione ambigua: {} corrispondenze del contenuto atteso, nessuna all'offset dichiarato",
                matches.len()
            ),
        }
    };

    OffsetVerdict {
        name: decl.name.clone(),
        declared,
        verdict,
    }
}

/// Classifica **ogni** `Offset_Declaration` nell'ordine di dichiarazione,
/// producendo **esattamente un** [`OffsetVerdict`] per dichiarazione e
/// **proseguendo** anche sulle dichiarazioni non determinabili (Req 4.1).
///
/// Funzione **pura**: nessun I/O, nessun effetto collaterale.
pub fn check_all(binary: &[u8], decls: &[OffsetDeclaration]) -> Vec<OffsetVerdict> {
    decls.iter().map(|decl| classify(binary, decl)).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn decl(name: &str, offset: &str, signature: Option<&str>) -> OffsetDeclaration {
        OffsetDeclaration {
            name: name.to_string(),
            offset: offset.to_string(),
            signature: signature.map(str::to_string),
        }
    }

    #[test]
    fn valid_when_expected_content_at_declared_offset() {
        // "48 8B" all'offset 1.
        let binary = [0x00, 0x48, 0x8B, 0xFF, 0x00];
        let v = classify(&binary, &decl("m", "1", Some("48 8B")));
        assert_eq!(v.declared, 1);
        assert_eq!(v.verdict, Verdict::Valid);
    }

    #[test]
    fn valid_accepts_hex_declared_offset() {
        let binary = [0x00, 0x48, 0x8B, 0xFF, 0x00];
        let v = classify(&binary, &decl("m", "0x1", Some("48 8B")));
        assert_eq!(v.declared, 1);
        assert_eq!(v.verdict, Verdict::Valid);
    }

    #[test]
    fn valid_when_declared_among_multiple_matches() {
        // "AA AA" combacia a 0 e 1; dichiarato 1 è tra i match ⇒ Valid.
        let binary = [0xAA, 0xAA, 0xAA];
        let v = classify(&binary, &decl("m", "1", Some("AA AA")));
        assert_eq!(v.verdict, Verdict::Valid);
    }

    #[test]
    fn shifted_when_single_match_elsewhere() {
        // "48 8B" solo all'offset 3, dichiarato 1 ⇒ Shifted delta = +2.
        let binary = [0x00, 0x00, 0x00, 0x48, 0x8B, 0xFF];
        let v = classify(&binary, &decl("m", "1", Some("48 8B")));
        assert_eq!(
            v.verdict,
            Verdict::Shifted {
                detected: 3,
                delta: 2
            }
        );
    }

    #[test]
    fn shifted_reports_negative_delta() {
        // match a 1, dichiarato 4 ⇒ delta = 1 - 4 = -3.
        let binary = [0x00, 0x48, 0x8B, 0xFF, 0x00, 0x00];
        let v = classify(&binary, &decl("m", "4", Some("48 8B")));
        assert_eq!(
            v.verdict,
            Verdict::Shifted {
                detected: 1,
                delta: -3
            }
        );
    }

    #[test]
    fn invalid_when_no_match_and_well_formed() {
        let binary = [0x00, 0x01, 0x02, 0x03];
        let v = classify(&binary, &decl("m", "1", Some("48 8B")));
        assert_eq!(v.verdict, Verdict::Invalid);
    }

    #[test]
    fn undeterminable_when_signature_absent() {
        let binary = [0x48, 0x8B];
        let v = classify(&binary, &decl("m", "0", None));
        assert!(matches!(v.verdict, Verdict::Undeterminable { .. }));
    }

    #[test]
    fn undeterminable_when_signature_empty() {
        let binary = [0x48, 0x8B];
        let v = classify(&binary, &decl("m", "0", Some("   ")));
        assert!(matches!(v.verdict, Verdict::Undeterminable { .. }));
    }

    #[test]
    fn undeterminable_when_signature_malformed() {
        let binary = [0x48, 0x8B];
        let v = classify(&binary, &decl("m", "0", Some("ZZ 8B")));
        assert!(matches!(v.verdict, Verdict::Undeterminable { .. }));
    }

    #[test]
    fn undeterminable_when_declared_offset_malformed() {
        let binary = [0x48, 0x8B];
        let v = classify(&binary, &decl("m", "notanumber", Some("48 8B")));
        assert_eq!(v.declared, 0);
        assert!(matches!(v.verdict, Verdict::Undeterminable { .. }));
    }

    #[test]
    fn undeterminable_when_ambiguous_no_match_at_declared() {
        // "AA" combacia a 0,1,2,3; dichiarato 10 non è tra i match e ci sono >1
        // corrispondenze ⇒ ambiguo ⇒ Undeterminable, mai Valid (fail-closed).
        let binary = [0xAA, 0xAA, 0xAA, 0xAA];
        let v = classify(&binary, &decl("m", "10", Some("AA")));
        assert!(matches!(v.verdict, Verdict::Undeterminable { .. }));
    }

    #[test]
    fn check_all_one_verdict_per_declaration_in_order() {
        let binary = [0x00, 0x48, 0x8B, 0xFF, 0x00];
        let decls = vec![
            decl("valid", "1", Some("48 8B")),
            decl("undet", "0", None),
            decl("invalid", "1", Some("11 22")),
        ];
        let verdicts = check_all(&binary, &decls);
        // Esattamente un verdetto per dichiarazione, nell'ordine.
        assert_eq!(verdicts.len(), 3);
        assert_eq!(verdicts[0].name, "valid");
        assert_eq!(verdicts[0].verdict, Verdict::Valid);
        assert_eq!(verdicts[1].name, "undet");
        assert!(matches!(verdicts[1].verdict, Verdict::Undeterminable { .. }));
        assert_eq!(verdicts[2].name, "invalid");
        assert_eq!(verdicts[2].verdict, Verdict::Invalid);
    }

    #[test]
    fn check_all_continues_past_undeterminable() {
        let binary = [0x00, 0x48, 0x8B];
        let decls = vec![
            decl("undet", "0", None),
            decl("valid", "1", Some("48 8B")),
        ];
        let verdicts = check_all(&binary, &decls);
        assert_eq!(verdicts.len(), 2);
        // La dichiarazione successiva è stata comunque elaborata.
        assert_eq!(verdicts[1].verdict, Verdict::Valid);
    }

    #[test]
    fn check_all_empty_declarations_yields_empty() {
        let binary = [0x00, 0x01];
        assert!(check_all(&binary, &[]).is_empty());
    }
}
