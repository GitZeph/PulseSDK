//! Integration test — Task 11.2: seed della GD_API_Surface per il Prioritized_Target.
//!
//! Copre i Requisiti 1.6 e 7.5 della feature `gd-api-surface`, verificando
//! **end-to-end** che il `Surface_Manifest` seed committato
//! (`mod-index/surface/surface.toml`) produca, **riusando** la `Catalog_Entry`
//! seed di `MenuLayer::init` già prodotta da `bindings-pipeline`
//! (`mod-index/catalog/symbols/MenuLayer__init.toml`, RVA `0x316688`,
//! `verified = true` per `(2.2081, macos-arm64)`):
//!
//! - **Req 1.6 (Seed_Surface):** il flusso `pulse surface generate`
//!   (`load_manifest` + `load_catalog` → `compile_surface` → `generate_cpp`)
//!   emette la specializzazione `BindingTraits<FixedString("MenuLayer_init")>`
//!   con `using Fn = bool(pulse::gd::MenuLayer*);` (this-first) e la
//!   registrazione `PULSE_GD_HOOK` canonica `"MenuLayer::init"`.
//! - **Req 7.5 (risolvibilità per il Prioritized_Target):**
//!   `validate_surface` classifica `MenuLayer::init` come `resolvable = true`
//!   per `(2.2081, macos-arm64)`.
//!
//! Test-only: **non** modifica alcun file sorgente e **non** scrive header nella
//! vera `sdk/include/pulse/gd/` (usa una directory temporanea auto-pulente). Il
//! seed `surface.toml` è invece un artefatto reale committato. Usa l'API esposta
//! dal crate libreria `pulse_cli` (nessun shell-out), come `bindings_scaling.rs`.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};

use pulse_cli::bindings::catalog::load_catalog;
use pulse_cli::bindings::{GdVersion, SymbolId, TargetPair, TargetPlatform};
use pulse_cli::surface::compiler::compile_surface;
use pulse_cli::surface::cppgen::generate_cpp;
use pulse_cli::surface::manifest::load_manifest;
use pulse_cli::surface::validation::validate_surface;

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
            "pulse-surface-seed-{}-{}",
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

/// Radice del repo (un livello sopra `cli/`), risolta via `CARGO_MANIFEST_DIR`
/// come in `bindings_scaling.rs`.
fn repo_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("..")
}

/// Percorso del `Surface_Manifest` seed committato.
fn seed_manifest_path() -> PathBuf {
    repo_root()
        .join("mod-index")
        .join("surface")
        .join("surface.toml")
}

/// Percorso della radice del catalogo `mod-index/catalog/` (riuso del seed).
fn seed_catalog_root() -> PathBuf {
    repo_root().join("mod-index").join("catalog")
}

/// Percorso del file seed della `Catalog_Entry` di `MenuLayer::init`.
fn seed_catalog_entry_path() -> PathBuf {
    seed_catalog_root()
        .join("symbols")
        .join("MenuLayer__init.toml")
}

fn prioritized_target() -> TargetPair {
    TargetPair::new(GdVersion::new(2, 2081), TargetPlatform::MacosArm64)
}

/// Compila la superficie seed riusando il catalogo seed, restituendo la IR.
fn compile_seed_surface() -> pulse_cli::surface::ir::SurfaceIr {
    let manifest = load_manifest(&seed_manifest_path())
        .expect("il Surface_Manifest seed deve caricare");
    let catalog = load_catalog(&seed_catalog_root())
        .expect("l'albero del catalogo seed deve caricare (riuso)");

    let compiled = compile_surface(&manifest, &catalog)
        .expect("la compilazione della superficie seed deve riuscire");
    assert!(
        compiled.is_clean(),
        "il seed non deve avere diagnostiche di esclusione: {:?}",
        compiled.diagnostics
    );
    compiled.ir
}

