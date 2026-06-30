//! `Pbind_Linter` — controllo di schema e valori dei `Binding_Set_File` `.pbind`.
//!
//! Il linter analizza il contenuto testuale di un `.pbind` e **accumula** tutte
//! le violazioni di schema e di valore in un [`Vec<LintFinding>`], **senza mai
//! terminare il processo** e **proseguendo fino a fine file**: un problema su una
//! riga non interrompe l'analisi delle righe successive. Un vettore **vuoto**
//! significa che a questo stadio non sono state rilevate violazioni di schema o
//! di valore.
//!
//! Lo scopo del linter è:
//!
//! - verificare la **presenza** delle chiavi d'intestazione `pbind_version`,
//!   `gd_version`, `platform` e, per ogni blocco `[function]`, dei campi
//!   `symbol`, `offset`, `return`, `params`, `verified`, emettendo per **ciascun**
//!   elemento mancante un finding che identifica la chiave/campo e il numero di
//!   riga (Req 7.1, 7.2);
//! - verificare il **formato/tipo** dei valori, emettendo per **ciascun** valore
//!   malformato un finding con la causa e il numero di riga (Req 7.3);
//! - verificare la **concordanza percorso↔header**: i valori interni
//!   `gd_version`/`platform` devono combaciare con la coppia derivata dal
//!   percorso `mod-index/bindings/{version}/{platform}.pbind`; in caso contrario
//!   un finding identifica la discordanza (atteso vs rilevato) (Req 7.4);
//! - verificare che `pbind_version` sia una **versione di formato supportata**
//!   da `Pbind_Format` (`kPbindFormatVersion = 1`); se non supportata, un finding
//!   identifica la versione rilevata e l'insieme di quelle supportate (Req 7.5);
//! - esprimere, in assenza di violazioni, un **esito di successo distinto**
//!   dall'errore — "idoneo alla distribuzione" — tramite [`lint_result`] e
//!   [`LintReport`] (Req 7.6).
//!
//! Il formato canonico rispecchiato è quello di
//! `loader/bindings/pbind_format.cpp` (`parse_pbind`): righe `key = value`
//! divise sul **primo** `=`; righe vuote e commenti (`#`) ignorati; `[function]`
//! apre un blocco. I formati dei valori attesi:
//!
//! - `pbind_version`: intero non negativo;
//! - `gd_version`: `major.minor` (due interi non negativi);
//! - `platform`: stringa non vuota;
//! - `offset`: intero senza segno decimale **oppure** `0x…` esadecimale;
//! - `symbol` / `return`: stringa non vuota;
//! - `params`: lista separata da virgola (vuota ammessa = zero parametri);
//! - `verified`: esattamente `true` oppure `false`.
//!
//! **Ambito del linter:** schema + valori + continuità fino a EOF (Req 7.1–7.3),
//! concordanza percorso↔header (Req 7.4), versione di formato supportata
//! (Req 7.5) ed esito di successo distinto (Req 7.6). Il parametro `path` è qui
//! impiegato per derivare la coppia `(version, platform)` attesa dal percorso
//! canonico.
//!
//! _Requisiti: 7.1, 7.2, 7.3, 7.4, 7.5, 7.6._

use std::path::Path;

/// Versione di formato `.pbind` supportata dal `Pbind_Format`, allineata a
/// `kPbindFormatVersion = 1` di `loader/bindings/pbind_format.hpp` (e a
/// `generator::PBIND_FORMAT_VERSION`). L'insieme delle versioni supportate è il
/// singoletto `{ KPBIND_FORMAT_VERSION }`.
const KPBIND_FORMAT_VERSION: u32 = 1;

