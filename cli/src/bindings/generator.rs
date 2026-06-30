//! `Binding_Generator` — pivot catalogo→coppia ed emissione canonica `Pbind_Format`.
//!
//! Il catalogo è **denormalizzato per simbolo** (un file = un simbolo, tutte le
//! sue coppie). La generazione lo **trasforma per coppia**: per ogni
//! `(GD_Version, Target_Platform)` presente nel catalogo raccoglie da tutte le
//! [`CatalogEntry`] l'[`OffsetRecord`] di quella coppia, **ordina i blocchi
//! `[function]` per `symbol` crescente e univoco**, ed emette **esattamente un**
//! `.pbind` per coppia a `mod-index/bindings/{version}/{platform}.pbind`,
//! sovrascrivendo integralmente l'eventuale file preesistente.
//!
//! ```text
//! catalog/symbols/*.toml   (N simboli × M coppie)
//!         │  pivot per coppia + sort per simbolo
//!         ▼
//! mod-index/bindings/{version}/{platform}.pbind   (M file, ognuno con ≤ N funzioni)
//! ```
//!
//! Il formato emesso **rispecchia byte-per-byte** la serializzazione canonica di
//! `loader/bindings/pbind_format.cpp` (`serialize_pbind`), così che il parser
//! C++ faccia round-trip esatto:
//!
//! - intestazione in ordine fisso `pbind_version` / `gd_version` / `platform`,
//!   con una sola spaziatura attorno a `=` (nessun allineamento);
//! - una riga vuota prima di ogni blocco `[function]`;
//! - campi del blocco in ordine fisso `symbol` / `offset` / `return` / `params`
//!   / `verified`;
//! - `offset` come `0x` + esadecimale **maiuscolo senza zero-padding**
//!   (es. `0x316688`), coerente con `std::hex << std::uppercase`;
//! - `params` come lista separata da `", "` (virgola + spazio);
//! - `verified` reso come `true` / `false`.
//!
//! I valori interni `gd_version` / `platform` **coincidono sempre** con la coppia
//! derivata dal percorso (concordanza header↔coppia, Req 9.2/9.4).
//!
//! **Ambito di questo task (9.1/9.2/9.3/9.4):** pivot + emissione canonica +
//! ordinamento per simbolo + un-file-per-coppia + sovrascrittura integrale +
//! concordanza header (Req 3.1, 3.2, 3.4, 8.3), **emissione `verified`
//! fail-closed completa** (Req 3.3, 11.1, 11.2, 11.4), **scrittura atomica con
//! preservazione su fallimento** (Req 3.6) e **assenza di propagazione
//! cross-coppia** (Req 8.4, 8.6). Il campo `verified` emesso è
//! disciplinato da [`emit_verified`]: vale `true` **solo** per un offset
//! presente, non-zero, diverso dal [`SENTINEL_VALUE`] e con `Verified_Flag` del
//! catalogo vero; in **ogni** altro caso (offset assente, sentinel, zero, o
//! `Verified_Flag = false`) vale `false` indipendentemente da qualunque flag
//! dichiarato. Il **valore** dell'offset emesso resta `effective_rva()`
//! (sentinel per l'assenza).
//!
//! **Scrittura atomica (Req 3.6):** ogni `.pbind` è scritto prima in un file
//! temporaneo nella **stessa directory** della destinazione (così il `rename`
//! resta sullo stesso filesystem ed è atomico), poi promosso con
//! [`std::fs::rename`]. Se la serializzazione, la scrittura del temporaneo o il
//! `rename` falliscono per una coppia, il `.pbind` **precedente** di quella
//! coppia resta intatto **byte-per-byte**, il file temporaneo viene rimosso
//! (nessun file parziale lasciato a terra) e l'errore identifica **coppia +
//! causa** ([`GenError::Io`]). Vedi [`atomic_write`].
//!
//! **Catalogo vuoto (Req 3.7):** [`generate`] riceve un [`BindingCatalog`] già
//! analizzato; il rifiuto di una sorgente **illeggibile/malformata/vuota** è
//! responsabilità del layer a monte `load_catalog` (`catalog.rs`), che fallisce
//! in chiusura con la causa senza produrre alcun catalogo. Coerentemente con
//! questo confine, qui un catalogo **privo di coppie** è fail-closed *per
//! costruzione*: nessuna coppia ⇒ **nessun output emesso** e `GenReport` vuoto,
//! senza creare alcuna directory `bindings/`.
//!
//! La non-propagazione cross-coppia (9.4) è garantita dalla selezione mirata
//! dell'`OffsetRecord` di ciascuna coppia ed è segnalata in [`GenReport::unresolved`].
//!
//! _Requisiti: 3.1, 3.2, 3.3, 3.4, 3.6, 3.7, 8.3, 8.4, 8.6, 11.1, 11.2, 11.4._

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

use super::catalog::{BindingCatalog, OffsetRecord};
use super::{Signature, SymbolId, TargetPair, SENTINEL_VALUE};

/// Versione del formato `.pbind` emessa, allineata a `kPbindFormatVersion = 1`
/// di `loader/bindings/pbind_format.hpp`.
pub const PBIND_FORMAT_VERSION: u32 = 1;

