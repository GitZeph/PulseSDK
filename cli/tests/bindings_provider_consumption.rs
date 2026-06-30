//! Integration test — Task 15.3: garanzie di consumo dei provider
//! (`EmbeddedBindingsProvider` / `OnlineBindingsProvider`) **a contratto
//! invariato**.
//!
//! Questo test **verifica** (non re-implementa né modifica) che l'output del
//! `Binding_Generator` soddisfi il contratto che i provider C++ del loader già
//! sanno consumare. Nessun sorgente del loader viene toccato: i provider C++
//! (`loader/bindings/embedded_bindings_provider.*`,
//! `online_bindings_provider.*`) restano consumatori a valle del catalogo
//! prodotto al percorso canonico `mod-index/bindings/{version}/{platform}.pbind`.
//!
//! Copre i Requisiti 9.1–9.5 della feature `bindings-pipeline`:
//!
//! - **Req 9.1 — un file per coppia al percorso canonico.** Un catalogo con *N*
//!   coppie genera esattamente *N* `.pbind`, ciascuno a
//!   `out_root/bindings/{version}/{platform}.pbind`, e il [`GenReport::written`]
//!   contiene esattamente quei percorsi.
//! - **Req 9.3 — nessun file per una coppia mancante.** Per una coppia non
//!   presente nel catalogo **nessun** file viene creato al suo percorso
//!   canonico, così un provider riporta l'assenza di match esatto senza caricare
//!   il `.pbind` di una coppia diversa.
//! - **Req 9.2 / 9.4 — header interni concordi con la coppia del percorso.** Per
//!   ogni `.pbind` emesso i valori interni `gd_version` / `platform` coincidono
//!   con la coppia derivata dal percorso: nessun file con header discordante è
//!   mai prodotto (verificato sia parsando l'header sia tramite il
//!   `Pbind_Linter`, che è la stessa diagnostica path↔header del Req 7.4).
//! - **Req 9.5 — RVA invariato.** L'RVA del catalogo è preservato **esattamente**
//!   nel `.pbind` (`offset = 0x…`), così la convenzione runtime del loader
//!   RVA→indirizzo assoluto (`imageBase + RVA`) resta corretta: la pipeline non
//!   altera mai l'RVA.
//!
//! Test-only: usa esclusivamente l'API esposta dal crate libreria `pulse_cli`
//! (`bindings::{catalog, generator, linter, …}`).

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};

use pulse_cli::bindings::catalog::{
    BindingCatalog, CatalogEntry, OffsetRecord, ProvenanceRecord,
};
use pulse_cli::bindings::generator::generate;
use pulse_cli::bindings::linter::lint_result;
use pulse_cli::bindings::{GdVersion, Signature, SymbolId, TargetPair, TargetPlatform};

// ---------------------------------------------------------------------------
// Utilità di test (nessuna dipendenza esterna).
// ---------------------------------------------------------------------------

/// Directory temporanea auto-pulente: crea una directory unica sotto
/// `std::env::temp_dir()` e la rimuove al `Drop`.
struct TempDir {
    root: PathBuf,
}

impl TempDir {
    fn new() -> Self {
        static COUNTER: AtomicU32 = AtomicU32::new(0);
        let unique = format!(
            "pulse-provider-consumption-{}-{}",
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
        provenance: ProvenanceRecord::empty(SymbolId::new("MenuLayer::init"), pair),
    }
}

/// Percorso canonico di una coppia sotto `out_root`:
/// `out_root/bindings/{version}/{platform}.pbind`.
fn canonical_path(out_root: &Path, pair: TargetPair) -> PathBuf {
    out_root
        .join("bindings")
        .join(pair.gd.to_string())
        .join(format!("{}.pbind", pair.platform.platform_id()))
}

/// Estrae il valore (trimmato) di una chiave `chiave = valore` dal contenuto di
/// un `.pbind`. Restituisce la **prima** occorrenza (le chiavi d'intestazione
/// sono uniche e in testa al file).
fn header_value<'a>(content: &'a str, key: &str) -> Option<&'a str> {
    content.lines().find_map(|line| {
        let (k, v) = line.split_once('=')?;
        if k.trim() == key {
            Some(v.trim())
        } else {
            None
        }
    })
}

// ---------------------------------------------------------------------------
// Cataloghi di test.
// ---------------------------------------------------------------------------