/// Categoria di una violazione rilevata dal [`lint_pbind`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LintKind {
    /// Una chiave d'intestazione richiesta (`pbind_version`/`gd_version`/
    /// `platform`) è assente dal file.
    MissingHeaderKey,
    /// Un campo `[function]` richiesto (`symbol`/`offset`/`return`/`params`/
    /// `verified`) è assente dal blocco.
    MissingFunctionField,
    /// Il valore di un campo non rispetta il tipo/formato atteso.
    MalformedValue,
    /// Una riga non vuota e non commento non è né `[function]` né
    /// `key = value`.
    MalformedLine,
    /// Una chiave non riconosciuta nel contesto (intestazione o blocco).
    UnknownKey,
    /// Il valore interno di `gd_version` o `platform` non combacia con la coppia
    /// derivata dal percorso `mod-index/bindings/{version}/{platform}.pbind`
    /// (Req 7.4).
    PathHeaderMismatch,
    /// Il valore di `pbind_version` non appartiene all'insieme delle versioni di
    /// formato supportate da `Pbind_Format` (Req 7.5).
    UnsupportedFormatVersion,
}

/// Una singola violazione di schema o di valore rilevata in un `.pbind`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LintFinding {
    /// Numero di riga (1-based) a cui si riferisce la violazione. Per le chiavi
    /// d'intestazione mancanti, prive di una riga specifica, vale `0`.
    pub line: usize,
    /// Categoria della violazione.
    pub kind: LintKind,
    /// Messaggio leggibile che identifica chiave/campo o causa.
    pub message: String,
}

impl LintFinding {
    fn new(line: usize, kind: LintKind, message: impl Into<String>) -> Self {
        Self {
            line,
            kind,
            message: message.into(),
        }
    }
}

/// Stato accumulato per il blocco `[function]` corrente durante l'analisi:
/// la riga del suo header e quali campi richiesti sono stati visti.
struct PendingFunction {
    header_line: usize,
    symbol: bool,
    offset: bool,
    return_type: bool,
    params: bool,
    verified: bool,
}

impl PendingFunction {
    fn new(header_line: usize) -> Self {
        Self {
            header_line,
            symbol: false,
            offset: false,
            return_type: false,
            params: false,
            verified: false,
        }
    }
}

