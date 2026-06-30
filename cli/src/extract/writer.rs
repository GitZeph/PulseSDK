//! `CatalogEntryWriter` — emissione **deterministica, atomica e fail-closed** dei
//! `Catalog_Entry` derivati nel **formato TOML esistente** sotto
//! `mod-index/catalog/symbols/*.toml` (Req 6.4, 8.5, 8.6, 9.1, 9.2, 9.5, 9.6,
//! 10.3, 11.1, 11.2, 11.3, 11.4).
//!
//! Questo è l'**unico** output del `Binding_Extractor`: il writer **non**
//! introduce alcuno schema nuovo, ma scrive lo **stesso** formato che
//! `MenuLayer__init.toml` usa oggi (`schema_version`, `symbol`, `[signature]`
//! `return`/`params`, `[[offset]]` con `gd_version`/`platform`/`rva`/`verified`
//! e `[offset.provenance]`), così che `load_catalog` (`catalog.rs`) lo carichi
//! **invariato** e il `Binding_Generator` lo pivoti in `.pbind` **senza
//! modifiche** (Req 11.1, 11.2, 11.5). L'estensione di provenienza per i tier è
//! **additiva** nella sola sezione opaca `[offset.provenance]` (Req 6.2).
//!
//! ## Scelta del tipo di ingresso (documentata)
//!
//! La firma del design è
//! `write_catalog_entries(entries, catalog_root) -> Result<ExtractReport, …>`,
//! ma il tipo [`crate::bindings::catalog::CatalogEntry`] **riusato** porta solo
//! il [`crate::bindings::catalog::ProvenanceRecord`] *base* dello schema core e
//! **non** può trasportare i campi estesi dei tier (`tier`,
//! `derivation_method`, `binary_identity`) prodotti dalla Phase F. Per non
//! falsare lo schema core né perdere la provenienza estesa, il writer accetta un
//! tipo d'ingresso **più ricco**, [`ExtractedEntry`]/[`ExtractedOffset`], che
//! impacchetta l'[`ExtractionProvenance`] della Phase F accanto a simbolo, firma
//! e offset. Il writer poi **mappa additivamente** quella provenienza sul TOML
//! esistente (vedi [`serialize_provenance`]). Questa è la scelta pulita rispetto
//! all'alternativa di gonfiare `CatalogEntry` con campi che `load_catalog`
//! ignorerebbe.
//!
//! ## Determinismo (Req 9.1, 9.2, 9.5, 9.6)
//!
//! - Le voci sono ordinate per [`SymbolId`]; dentro una voce gli `[[offset]]`
//!   sono ordinati per [`TargetPair`] (`Ord`).
//! - La serializzazione è **byte-identica** tra esecuzioni: nessun timestamp,
//!   nessun percorso assoluto, nessun ordine instabile (gli hash della
//!   `Binary_Identity` derivano solo dal contenuto dei binari, Req 9.4). Gli
//!   stessi ingressi su un catalogo-radice equivalente producono gli stessi
//!   byte.
//!
//! ## Scrittura atomica (Req 9.x, 10.3)
//!
//! Ogni file è prodotto in memoria, scritto in un file **temporaneo nella stessa
//! directory** e promosso con un `rename` atomico (mirror di
//! `bindings/generator.rs::atomic_write`). Su fallimento il file precedente
//! resta intatto byte-per-byte e nessun file parziale è lasciato. Nessun byte di
//! contenuto eseguibile dei `Source_Binary` finisce mai nell'output (Req 10.3):
//! il writer emette solo identificatori, firme, valori numerici ed etichette di
//! identità (hash del contenuto).
//!
//! ## Conflitti e politica di merge (Req 11.4, 8.5, 6.4, 8.6)
//!
//! Il catalogo è **un file per simbolo**, e quel file contiene **tutte** le
//! coppie di quel simbolo. Quando una voce derivata riguarda un simbolo già
//! presente su disco, il writer applica una politica **fail-closed** che non
//! declassa **mai** dati esistenti:
//!
//! - **firma divergente** rispetto alla voce esistente → [`ExtractError::Conflict`]
//!   (la voce esistente resta invariata byte-per-byte, niente viene scritto);
//! - **coppia già presente** su disco con un offset esistente:
//!   - se il nuovo offset è **risolto** (RVA reale, non sentinel) → è un
//!     conflitto reale per quella coppia → [`ExtractError::Conflict`] (Req 11.4):
//!     la voce esistente — incluso il **seed** `MenuLayer::init`
//!     (`manual-prologue-confirmed`, `verified = true`) — è lasciata invariata e
//!     **mai** sovrascritta o declassata (Req 6.4, 8.6);
//!   - se il nuovo offset è **sentinel** (nessun RVA risolvibile) → viene
//!     **scartato** silenziosamente: il dato esistente vince e resta intatto,
//!     senza creare un doppione di coppia (che `load_catalog` rifiuterebbe
//!     comunque). Nessun declassamento, nessun conflitto.
//! - **coppia nuova** per un simbolo esistente → il nuovo `[[offset]]` è
//!   **appeso** al contenuto esistente lasciato **byte-per-byte invariato**
//!   (Req 8.5): gli offset delle altre coppie non sono mai riscritti.
//!
//! Il rilevamento dei conflitti avviene in una **fase di pianificazione**
//! interamente precedente a qualunque scrittura: se un solo conflitto è
//! rilevato, il writer ritorna l'errore **senza aver toccato alcun file**
//! (semantica tutto-o-niente), e il `Provenance_Record`/seed resta intatto.
//!
//! _Requisiti: 6.4, 8.5, 8.6, 9.1, 9.2, 9.5, 9.6, 10.3, 11.1, 11.2, 11.3, 11.4._

