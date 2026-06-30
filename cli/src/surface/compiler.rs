//! `Surface_Compiler` — fusione `Surface_Manifest × Binding_Catalog` →
//! `Surface_IR`.
//!
//! È il cuore della logica **pura** della superficie: data la *selezione curata*
//! del [`SurfaceManifest`] e la *fonte di verità* del [`BindingCatalog`] (caricato
//! altrove con `load_catalog`, qui solo **letto**), produce la
//! [`SurfaceIr`](crate::surface::ir::SurfaceIr) — la rappresentazione
//! indipendente dal linguaggio degli `API_Element` (Req 10.2).
//!
//! ## Cosa fa il task 3.1
//!
//! Per ogni `ManifestMethod.symbol` cerca la [`CatalogEntry`] per `SymbolId`
//! (corrispondenza **esatta**, riuso del catalogo di `load_catalog`):
//!
//! - **simbolo privo di `Catalog_Entry`** → l'`API_Element` è **escluso** dalla
//!   superficie e si registra un [`SurfaceError::MissingCatalogEntry`] che nomina
//!   il simbolo, **proseguendo** con gli altri elementi (Req 1.5);
//! - **simbolo con `Catalog_Entry`** → si materializza un
//!   [`ApiElement`](crate::surface::ir::ApiElement) la cui
//!   [`CanonicalSignature`](crate::surface::ir::CanonicalSignature) è **derivata**
//!   dalla `Signature` del catalogo (this-first, tipi GD), esponendo l'elemento
//!   **se e solo se** `SymbolId` (e — quando dichiarata — `Signature`)
//!   corrispondono (Req 2.1).
//!
//! ## Cosa formalizzano i task 3.2 / 3.3 / 3.4
//!
//! I task successivi non riscrivono il join di 3.1 ma ne **formalizzano** e
//! **verificano** le proprietà già emergenti dalla costruzione:
//!
//! - **3.2 — pivot in `API_Element` + propagazione della priorità (Req 1.1,
//!   1.2, 1.3).** I `Method_Binding` sono raggruppati nei rispettivi
//!   [`ClassBinding`] (`ClassBinding.methods` elenca i soli simboli **esposti**
//!   della classe). Ogni `API_Element` è classificato come **Class_Binding**
//!   (il raggruppamento), **Method_Binding** (l'elemento) e — quando il manifest
//!   dichiara `hook = true` — anche **Hook_Point** ([`ApiElement::is_hook_point`],
//!   Req 1.1). La mappatura è **1:1** verso una sola [`CatalogEntry`] tramite un
//!   `SymbolId` univoco nella superficie (l'unicità è garantita a monte da
//!   `load_manifest`, Req 1.2/2.4). La `priority` del manifest è propagata
//!   **fedelmente** come valore **ordinabile** (`i64`) della `Surface_Selection`
//!   (Req 1.3).
//! - **3.3 — `CanonicalSignature` `this`-first derivata (mirror, non copia)
//!   (Req 1.4, 2.5, 5.4).** La firma esposta **rispecchia esattamente** la
//!   `Signature` della `Catalog_Entry` — stesso ritorno, stessa lista di
//!   parametri, con il puntatore alla classe ricevente come **primo** parametro
//!   (es. `MenuLayer::init` → `bool`, `["MenuLayer*"]`) — con i **tipi GD
//!   originali**, senza riordinare né ridefinire. Essendo **derivata** e non
//!   copiata, una `Signature` modificata nel catalogo si riflette
//!   automaticamente alla ricompilazione (Req 2.5). La mappatura ai **tipi C++**
//!   **non** avviene qui (resta nel `Cpp_Generator`, Phase D): la IR resta
//!   **indipendente dal linguaggio** (Req 10.2).
//! - **3.4 — stabilità degli elementi + nessuna sorgente di offset parallela
//!   (Req 2.2, 2.3).** [`compile_surface`] è una **funzione pura** di
//!   `(&SurfaceManifest, &BindingCatalog)`: non legge stato globale, file o
//!   tempo, quindi a parità di input produce sempre la stessa IR. Per
//!   costruzione, aggiungere un nuovo `API_Element` al manifest **non altera**
//!   `SymbolId`, `Signature` né `priority` degli elementi preesistenti (Req 2.2),
//!   perché ciascun elemento è materializzato indipendentemente dagli altri,
//!   leggendo solo la propria `Catalog_Entry`. La copertura è alimentata
//!   **esclusivamente** dal [`BindingCatalog`] (caricato da `load_catalog`):
//!   offset e `verified` **vivono nel catalogo** e la superficie li **legge**,
//!   non li possiede — infatti la [`SurfaceIr`] **non trasporta alcun offset**
//!   (nessuna sorgente parallela, Req 2.3).
//!
//! ## Decisione di progetto: "escludi **e** prosegui **e** segnala" (Req 1.5)
//!
//! Il requisito 1.5 chiede tre cose insieme: escludere l'elemento difettoso,
//! **proseguire** con gli altri, e **segnalare** l'anomalia. Un puro
//! `Result<SurfaceIr, SurfaceError>` che fallisce al primo errore non potrebbe
//! "proseguire". Perciò `compile_surface` restituisce
//! `Result<`[`CompiledSurface`]`, SurfaceError>` dove:
//!
//! - [`CompiledSurface::ir`] contiene **solo** gli elementi sopravvissuti
//!   (la superficie parziale, deterministica);
//! - [`CompiledSurface::diagnostics`] raccoglie gli errori **per-elemento**
//!   (esclusioni: [`SurfaceError::MissingCatalogEntry`],
//!   [`SurfaceError::SignatureMismatch`]) preservando l'ordine di scoperta;
//! - il canale `Err(SurfaceError)` resta riservato agli errori **fail-closed
//!   d'intera compilazione** (oggi: derivazione del `cpp_token` non valida →
//!   [`SurfaceError::InvalidCppToken`]; in futuro altri rifiuti globali). Su un
//!   `Err` non viene prodotta alcuna superficie.
//!
//! Questa forma è coerente con l'uso previsto dalle fasi successive (3.2–3.4 che
//! arricchiscono gli `ApiElement`, e il `Surface_Linter` di 9.1 che ispeziona le
//! `diagnostics` per riportare mancanti/duplicati senza interrompere). Affina la
//! firma `compile_surface -> Result<SurfaceIr, SurfaceError>` del design,
//! mantenendone invariata la semantica di fail-closed sugli errori globali.
//!
//! ## Confini con altre fasi
//!
//! - La **mappatura ai tipi C++ non avviene qui** (resta nel `Cpp_Generator`):
//!   gli override di tipo del manifest sono trasportati come **dati** in
//!   [`SurfaceIr::type_overrides`] (Req 10.2).
//! - `resolvability`/`provenance` degli `ApiElement` sono **vuoti** in
//!   compilazione e popolati dal loader a runtime (task 13.x, Req 7/8).
//! - L'**ordinamento** deterministico della IR e la **serde** sono aggiunti in
//!   5.1; qui si conserva l'ordine del manifest.
//!
//! _Requisiti: 1.5, 2.1 (con 1.3, 1.4 per i campi derivati)._