/// Analizza il contenuto di un `Binding_Set_File` e restituisce **tutte** le
/// violazioni di schema e di valore rilevate, proseguendo fino a fine file senza
/// terminare il processo. Un vettore vuoto indica nessuna violazione.
///
/// `path` è il percorso del file analizzato: la coppia `(version, platform)` da
/// esso derivata (`mod-index/bindings/{version}/{platform}.pbind`) è confrontata
/// con i valori interni `gd_version`/`platform` per la concordanza
/// percorso↔header (Req 7.4).
///
/// Per un esito di successo **distinto** dall'errore ("idoneo alla
/// distribuzione", Req 7.6) si veda [`lint_result`], che incapsula questo
/// vettore in un [`LintReport`].
pub fn lint_pbind(path: &Path, content: &str) -> Vec<LintFinding> {
    let mut findings: Vec<LintFinding> = Vec::new();

    // Presenza delle chiavi d'intestazione richieste.
    let mut seen_pbind_version = false;
    let mut seen_gd_version = false;
    let mut seen_platform = false;

    // Valore (e riga) della prima occorrenza delle chiavi d'intestazione, per i
    // controlli di concordanza percorso↔header (Req 7.4) e di versione di
    // formato supportata (Req 7.5), eseguiti a valle del ciclo.
    let mut pbind_version_value: Option<(usize, String)> = None;
    let mut gd_version_value: Option<(usize, String)> = None;
    let mut platform_value: Option<(usize, String)> = None;

    // Blocco [function] in corso di analisi (None ⇒ sezione d'intestazione).
    let mut pending: Option<PendingFunction> = None;

    for (idx, raw_line) in content.lines().enumerate() {
        let line_no = idx + 1;
        let line = raw_line.trim();

        // Righe vuote e commenti ('#') sono ignorate.
        if line.is_empty() || line.starts_with('#') {
            continue;
        }

        // Inizio di un nuovo blocco [function]: chiude il precedente.
        if line == "[function]" {
            if let Some(prev) = pending.take() {
                finalize_function(&mut findings, &prev);
            }
            pending = Some(PendingFunction::new(line_no));
            continue;
        }

        // Qualunque altra riga deve essere "key = value" (split sul primo '=').
        let Some((key, value)) = split_key_value(line) else {
            findings.push(LintFinding::new(
                line_no,
                LintKind::MalformedLine,
                "riga malformata: atteso 'key = value' oppure '[function]'",
            ));
            continue;
        };
        if key.is_empty() {
            findings.push(LintFinding::new(
                line_no,
                LintKind::MalformedLine,
                "chiave vuota a sinistra di '='",
            ));
            continue;
        }

        match pending.as_mut() {
            // Sezione d'intestazione (prima di qualunque [function]).
            None => match key {
                "pbind_version" => {
                    seen_pbind_version = true;
                    pbind_version_value
                        .get_or_insert_with(|| (line_no, value.trim().to_owned()));
                    if !is_valid_unsigned_decimal(value) {
                        findings.push(LintFinding::new(
                            line_no,
                            LintKind::MalformedValue,
                            "'pbind_version' non è un intero non negativo valido",
                        ));
                    }
                }
                "gd_version" => {
                    seen_gd_version = true;
                    gd_version_value
                        .get_or_insert_with(|| (line_no, value.trim().to_owned()));
                    if !is_valid_gd_version(value) {
                        findings.push(LintFinding::new(
                            line_no,
                            LintKind::MalformedValue,
                            "'gd_version' malformata: atteso 'major.minor' (due interi non negativi)",
                        ));
                    }
                }
                "platform" => {
                    seen_platform = true;
                    platform_value
                        .get_or_insert_with(|| (line_no, value.trim().to_owned()));
                    if value.is_empty() {
                        findings.push(LintFinding::new(
                            line_no,
                            LintKind::MalformedValue,
                            "'platform' non può essere vuota",
                        ));
                    }
                }
                other => {
                    findings.push(LintFinding::new(
                        line_no,
                        LintKind::UnknownKey,
                        format!("chiave d'intestazione sconosciuta: '{other}'"),
                    ));
                }
            },
            // Campo all'interno di un blocco [function].
            Some(func) => match key {
                "symbol" => {
                    func.symbol = true;
                    if value.is_empty() {
                        findings.push(LintFinding::new(
                            line_no,
                            LintKind::MalformedValue,
                            "'symbol' non può essere vuoto",
                        ));
                    }
                }
                "offset" => {
                    func.offset = true;
                    if !is_valid_offset(value) {
                        findings.push(LintFinding::new(
                            line_no,
                            LintKind::MalformedValue,
                            "'offset' non è un intero senza segno valido (atteso decimale o 0x...)",
                        ));
                    }
                }
                "return" => {
                    func.return_type = true;
                    if value.is_empty() {
                        findings.push(LintFinding::new(
                            line_no,
                            LintKind::MalformedValue,
                            "'return' non può essere vuoto",
                        ));
                    }
                }
                "params" => {
                    func.params = true;
                    if !is_valid_params(value) {
                        findings.push(LintFinding::new(
                            line_no,
                            LintKind::MalformedValue,
                            "'params' malformati: lista separata da virgola senza elementi vuoti",
                        ));
                    }
                }
                "verified" => {
                    func.verified = true;
                    if !is_valid_bool(value) {
                        findings.push(LintFinding::new(
                            line_no,
                            LintKind::MalformedValue,
                            "'verified' deve essere 'true' o 'false'",
                        ));
                    }
                }
                other => {
                    findings.push(LintFinding::new(
                        line_no,
                        LintKind::UnknownKey,
                        format!("chiave di [function] sconosciuta: '{other}'"),
                    ));
                }
            },
        }
    }

    // Chiude l'ultimo blocco [function] eventualmente aperto.
    if let Some(prev) = pending.take() {
        finalize_function(&mut findings, &prev);
    }

    // Chiavi d'intestazione mancanti: nessuna riga specifica ⇒ riga 0.
    if !seen_pbind_version {
        findings.push(LintFinding::new(
            0,
            LintKind::MissingHeaderKey,
            "chiave d'intestazione mancante: 'pbind_version'",
        ));
    }
    if !seen_gd_version {
        findings.push(LintFinding::new(
            0,
            LintKind::MissingHeaderKey,
            "chiave d'intestazione mancante: 'gd_version'",
        ));
    }
    if !seen_platform {
        findings.push(LintFinding::new(
            0,
            LintKind::MissingHeaderKey,
            "chiave d'intestazione mancante: 'platform'",
        ));
    }

    // --- Versione di formato supportata (Req 7.5) ----------------------------
    // Se `pbind_version` è presente ed è un intero valido (il task 11.1 ha già
    // segnalato i valori non interi come MalformedValue), verifica che appartenga
    // all'insieme delle versioni supportate `{ KPBIND_FORMAT_VERSION }`.
    if let Some((line_no, value)) = &pbind_version_value {
        if let Ok(found) = value.parse::<u32>() {
            if found != KPBIND_FORMAT_VERSION {
                findings.push(LintFinding::new(
                    *line_no,
                    LintKind::UnsupportedFormatVersion,
                    format!(
                        "'pbind_version' = {found} non supportata da Pbind_Format; \
                         versioni supportate: {{{KPBIND_FORMAT_VERSION}}}"
                    ),
                ));
            }
        }
    }

    // --- Concordanza percorso↔header (Req 7.4) -------------------------------
    // La coppia attesa è derivata dal percorso canonico
    // `mod-index/bindings/{version}/{platform}.pbind`: la directory genitore è la
    // `{version}` (es. "2.2081", come `GdVersion` Display) e lo stem del file è il
    // `{platform}` (es. "macos-arm64", come `TargetPlatform::platform_id`). Il
    // confronto è stringa-su-stringa secondo la stessa convenzione del percorso
    // prodotto dal generatore (`generator::pbind_path`). I valori d'intestazione
    // assenti sono già coperti da `MissingHeaderKey`: qui si confrontano solo i
    // valori presenti.
    if let Some((expected_version, expected_platform)) = derive_pair_from_path(path) {
        if let Some((line_no, found)) = &gd_version_value {
            if found != &expected_version {
                findings.push(LintFinding::new(
                    *line_no,
                    LintKind::PathHeaderMismatch,
                    format!(
                        "'gd_version' interna non concorda col percorso: \
                         atteso '{expected_version}', rilevato '{found}'"
                    ),
                ));
            }
        }
        if let Some((line_no, found)) = &platform_value {
            if found != &expected_platform {
                findings.push(LintFinding::new(
                    *line_no,
                    LintKind::PathHeaderMismatch,
                    format!(
                        "'platform' interna non concorda col percorso: \
                         atteso '{expected_platform}', rilevato '{found}'"
                    ),
                ));
            }
        }
    }

    findings
}