// ---------------------------------------------------------------------------
// Pre-condizioni: il seed di catalogo riusato esiste (non viene riseminato).
// ---------------------------------------------------------------------------

#[test]
fn reused_catalog_seed_exists_on_disk() {
    // La superficie RIUSA la Catalog_Entry seed della bindings-pipeline: deve
    // esistere, ma questo test NON la modifica né la rigenera.
    let path = seed_catalog_entry_path();
    assert!(
        path.is_file(),
        "il seed di catalogo riusato deve esistere: {} \
         (prodotto da bindings-pipeline, non riseminato qui)",
        path.display()
    );
}

#[test]
fn seed_manifest_exists_and_loads() {
    // Il Surface_Manifest seed è un artefatto reale committato.
    let path = seed_manifest_path();
    assert!(
        path.is_file(),
        "il Surface_Manifest seed deve esistere: {}",
        path.display()
    );

    let manifest = load_manifest(&path).expect("il manifest seed deve caricare");

    // Una sola classe MenuLayer con il solo metodo MenuLayer::init, hook-point,
    // priorità ordinabile. Il manifest NON porta offset né firme (Req 2.3).
    assert_eq!(manifest.classes.len(), 1);
    let class = &manifest.classes[0];
    assert_eq!(class.name, "MenuLayer");
    assert_eq!(class.cpp_type, "MenuLayer");
    assert_eq!(class.methods.len(), 1);

    let method = &class.methods[0];
    assert_eq!(method.symbol, SymbolId::new("MenuLayer::init"));
    assert!(method.hook, "MenuLayer::init è un Hook_Point (hook = true)");
    assert_eq!(method.priority, 100);
}

// ---------------------------------------------------------------------------
// Req 1.6 — il flusso compile→generate produce BindingTraits + PULSE_GD_HOOK.
// ---------------------------------------------------------------------------

#[test]
fn surface_generate_emits_binding_traits_this_first_for_menulayer_init() {
    // Req 1.6: `pulse surface generate` (compile → generate) sul manifest seed,
    // riusando la Catalog_Entry, produce la BindingTraits di MenuLayer::init con
    // la firma canonica this-first `bool(pulse::gd::MenuLayer*)`.
    let ir = compile_seed_surface();

    // L'API_Element MenuLayer::init è presente, hook-point, firma derivata dal
    // catalogo (bool, ["MenuLayer*"]).
    let element = ir
        .elements
        .iter()
        .find(|e| e.symbol.canonical == SymbolId::new("MenuLayer::init"))
        .expect("MenuLayer::init presente nella Surface_IR");
    assert_eq!(element.symbol.cpp_token, "MenuLayer_init");
    assert!(element.is_hook_point);
    assert_eq!(element.signature.return_gd, "bool");
    assert_eq!(element.signature.param_gds, vec!["MenuLayer*".to_owned()]);

    // Generazione nella directory TEMP (NON la vera sdk/include/pulse/gd/).
    let out = TempDir::new();
    let report = generate_cpp(&ir, &out.root).expect("la generazione header deve riuscire");
    assert_eq!(report.written.len(), 3, "types/bindings/hooks generati");

    let gd_dir = out
        .root
        .join("sdk")
        .join("include")
        .join("pulse")
        .join("gd");

    // bindings.gen.hpp: specializzazione BindingTraits this-first.
    let bindings = std::fs::read_to_string(gd_dir.join("bindings.gen.hpp")).unwrap();
    assert!(
        bindings.contains(
            "struct pulse::hooks::BindingTraits<pulse::hooks::FixedString(\"MenuLayer_init\")>"
        ),
        "bindings.gen.hpp deve specializzare BindingTraits sul token MenuLayer_init"
    );
    assert!(
        bindings.contains("using Fn = bool(pulse::gd::MenuLayer*);"),
        "il Fn deve essere la firma canonica this-first bool(pulse::gd::MenuLayer*)"
    );
    // Marcatore d'appartenenza alla superficie (Build_Check, Req 5.3).
    assert!(bindings.contains("kPulseGdInSurface_MenuLayer_init = true"));

    // types.gen.hpp: forward-declaration opaca del tipo MenuLayer.
    let types = std::fs::read_to_string(gd_dir.join("types.gen.hpp")).unwrap();
    assert!(
        types.contains("struct MenuLayer;"),
        "types.gen.hpp deve forward-dichiarare il tipo opaco MenuLayer"
    );
}