use crate::bindings::catalog::{BindingCatalog, CatalogEntry};

use super::ir::{ApiElement, CanonicalSignature, ClassBinding, SurfaceIr};
use super::manifest::SurfaceManifest;
use super::{SurfaceError, SurfaceSymbol, SymbolId};

/// Esito della compilazione della superficie: la `Surface_IR` parziale prodotta
/// più le diagnostiche **per-elemento** raccolte durante il join.
///
/// Vedi la decisione di progetto nel doc del modulo: gli errori che **escludono
/// un singolo elemento ma non interrompono** la compilazione (Req 1.5) vivono in
/// [`CompiledSurface::diagnostics`]; gli errori fail-closed d'intera
/// compilazione restano nel canale `Err` di [`compile_surface`].
///
/// Deriva solo `Debug`: contiene `Vec<SurfaceError>` e
/// [`SurfaceError`](crate::surface::SurfaceError) non è `Clone`/`PartialEq`
/// (trasporta sorgenti `io`/`toml`), quindi i confronti nei test usano
/// `matches!` sulle singole diagnostiche.
#[derive(Debug)]
pub struct CompiledSurface {
    /// La `Surface_IR` con i soli `API_Element` sopravvissuti.
    pub ir: SurfaceIr,
    /// Errori per-elemento (esclusioni) in ordine di scoperta; vuoto se tutti
    /// i simboli del manifest avevano una `Catalog_Entry` corrispondente.
    pub diagnostics: Vec<SurfaceError>,
}