/// Coppia `(version, platform)` attesa derivata dal percorso canonico
/// `mod-index/bindings/{version}/{platform}.pbind`: la `{version}` è il nome
/// della directory genitore (es. "2.2081") e il `{platform}` è lo stem del file
/// (es. "macos-arm64"). Restituisce `None` se il percorso non espone entrambi i
/// componenti (in tal caso la concordanza percorso↔header non viene controllata).
fn derive_pair_from_path(path: &Path) -> Option<(String, String)> {
    let platform = path.file_stem()?.to_str()?.to_owned();
    let version = path.parent()?.file_name()?.to_str()?.to_owned();
    Some((version, platform))
}

/// Esito della lintatura di un `Binding_Set_File`: un risultato di **successo
/// distinto** dall'errore (Req 7.6).
///
/// - [`LintReport::Distributable`]: nessuna violazione di schema, di valore, di
///   concordanza percorso↔header o di versione di formato — il file è **idoneo
///   alla distribuzione**.
/// - [`LintReport::Findings`]: una o più violazioni; il file **non** è idoneo
///   alla distribuzione e il vettore le elenca tutte.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LintReport {
    /// Nessuna violazione: il file è idoneo alla distribuzione (Req 7.6).
    Distributable,
    /// Una o più violazioni rilevate; il file non è idoneo alla distribuzione.
    Findings(Vec<LintFinding>),
}