/// Catalogo multi-coppia: `MenuLayer::init` su quattro coppie distinte
/// `(2.2081, macos-arm64)` verificato, `(2.2081, macos-x64)`,
/// `(2.2074, macos-arm64)`, `(2.2074, windows-x64)`. Esercita un-file-per-coppia,
/// concordanza header e preservazione dell'RVA su più versioni/piattaforme.
fn multi_pair_catalog() -> BindingCatalog {
    let arm_2081 = pair((2, 2081), TargetPlatform::MacosArm64);
    let x64_2081 = pair((2, 2081), TargetPlatform::MacosX64);
    let arm_2074 = pair((2, 2074), TargetPlatform::MacosArm64);
    let win_2074 = pair((2, 2074), TargetPlatform::WindowsX64);

    BindingCatalog {
        entries: vec![CatalogEntry {
            symbol: SymbolId::new("MenuLayer::init"),
            signature: Signature::new("bool", vec!["MenuLayer*".into()]),
            offsets: vec![
                offset(arm_2081, Some(0x316688), true),
                offset(x64_2081, Some(0x401000), false),
                offset(arm_2074, Some(0x2F0000), false),
                offset(win_2074, Some(0x123456), false),
            ],
        }],
    }
}

/// Le quattro coppie attese da [`multi_pair_catalog`].
fn multi_pair_pairs() -> Vec<TargetPair> {
    vec![
        pair((2, 2081), TargetPlatform::MacosArm64),
        pair((2, 2081), TargetPlatform::MacosX64),
        pair((2, 2074), TargetPlatform::MacosArm64),
        pair((2, 2074), TargetPlatform::WindowsX64),
    ]
}

// ---------------------------------------------------------------------------
// Req 9.1 — un file per coppia al percorso canonico, GenReport coerente.
// ---------------------------------------------------------------------------

#[test]
fn req_9_1_generates_exactly_one_file_per_pair_at_canonical_path() {
    let catalog = multi_pair_catalog();
    let pairs = multi_pair_pairs();

    let out = TempDir::new();
    let report = generate(&catalog, &out.root).expect("la generazione deve riuscire");

    // Esattamente N file per N coppie.
    assert_eq!(
        report.written.len(),
        pairs.len(),
        "deve essere emesso esattamente un .pbind per coppia (Req 9.1)"
    );

    for pair in &pairs {
        let expected = canonical_path(&out.root, *pair);
        // Ogni file esiste al percorso canonico…
        assert!(
            expected.is_file(),
            "manca il .pbind al percorso canonico per la coppia {pair}: {}",
            expected.display()
        );
        // …e il GenReport.written elenca esattamente quei percorsi.
        assert!(
            report.written.contains(&expected),
            "GenReport.written deve contenere il percorso canonico della coppia {pair}"
        );
    }

    // Nessun percorso extra oltre alle coppie attese.
    for written in &report.written {
        let matches_some_pair = pairs.iter().any(|p| &canonical_path(&out.root, *p) == written);
        assert!(
            matches_some_pair,
            "GenReport.written contiene un percorso non riconducibile ad alcuna coppia: {}",
            written.display()
        );
    }
}

// ---------------------------------------------------------------------------
// Req 9.3 — nessun file per una coppia mancante.
// ---------------------------------------------------------------------------

#[test]
fn req_9_3_absent_pair_produces_no_file_at_its_canonical_path() {
    // Catalogo con UNA sola coppia presente.
    let present = pair((2, 2081), TargetPlatform::MacosArm64);
    let catalog = BindingCatalog {
        entries: vec![CatalogEntry {
            symbol: SymbolId::new("MenuLayer::init"),
            signature: Signature::new("bool", vec!["MenuLayer*".into()]),
            offsets: vec![offset(present, Some(0x316688), true)],
        }],
    };

    let out = TempDir::new();
    let report = generate(&catalog, &out.root).expect("la generazione deve riuscire");

    // La coppia presente esiste…
    let present_path = canonical_path(&out.root, present);
    assert!(present_path.is_file());
    assert_eq!(report.written.len(), 1);
    assert!(report.written.contains(&present_path));

    // …mentre le coppie ASSENTI non hanno alcun file al loro percorso canonico,
    // così il provider riporta "nessun match esatto" senza caricare una coppia
    // diversa (Req 9.3).
    let absent_same_version = pair((2, 2081), TargetPlatform::MacosX64);
    let absent_other_version = pair((2, 2074), TargetPlatform::MacosArm64);

    for absent in [absent_same_version, absent_other_version] {
        let absent_path = canonical_path(&out.root, absent);
        assert!(
            !absent_path.exists(),
            "nessun file deve esistere per la coppia assente {absent}: {}",
            absent_path.display()
        );
        assert!(
            !report.written.contains(&absent_path),
            "GenReport.written non deve elencare la coppia assente {absent}"
        );
    }
}