use std::path::{Path, PathBuf};

use crate::bindings::catalog::{load_catalog_entry, CatalogError};
use crate::bindings::contribution::symbol_file_stem;

use super::provenance::ExtractionProvenance;
use super::report::ExtractReport;
use super::{ExtractError, Signature, SymbolId, TargetPair, SENTINEL_VALUE};

/// Un offset derivato pronto per l'emissione nel catalogo, con la sua
/// provenienza estesa (Phase F).
///
/// `rva == None` (o `Some(SENTINEL_VALUE)`) rappresenta il **sentinel logico**:
/// un offset non risolvibile (NoMatch/Ambiguous, prologo non superato o
/// cross-derivazione non sana). In tal caso `provenance` è `None` (un offset
/// sentinel non riceve alcun tier, Req 6.6) e `verified` è forzato a `false`
/// (Req 11.3).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExtractedOffset {
    /// Coppia `(GD_Version, Target_Platform)` che indicizza l'offset (Req 8.4).
    pub pair: TargetPair,
    /// RVA derivato; `None`/`Some(SENTINEL_VALUE)` ⇒ sentinel (Req 11.3).
    pub rva: Option<u64>,
    /// `Verified_Flag` del gate prologo/auditabilità; sempre `false` per il
    /// sentinel (Req 11.3).
    pub verified: bool,
    /// Provenienza estesa (Phase F), additiva su `[offset.provenance]`; `None`
    /// per un offset sentinel (nessun tier, Req 6.6).
    pub provenance: Option<ExtractionProvenance>,
}

impl ExtractedOffset {
    /// `true` se l'offset è il sentinel logico (RVA assente o pari al sentinel).
    pub fn is_sentinel(&self) -> bool {
        matches!(self.rva, None | Some(SENTINEL_VALUE))
    }

    /// `Verified_Flag` da emettere, **fail-closed**: `true` solo per un offset
    /// risolto con `verified` vero; sempre `false` per il sentinel (Req 11.3).
    fn emit_verified(&self) -> bool {
        !self.is_sentinel() && self.verified
    }
}

