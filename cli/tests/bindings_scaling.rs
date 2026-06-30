//! Integration test — Task 15.1: stabilità di scaling e ancora del Prioritized_Target.
//!
//! Copre i Requisiti 8.1, 8.2, 8.5 della feature `bindings-pipeline`:
//!
//! - **Req 8.5 (ancora del Prioritized_Target):** il file seed
//!   `mod-index/catalog/symbols/MenuLayer__init.toml` esiste, fa round-trip
//!   attraverso il parser del catalogo (`load_catalog_entry` / `load_catalog`)
//!   con `MenuLayer::init` `bool(MenuLayer*)` e offset `0x316688`
//!   `verified = true` per `(2.2081, macos-arm64)`; il `.pbind`
//!   `2.2081/macos-arm64` generato dal catalogo lo contiene verificato.
//! - **Req 8.1 (stabilità di scaling):** aggiungere una nuova `GD_Version` al
//!   catalogo lascia **byte-identico** il `.pbind` della `GD_Version`
//!   preesistente e **invariata** la sua `Catalog_Entry`/`OffsetRecord`.
//! - **Req 8.2 (offset isolati per versione):** il `.pbind` della nuova
//!   `GD_Version` usa **esclusivamente** gli offset delle sue `Catalog_Entry`,
//!   senza derivare/sostituire l'offset verificato della versione preesistente.
//!
//! Test-only: non modifica alcun file sorgente. Usa l'API esposta dal crate
//! libreria `pulse_cli` (`bindings::{catalog, generator, …}`).

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};

use pulse_cli::bindings::catalog::{
    load_catalog, load_catalog_entry, BindingCatalog, CatalogEntry, OffsetRecord, ProvenanceRecord,
};
use pulse_cli::bindings::generator::generate;
use pulse_cli::bindings::{
    GdVersion, Signature, SymbolId, TargetPair, TargetPlatform,
};

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
            "pulse-bindings-scaling-{}-{}",
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

/// Percorso del file seed `mod-index/catalog/symbols/MenuLayer__init.toml`,
/// risolto rispetto alla radice del repo (un livello sopra `cli/`).
fn seed_symbol_path() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("mod-index")
        .join("catalog")
        .join("symbols")
        .join("MenuLayer__init.toml")
}

/// Percorso della radice del catalogo `mod-index/catalog/`.
fn seed_catalog_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("mod-index")
        .join("catalog")
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

fn read_pbind(out_root: &Path, version: &str, platform: &str) -> Vec<u8> {
    std::fs::read(
        out_root
            .join("bindings")
            .join(version)
            .join(format!("{platform}.pbind")),
    )
    .unwrap()
}

// ---------------------------------------------------------------------------
// Req 8.5 — ancora del Prioritized_Target (seed round-trip + generazione).
// ---------------------------------------------------------------------------

#[test]
fn seed_file_exists_on_disk() {
    let path = seed_symbol_path();
    assert!(
        path.is_file(),
        "il file seed del Prioritized_Target deve esistere: {}",
        path.display()
    );
}

#[test]
fn seed_file_round_trips_through_load_catalog_entry() {
    // Req 8.5: il seed fa round-trip attraverso il parser del catalogo.
    let entry = load_catalog_entry(&seed_symbol_path())
        .expect("il file seed deve essere analizzabile da load_catalog_entry");

    assert_eq!(entry.symbol, SymbolId::new("MenuLayer::init"));
    assert_eq!(entry.signature.return_type, "bool");
    assert_eq!(entry.signature.param_types, vec!["MenuLayer*".to_owned()]);

    let arm = entry
        .offsets
        .iter()
        .find(|o| o.pair == pair((2, 2081), TargetPlatform::MacosArm64))
        .expect("offset (2.2081, macos-arm64) presente");

    assert_eq!(arm.rva, Some(0x316688));
    assert!(arm.verified, "il Prioritized_Target deve essere verified=true");
    assert!(!arm.is_sentinel());

    // Provenienza documentata (Req 10.1, 4.6).
    assert_eq!(
        arm.provenance.address_source.as_deref(),
        Some("contributor:tomas")
    );
    assert_eq!(arm.provenance.cross_check.as_deref(), Some("concordant"));
    assert!(arm.provenance.cross_check_no_reuse);
    assert_eq!(
        arm.provenance.prologue_method.as_deref(),
        Some("otool-manual")
    );
    assert_eq!(arm.provenance.prologue_outcome.as_deref(), Some("match"));
}

#[test]
fn seed_catalog_loads_via_load_catalog_and_contains_prioritized_target() {
    // Req 8.5: l'albero del catalogo seedato carica con load_catalog e contiene
    // la Catalog_Entry del Prioritized_Target.
    let catalog =
        load_catalog(&seed_catalog_root()).expect("l'albero del catalogo seed deve caricare");

    let entry = catalog
        .entries
        .iter()
        .find(|e| e.symbol == SymbolId::new("MenuLayer::init"))
        .expect("MenuLayer::init presente nel catalogo seed");

    let arm = entry
        .offsets
        .iter()
        .find(|o| o.pair == pair((2, 2081), TargetPlatform::MacosArm64))
        .expect("offset (2.2081, macos-arm64) presente");
    assert_eq!(arm.rva, Some(0x316688));
    assert!(arm.verified);
}