#[test]
fn surface_generate_emits_pulse_gd_hook_registering_canonical_symbol() {
    // Req 1.6: hooks.gen.hpp espone la macro PULSE_GD_HOOK e documenta
    // l'Hook_Point MenuLayer::init che registra il SIMBOLO CANONICO
    // "MenuLayer::init" (percorso esplicito, nessun letterale di indirizzo).
    let ir = compile_seed_surface();

    let out = TempDir::new();
    generate_cpp(&ir, &out.root).expect("la generazione header deve riuscire");

    let hooks = std::fs::read_to_string(
        out.root
            .join("sdk")
            .join("include")
            .join("pulse")
            .join("gd")
            .join("hooks.gen.hpp"),
    )
    .unwrap();

    // La macro ergonomica è definita.
    assert!(
        hooks.contains("#define PULSE_GD_HOOK(Class, Method, Ret, Params)"),
        "hooks.gen.hpp deve definire la macro PULSE_GD_HOOK"
    );
    // La registrazione avviene sul simbolo canonico Class::Method.
    assert!(
        hooks.contains("#Class \"::\" #Method"),
        "PULSE_GD_HOOK deve registrare il simbolo canonico Class::Method"
    );
    assert!(
        hooks.contains("register_hook"),
        "PULSE_GD_HOOK deve usare register_hook (riuso SDK)"
    );
    // L'Hook_Point seed MenuLayer::init è documentato come canonico.
    assert!(
        hooks.contains("registra \"MenuLayer::init\""),
        "l'Hook_Point seed deve registrare il canonico \"MenuLayer::init\""
    );
    // Nessun letterale di indirizzo della RVA (Req 4.1).
    assert!(
        !hooks.contains("0x316688"),
        "la macro non deve contenere alcun letterale di indirizzo"
    );
}

// ---------------------------------------------------------------------------
// Req 7.5 — MenuLayer::init risolvibile per il Prioritized_Target.
// ---------------------------------------------------------------------------

#[test]
fn seed_element_is_resolvable_for_the_prioritized_target() {
    // Req 7.5: validando la Surface_IR seed contro il catalogo seed,
    // MenuLayer::init risulta resolvable = true per (2.2081, macos-arm64), con
    // riferimento a un Provenance_Record completo (Req 8.1).
    let ir = compile_seed_surface();
    let catalog = load_catalog(&seed_catalog_root())
        .expect("l'albero del catalogo seed deve caricare (riuso)");

    let outcome = validate_surface(&ir, &catalog);

    // Nessun declassamento per auditabilità: la provenienza del seed è completa.
    assert!(
        outcome.is_clean(),
        "la provenienza del seed deve essere completa: {:?}",
        outcome.auditability_errors
    );

    let target = prioritized_target();
    let classification = outcome
        .classifications
        .iter()
        .find(|c| c.symbol == SymbolId::new("MenuLayer::init") && c.pair == target)
        .expect("classificazione di MenuLayer::init per il Prioritized_Target presente");

    assert!(
        classification.resolvable,
        "MenuLayer::init deve essere resolvable = true per (2.2081, macos-arm64) (Req 7.5)"
    );
    let provenance = classification
        .provenance
        .as_ref()
        .expect("un elemento risolvibile ha un riferimento di provenienza (Req 8.1)");
    assert_eq!(provenance.pair, target);
    assert!(provenance.complete, "il Provenance_Record è completo");
}