// ---------------------------------------------------------------------------
// Req 9.2 / 9.4 — header interni concordi con la coppia del percorso.
// ---------------------------------------------------------------------------

#[test]
fn req_9_2_and_9_4_internal_headers_match_pair_derived_from_path() {
    let catalog = multi_pair_catalog();
    let out = TempDir::new();
    let report = generate(&catalog, &out.root).expect("la generazione deve riuscire");

    for path in &report.written {
        let content = std::fs::read_to_string(path).unwrap();

        // Coppia derivata dal percorso: .../bindings/{version}/{platform}.pbind
        let platform_seg = path
            .file_stem()
            .and_then(|s| s.to_str())
            .expect("nome file .pbind valido");
        let version_seg = path
            .parent()
            .and_then(|p| p.file_name())
            .and_then(|s| s.to_str())
            .expect("segmento di versione presente nel percorso");

        // Gli header interni devono coincidere con la coppia del percorso.
        assert_eq!(
            header_value(&content, "gd_version"),
            Some(version_seg),
            "gd_version interno discordante dal percorso in {}",
            path.display()
        );
        assert_eq!(
            header_value(&content, "platform"),
            Some(platform_seg),
            "platform interno discordante dal percorso in {}",
            path.display()
        );

        // Il Pbind_Linter applica la stessa diagnostica path↔header (Req 7.4):
        // un file emesso dal generatore non deve mai produrre finding di
        // discordanza ed è idoneo alla distribuzione.
        let report = lint_result(path, &content);
        assert!(
            report.is_distributable(),
            "il .pbind generato deve superare il lint (header concordi) ma ha: {:?}",
            report.findings()
        );
    }
}

// ---------------------------------------------------------------------------
// Req 9.5 — RVA preservato esattamente attraverso la generazione.
// ---------------------------------------------------------------------------

#[test]
fn req_9_5_rva_is_preserved_exactly_in_the_generated_file() {
    // Il Prioritized_Target: RVA 0x316688 deve apparire ESATTAMENTE come
    // `offset = 0x316688` nel .pbind generato (il loader applica a runtime
    // RVA→assoluto = imageBase + RVA; la pipeline non altera l'RVA).
    let target = pair((2, 2081), TargetPlatform::MacosArm64);
    let catalog = BindingCatalog {
        entries: vec![CatalogEntry {
            symbol: SymbolId::new("MenuLayer::init"),
            signature: Signature::new("bool", vec!["MenuLayer*".into()]),
            offsets: vec![offset(target, Some(0x316688), true)],
        }],
    };

    let out = TempDir::new();
    generate(&catalog, &out.root).expect("la generazione deve riuscire");

    let content = std::fs::read_to_string(canonical_path(&out.root, target)).unwrap();
    // L'offset emesso (hex maiuscolo senza zero-padding) coincide ESATTAMENTE con
    // l'RVA del catalogo per la voce verificata.
    assert!(
        content.contains("offset = 0x316688\n"),
        "l'RVA 0x316688 deve essere preservato esattamente; contenuto:\n{content}"
    );
}

#[test]
fn req_9_5_rva_preserved_across_multiple_pairs_and_values() {
    // L'RVA di ciascuna coppia è preservato esattamente, senza arrotondamenti né
    // riformattazioni che ne cambino il valore numerico.
    let catalog = multi_pair_catalog();
    let out = TempDir::new();
    generate(&catalog, &out.root).expect("la generazione deve riuscire");

    let expected: &[(TargetPair, u64)] = &[
        (pair((2, 2081), TargetPlatform::MacosArm64), 0x316688),
        (pair((2, 2081), TargetPlatform::MacosX64), 0x401000),
        (pair((2, 2074), TargetPlatform::MacosArm64), 0x2F0000),
        (pair((2, 2074), TargetPlatform::WindowsX64), 0x123456),
    ];

    for (p, rva) in expected {
        let content = std::fs::read_to_string(canonical_path(&out.root, *p)).unwrap();
        let needle = format!("offset = 0x{rva:X}\n");
        assert!(
            content.contains(&needle),
            "l'RVA {rva:#x} della coppia {p} deve essere preservato esattamente come `{}`; contenuto:\n{content}",
            needle.trim_end()
        );
    }
}