/// Esito di una generazione riuscita: l'elenco — **ordinato e deterministico** —
/// dei percorsi `.pbind` scritti, uno per coppia `(GD_Version, Target_Platform)`,
/// più l'**indicazione delle voci non risolte** emesse fail-closed (Req 8.6).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct GenReport {
    /// Percorsi dei `Binding_Set_File` scritti, in ordine per coppia.
    pub written: Vec<PathBuf>,

    /// **Segnale della condizione di non-risoluzione (Req 8.6).** Elenca, per
    /// coppia e simbolo, ogni voce `[function]` emessa con `verified = false`
    /// perché il suo `OffsetRecord` — quello **di quella stessa coppia** — è
    /// assente, vale il [`SENTINEL_VALUE`], è zero, oppure ha `Verified_Flag`
    /// falso. Poiché l'emissione resta byte-identica (il formato `.pbind` non
    /// cambia), questa lista è il canale con cui il generatore **rende
    /// osservabile** la condizione richiesta dal Req 8.6 senza mai derivare né
    /// sostituire l'offset da un'altra coppia.
    ///
    /// L'ordine è **deterministico**: le coppie compaiono in ordine crescente e,
    /// all'interno di ciascuna, i simboli in ordine crescente (lo stesso dei
    /// blocchi `[function]` emessi).
    pub unresolved: Vec<(TargetPair, SymbolId)>,
}

/// Errore di generazione dei `Binding_Set_File`.
///
/// Coerente con la disciplina fail-closed della pipeline: ogni variante
/// identifica la coppia interessata e la causa (Req 3.6).
#[derive(Debug, thiserror::Error)]
pub enum GenError {
    /// La scrittura su disco di un `.pbind` (o la creazione delle directory
    /// intermedie) è fallita per la coppia indicata.
    #[error("scrittura del .pbind fallita per la coppia {pair} ({path}): {source}")]
    Io {
        /// Coppia `(GD_Version, Target_Platform)` interessata.
        pair: TargetPair,
        /// Percorso del file che si stava scrivendo.
        path: PathBuf,
        /// Causa di I/O sottostante.
        #[source]
        source: std::io::Error,
    },
}

/// Una funzione pronta per l'emissione in un `.pbind`: il riferimento al simbolo,
/// alla firma unica della voce e all'[`OffsetRecord`] della coppia corrente.
struct PivotFunction<'a> {
    symbol: &'a SymbolId,
    signature: &'a Signature,
    offset: &'a OffsetRecord,
}