impl CompiledSurface {
    /// `true` se la compilazione non ha escluso alcun elemento (nessuna
    /// diagnostica raccolta): la superficie copre l'intero manifest.
    pub fn is_clean(&self) -> bool {
        self.diagnostics.is_empty()
    }
}

/// Cerca la [`CatalogEntry`] per `SymbolId` (corrispondenza **esatta**) nel
/// [`BindingCatalog`]. È una scansione lineare sull'insieme delle voci, coerente
/// con la risoluzione a corrispondenza esatta del loader (nessun fuzzy-match).
fn find_entry<'a>(catalog: &'a BindingCatalog, symbol: &SymbolId) -> Option<&'a CatalogEntry> {
    catalog.entries.iter().find(|entry| &entry.symbol == symbol)
}

/// Compila la [`SurfaceIr`] fondendo `manifest` e `catalog`.
///
/// Per ogni `ManifestMethod.symbol` cerca la [`CatalogEntry`] per `SymbolId`:
/// in assenza **esclude** l'`API_Element` e registra
/// [`SurfaceError::MissingCatalogEntry`] proseguendo con gli altri (Req 1.5); in
/// presenza materializza un [`ApiElement`] con
/// [`CanonicalSignature`] derivata dal catalogo, esponendolo **se e solo se**
/// `SymbolId` (e — quando dichiarata — `Signature`) corrispondono (Req 2.1).
///
/// Restituisce `Err` solo su anomalie **fail-closed d'intera compilazione**
/// (oggi: `cpp_token` non derivabile → [`SurfaceError::InvalidCppToken`]); in tal
/// caso non viene prodotta alcuna superficie.
pub fn compile_surface(
    manifest: &SurfaceManifest,
    catalog: &BindingCatalog,
) -> Result<CompiledSurface, SurfaceError> {
    let mut classes: Vec<ClassBinding> = Vec::with_capacity(manifest.classes.len());
    let mut elements: Vec<ApiElement> = Vec::new();
    let mut diagnostics: Vec<SurfaceError> = Vec::new();

    for manifest_class in &manifest.classes {
        // 3.2 — pivot: i Method_Binding di questa classe sono raggruppati nel
        // suo Class_Binding. `class_methods` elenca i soli simboli SOPRAVVISSUTI
        // (quelli con Catalog_Entry), così il Class_Binding resta coerente con
        // gli API_Element effettivamente esposti.
        let mut class_methods: Vec<SymbolId> = Vec::with_capacity(manifest_class.methods.len());

        for method in &manifest_class.methods {
            let entry = match find_entry(catalog, &method.symbol) {
                // Req 1.5: simbolo privo di Catalog_Entry → escludi e segnala,
                // proseguendo con gli altri metodi/classi.
                None => {
                    diagnostics.push(SurfaceError::MissingCatalogEntry {
                        symbol: method.symbol.clone(),
                    });
                    continue;
                }
                Some(entry) => entry,
            };

            // Doppia identità del simbolo. Un cpp_token non derivabile è un
            // errore fail-closed d'intera compilazione (fail-closed): propaghiamo.
            let symbol = SurfaceSymbol::from_canonical(method.symbol.clone())?;

            // 3.3 — Req 2.1/1.4/2.5/5.4: la firma esposta è quella **autorevole**
            // del catalogo, rispecchiata 1:1 senza trasformazioni (this-first,
            // tipi GD; nessun riordino, nessuna mappatura a C++). È DERIVATA, non
            // copiata: cambiando la Signature del catalogo cambia qui alla
            // ricompilazione (Req 2.5). Il manifest non dichiara firme, quindi
            // non c'è SignatureMismatch da emettere in 3.x (vedi nota su
            // SurfaceError::SignatureMismatch).
            let signature = CanonicalSignature::from_signature(&entry.signature);

            // 3.4 — stabilità: ogni API_Element è materializzato in modo
            // **indipendente** dagli altri, leggendo solo la propria
            // Catalog_Entry (`entry`) e il proprio ManifestMethod (`method`).
            // Nessun offset è letto qui né trasportato nell'IR: la copertura
            // dipende esclusivamente dal BindingCatalog (Req 2.3).
            class_methods.push(method.symbol.clone());
            elements.push(ApiElement {
                symbol,
                class_name: manifest_class.name.clone(),
                signature,
                // 3.2 — Req 1.3: priorità propagata fedelmente come valore i64
                // ordinabile della Surface_Selection.
                priority: method.priority,
                // 3.2 — Req 1.1: un Method_Binding con `hook = true` è anche un
                // Hook_Point dichiarabile con PULSE_GD_HOOK.
                is_hook_point: method.hook,
                // Derivati a runtime (task 13.x): vuoti in compilazione.
                resolvability: Vec::new(),
                provenance: Vec::new(),
            });
        }

        classes.push(ClassBinding {
            name: manifest_class.name.clone(),
            methods: class_methods,
        });
    }

    let ir = SurfaceIr {
        classes,
        elements,
        // Override di tipo trasportati come dati; applicati dal Cpp_Generator.
        type_overrides: manifest.type_map.clone(),
    };

    Ok(CompiledSurface { ir, diagnostics })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bindings::catalog::CatalogEntry;
    use crate::bindings::Signature;
    use crate::surface::manifest::{ManifestClass, ManifestMethod};

    /// Costruisce una `CatalogEntry` minimale (senza offset) per i test del join.
    fn entry(symbol: &str, ret: &str, params: &[&str]) -> CatalogEntry {
        CatalogEntry {
            symbol: SymbolId::new(symbol),
            signature: Signature::new(ret, params.iter().map(|s| s.to_string()).collect()),
            offsets: Vec::new(),
        }
    }

    /// Costruisce un `ManifestMethod`.
    fn method(symbol: &str, priority: i64, hook: bool) -> ManifestMethod {
        ManifestMethod {
            symbol: SymbolId::new(symbol),
            priority,
            hook,
        }
    }

    /// Manifest con una sola classe e i metodi forniti.
    fn manifest_with(class: &str, methods: Vec<ManifestMethod>) -> SurfaceManifest {
        SurfaceManifest {
            classes: vec![ManifestClass {
                name: class.to_owned(),
                cpp_type: class.to_owned(),
                methods,
            }],
            type_map: Vec::new(),
        }
    }

    #[test]
    fn symbol_present_in_catalog_maps_one_to_one_to_api_element() {
        // Un simbolo del manifest presente nel catalogo produce esattamente un
        // ApiElement, mappato 1:1, con firma derivata dal catalogo.
        let catalog = BindingCatalog {
            entries: vec![entry("MenuLayer::init", "bool", &["MenuLayer*"])],
        };
        let manifest = manifest_with("MenuLayer", vec![method("MenuLayer::init", 100, true)]);

        let compiled = compile_surface(&manifest, &catalog).unwrap();

        assert!(compiled.is_clean(), "nessuna diagnostica attesa");
        assert_eq!(compiled.ir.elements.len(), 1);

        let element = &compiled.ir.elements[0];
        assert_eq!(element.symbol.canonical, SymbolId::new("MenuLayer::init"));
        assert_eq!(element.symbol.cpp_token, "MenuLayer_init");
        assert_eq!(element.class_name, "MenuLayer");
        assert_eq!(element.priority, 100);
        assert!(element.is_hook_point);

        // CanonicalSignature derivata 1:1 dalla Catalog_Entry (this-first, tipi GD).
        assert_eq!(element.signature.return_gd, "bool");
        assert_eq!(element.signature.param_gds, vec!["MenuLayer*".to_owned()]);

        // Risolvibilità/provenienza sono popolate a runtime: vuote in compilazione.
        assert!(element.resolvability.is_empty());
        assert!(element.provenance.is_empty());

        // Il Class_Binding elenca il simbolo esposto.
        assert_eq!(compiled.ir.classes.len(), 1);
        assert_eq!(compiled.ir.classes[0].name, "MenuLayer");
        assert_eq!(
            compiled.ir.classes[0].methods,
            vec![SymbolId::new("MenuLayer::init")]
        );
    }

    #[test]
    fn symbol_absent_from_catalog_is_excluded_and_signalled_while_others_compile() {
        // Req 1.5: un simbolo assente dal catalogo è ESCLUSO e segnalato con
        // MissingCatalogEntry che ne nomina il simbolo, mentre gli altri
        // elementi validi continuano a compilare.
        let catalog = BindingCatalog {
            entries: vec![entry("MenuLayer::init", "bool", &["MenuLayer*"])],
        };
        let manifest = manifest_with(
            "MenuLayer",
            vec![
                method("MenuLayer::init", 100, true),
                method("MenuLayer::ghost", 50, false), // assente dal catalogo
            ],
        );

        let compiled = compile_surface(&manifest, &catalog).unwrap();

        // L'elemento valido compila comunque.
        assert_eq!(compiled.ir.elements.len(), 1);
        assert_eq!(
            compiled.ir.elements[0].symbol.canonical,
            SymbolId::new("MenuLayer::init")
        );

        // Il simbolo escluso non compare nel Class_Binding.
        assert_eq!(
            compiled.ir.classes[0].methods,
            vec![SymbolId::new("MenuLayer::init")]
        );

        // L'esclusione è segnalata e identifica il simbolo mancante.
        assert_eq!(compiled.diagnostics.len(), 1);
        match &compiled.diagnostics[0] {
            SurfaceError::MissingCatalogEntry { symbol } => {
                assert_eq!(symbol, &SymbolId::new("MenuLayer::ghost"));
            }
            other => panic!("attesa MissingCatalogEntry, trovato {other:?}"),
        }
    }

    #[test]
    fn empty_manifest_yields_empty_surface() {
        let catalog = BindingCatalog {
            entries: vec![entry("MenuLayer::init", "bool", &["MenuLayer*"])],
        };
        let manifest = SurfaceManifest {
            classes: Vec::new(),
            type_map: Vec::new(),
        };
        let compiled = compile_surface(&manifest, &catalog).unwrap();
        assert!(compiled.ir.classes.is_empty());
        assert!(compiled.ir.elements.is_empty());
        assert!(compiled.is_clean());
    }

    #[test]
    fn type_overrides_are_carried_as_data_into_the_ir() {
        use crate::surface::manifest::TypeMapRule;
        let catalog = BindingCatalog {
            entries: vec![entry("MenuLayer::init", "bool", &["MenuLayer*"])],
        };
        let mut manifest = manifest_with("MenuLayer", vec![method("MenuLayer::init", 1, false)]);
        manifest.type_map = vec![TypeMapRule {
            gd: "cocos2d::CCObject*".to_owned(),
            cpp: "pulse::gd::CCObject*".to_owned(),
        }];

        let compiled = compile_surface(&manifest, &catalog).unwrap();
        assert_eq!(compiled.ir.type_overrides.len(), 1);
        assert_eq!(compiled.ir.type_overrides[0].gd, "cocos2d::CCObject*");
    }

    #[test]
    fn invalid_cpp_token_fails_closed_for_whole_compilation() {
        // Un simbolo presente nel catalogo ma con cpp_token non derivabile è un
        // errore fail-closed (Err), non una semplice esclusione.
        let catalog = BindingCatalog {
            entries: vec![entry("3Bad::init", "void", &[])],
        };
        let manifest = manifest_with("Bad", vec![method("3Bad::init", 1, false)]);
        let err = compile_surface(&manifest, &catalog).unwrap_err();
        assert!(matches!(err, SurfaceError::InvalidCppToken { .. }));
    }

    // -----------------------------------------------------------------------
    // 3.2 — pivot in API_Element, classificazione Hook_Point e priorità.
    // -----------------------------------------------------------------------

    #[test]
    fn hook_flag_classifies_the_element_as_hook_point() {
        // Req 1.1: un Method_Binding con `hook = true` è anche un Hook_Point;
        // con `hook = false` è un semplice Method_Binding (non un Hook_Point).
        let catalog = BindingCatalog {
            entries: vec![
                entry("MenuLayer::init", "bool", &["MenuLayer*"]),
                entry("MenuLayer::onPlay", "void", &["MenuLayer*", "cocos2d::CCObject*"]),
            ],
        };
        let manifest = manifest_with(
            "MenuLayer",
            vec![
                method("MenuLayer::init", 100, true),   // Hook_Point
                method("MenuLayer::onPlay", 50, false), // solo Method_Binding
            ],
        );

        let compiled = compile_surface(&manifest, &catalog).unwrap();

        let init = compiled
            .ir
            .elements
            .iter()
            .find(|e| e.symbol.canonical == SymbolId::new("MenuLayer::init"))
            .expect("MenuLayer::init presente");
        let on_play = compiled
            .ir
            .elements
            .iter()
            .find(|e| e.symbol.canonical == SymbolId::new("MenuLayer::onPlay"))
            .expect("MenuLayer::onPlay presente");

        assert!(init.is_hook_point, "hook = true ⇒ Hook_Point");
        assert!(!on_play.is_hook_point, "hook = false ⇒ non Hook_Point");
    }

    #[test]
    fn class_binding_groups_only_exposed_methods_of_the_class() {
        // 3.2 pivot: i Method_Binding sono raggruppati nel rispettivo
        // Class_Binding e ogni elemento mappa 1:1 alla sua Catalog_Entry tramite
        // un SymbolId univoco, appartenendo alla classe dichiarata nel manifest.
        let catalog = BindingCatalog {
            entries: vec![
                entry("MenuLayer::init", "bool", &["MenuLayer*"]),
                entry("MenuLayer::onPlay", "void", &["MenuLayer*"]),
            ],
        };
        let manifest = manifest_with(
            "MenuLayer",
            vec![
                method("MenuLayer::init", 100, true),
                method("MenuLayer::onPlay", 50, false),
            ],
        );

        let compiled = compile_surface(&manifest, &catalog).unwrap();

        // Il Class_Binding elenca esattamente i due simboli esposti, in ordine.
        assert_eq!(compiled.ir.classes.len(), 1);
        assert_eq!(compiled.ir.classes[0].name, "MenuLayer");
        assert_eq!(
            compiled.ir.classes[0].methods,
            vec![
                SymbolId::new("MenuLayer::init"),
                SymbolId::new("MenuLayer::onPlay"),
            ]
        );

        // Mappatura 1:1: tanti API_Element quante le voci esposte, ciascuno con
        // la classe di appartenenza corretta.
        assert_eq!(compiled.ir.elements.len(), 2);
        for element in &compiled.ir.elements {
            assert_eq!(element.class_name, "MenuLayer");
        }
    }

    #[test]
    fn priority_is_propagated_faithfully_and_is_orderable() {
        // Req 1.3: la priorità dichiarata nel manifest è propagata fedelmente a
        // ciascun API_Element ed è un valore ordinabile (i64), inclusi i negativi.
        let catalog = BindingCatalog {
            entries: vec![
                entry("L::a", "void", &["L*"]),
                entry("L::b", "void", &["L*"]),
                entry("L::c", "void", &["L*"]),
            ],
        };
        let manifest = manifest_with(
            "L",
            vec![
                method("L::a", 100, false),
                method("L::b", -5, false),
                method("L::c", 0, false),
            ],
        );

        let compiled = compile_surface(&manifest, &catalog).unwrap();

        let pri = |sym: &str| {
            compiled
                .ir
                .elements
                .iter()
                .find(|e| e.symbol.canonical == SymbolId::new(sym))
                .unwrap()
                .priority
        };
        assert_eq!(pri("L::a"), 100);
        assert_eq!(pri("L::b"), -5);
        assert_eq!(pri("L::c"), 0);

        // Ordinabilità: le priorità si possono ordinare in modo totale.
        let mut priorities: Vec<i64> = compiled.ir.elements.iter().map(|e| e.priority).collect();
        priorities.sort();
        assert_eq!(priorities, vec![-5, 0, 100]);
    }

    // -----------------------------------------------------------------------
    // 3.3 — CanonicalSignature this-first, mirror esatto del catalogo.
    // -----------------------------------------------------------------------

    #[test]
    fn canonical_signature_mirrors_the_catalog_signature_this_first() {
        // Req 1.4/5.4: la firma esposta rispecchia ESATTAMENTE quella della
        // Catalog_Entry — stesso ritorno e stessa lista parametri — con il
        // puntatore alla classe ricevente come PRIMO parametro, senza riordino
        // né mappatura a C++ (tipi GD originali conservati).
        let catalog = BindingCatalog {
            entries: vec![entry(
                "PlayLayer::collidedWithObject",
                "bool",
                &["PlayLayer*", "GameObject*", "PlayerObject*"],
            )],
        };
        let manifest = manifest_with(
            "PlayLayer",
            vec![method("PlayLayer::collidedWithObject", 1, false)],
        );

        let compiled = compile_surface(&manifest, &catalog).unwrap();
        let sig = &compiled.ir.elements[0].signature;

        // Ritorno identico.
        assert_eq!(sig.return_gd, "bool");
        // Lista parametri identica e nello STESSO ordine, this-first.
        assert_eq!(
            sig.param_gds,
            vec![
                "PlayLayer*".to_owned(), // this come primo parametro
                "GameObject*".to_owned(),
                "PlayerObject*".to_owned(),
            ]
        );
        assert_eq!(sig.param_gds.first().unwrap(), "PlayLayer*");

        // Mirror esatto: coincide con CanonicalSignature::from_signature del catalogo.
        let from_catalog = CanonicalSignature::from_signature(&catalog.entries[0].signature);
        assert_eq!(sig, &from_catalog);
    }

    #[test]
    fn canonical_signature_is_derived_not_copied_when_catalog_changes() {
        // Req 2.5: la firma è derivata dal catalogo; cambiando la Signature del
        // catalogo cambia la CanonicalSignature alla ricompilazione.
        let manifest = manifest_with("PlayLayer", vec![method("PlayLayer::update", 10, false)]);

        let catalog_a = BindingCatalog {
            entries: vec![entry("PlayLayer::update", "void", &["PlayLayer*", "float"])],
        };
        let a = compile_surface(&manifest, &catalog_a).unwrap();
        assert_eq!(
            a.ir.elements[0].signature.param_gds,
            vec!["PlayLayer*".to_owned(), "float".to_owned()]
        );

        let catalog_b = BindingCatalog {
            entries: vec![entry("PlayLayer::update", "void", &["PlayLayer*", "double"])],
        };
        let b = compile_surface(&manifest, &catalog_b).unwrap();
        assert_eq!(
            b.ir.elements[0].signature.param_gds,
            vec!["PlayLayer*".to_owned(), "double".to_owned()]
        );
    }

    // -----------------------------------------------------------------------
    // 3.4 — stabilità degli elementi e assenza di sorgenti di offset parallele.
    // -----------------------------------------------------------------------

    #[test]
    fn adding_a_new_element_leaves_pre_existing_elements_unchanged() {
        // Req 2.2: ricompilare S ∪ {nuovo} lascia invariati SymbolId, Signature e
        // priority di tutti gli elementi preesistenti; cambia solo l'insieme con
        // il nuovo elemento.
        let catalog = BindingCatalog {
            entries: vec![
                entry("MenuLayer::init", "bool", &["MenuLayer*"]),
                entry("MenuLayer::onPlay", "void", &["MenuLayer*"]),
                entry("PlayLayer::update", "void", &["PlayLayer*", "float"]),
            ],
        };

        // Superficie S: due metodi su MenuLayer.
        let surface_s = SurfaceManifest {
            classes: vec![ManifestClass {
                name: "MenuLayer".to_owned(),
                cpp_type: "MenuLayer".to_owned(),
                methods: vec![
                    method("MenuLayer::init", 100, true),
                    method("MenuLayer::onPlay", 50, false),
                ],
            }],
            type_map: Vec::new(),
        };

        // Superficie S ∪ {nuovo}: aggiunge PlayLayer::update in una nuova classe.
        let mut surface_s_plus = surface_s.clone();
        surface_s_plus.classes.push(ManifestClass {
            name: "PlayLayer".to_owned(),
            cpp_type: "PlayLayer".to_owned(),
            methods: vec![method("PlayLayer::update", 10, false)],
        });

        let compiled_s = compile_surface(&surface_s, &catalog).unwrap();
        let compiled_s_plus = compile_surface(&surface_s_plus, &catalog).unwrap();

        // Ogni elemento preesistente è IDENTICO (simbolo, firma, priorità,
        // hook-point, classe) tra S e S ∪ {nuovo}.
        for old in &compiled_s.ir.elements {
            let same = compiled_s_plus
                .ir
                .elements
                .iter()
                .find(|e| e.symbol.canonical == old.symbol.canonical)
                .expect("l'elemento preesistente è ancora presente in S ∪ {nuovo}");
            assert_eq!(&same.symbol, &old.symbol, "SymbolId invariato");
            assert_eq!(&same.signature, &old.signature, "Signature invariata");
            assert_eq!(same.priority, old.priority, "priority invariata");
            assert_eq!(same.is_hook_point, old.is_hook_point);
            assert_eq!(same.class_name, old.class_name);
        }

        // L'unico cambiamento è l'aggiunta del nuovo elemento.
        assert_eq!(compiled_s.ir.elements.len() + 1, compiled_s_plus.ir.elements.len());
        assert!(compiled_s_plus
            .ir
            .elements
            .iter()
            .any(|e| e.symbol.canonical == SymbolId::new("PlayLayer::update")));
    }

    #[test]
    fn compile_surface_is_a_pure_function_of_its_inputs() {
        // Req 2.2/2.3 (determinismo): a parità di (manifest, catalog) due
        // compilazioni producono una IR identica. La funzione non legge stato
        // globale, file o tempo: la sua firma è (&SurfaceManifest, &BindingCatalog).
        let catalog = BindingCatalog {
            entries: vec![
                entry("MenuLayer::init", "bool", &["MenuLayer*"]),
                entry("PlayLayer::update", "void", &["PlayLayer*", "float"]),
            ],
        };
        let manifest = SurfaceManifest {
            classes: vec![
                ManifestClass {
                    name: "MenuLayer".to_owned(),
                    cpp_type: "MenuLayer".to_owned(),
                    methods: vec![method("MenuLayer::init", 100, true)],
                },
                ManifestClass {
                    name: "PlayLayer".to_owned(),
                    cpp_type: "PlayLayer".to_owned(),
                    methods: vec![method("PlayLayer::update", 10, false)],
                },
            ],
            type_map: Vec::new(),
        };

        let first = compile_surface(&manifest, &catalog).unwrap();
        let second = compile_surface(&manifest, &catalog).unwrap();

        // SurfaceIr deriva PartialEq/Eq: l'uguaglianza struttura-per-struttura
        // prova il determinismo.
        assert_eq!(first.ir, second.ir);
        assert!(first.is_clean() && second.is_clean());
    }

    #[test]
    fn surface_ir_carries_no_offsets_only_catalog_fed_coverage() {
        // Req 2.3: la superficie LEGGE offset/verified dal catalogo ma non li
        // possiede. La IR non trasporta alcun offset: anche con un catalogo ricco
        // di OffsetRecord, l'API_Element espone solo simbolo, firma (tipi GD),
        // priorità e hook-point; risolvibilità/provenienza restano vuote in
        // compilazione (popolate a runtime). Non esiste alcuna sorgente parallela.
        use crate::bindings::catalog::{OffsetRecord, ProvenanceRecord};
        use crate::bindings::{GdVersion, TargetPair, TargetPlatform};

        let pair = TargetPair::new(GdVersion::new(2, 2081), TargetPlatform::MacosArm64);
        let entry_with_offset = CatalogEntry {
            symbol: SymbolId::new("MenuLayer::init"),
            signature: Signature::new("bool", vec!["MenuLayer*".to_owned()]),
            offsets: vec![OffsetRecord {
                pair,
                rva: Some(0x316688),
                verified: true,
                provenance: ProvenanceRecord::empty(SymbolId::new("MenuLayer::init"), pair),
            }],
        };
        let catalog = BindingCatalog {
            entries: vec![entry_with_offset],
        };
        let manifest = manifest_with("MenuLayer", vec![method("MenuLayer::init", 100, true)]);

        let compiled = compile_surface(&manifest, &catalog).unwrap();
        let element = &compiled.ir.elements[0];

        // Nessun offset trapela nella IR: la copertura è alimentata SOLO dal
        // catalogo, senza una sorgente parallela. Risolvibilità/provenienza
        // (che a runtime leggeranno il catalogo) sono vuote in compilazione.
        assert!(element.resolvability.is_empty());
        assert!(element.provenance.is_empty());
        // La firma è comunque derivata dal catalogo (mirror).
        assert_eq!(element.signature.return_gd, "bool");
        assert_eq!(element.signature.param_gds, vec!["MenuLayer*".to_owned()]);
    }
}