/// Una `Catalog_Entry` derivata pronta per l'emissione: simbolo, firma unica e
/// offset per coppia, con la provenienza estesa di ciascun offset.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExtractedEntry {
    /// Identificatore canonico del simbolo (`"Class::method"`).
    pub symbol: SymbolId,
    /// Firma unica valida per tutte le coppie (this-first), dal demangler
    /// Android (Req 2.3).
    pub signature: Signature,
    /// Offset derivati, uno per coppia.
    pub offsets: Vec<ExtractedOffset>,
}

/// Piano di scrittura di un singolo file-per-simbolo, calcolato nella fase di
/// pianificazione (prima di qualunque scrittura).
enum FilePlan {
    /// Nuovo file da creare interamente con `content`.
    Create { path: PathBuf, content: String },
    /// File esistente da estendere: il contenuto esistente è preservato
    /// byte-per-byte e i nuovi blocchi `[[offset]]` sono appesi (Req 8.5).
    Append { path: PathBuf, content: String },
}

impl FilePlan {
    fn path(&self) -> &Path {
        match self {
            FilePlan::Create { path, .. } | FilePlan::Append { path, .. } => path,
        }
    }

    fn content(&self) -> &str {
        match self {
            FilePlan::Create { content, .. } | FilePlan::Append { content, .. } => content,
        }
    }
}