/// Emette esattamente un `Binding_Set_File` per ogni coppia `(GD_Version,
/// Target_Platform)` presente nel catalogo, al percorso canonico
/// `out_root/bindings/{version}/{platform}.pbind`, sovrascrivendo integralmente
/// qualsiasi file preesistente (Req 3.1, 8.3).
///
/// `out_root` è la radice `mod-index/`. Per ogni coppia il generatore raccoglie
/// da tutte le [`CatalogEntry`] l'offset di quella coppia (pivot), ordina i
/// blocchi `[function]` per `symbol` crescente e univoco (Req 3.4) ed emette il
/// formato canonico `Pbind_Format` (Req 3.2). I valori interni
/// `gd_version` / `platform` coincidono sempre con la coppia del percorso.
///
/// Il [`GenReport`] restituito elenca i percorsi scritti in ordine deterministico
/// per coppia, e in [`GenReport::unresolved`] le voci emesse `verified = false`
/// (segnale del Req 8.6, vedi sotto).
///
/// **Nessuna propagazione cross-coppia (Req 8.4, 8.6):** per ogni coppia il
/// generatore seleziona, da ciascuna [`CatalogEntry`], **solo** l'`OffsetRecord`
/// la cui `pair` coincide con quella corrente (`o.pair == pair`). L'offset e il
/// `Verified_Flag` emessi in un `.pbind` provengono perciò **esclusivamente**
/// dall'`OffsetRecord` di quella coppia: un offset verificato su una coppia non è
/// mai derivato né sostituito nell'offset di un'altra. Se una voce non possiede
/// un offset per la coppia corrente, non contribuisce l'offset di un'altra coppia
/// ma resta `verified = false` (sentinel), lasciando invariate le altre voci. La
/// condizione di non-risoluzione è **segnalata** in [`GenReport::unresolved`]
/// senza modificare il `.pbind` emesso.
///
/// **Scrittura atomica (Req 3.6):** ogni `.pbind` è prodotto in memoria, scritto
/// in un file temporaneo nella stessa directory e promosso con un `rename`
/// atomico (vedi [`atomic_write`]). Se la scrittura di una coppia fallisce, il
/// `.pbind` precedente di quella coppia resta intatto byte-per-byte, nessun file
/// parziale viene lasciato e l'errore identifica coppia + causa; la generazione
/// si interrompe restituendo l'errore.
pub fn generate(catalog: &BindingCatalog, out_root: &Path) -> Result<GenReport, GenError> {
    // Raccoglie l'insieme deterministico (ordinato) delle coppie presenti nel
    // catalogo, attraversando tutti gli offset di tutte le voci. Un catalogo
    // privo di coppie non emette alcun file (fail-closed per costruzione, Req 3.7).
    let mut pairs: BTreeSet<TargetPair> = BTreeSet::new();
    for entry in &catalog.entries {
        for offset in &entry.offsets {
            pairs.insert(offset.pair);
        }
    }

    let mut written = Vec::with_capacity(pairs.len());
    let mut unresolved: Vec<(TargetPair, SymbolId)> = Vec::new();

    // Pivot per coppia + sort per simbolo + emissione di un solo file per coppia.
    for pair in pairs {
        // Per la coppia corrente, raccoglie da ogni voce **esclusivamente**
        // l'offset di quella stessa coppia (`o.pair == pair`). Questo `find`
        // mirato è ciò che garantisce, **per costruzione**, l'assenza di
        // propagazione cross-coppia (Req 8.4, 8.6): l'offset e il `Verified_Flag`
        // emessi nel `.pbind` di una coppia provengono SOLO dall'`OffsetRecord`
        // di quella coppia; l'offset di un'altra coppia non viene mai letto né
        // sostituito qui. Se la voce non ha un offset per `pair`, semplicemente
        // non contribuisce un `OffsetRecord` di un'altra coppia: resterà
        // `verified = false` tramite il sentinel (vedi sotto). Le voci del
        // catalogo hanno simboli univoci (aggregati da `load_catalog`), quindi i
        // `symbol` qui sono già univoci.
        let mut functions: Vec<PivotFunction> = Vec::new();
        for entry in &catalog.entries {
            let offset = entry.offsets.iter().find(|o| o.pair == pair);
            // Invariante di non-propagazione: l'offset selezionato, se presente,
            // appartiene **sempre** alla coppia corrente.
            debug_assert!(
                offset.map(|o| o.pair == pair).unwrap_or(true),
                "non-propagazione cross-coppia violata: offset di un'altra coppia"
            );
            if let Some(offset) = offset {
                functions.push(PivotFunction {
                    symbol: &entry.symbol,
                    signature: &entry.signature,
                    offset,
                });
            }
        }

        // Ordinamento canonico per `symbol` crescente (Req 3.4): garantisce
        // determinismo byte-per-byte tra esecuzioni.
        functions.sort_by(|a, b| a.symbol.cmp(b.symbol));

        // Segnala la condizione del Req 8.6: ogni voce emessa con
        // `verified = false` (offset assente per questa coppia → sentinel, oppure
        // zero/sentinel/`Verified_Flag` falso) viene registrata in `unresolved`
        // SENZA alterare il `.pbind` emesso. L'ordine segue i blocchi già
        // ordinati per simbolo, quindi resta deterministico.
        for f in &functions {
            if !emit_verified(f.offset) {
                unresolved.push((pair, f.symbol.clone()));
            }
        }

        let content = serialize_set(pair, &functions);
        let path = pbind_path(out_root, pair);

        // Crea le directory intermedie: serve sia al file temporaneo sia alla
        // destinazione finale. Un fallimento qui non tocca alcun file preesistente.
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent).map_err(|source| GenError::Io {
                pair,
                path: path.clone(),
                source,
            })?;
        }

        // Scrittura atomica: temp nella stessa dir + rename. Su fallimento il
        // `.pbind` precedente resta intatto e nessun file parziale è lasciato
        // (Req 3.6). L'errore interrompe la generazione propagando coppia+causa.
        atomic_write(&path, content.as_bytes(), pair)?;

        written.push(path);
    }

    Ok(GenReport { written, unresolved })
}

/// Scrive `content` in `path` in modo **atomico** rispetto al filesystem: il
/// contenuto è prima riversato in un file temporaneo nella **stessa directory**
/// della destinazione, poi promosso con [`std::fs::rename`]. Sullo stesso
/// filesystem il `rename` è atomico e sostituisce integralmente l'eventuale file
/// preesistente: un consumatore osserva sempre **o** il vecchio `.pbind`
/// completo **o** il nuovo, mai un file parziale.
///
/// Disciplina fail-closed in caso di errore (Req 3.6):
///
/// - se la scrittura del file temporaneo fallisce, il temporaneo (eventualmente
///   parziale) viene rimosso e l'errore propaga coppia + causa; la destinazione
///   non viene mai toccata, quindi il `.pbind` precedente resta intatto
///   byte-per-byte;
/// - se il `rename` fallisce, il file temporaneo viene rimosso (nessun residuo)
///   e l'errore propaga coppia + causa; anche qui la destinazione resta intatta.
///
/// Il nome del file temporaneo è reso univoco combinando il nome del file di
/// destinazione, il PID e un contatore monotòno di processo, così da evitare
/// collisioni tra coppie diverse e tra esecuzioni concorrenti.
///
/// `path` deve avere una directory genitore già esistente (creata dal chiamante).
fn atomic_write(path: &Path, content: &[u8], pair: TargetPair) -> Result<(), GenError> {
    use std::sync::atomic::{AtomicU64, Ordering};

    /// Contatore monotòno di processo per nomi di file temporaneo univoci.
    static TMP_COUNTER: AtomicU64 = AtomicU64::new(0);

    let io_err = |source| GenError::Io {
        pair,
        path: path.to_path_buf(),
        source,
    };

    // Directory di destinazione: il temporaneo vi risiede affinché il `rename`
    // resti sullo stesso filesystem (condizione di atomicità).
    let parent = path.parent().unwrap_or_else(|| Path::new("."));
    let file_name = path
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or("out.pbind");
    let unique = TMP_COUNTER.fetch_add(1, Ordering::Relaxed);
    let tmp_path = parent.join(format!(".{file_name}.{}.{unique}.tmp", std::process::id()));

    // 1) Scrive l'intero contenuto nel file temporaneo.
    if let Err(source) = std::fs::write(&tmp_path, content) {
        // Rimuove un eventuale temporaneo parziale; ignora l'esito (best-effort).
        let _ = std::fs::remove_file(&tmp_path);
        return Err(io_err(source));
    }

    // 2) Promuove atomicamente il temporaneo a destinazione finale.
    if let Err(source) = std::fs::rename(&tmp_path, path) {
        // La destinazione resta quella precedente; rimuove il temporaneo.
        let _ = std::fs::remove_file(&tmp_path);
        return Err(io_err(source));
    }

    Ok(())
}