impl LintReport {
    /// `true` sse l'esito è di successo (idoneo alla distribuzione), cioè
    /// distinto da qualunque esito di errore (Req 7.6).
    pub fn is_distributable(&self) -> bool {
        matches!(self, LintReport::Distributable)
    }

    /// Alias di [`LintReport::is_distributable`]: `true` sse non vi sono
    /// violazioni.
    pub fn is_ok(&self) -> bool {
        self.is_distributable()
    }

    /// Le violazioni rilevate; vuoto quando l'esito è di successo.
    pub fn findings(&self) -> &[LintFinding] {
        match self {
            LintReport::Distributable => &[],
            LintReport::Findings(findings) => findings,
        }
    }

    /// Messaggio leggibile che distingue l'esito di successo da quello di errore.
    pub fn summary(&self) -> String {
        match self {
            LintReport::Distributable => "idoneo alla distribuzione".to_owned(),
            LintReport::Findings(findings) => {
                format!("non idoneo alla distribuzione: {} violazioni", findings.len())
            }
        }
    }
}

/// Analizza un `Binding_Set_File` e restituisce un [`LintReport`] che esprime un
/// **esito di successo distinto** dall'errore (Req 7.6): [`LintReport::Distributable`]
/// ("idoneo alla distribuzione") quando [`lint_pbind`] non rileva alcuna
/// violazione, altrimenti [`LintReport::Findings`] con l'elenco completo.
///
/// È costruita **sopra** [`lint_pbind`] e ne preserva il comportamento: il
/// vettore di finding resta accessibile via [`LintReport::findings`].
pub fn lint_result(path: &Path, content: &str) -> LintReport {
    let findings = lint_pbind(path, content);
    if findings.is_empty() {
        LintReport::Distributable
    } else {
        LintReport::Findings(findings)
    }
}

/// Emette un finding per **ciascun** campo richiesto assente dal blocco
/// `[function]`, riferito al numero di riga dell'header del blocco.
fn finalize_function(findings: &mut Vec<LintFinding>, func: &PendingFunction) {
    let missing: [(bool, &str); 5] = [
        (func.symbol, "symbol"),
        (func.offset, "offset"),
        (func.return_type, "return"),
        (func.params, "params"),
        (func.verified, "verified"),
    ];
    for (seen, field) in missing {
        if !seen {
            findings.push(LintFinding::new(
                func.header_line,
                LintKind::MissingFunctionField,
                format!("blocco [function] privo della chiave '{field}'"),
            ));
        }
    }
}

/// Divide una riga `key = value` sul **primo** `=`, restituendo le due metà
/// senza spazi iniziali/finali. `None` se manca `=`.
fn split_key_value(line: &str) -> Option<(&str, &str)> {
    line.find('=')
        .map(|eq| (line[..eq].trim(), line[eq + 1..].trim()))
}

/// Verifica che `text` sia un intero non negativo in base 10 (per
/// `pbind_version`).
fn is_valid_unsigned_decimal(text: &str) -> bool {
    let t = text.trim();
    !t.is_empty() && t.bytes().all(|b| b.is_ascii_digit()) && t.parse::<u64>().is_ok()
}

/// Verifica che `text` sia `major.minor` con due interi non negativi.
fn is_valid_gd_version(text: &str) -> bool {
    match text.trim().split_once('.') {
        Some((major, minor)) => is_valid_unsigned_decimal(major) && is_valid_unsigned_decimal(minor),
        None => false,
    }
}

/// Verifica che `text` sia un intero senza segno decimale **oppure** `0x…`
/// esadecimale (consumo totale richiesto), coerente con `parseUnsigned` C++.
fn is_valid_offset(text: &str) -> bool {
    let t = text.trim();
    if t.is_empty() {
        return false;
    }
    if let Some(hex) = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")) {
        !hex.is_empty()
            && hex.bytes().all(|b| b.is_ascii_hexdigit())
            && u64::from_str_radix(hex, 16).is_ok()
    } else {
        t.bytes().all(|b| b.is_ascii_digit()) && t.parse::<u64>().is_ok()
    }
}