/// Scrive le `Catalog_Entry` derivate nel formato TOML **esistente** sotto
/// `catalog_root/symbols/*.toml`, in modo **deterministico, atomico e
/// fail-closed**, restituendo l'[`ExtractReport`] degli offset emessi per tier
/// (Req 7.3).
///
/// Vedi la documentazione del modulo per determinismo, scrittura atomica e
/// politica di conflitto/merge. Il rilevamento dei conflitti precede ogni
/// scrittura: al primo conflitto il writer ritorna [`ExtractError::Conflict`]
/// **senza** aver toccato alcun file (Req 11.4), preservando il seed e gli
/// offset delle altre coppie byte-per-byte (Req 6.4, 8.5, 8.6).
pub fn write_catalog_entries(
    entries: &[ExtractedEntry],
    catalog_root: &Path,
) -> Result<ExtractReport, ExtractError> {
    // Ordine deterministico delle voci per SymbolId (Req 9.1).
    let mut ordered: Vec<&ExtractedEntry> = entries.iter().collect();
    ordered.sort_by(|a, b| a.symbol.cmp(&b.symbol));

    // Difesa contro simboli duplicati nell'ingresso (che scriverebbero lo stesso
    // file): fail-closed.
    for window in ordered.windows(2) {
        if window[0].symbol == window[1].symbol {
            return Err(ExtractError::Conflict {
                symbol: window[0].symbol.to_string(),
                pair: "-".to_owned(),
                reason: "simbolo duplicato nell'insieme di voci derivate".to_owned(),
            });
        }
    }

    // ---- Fase 1: pianificazione + rilevamento conflitti (nessuna scrittura) --
    let symbols_dir = catalog_root.join("symbols");
    let mut report = ExtractReport::new();
    let mut plans: Vec<FilePlan> = Vec::new();
    // Mappa percorso-pianificato → simbolo, per rilevare collisioni di file-stem
    // fra simboli distinti **prima** di qualunque scrittura (fail-closed).
    let mut planned_paths: std::collections::HashMap<PathBuf, SymbolId> =
        std::collections::HashMap::new();

    for entry in ordered {
        // Offset ordinati per coppia (Req 9.2) + dedup di coppia nell'ingresso.
        let mut offsets: Vec<&ExtractedOffset> = entry.offsets.iter().collect();
        offsets.sort_by(|a, b| a.pair.cmp(&b.pair));
        for window in offsets.windows(2) {
            if window[0].pair == window[1].pair {
                return Err(ExtractError::Conflict {
                    symbol: entry.symbol.to_string(),
                    pair: window[0].pair.to_string(),
                    reason: "coppia duplicata per lo stesso simbolo nell'ingresso derivato"
                        .to_owned(),
                });
            }
        }

        let stem = symbol_file_stem(&entry.symbol);

        // Validazione del file-stem in fase di pianificazione (fail-closed): uno
        // stem vuoto o contenente un separatore di percorso produrrebbe un
        // percorso fuori da `symbols/` o una sotto-directory inesistente (la
        // causa storica del crash a metà commit su `operator/`). Con la
        // sanitizzazione di `symbol_file_stem` ciò non accade, ma validiamo
        // comunque così un caso patologico futuro fallisce **prima** di scrivere.
        if stem.is_empty() || stem.contains(['/', '\\']) || stem.contains(std::path::MAIN_SEPARATOR)
        {
            return Err(ExtractError::InvalidStem {
                symbol: entry.symbol.to_string(),
                reason: format!("stem non utilizzabile come nome di file: {stem:?}"),
            });
        }

        let path = symbols_dir.join(format!("{stem}.toml"));

        // Difesa anti-collisione: due simboli **distinti** che derivano lo stesso
        // file-stem scriverebbero lo stesso percorso (l'uno sovrascrivendo
        // l'altro al commit). Lo rileviamo qui, in pianificazione, e falliamo in
        // chiusura prima di qualunque scrittura.
        if let Some(prev) = planned_paths.insert(path.clone(), entry.symbol.clone()) {
            return Err(ExtractError::InvalidStem {
                symbol: entry.symbol.to_string(),
                reason: format!(
                    "collisione di file-stem con il simbolo {prev}: entrambi mappano su {}",
                    path.display()
                ),
            });
        }

        if path.exists() {
            // Voce esistente: carica, verifica firma e coppie (fail-closed).
            let existing = match load_catalog_entry(&path) {
                Ok(existing) => existing,
                Err(CatalogError::Io { path, source }) => {
                    return Err(ExtractError::Io { path, source })
                }
                Err(err) => {
                    // File esistente non analizzabile: non lo tocchiamo (Req 11.4).
                    return Err(ExtractError::Conflict {
                        symbol: entry.symbol.to_string(),
                        pair: "-".to_owned(),
                        reason: format!("voce esistente non analizzabile: {err}"),
                    });
                }
            };

            // Firma divergente ⇒ conflitto, voce invariata (Req 11.4).
            if existing.signature != entry.signature {
                return Err(ExtractError::Conflict {
                    symbol: entry.symbol.to_string(),
                    pair: "-".to_owned(),
                    reason: format!(
                        "firma divergente dalla voce esistente: {} vs {}",
                        signature_to_string(&existing.signature),
                        signature_to_string(&entry.signature)
                    ),
                });
            }

            // Determina quali offset sono nuove coppie da appendere.
            let mut to_append: Vec<&ExtractedOffset> = Vec::new();
            for offset in &offsets {
                let pair_exists = existing.offsets.iter().any(|o| o.pair == offset.pair);
                if pair_exists {
                    // Coppia già presente: un offset risolto è un conflitto reale
                    // (Req 11.4); un sentinel è scartato (il dato esistente vince,
                    // mai declassato — Req 6.4, 8.6).
                    if !offset.is_sentinel() {
                        return Err(ExtractError::Conflict {
                            symbol: entry.symbol.to_string(),
                            pair: offset.pair.to_string(),
                            reason: "offset risolto in conflitto con la voce esistente per la \
                                     stessa coppia: la voce esistente è preservata invariata"
                                .to_owned(),
                        });
                    }
                    // sentinel su coppia esistente ⇒ scartato.
                } else {
                    to_append.push(offset);
                }
            }

            if to_append.is_empty() {
                // Nessuna coppia nuova: il file resta invariato byte-per-byte.
                continue;
            }

            // Contenuto esistente preservato byte-per-byte + nuovi blocchi (Req 8.5).
            let existing_content =
                std::fs::read_to_string(&path).map_err(|source| ExtractError::Io {
                    path: path.clone(),
                    source,
                })?;
            let mut content = existing_content;
            if !content.ends_with('\n') {
                content.push('\n');
            }
            for offset in &to_append {
                content.push('\n');
                content.push_str(&serialize_offset_block(offset));
                record_offset(&mut report, offset);
            }

            plans.push(FilePlan::Append { path, content });
        } else {
            // Nuovo file: serializzazione canonica completa, offset ordinati.
            let content = serialize_entry(&entry.symbol, &entry.signature, &offsets);
            for offset in &offsets {
                record_offset(&mut report, offset);
            }
            plans.push(FilePlan::Create { path, content });
        }
    }

    // ---- Fase 2: commit atomico (nessun conflitto rilevato) ------------------
    if !plans.is_empty() {
        std::fs::create_dir_all(&symbols_dir).map_err(|source| ExtractError::Io {
            path: symbols_dir.clone(),
            source,
        })?;
        for plan in &plans {
            atomic_write(plan.path(), plan.content().as_bytes())?;
        }
    }

    Ok(report)
}