/// Costruisce il percorso canonico del `.pbind` per una coppia:
/// `out_root/bindings/{version}/{platform}.pbind`.
fn pbind_path(out_root: &Path, pair: TargetPair) -> PathBuf {
    out_root
        .join("bindings")
        .join(pair.gd.to_string())
        .join(format!("{}.pbind", pair.platform.platform_id()))
}

/// Serializza un `Binding_Set_File` nel formato canonico `Pbind_Format`,
/// rispecchiando byte-per-byte `serialize_pbind` di
/// `loader/bindings/pbind_format.cpp`.
///
/// `gd_version` / `platform` emessi provengono **sempre** da `pair` (concordanza
/// header↔coppia). Le `functions` sono attese già ordinate per `symbol`.
fn serialize_set(pair: TargetPair, functions: &[PivotFunction]) -> String {
    let mut out = String::new();

    // Intestazione in ordine fisso (determinismo), una spaziatura attorno a `=`.
    out.push_str(&format!("pbind_version = {PBIND_FORMAT_VERSION}\n"));
    // `GdVersion` si stampa come "<major>.<minor>" (es. "2.2081").
    out.push_str(&format!("gd_version = {}\n", pair.gd));
    out.push_str(&format!("platform = {}\n", pair.platform.platform_id()));

    for f in functions {
        // Riga vuota prima di ogni blocco [function].
        out.push('\n');
        out.push_str("[function]\n");
        out.push_str(&format!("symbol = {}\n", f.symbol));

        // offset = 0x + esadecimale MAIUSCOLO senza zero-padding (mirror di
        // `std::hex << std::uppercase`). Un offset assente/sentinel emette il
        // SENTINEL_VALUE (effective_rva) — raffinato in 9.2.
        out.push_str(&format!("offset = 0x{:X}\n", f.offset.effective_rva()));

        out.push_str(&format!("return = {}\n", f.signature.return_type));
        // params come lista separata da ", " (round-trip con il parser C++).
        out.push_str(&format!("params = {}\n", f.signature.param_types.join(", ")));

        // Emissione fail-closed del campo `verified` (Req 3.3, 11.1, 11.2, 11.4):
        // `true` solo per offset presente, non-zero, != sentinel e con
        // `Verified_Flag` vero; in ogni altro caso `false`. Vedi `emit_verified`.
        out.push_str(&format!(
            "verified = {}\n",
            if emit_verified(f.offset) { "true" } else { "false" }
        ));
    }

    out
}