/// Verifica una lista di parametri separati da virgola: la stringa vuota è
/// ammessa (zero parametri); altrimenti nessun elemento può essere vuoto.
fn is_valid_params(text: &str) -> bool {
    let t = text.trim();
    if t.is_empty() {
        return true;
    }
    t.split(',').all(|token| !token.trim().is_empty())
}

/// Verifica che `text` sia esattamente `true` o `false`.
fn is_valid_bool(text: &str) -> bool {
    matches!(text.trim(), "true" | "false")
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    /// Percorso fittizio: il task 11.1 non usa il percorso per validare.
    fn fake_path() -> PathBuf {
        PathBuf::from("mod-index/bindings/2.2081/macos-arm64.pbind")
    }

    /// Un `.pbind` ben formato (formato canonico del generatore) non produce
    /// alcun finding.
    #[test]
    fn well_formed_file_yields_no_findings() {
        let content = "\
pbind_version = 1
gd_version = 2.2081
platform = macos-arm64

[function]
symbol = MenuLayer::init
offset = 0x316688
return = bool
params = MenuLayer*
verified = true

[function]
symbol = PlayLayer::update
offset = 0x4000
return = void
params = PlayLayer*, float
verified = false
";
        let findings = lint_pbind(&fake_path(), content);
        assert!(findings.is_empty(), "atteso nessun finding, trovati: {findings:?}");
    }

    /// Un blocco `[function]` senza parametri (`params =` vuoto) è valido:
    /// zero parametri sono ammessi.
    #[test]
    fn empty_params_is_valid() {
        let content = "\
pbind_version = 1
gd_version = 2.2081
platform = macos-arm64

[function]
symbol = SomeClass::ctor
offset = 0x10
return = void
params =
verified = false
";
        let findings = lint_pbind(&fake_path(), content);
        assert!(findings.is_empty(), "params vuoti devono essere validi: {findings:?}");
    }

    /// Chiavi d'intestazione mancanti: una finding per ciascuna, con riga 0.
    #[test]
    fn missing_header_keys_yield_one_finding_each() {
        // Nessuna chiave d'intestazione, ma un blocco [function] completo.
        let content = "\
[function]
symbol = A::b
offset = 0x1
return = void
params =
verified = false
";
        let findings = lint_pbind(&fake_path(), content);

        let header_missing: Vec<_> = findings
            .iter()
            .filter(|f| f.kind == LintKind::MissingHeaderKey)
            .collect();
        assert_eq!(header_missing.len(), 3, "attese 3 chiavi header mancanti: {findings:?}");
        for f in &header_missing {
            assert_eq!(f.line, 0, "chiave header mancante deve usare riga 0");
        }
        assert!(header_missing.iter().any(|f| f.message.contains("pbind_version")));
        assert!(header_missing.iter().any(|f| f.message.contains("gd_version")));
        assert!(header_missing.iter().any(|f| f.message.contains("platform")));
    }

    /// Campi `[function]` mancanti: una finding per ciascuno, con la riga
    /// dell'header del blocco; l'analisi prosegue fino a EOF coprendo entrambi
    /// i blocchi.
    #[test]
    fn missing_function_fields_yield_one_finding_each_continuing_to_eof() {
        // Header completo. Primo blocco (riga 5) privo di tutti i 5 campi;
        // secondo blocco (riga 7) privo solo di 'verified'.
        let content = "\
pbind_version = 1
gd_version = 2.2081
platform = macos-arm64

[function]

[function]
symbol = B::c
offset = 0x2
return = int
params = int
";
        let findings = lint_pbind(&fake_path(), content);

        let block1: Vec<_> = findings
            .iter()
            .filter(|f| f.kind == LintKind::MissingFunctionField && f.line == 5)
            .collect();
        // Primo blocco: tutti e 5 i campi mancanti.
        assert_eq!(block1.len(), 5, "atteso 5 campi mancanti per il blocco a riga 5: {findings:?}");

        let block2: Vec<_> = findings
            .iter()
            .filter(|f| f.kind == LintKind::MissingFunctionField && f.line == 7)
            .collect();
        // Secondo blocco (header a riga 7): manca solo 'verified'.
        assert_eq!(block2.len(), 1, "atteso 1 campo mancante per il blocco a riga 7: {findings:?}");
        assert!(block2[0].message.contains("verified"));
    }

    /// Valori malformati (offset, verified, gd_version) producono finding con
    /// causa e riga; l'analisi prosegue oltre il primo errore.
    #[test]
    fn malformed_values_yield_findings_with_cause_and_line() {
        let content = "\
pbind_version = 1
gd_version = 2.x
platform = macos-arm64

[function]
symbol = A::b
offset = 0xZZ
return = void
params =
verified = maybe
";
        let findings = lint_pbind(&fake_path(), content);

        // gd_version malformata a riga 2.
        let gd = findings
            .iter()
            .find(|f| f.kind == LintKind::MalformedValue && f.line == 2)
            .expect("atteso finding gd_version a riga 2");
        assert!(gd.message.contains("gd_version"));

        // offset malformato a riga 7.
        let off = findings
            .iter()
            .find(|f| f.kind == LintKind::MalformedValue && f.line == 7)
            .expect("atteso finding offset a riga 7");
        assert!(off.message.contains("offset"));

        // verified malformato a riga 10 (l'analisi è proseguita fino a EOF).
        let ver = findings
            .iter()
            .find(|f| f.kind == LintKind::MalformedValue && f.line == 10)
            .expect("atteso finding verified a riga 10");
        assert!(ver.message.contains("verified"));
    }

    /// Una riga senza `=` e non `[function]` è segnalata come riga malformata,
    /// senza interrompere l'analisi.
    #[test]
    fn malformed_line_is_reported_and_analysis_continues() {
        let content = "\
pbind_version = 1
gd_version = 2.2081
platform = macos-arm64
questa riga non ha un uguale
";
        let findings = lint_pbind(&fake_path(), content);
        assert!(
            findings
                .iter()
                .any(|f| f.kind == LintKind::MalformedLine && f.line == 4),
            "attesa riga malformata a riga 4: {findings:?}"
        );
    }

    /// Decimale e 0x esadecimale sono entrambi offset validi.
    #[test]
    fn offset_accepts_decimal_and_hex() {
        assert!(is_valid_offset("4096"));
        assert!(is_valid_offset("0x316688"));
        assert!(is_valid_offset("0X1A"));
        assert!(!is_valid_offset("0x"));
        assert!(!is_valid_offset("0xZZ"));
        assert!(!is_valid_offset(""));
        assert!(!is_valid_offset("12g"));
    }

    /// Contenuto ben formato del generatore, coerente con il percorso, riusato
    /// dai test 11.2.
    fn clean_content() -> &'static str {
        "\
pbind_version = 1
gd_version = 2.2081
platform = macos-arm64

[function]
symbol = MenuLayer::init
offset = 0x316688
return = bool
params = MenuLayer*
verified = true
"
    }

    /// Req 7.4 — `gd_version` interna che NON combacia con la versione del
    /// percorso → finding di discordanza con atteso vs rilevato.
    #[test]
    fn internal_gd_version_mismatching_path_yields_finding() {
        // Percorso 2.2081, ma header dichiara 2.9999.
        let path = PathBuf::from("mod-index/bindings/2.2081/macos-arm64.pbind");
        let content = "\
pbind_version = 1
gd_version = 2.9999
platform = macos-arm64

[function]
symbol = A::b
offset = 0x10
return = void
params =
verified = false
";
        let findings = lint_pbind(&path, content);
        let mismatch = findings
            .iter()
            .find(|f| f.kind == LintKind::PathHeaderMismatch && f.message.contains("gd_version"))
            .expect("attesa discordanza gd_version↔percorso");
        assert!(mismatch.message.contains("2.2081"), "deve indicare l'atteso: {mismatch:?}");
        assert!(mismatch.message.contains("2.9999"), "deve indicare il rilevato: {mismatch:?}");
    }

    /// Req 7.4 — `platform` interna che NON combacia con quella del percorso →
    /// finding di discordanza con atteso vs rilevato.
    #[test]
    fn internal_platform_mismatching_path_yields_finding() {
        // Percorso macos-arm64, ma header dichiara windows-x64.
        let path = PathBuf::from("mod-index/bindings/2.2081/macos-arm64.pbind");
        let content = "\
pbind_version = 1
gd_version = 2.2081
platform = windows-x64

[function]
symbol = A::b
offset = 0x10
return = void
params =
verified = false
";
        let findings = lint_pbind(&path, content);
        let mismatch = findings
            .iter()
            .find(|f| f.kind == LintKind::PathHeaderMismatch && f.message.contains("platform"))
            .expect("attesa discordanza platform↔percorso");
        assert!(mismatch.message.contains("macos-arm64"), "deve indicare l'atteso: {mismatch:?}");
        assert!(mismatch.message.contains("windows-x64"), "deve indicare il rilevato: {mismatch:?}");
    }

    /// Req 7.5 — `pbind_version` non supportata (es. 2) → finding che indica la
    /// versione rilevata e l'insieme supportato `{1}`.
    #[test]
    fn unsupported_pbind_version_yields_finding_with_supported_set() {
        let path = PathBuf::from("mod-index/bindings/2.2081/macos-arm64.pbind");
        let content = "\
pbind_version = 2
gd_version = 2.2081
platform = macos-arm64

[function]
symbol = A::b
offset = 0x10
return = void
params =
verified = false
";
        let findings = lint_pbind(&path, content);
        let unsupported = findings
            .iter()
            .find(|f| f.kind == LintKind::UnsupportedFormatVersion)
            .expect("attesa versione di formato non supportata");
        assert!(unsupported.message.contains('2'), "deve indicare la versione rilevata: {unsupported:?}");
        assert!(unsupported.message.contains('1'), "deve indicare l'insieme supportato {{1}}: {unsupported:?}");
        // La versione supportata (1) NON deve produrre alcun finding di questo tipo.
        let supported = lint_pbind(&path, clean_content());
        assert!(
            !supported.iter().any(|f| f.kind == LintKind::UnsupportedFormatVersion),
            "la versione 1 è supportata: {supported:?}"
        );
    }

    /// Req 7.6 — un file pulito del generatore al percorso corretto produce un
    /// esito di successo **distinto** dall'errore: "idoneo alla distribuzione".
    #[test]
    fn clean_file_yields_distributable_result_distinct_from_error() {
        let path = PathBuf::from("mod-index/bindings/2.2081/macos-arm64.pbind");
        let report = lint_result(&path, clean_content());

        assert!(report.is_distributable(), "atteso esito di successo: {report:?}");
        assert!(report.is_ok());
        assert_eq!(report, LintReport::Distributable);
        assert!(report.findings().is_empty());
        assert_eq!(report.summary(), "idoneo alla distribuzione");
    }

    /// Req 7.6 — un file con violazioni produce un esito di errore distinto dal
    /// successo, con i finding accessibili.
    #[test]
    fn file_with_violations_yields_error_result_distinct_from_success() {
        // Percorso macos-arm64 ma platform interna discorde ⇒ almeno un finding.
        let path = PathBuf::from("mod-index/bindings/2.2081/macos-arm64.pbind");
        let content = "\
pbind_version = 1
gd_version = 2.2081
platform = windows-x64

[function]
symbol = A::b
offset = 0x10
return = void
params =
verified = false
";
        let report = lint_result(&path, content);

        assert!(!report.is_distributable(), "atteso esito di errore: {report:?}");
        assert!(!report.is_ok());
        assert_ne!(report, LintReport::Distributable);
        assert!(!report.findings().is_empty());
        assert!(report.summary().contains("non idoneo"));
    }
}