/// Registra nel report un offset **risolto** (con tier) effettivamente scritto;
/// i sentinel (privi di provenienza/tier) non sono contati (Req 6.6, 7.3).
fn record_offset(report: &mut ExtractReport, offset: &ExtractedOffset) {
    if offset.is_sentinel() {
        return;
    }
    if let Some(prov) = &offset.provenance {
        report.record(prov.tier);
    }
}

// ---------------------------------------------------------------------------
// Serializzazione TOML (formato esistente del Binding_Catalog).
// ---------------------------------------------------------------------------

/// Serializza una `Catalog_Entry` completa nel formato TOML esistente
/// (`schema_version`/`symbol`/`[signature]`/`[[offset]]`), con gli `offsets`
/// **già ordinati per coppia** (Req 9.2, 11.1, 11.2).
fn serialize_entry(symbol: &SymbolId, signature: &Signature, offsets: &[&ExtractedOffset]) -> String {
    let mut out = String::new();
    out.push_str("schema_version = 1\n");
    out.push_str(&format!("symbol = {}\n", toml_string(symbol.as_str())));
    out.push('\n');
    out.push_str("[signature]\n");
    out.push_str(&format!("return = {}\n", toml_string(&signature.return_type)));
    out.push_str(&format!(
        "params = {}\n",
        toml_string_array(&signature.param_types)
    ));
    for offset in offsets {
        out.push('\n');
        out.push_str(&serialize_offset_block(offset));
    }
    out
}

/// Serializza un singolo blocco `[[offset]]` (più l'eventuale
/// `[offset.provenance]` additivo). Il `rva` è emesso **solo** per un offset
/// risolto (`0x` esadecimale maiuscolo, mirror del generator); per il sentinel
/// la riga `rva` è **omessa** — `load_catalog` la interpreta come sentinel
/// logico con `verified = false` (Req 11.3).
fn serialize_offset_block(offset: &ExtractedOffset) -> String {
    let mut out = String::new();
    out.push_str("[[offset]]\n");
    out.push_str(&format!(
        "gd_version = {}\n",
        toml_string(&offset.pair.gd.to_string())
    ));
    out.push_str(&format!(
        "platform = {}\n",
        toml_string(offset.pair.platform.platform_id())
    ));
    // RVA solo se risolto (non sentinel): 0x esadecimale maiuscolo senza padding.
    if let Some(rva) = offset.rva {
        if rva != SENTINEL_VALUE {
            out.push_str(&format!("rva = 0x{rva:X}\n"));
        }
    }
    out.push_str(&format!("verified = {}\n", offset.emit_verified()));

    if let Some(prov) = &offset.provenance {
        out.push('\n');
        out.push_str(&serialize_provenance(prov));
    }
    out
}