/// Decide il valore del campo `verified` da emettere per un offset, in modo
/// **fail-closed** (Req 3.3, 11.1, 11.2, 11.4).
///
/// Rispecchia la semantica di `address_is_verified` di
/// `loader/bindings/binding_verifier`: un offset è verificato **se e solo se**
/// — simultaneamente — è **presente** (`rva` non `None`), è **diverso da zero**
/// (uno zero non è mai risolvibile), è **diverso dal** [`SENTINEL_VALUE`], e il
/// `Verified_Flag` del catalogo è `true`.
///
/// In **ogni** altro caso emette `false`, **indipendentemente** da qualunque
/// flag dichiarato:
///
/// - offset assente (`rva == None`) ⇒ `false` (Req 11.4);
/// - offset pari al [`SENTINEL_VALUE`] ⇒ `false` (Req 11.2, 11.4);
/// - offset pari a zero ⇒ `false` (placeholder non risolvibile, Req 11.4);
/// - `Verified_Flag` del catalogo `false` ⇒ `false` (Req 11.1).
fn emit_verified(offset: &OffsetRecord) -> bool {
    match offset.rva {
        Some(rva) => offset.verified && rva != 0 && rva != SENTINEL_VALUE,
        None => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bindings::catalog::{BindingCatalog, CatalogEntry, OffsetRecord, ProvenanceRecord};
    use crate::bindings::{GdVersion, Signature, SymbolId, TargetPair, TargetPlatform};
    use std::sync::atomic::{AtomicU32, Ordering};

    /// Directory temporanea auto-pulente (non dipende da `tempfile`): crea una
    /// directory unica sotto `std::env::temp_dir()` e la rimuove al `Drop`.
    struct TempDir {
        root: PathBuf,
    }

    impl TempDir {
        fn new() -> Self {
            static COUNTER: AtomicU32 = AtomicU32::new(0);
            let unique = format!(
                "pulse-generator-test-{}-{}",
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

    fn pair(gd: (u32, u32), platform: TargetPlatform) -> TargetPair {
        TargetPair::new(GdVersion::new(gd.0, gd.1), platform)
    }

    fn offset(pair: TargetPair, rva: Option<u64>, verified: bool) -> OffsetRecord {
        OffsetRecord {
            pair,
            rva,
            verified,
            provenance: ProvenanceRecord::empty(SymbolId::new("ignored"), pair),
        }
    }

    fn entry(symbol: &str, sig: Signature, offsets: Vec<OffsetRecord>) -> CatalogEntry {
        CatalogEntry {
            symbol: SymbolId::new(symbol),
            signature: sig,
            offsets,
        }
    }

    /// Catalogo del Prioritized_Target: `MenuLayer::init` verificato su
    /// `(2.2081, macos-arm64)` e privo di RVA su `(2.2081, macos-x64)`; più un
    /// `PlayLayer::update` su `macos-arm64`. Utile a coprire ordinamento,
    /// pivot multi-coppia e fail-closed sul sentinel.
    fn sample_catalog() -> BindingCatalog {
        let arm = pair((2, 2081), TargetPlatform::MacosArm64);
        let x64 = pair((2, 2081), TargetPlatform::MacosX64);

        BindingCatalog {
            entries: vec![
                // Volutamente NON in ordine alfabetico, per esercitare il sort.
                entry(
                    "PlayLayer::update",
                    Signature::new("void", vec!["PlayLayer*".into(), "float".into()]),
                    vec![offset(arm, Some(0x4000), false)],
                ),
                entry(
                    "MenuLayer::init",
                    Signature::new("bool", vec!["MenuLayer*".into()]),
                    vec![
                        offset(arm, Some(0x316688), true),
                        offset(x64, None, false),
                    ],
                ),
            ],
        }
    }

    #[test]
    fn emits_one_file_per_pair_at_canonical_path() {
        let tmp = TempDir::new();
        let report = generate(&sample_catalog(), &tmp.root).unwrap();

        let arm_path = tmp
            .root
            .join("bindings")
            .join("2.2081")
            .join("macos-arm64.pbind");
        let x64_path = tmp
            .root
            .join("bindings")
            .join("2.2081")
            .join("macos-x64.pbind");

        // Esattamente due coppie ⇒ due file, ai percorsi canonici.
        assert_eq!(report.written.len(), 2);
        assert!(report.written.contains(&arm_path));
        assert!(report.written.contains(&x64_path));
        assert!(arm_path.is_file());
        assert!(x64_path.is_file());
    }

    #[test]
    fn functions_are_sorted_by_symbol_ascending() {
        let tmp = TempDir::new();
        generate(&sample_catalog(), &tmp.root).unwrap();

        let content = std::fs::read_to_string(
            tmp.root
                .join("bindings")
                .join("2.2081")
                .join("macos-arm64.pbind"),
        )
        .unwrap();

        let menu = content.find("symbol = MenuLayer::init").unwrap();
        let play = content.find("symbol = PlayLayer::update").unwrap();
        // "MenuLayer::init" < "PlayLayer::update" ⇒ deve precederlo.
        assert!(menu < play, "i blocchi devono essere ordinati per symbol");
    }

    #[test]
    fn header_gd_version_and_platform_match_the_path() {
        let tmp = TempDir::new();
        generate(&sample_catalog(), &tmp.root).unwrap();

        let arm = std::fs::read_to_string(
            tmp.root
                .join("bindings")
                .join("2.2081")
                .join("macos-arm64.pbind"),
        )
        .unwrap();
        assert!(arm.contains("gd_version = 2.2081\n"));
        assert!(arm.contains("platform = macos-arm64\n"));

        let x64 = std::fs::read_to_string(
            tmp.root
                .join("bindings")
                .join("2.2081")
                .join("macos-x64.pbind"),
        )
        .unwrap();
        assert!(x64.contains("gd_version = 2.2081\n"));
        assert!(x64.contains("platform = macos-x64\n"));
    }

    #[test]
    fn output_mirrors_canonical_pbind_format_byte_for_byte() {
        let tmp = TempDir::new();
        generate(&sample_catalog(), &tmp.root).unwrap();

        let arm = std::fs::read_to_string(
            tmp.root
                .join("bindings")
                .join("2.2081")
                .join("macos-arm64.pbind"),
        )
        .unwrap();

        // Mirror esatto di `serialize_pbind` (header + blocchi ordinati,
        // offset 0x maiuscolo senza padding, params separati da ", ").
        let expected = "\
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
        assert_eq!(arm, expected);
    }

    #[test]
    fn sentinel_offset_emits_verified_false_and_sentinel_value() {
        let tmp = TempDir::new();
        generate(&sample_catalog(), &tmp.root).unwrap();

        let x64 = std::fs::read_to_string(
            tmp.root
                .join("bindings")
                .join("2.2081")
                .join("macos-x64.pbind"),
        )
        .unwrap();

        // RVA assente ⇒ SENTINEL_VALUE (u64::MAX) in hex + verified = false.
        assert!(x64.contains("offset = 0xFFFFFFFFFFFFFFFF\n"));
        assert!(x64.contains("verified = false\n"));
    }

    #[test]
    fn two_runs_produce_byte_identical_files() {
        let catalog = sample_catalog();

        let tmp_a = TempDir::new();
        let tmp_b = TempDir::new();
        generate(&catalog, &tmp_a.root).unwrap();
        generate(&catalog, &tmp_b.root).unwrap();

        for platform in ["macos-arm64", "macos-x64"] {
            let a = std::fs::read(
                tmp_a
                    .root
                    .join("bindings")
                    .join("2.2081")
                    .join(format!("{platform}.pbind")),
            )
            .unwrap();
            let b = std::fs::read(
                tmp_b
                    .root
                    .join("bindings")
                    .join("2.2081")
                    .join(format!("{platform}.pbind")),
            )
            .unwrap();
            assert_eq!(a, b, "determinismo violato per {platform}");
        }
    }

    #[test]
    fn overwrites_a_preexisting_file_entirely() {
        let tmp = TempDir::new();
        let arm_path = tmp
            .root
            .join("bindings")
            .join("2.2081")
            .join("macos-arm64.pbind");

        // File preesistente con contenuto arbitrario più lungo del nuovo.
        std::fs::create_dir_all(arm_path.parent().unwrap()).unwrap();
        std::fs::write(&arm_path, "STALE CONTENT che deve sparire del tutto\n").unwrap();

        generate(&sample_catalog(), &tmp.root).unwrap();

        let content = std::fs::read_to_string(&arm_path).unwrap();
        assert!(!content.contains("STALE"), "il file va sovrascritto integralmente");
        assert!(content.starts_with("pbind_version = 1\n"));
    }

    #[test]
    fn empty_catalog_writes_no_files() {
        let tmp = TempDir::new();
        let report = generate(&BindingCatalog::new(), &tmp.root).unwrap();
        assert!(report.written.is_empty());
        assert!(!tmp.root.join("bindings").exists());
    }

    // -----------------------------------------------------------------------
    // 9.3 — scrittura atomica e preservazione su fallimento (Req 3.6, 3.7).
    // -----------------------------------------------------------------------

    /// Raccoglie ricorsivamente i percorsi dei file la cui estensione è `.tmp`
    /// sotto `dir`. Serve a provare che la scrittura atomica non lascia residui.
    fn collect_tmp_files(dir: &Path) -> Vec<PathBuf> {
        let mut found = Vec::new();
        let entries = match std::fs::read_dir(dir) {
            Ok(e) => e,
            Err(_) => return found,
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                found.extend(collect_tmp_files(&path));
            } else if path.extension().and_then(|e| e.to_str()) == Some("tmp") {
                found.push(path);
            }
        }
        found
    }

    #[test]
    fn successful_generation_leaves_no_temp_files() {
        let tmp = TempDir::new();
        generate(&sample_catalog(), &tmp.root).unwrap();

        // La promozione atomica (rename del temporaneo) non deve lasciare alcun
        // `.tmp` a terra dopo una generazione riuscita.
        let leftovers = collect_tmp_files(&tmp.root);
        assert!(
            leftovers.is_empty(),
            "nessun file temporaneo deve restare dopo il rename atomico: {leftovers:?}"
        );
    }

    #[test]
    fn rerun_overwrites_atomically_without_temp_residue() {
        let tmp = TempDir::new();
        let arm_path = tmp
            .root
            .join("bindings")
            .join("2.2081")
            .join("macos-arm64.pbind");

        // Prima esecuzione: produce il set canonico.
        generate(&sample_catalog(), &tmp.root).unwrap();
        let first = std::fs::read(&arm_path).unwrap();

        // Seconda esecuzione sullo stesso out_root: il rename atomico sostituisce
        // integralmente il file preesistente, byte-identico (determinismo) e
        // senza residui temporanei.
        generate(&sample_catalog(), &tmp.root).unwrap();
        let second = std::fs::read(&arm_path).unwrap();

        assert_eq!(first, second, "la riesecuzione deve riprodurre lo stesso file");
        assert!(collect_tmp_files(&tmp.root).is_empty());
    }

    /// Su Unix simuliamo il fallimento di scrittura di una coppia rendendo la
    /// directory di destinazione di sola lettura **dopo** una prima generazione
    /// riuscita: la scrittura del file temporaneo della coppia fallisce, perciò il
    /// `.pbind` precedente deve restare intatto byte-per-byte e nessun file
    /// parziale/temporaneo deve essere lasciato (Req 3.6).
    #[cfg(unix)]
    #[test]
    fn write_failure_preserves_previous_file_byte_for_byte() {
        use std::os::unix::fs::PermissionsExt;

        let tmp = TempDir::new();
        let version_dir = tmp.root.join("bindings").join("2.2081");
        let arm_path = version_dir.join("macos-arm64.pbind");

        // 1) Prima generazione riuscita: cattura il contenuto "precedente".
        generate(&sample_catalog(), &tmp.root).unwrap();
        let previous = std::fs::read(&arm_path).unwrap();
        assert!(!previous.is_empty());

        // 2) Rende la directory di versione di sola lettura: creare il file
        //    temporaneo al suo interno fallirà (EACCES).
        let original = std::fs::metadata(&version_dir).unwrap().permissions();
        std::fs::set_permissions(&version_dir, std::fs::Permissions::from_mode(0o555)).unwrap();

        // 3) Seconda generazione: deve fallire con un errore che identifica la
        //    coppia (la prima coppia in ordine è macos-arm64).
        let result = generate(&sample_catalog(), &tmp.root);

        // Ripristina i permessi PRIMA delle asserzioni, così il cleanup del
        // TempDir (Drop) può rimuovere la directory anche se un assert fallisse.
        std::fs::set_permissions(&version_dir, original).unwrap();

        let err = result.expect_err("la scrittura in directory read-only deve fallire");
        match err {
            GenError::Io { pair, .. } => {
                assert_eq!(pair.platform, TargetPlatform::MacosArm64);
                assert_eq!(pair.gd, GdVersion::new(2, 2081));
            }
        }

        // 4) Il `.pbind` precedente deve essere preservato byte-per-byte.
        let after = std::fs::read(&arm_path).unwrap();
        assert_eq!(previous, after, "il file precedente va preservato byte-per-byte");

        // 5) Nessun file temporaneo/parziale lasciato a terra.
        let leftovers = collect_tmp_files(&tmp.root);
        assert!(
            leftovers.is_empty(),
            "nessun file temporaneo deve restare dopo un fallimento: {leftovers:?}"
        );
    }

    // -----------------------------------------------------------------------
    // 9.4 — assenza di propagazione cross-coppia + segnale Req 8.6 (Req 8.4, 8.6).
    // -----------------------------------------------------------------------

    /// Un simbolo verificato su `macos-arm64` ma **assente** su `macos-x64` non
    /// deve mai vedere il proprio offset propagato all'altra coppia: il `.pbind`
    /// di `x64` emette quel simbolo con `verified = false` e con l'offset sentinel
    /// — **non** l'RVA verificato di `arm64` (Req 8.4, 8.6).
    #[test]
    fn verified_offset_is_not_leaked_to_another_pair() {
        let tmp = TempDir::new();
        generate(&sample_catalog(), &tmp.root).unwrap();

        let arm = std::fs::read_to_string(
            tmp.root
                .join("bindings")
                .join("2.2081")
                .join("macos-arm64.pbind"),
        )
        .unwrap();
        let x64 = std::fs::read_to_string(
            tmp.root
                .join("bindings")
                .join("2.2081")
                .join("macos-x64.pbind"),
        )
        .unwrap();

        // Su arm64 il simbolo è verificato col suo RVA reale.
        assert!(arm.contains("symbol = MenuLayer::init\n"));
        assert!(arm.contains("offset = 0x316688\n"));

        // Su x64 lo stesso simbolo è presente ma fail-closed: verified = false e
        // offset sentinel. L'RVA di arm64 NON deve comparire (nessuna
        // propagazione cross-coppia).
        assert!(x64.contains("symbol = MenuLayer::init\n"));
        assert!(x64.contains("offset = 0xFFFFFFFFFFFFFFFF\n"));
        assert!(
            !x64.contains("0x316688"),
            "l'offset verificato di arm64 non deve mai comparire nel .pbind di x64"
        );
        // Il blocco di MenuLayer::init su x64 deve essere verified = false.
        assert!(x64.contains("verified = false\n"));
        assert!(!x64.contains("verified = true\n"));
    }

    /// Anche quando l'altra coppia possiede un offset **presente ma non
    /// verificato** (e diverso), il generatore non sostituisce mai quell'offset
    /// con quello verificato dell'altra coppia: ogni coppia emette esclusivamente
    /// il proprio `OffsetRecord` (Req 8.4, 8.6).
    #[test]
    fn unverified_offset_on_other_pair_keeps_its_own_value() {
        let arm = pair((2, 2081), TargetPlatform::MacosArm64);
        let x64 = pair((2, 2081), TargetPlatform::MacosX64);

        let catalog = BindingCatalog {
            entries: vec![entry(
                "MenuLayer::init",
                Signature::new("bool", vec!["MenuLayer*".into()]),
                vec![
                    // Verificato su arm64.
                    offset(arm, Some(0x316688), true),
                    // Presente ma NON verificato su x64, con un RVA diverso.
                    offset(x64, Some(0x999000), false),
                ],
            )],
        };

        let tmp = TempDir::new();
        generate(&catalog, &tmp.root).unwrap();

        let x64_content = std::fs::read_to_string(
            tmp.root
                .join("bindings")
                .join("2.2081")
                .join("macos-x64.pbind"),
        )
        .unwrap();

        // x64 mantiene il proprio offset (0x999000) con verified = false; non
        // adotta mai l'RVA verificato di arm64 (0x316688).
        assert!(x64_content.contains("offset = 0x999000\n"));
        assert!(x64_content.contains("verified = false\n"));
        assert!(
            !x64_content.contains("0x316688"),
            "x64 non deve adottare l'RVA verificato di arm64"
        );
    }

    /// La generazione segnala in [`GenReport::unresolved`] ogni voce emessa
    /// `verified = false`, in ordine deterministico per coppia e simbolo, senza
    /// includere le voci verificate (segnale del Req 8.6).
    #[test]
    fn report_signals_unresolved_entries_in_deterministic_order() {
        let tmp = TempDir::new();
        let report = generate(&sample_catalog(), &tmp.root).unwrap();

        let arm = pair((2, 2081), TargetPlatform::MacosArm64);
        let x64 = pair((2, 2081), TargetPlatform::MacosX64);

        // Atteso:
        //  - (arm64, PlayLayer::update): presente ma Verified_Flag = false;
        //  - (x64,   MenuLayer::init):   offset assente → sentinel, verified false.
        // MenuLayer::init su arm64 è verificato ⇒ NON compare.
        let expected = vec![
            (arm, SymbolId::new("PlayLayer::update")),
            (x64, SymbolId::new("MenuLayer::init")),
        ];
        assert_eq!(report.unresolved, expected);

        // La voce verificata non deve mai essere segnalata come non risolta.
        assert!(!report
            .unresolved
            .contains(&(arm, SymbolId::new("MenuLayer::init"))));
    }

    /// Un catalogo interamente verificato non produce alcuna segnalazione: la
    /// lista `unresolved` resta vuota (assenza di falsi positivi nel segnale 8.6).
    #[test]
    fn report_has_no_unresolved_when_all_verified() {
        let arm = pair((2, 2081), TargetPlatform::MacosArm64);
        let catalog = BindingCatalog {
            entries: vec![entry(
                "MenuLayer::init",
                Signature::new("bool", vec!["MenuLayer*".into()]),
                vec![offset(arm, Some(0x316688), true)],
            )],
        };

        let tmp = TempDir::new();
        let report = generate(&catalog, &tmp.root).unwrap();
        assert!(report.unresolved.is_empty());
    }

    // -----------------------------------------------------------------------
    // emit_verified — tabella di verità fail-closed (Req 3.3, 11.1, 11.2, 11.4).
    // -----------------------------------------------------------------------

    fn arm() -> TargetPair {
        pair((2, 2081), TargetPlatform::MacosArm64)
    }

    /// Costruisce un `OffsetRecord` con un RVA esplicito (anche sentinel/zero) e
    /// un `Verified_Flag` dichiarato, bypassando il `None ⇒ false` del parser per
    /// poter esercitare direttamente i casi limite del generatore.
    fn offset_rva(rva: Option<u64>, verified: bool) -> OffsetRecord {
        OffsetRecord {
            pair: arm(),
            rva,
            verified,
            provenance: ProvenanceRecord::empty(SymbolId::new("ignored"), arm()),
        }
    }

    #[test]
    fn emit_verified_true_only_for_fully_valid_offset() {
        // Unico caso `true`: presente, non-zero, != sentinel, Verified_Flag vero.
        assert!(emit_verified(&offset_rva(Some(0x316688), true)));
    }

    #[test]
    fn emit_verified_false_when_flag_false_even_with_valid_rva() {
        // Req 11.1: Verified_Flag false ⇒ verified false anche con RVA valido.
        assert!(!emit_verified(&offset_rva(Some(0x316688), false)));
    }

    #[test]
    fn emit_verified_false_for_absent_offset() {
        // Req 11.4: offset assente (None) ⇒ verified false anche se flag true.
        assert!(!emit_verified(&offset_rva(None, true)));
    }

    #[test]
    fn emit_verified_false_for_sentinel_offset_even_if_flag_true() {
        // Req 11.2/11.4: offset == SENTINEL_VALUE ⇒ verified false a prescindere
        // dal flag dichiarato.
        assert!(!emit_verified(&offset_rva(Some(SENTINEL_VALUE), true)));
    }

    #[test]
    fn emit_verified_false_for_zero_offset_even_if_flag_true() {
        // Req 11.4: un offset zero non è mai risolvibile (placeholder) ⇒ false,
        // indipendentemente dal Verified_Flag dichiarato.
        assert!(!emit_verified(&offset_rva(Some(0), true)));
    }

    #[test]
    fn zero_offset_emits_verified_false_in_serialized_output() {
        // Verifica end-to-end attraverso `serialize_set`: un offset zero con flag
        // dichiarato `true` esce comunque con `verified = false`.
        let symbol = SymbolId::new("PlayLayer::update");
        let signature = Signature::new("void", vec!["PlayLayer*".into(), "float".into()]);
        let offset = offset_rva(Some(0), true);
        let functions = vec![PivotFunction {
            symbol: &symbol,
            signature: &signature,
            offset: &offset,
        }];

        let content = serialize_set(arm(), &functions);
        // L'offset emesso è il valore concreto (0x0); il campo verified è false.
        assert!(content.contains("offset = 0x0\n"));
        assert!(content.contains("verified = false\n"));
        assert!(!content.contains("verified = true\n"));
    }
}