#[test]
fn generating_seed_catalog_emits_verified_prioritized_target_pbind() {
    // Req 8.5: il .pbind 2.2081/macos-arm64 generato dal catalogo seed contiene
    // MenuLayer::init verificato all'offset 0x316688.
    let catalog =
        load_catalog(&seed_catalog_root()).expect("l'albero del catalogo seed deve caricare");

    let out = TempDir::new();
    let report = generate(&catalog, &out.root).expect("la generazione deve riuscire");

    let arm_path = out
        .root
        .join("bindings")
        .join("2.2081")
        .join("macos-arm64.pbind");
    assert!(
        report.written.contains(&arm_path),
        "il .pbind del Prioritized_Target deve essere scritto"
    );

    let content = std::fs::read_to_string(&arm_path).unwrap();
    assert!(content.contains("gd_version = 2.2081\n"));
    assert!(content.contains("platform = macos-arm64\n"));
    assert!(content.contains("symbol = MenuLayer::init\n"));
    assert!(content.contains("offset = 0x316688\n"));
    assert!(content.contains("verified = true\n"));
}

// ---------------------------------------------------------------------------
// Req 8.1 / 8.2 — stabilità di scaling all'aggiunta di una GD_Version.
// ---------------------------------------------------------------------------

/// Catalogo di partenza: solo il Prioritized_Target `(2.2081, macos-arm64)`
/// con `MenuLayer::init` verificato all'offset `0x316688`.
fn base_catalog() -> BindingCatalog {
    let arm_2081 = pair((2, 2081), TargetPlatform::MacosArm64);
    BindingCatalog {
        entries: vec![CatalogEntry {
            symbol: SymbolId::new("MenuLayer::init"),
            signature: Signature::new("bool", vec!["MenuLayer*".into()]),
            offsets: vec![offset(arm_2081, Some(0x316688), true)],
        }],
    }
}

/// Catalogo "scalato": il Prioritized_Target preesistente **invariato**, più una
/// nuova `GD_Version` `2.2074` aggiunta come secondo blocco `[[offset]]` sullo
/// stesso simbolo, con un offset **diverso** e non ancora verificato.
fn scaled_catalog() -> BindingCatalog {
    let arm_2081 = pair((2, 2081), TargetPlatform::MacosArm64);
    let arm_2074 = pair((2, 2074), TargetPlatform::MacosArm64);
    BindingCatalog {
        entries: vec![CatalogEntry {
            symbol: SymbolId::new("MenuLayer::init"),
            signature: Signature::new("bool", vec!["MenuLayer*".into()]),
            offsets: vec![
                // Versione preesistente: identica al base_catalog (Req 8.1).
                offset(arm_2081, Some(0x316688), true),
                // Nuova GD_Version 2.2074: offset distinto, non verificato.
                offset(arm_2074, Some(0x300000), false),
            ],
        }],
    }
}

#[test]
fn adding_a_gd_version_keeps_existing_pbind_byte_identical() {
    // Req 8.1: rigenerare dopo aver aggiunto una nuova GD_Version lascia
    // byte-identico il .pbind della GD_Version preesistente.
    let out_base = TempDir::new();
    generate(&base_catalog(), &out_base.root).expect("generazione base");
    let before = read_pbind(&out_base.root, "2.2081", "macos-arm64");

    let out_scaled = TempDir::new();
    generate(&scaled_catalog(), &out_scaled.root).expect("generazione scalata");
    let after = read_pbind(&out_scaled.root, "2.2081", "macos-arm64");

    assert_eq!(
        before, after,
        "il .pbind della GD_Version preesistente deve restare byte-identico (Req 8.1)"
    );
}

#[test]
fn adding_a_gd_version_leaves_existing_catalog_entry_unchanged() {
    // Req 8.1: la Catalog_Entry / OffsetRecord della GD_Version preesistente non
    // viene modificata dall'aggiunta della nuova versione.
    let base = base_catalog();
    let scaled = scaled_catalog();

    let arm_2081 = pair((2, 2081), TargetPlatform::MacosArm64);

    let base_offset = base.entries[0]
        .offsets
        .iter()
        .find(|o| o.pair == arm_2081)
        .unwrap();
    let scaled_offset = scaled.entries[0]
        .offsets
        .iter()
        .find(|o| o.pair == arm_2081)
        .unwrap();

    assert_eq!(
        base_offset, scaled_offset,
        "l'OffsetRecord preesistente (2.2081) deve restare invariato (Req 8.1)"
    );
    assert_eq!(base.entries[0].signature, scaled.entries[0].signature);
}

#[test]
fn new_gd_version_pbind_uses_only_its_own_offsets() {
    // Req 8.2 / 8.4: il .pbind della nuova GD_Version usa ESCLUSIVAMENTE gli
    // offset delle sue Catalog_Entry; non deriva l'offset verificato della
    // versione preesistente.
    let out = TempDir::new();
    generate(&scaled_catalog(), &out.root).expect("generazione scalata");

    let new_version = std::fs::read_to_string(
        out.root
            .join("bindings")
            .join("2.2074")
            .join("macos-arm64.pbind"),
    )
    .unwrap();

    // Header concorde con la coppia del percorso.
    assert!(new_version.contains("gd_version = 2.2074\n"));
    assert!(new_version.contains("platform = macos-arm64\n"));
    // Usa il proprio offset 0x300000, NON quello della versione preesistente.
    assert!(new_version.contains("offset = 0x300000\n"));
    assert!(
        !new_version.contains("0x316688"),
        "l'offset della versione preesistente non deve propagarsi (Req 8.2/8.4)"
    );
    // Non verificato per la nuova versione (fail-closed).
    assert!(new_version.contains("verified = false\n"));

    // E la versione preesistente resta verificata col proprio offset.
    let existing = std::fs::read_to_string(
        out.root
            .join("bindings")
            .join("2.2081")
            .join("macos-arm64.pbind"),
    )
    .unwrap();
    assert!(existing.contains("offset = 0x316688\n"));
    assert!(existing.contains("verified = true\n"));
}