/// Serializza la sezione `[offset.provenance]` **additiva** dalla
/// [`ExtractionProvenance`] della Phase F, riconciliata con lo schema core
/// esistente (Req 6.2):
///
/// - `tier` / `derivation_method` / `binary_identity` — campi **additivi** (dati
///   opachi per `load_catalog`, che li conserva);
/// - `prologue_method` = `"auto-sanity"` — distinto dal `"otool-manual"` del seed
///   (Req 5.6);
/// - `prologue_outcome` — campo core (Req 5.5);
/// - `cross_check` — la `Geode_Concordance`, mappata sul campo core `cross_check`
///   (stesso oracolo numerico del seed, Req 6.3); emesso solo se eseguito;
/// - `cross_check_no_reuse` = `true` — invariante condiviso (Req 1.7).
fn serialize_provenance(prov: &ExtractionProvenance) -> String {
    let mut out = String::new();
    out.push_str("[offset.provenance]\n");
    out.push_str(&format!("tier = {}\n", toml_string(prov.tier.as_str())));
    out.push_str(&format!(
        "derivation_method = {}\n",
        toml_string(&prov.derivation_method)
    ));
    out.push_str(&format!(
        "binary_identity = {}\n",
        toml_string_array(&prov.identity_labels())
    ));
    out.push_str(&format!(
        "prologue_method = {}\n",
        toml_string(prov.prologue_method_label())
    ));
    if let Some(label) = prov.prologue_outcome_label() {
        out.push_str(&format!("prologue_outcome = {}\n", toml_string(label)));
    }
    if let Some(cross_check) = prov.geode_concordance_label() {
        out.push_str(&format!("cross_check = {}\n", toml_string(cross_check)));
    }
    out.push_str(&format!("cross_check_no_reuse = {}\n", prov.geode_no_reuse));
    out
}

/// Formatta una [`Signature`] leggibile per i messaggi d'errore di conflitto.
fn signature_to_string(signature: &Signature) -> String {
    format!(
        "{}({})",
        signature.return_type,
        signature.param_types.join(", ")
    )
}

/// Rende `s` come stringa TOML basic (`"…"`) con l'escaping minimo necessario.
/// Identificatori, firme (`MenuLayer*`) ed etichette non contengono normalmente
/// virgolette o backslash, ma l'escaping garantisce un output sempre valido.
fn toml_string(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            _ => out.push(c),
        }
    }
    out.push('"');
    out
}

/// Rende una lista di stringhe come array TOML inline (`["a", "b"]`); lista
/// vuota ⇒ `[]`.
fn toml_string_array(items: &[String]) -> String {
    let inner: Vec<String> = items.iter().map(|s| toml_string(s)).collect();
    format!("[{}]", inner.join(", "))
}

// ---------------------------------------------------------------------------
// Scrittura atomica (mirror di bindings/generator.rs::atomic_write).
// ---------------------------------------------------------------------------

/// Scrive `content` in `path` in modo **atomico**: contenuto in un file
/// temporaneo nella **stessa directory** + [`std::fs::rename`]. Su fallimento il
/// file precedente resta intatto byte-per-byte e il temporaneo (eventualmente
/// parziale) è rimosso (nessun residuo). `path` deve avere una directory
/// genitore già esistente (creata dal chiamante).
fn atomic_write(path: &Path, content: &[u8]) -> Result<(), ExtractError> {
    use std::sync::atomic::{AtomicU64, Ordering};

    /// Contatore monotòno di processo per nomi di temporaneo univoci.
    static TMP_COUNTER: AtomicU64 = AtomicU64::new(0);

    let io_err = |source| ExtractError::Io {
        path: path.to_path_buf(),
        source,
    };

    let parent = path.parent().unwrap_or_else(|| Path::new("."));
    let file_name = path
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or("out.toml");
    let unique = TMP_COUNTER.fetch_add(1, Ordering::Relaxed);
    let tmp_path = parent.join(format!(".{file_name}.{}.{unique}.tmp", std::process::id()));

    if let Err(source) = std::fs::write(&tmp_path, content) {
        let _ = std::fs::remove_file(&tmp_path);
        return Err(io_err(source));
    }
    if let Err(source) = std::fs::rename(&tmp_path, path) {
        let _ = std::fs::remove_file(&tmp_path);
        return Err(io_err(source));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bindings::catalog::load_catalog_entry;
    use crate::bindings::{GdVersion, TargetPlatform};
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicU32, Ordering};

    /// Directory temporanea auto-pulente (nessuna dipendenza da `tempfile`).
    struct TempDir {
        root: PathBuf,
    }

    impl TempDir {
        fn new() -> Self {
            static COUNTER: AtomicU32 = AtomicU32::new(0);
            let unique = format!(
                "pulse-writer-test-{}-{}",
                std::process::id(),
                COUNTER.fetch_add(1, Ordering::Relaxed)
            );
            let root = std::env::temp_dir().join(unique);
            std::fs::create_dir_all(&root).unwrap();
            Self { root }
        }
    }

    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.root);
        }
    }

    fn arm64_pair() -> TargetPair {
        TargetPair::new(GdVersion::new(2, 2081), TargetPlatform::AndroidArm64)
    }

    fn sentinel_entry(symbol: &str) -> ExtractedEntry {
        ExtractedEntry {
            symbol: SymbolId::new(symbol),
            signature: Signature::new("void", vec!["CCPoint*".to_owned(), "float".to_owned()]),
            offsets: vec![ExtractedOffset {
                pair: arm64_pair(),
                rva: Some(0x1234),
                verified: false,
                provenance: None,
            }],
        }
    }

    #[test]
    fn writes_operator_overload_to_safe_path_without_subdir() {
        // Regressione del crash a metà commit: un overload `operator/` deve
        // scrivere un singolo file sotto `symbols/` senza creare sotto-directory.
        let tmp = TempDir::new();
        let entries = vec![sentinel_entry("cocos2d::CCPoint::operator/")];
        write_catalog_entries(&entries, &tmp.root).unwrap();

        let path = tmp
            .root
            .join("symbols")
            .join("cocos2d__CCPoint__operator_x2F_.toml");
        assert!(path.exists(), "il file dell'overload deve esistere: {path:?}");

        // Il `symbol` canonico è intatto nel TOML (l'identità logica non cambia).
        let entry = load_catalog_entry(&path).unwrap();
        assert_eq!(entry.symbol, SymbolId::new("cocos2d::CCPoint::operator/"));
    }

    #[test]
    fn fail_closed_on_stem_collision_before_writing() {
        // Due simboli DISTINTI che mappano sullo stesso file-stem devono essere
        // rilevati in pianificazione e far fallire in chiusura **senza** scrivere
        // alcun file. Sfruttiamo l'unico aliasing residuo del comportamento
        // legacy: `"A::B"` → `A__B` e il letterale `"A__B"` → `A__B`.
        let tmp = TempDir::new();
        let symbols_dir = tmp.root.join("symbols");
        let entries = vec![sentinel_entry("A::B"), sentinel_entry("A__B")];

        let err = write_catalog_entries(&entries, &tmp.root).unwrap_err();
        assert!(
            matches!(err, ExtractError::InvalidStem { .. }),
            "atteso InvalidStem per collisione di stem, trovato: {err:?}"
        );
        // Fail-closed: nessun file scritto (la directory non è nemmeno creata).
        assert!(
            !symbols_dir.exists() || std::fs::read_dir(&symbols_dir).unwrap().next().is_none(),
            "nessun file deve essere scritto in caso di collisione"
        );
    }

    #[test]
    fn distinct_operator_overloads_write_without_collision() {
        // Operatori distinti producono stem distinti → entrambi scritti.
        let tmp = TempDir::new();
        let symbols_dir = tmp.root.join("symbols");
        let entries = vec![
            sentinel_entry("A::operator/"),
            sentinel_entry("A::operator%"),
        ];
        write_catalog_entries(&entries, &tmp.root).unwrap();
        assert!(symbols_dir.join("A__operator_x2F_.toml").exists());
        assert!(symbols_dir.join("A__operator_x25_.toml").exists());
    }
}
