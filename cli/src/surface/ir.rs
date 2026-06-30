//! Surface_IR — rappresentazione **indipendente dal linguaggio** della
//! superficie d'API (Req 10.2).
//!
//! La `Surface_IR` è la fusione `Surface_Manifest × Binding_Catalog` prodotta dal
//! [`crate::surface::compiler::compile_surface`]: descrive *quali* `API_Element`
//! sono esposti, in quale `Class_Binding`, con quale firma (a **tipi GD**,
//! non C++), priorità ed eventuale natura di `Hook_Point`. È **fondata** sul
//! `Binding_Catalog` (firma e — più avanti — offset/provenienza derivano da
//! lì), ma resta neutra rispetto al linguaggio: la mappatura ai tipi C++ è
//! responsabilità del `Cpp_Generator`, non di questa IR (Req 10.2, 10.3).
//!
//! ## Confini di fase (cosa popola cosa)
//!
//! Questo file definisce le **strutture di dominio in memoria** che il compiler
//! costruisce e popola nella fase corrente (task 3.1). Le fasi successive le
//! estendono senza ridefinirle:
//!
//! - **3.1 (questa)** — definizione delle strutture; il compiler popola
//!   identità ([`SurfaceSymbol`]), `class_name`, [`CanonicalSignature`] derivata
//!   dalla `Catalog_Entry`, `priority` e `is_hook_point`. I vettori
//!   [`ApiElement::resolvability`] e [`ApiElement::provenance`] restano **vuoti**
//!   (sono derivati a runtime, non in compilazione).
//! - **3.2 / 3.3 / 3.4** — rifiniscono il pivot in [`ClassBinding`], la
//!   propagazione della priorità, la canonicalizzazione `this`-first della firma
//!   e la stabilità dell'identità.
//! - **5.1 (questa)** — aggiunge la **serializzazione serde** verso
//!   `mod-index/surface/surface.ir.json` ([`to_json`]/[`write_ir`]) e le
//!   garanzie di **ordinamento** deterministico ([`SurfaceIr::sorted`]: classi
//!   per nome, elementi per `SymbolId`) e d'indipendenza dal linguaggio su
//!   questi stessi tipi.
//! - **13.x (runtime)** — popolano [`ApiElement::resolvability`] e
//!   [`ApiElement::provenance`] dal loader, per coppia `(GD_Version,
//!   Target_Platform)` (Req 7, 8).
//!
//! _Requisiti: 10.2, 10.3 (con 1.3, 1.4 per i campi popolati in 3.x)._

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use super::manifest::TypeMapRule;
use super::{GdVersion, SurfaceError, SurfaceSymbol, SymbolId, TargetPair, TargetPlatform};

/// La `Surface_IR`: l'insieme degli `API_Element` esposti, i loro
/// raggruppamenti in `Class_Binding` e i dati di override dei tipi.
///
/// Strutturata per essere **serializzabile** (serde aggiunta in 5.1, vedi
/// [`to_json`]/[`write_ir`]) e indipendente dal linguaggio: contiene solo
/// simboli, firme a tipi GD, priorità e — più avanti — risolvibilità/provenienza
/// per coppia. Nessun tipo C++ vive qui (Req 10.2).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SurfaceIr {
    /// I `Class_Binding` esposti. L'ordinamento deterministico (per nome) è
    /// garantito in 5.1; in 3.1 si conserva l'ordine del manifest.
    pub classes: Vec<ClassBinding>,
    /// Gli `API_Element` esposti. L'ordinamento deterministico (per `SymbolId`)
    /// è garantito in 5.1; in 3.1 si conserva l'ordine del manifest.
    pub elements: Vec<ApiElement>,
    /// Le regole di mappatura di tipo del manifest, trasportate come **dati**
    /// nella IR e applicate **solo** dal generatore di linguaggio (Req 3.3,
    /// 10.2). La IR non le interpreta.
    pub type_overrides: Vec<TypeMapRule>,
}

/// Un `Class_Binding`: il raggruppamento dei `Method_Binding` (per `SymbolId`)
/// sotto un nome di classe GD, indipendente dal linguaggio.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ClassBinding {
    /// Nome della classe (nome GD, es. `"MenuLayer"`), language-agnostic.
    pub name: String,
    /// I simboli dei `Method_Binding` appartenenti alla classe, **limitati** ai
    /// soli simboli effettivamente esposti (quelli con `Catalog_Entry`).
    pub methods: Vec<SymbolId>,
}

/// Un `API_Element`: un metodo esposto dalla superficie, con la sua doppia
/// identità, la classe di appartenenza, la firma derivata dal catalogo, la
/// priorità e la natura di `Hook_Point`.
///
/// I campi [`ApiElement::resolvability`] e [`ApiElement::provenance`] sono
/// **derivati a runtime** (task 13.x) e in fase di compilazione (3.1) restano
/// vuoti: la compilazione è una funzione pura di `(manifest, catalog)` e non
/// conosce la coppia `(GD_Version, Target_Platform)` di esecuzione.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ApiElement {
    /// Doppia identità del simbolo: canonico (runtime) + `cpp_token` (header).
    pub symbol: SurfaceSymbol,
    /// Nome del `Class_Binding` di appartenenza (es. `"MenuLayer"`).
    pub class_name: String,
    /// Firma canonica a **tipi GD** derivata dalla `Catalog_Entry` (this-first).
    pub signature: CanonicalSignature,
    /// Valore ordinabile di priorità della `Surface_Selection` (Req 1.3).
    pub priority: i64,
    /// `true` se è un `Hook_Point` dichiarabile con `PULSE_GD_HOOK` (Req 1.1).
    pub is_hook_point: bool,
    /// Risolvibilità per coppia (Req 7). **Vuoto in compilazione**: popolato dal
    /// loader a runtime (task 13.x).
    pub resolvability: Vec<PairResolvability>,
    /// Riferimenti di provenienza per coppia (Req 8). **Vuoto in compilazione**:
    /// popolato dal loader a runtime (task 13.x).
    pub provenance: Vec<ProvenanceRef>,
}

/// Firma canonica di un `API_Element`, espressa con i **tipi GD originali**.
///
/// È una funzione **pura** della [`super::Signature`] della `Catalog_Entry`: il
/// compiler la rispecchia senza riordinare, ridefinire o mappare i tipi (Req
/// 1.4, 5.4). La convenzione "il `this` è il primo parametro" è già quella del
/// catalogo (es. `MenuLayer::init` → `return_gd = "bool"`, `param_gds =
/// ["MenuLayer*"]`). Quando la firma del catalogo cambia, questa cambia di
/// conseguenza alla rigenerazione (Req 2.5), perché è **derivata, non copiata**.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CanonicalSignature {
    /// Tipo di ritorno con il **tipo GD** del catalogo, es. `"bool"`.
    pub return_gd: String,
    /// Tipi dei parametri (tipi GD), this-first, es. `["MenuLayer*"]`.
    pub param_gds: Vec<String>,
}

impl CanonicalSignature {
    /// Deriva la [`CanonicalSignature`] dalla [`super::Signature`] di una
    /// `Catalog_Entry`, rispecchiando i tipi GD senza alcuna trasformazione
    /// (nessun riordino, nessuna mappatura a C++).
    pub fn from_signature(signature: &super::Signature) -> Self {
        Self {
            return_gd: signature.return_type.clone(),
            param_gds: signature.param_types.clone(),
        }
    }
}

/// Risolvibilità di un `API_Element` per una specifica coppia.
///
/// Calcolata **esclusivamente** dal binding di *quella* coppia (Req 7.2): nessuna
/// derivazione cross-coppia. Popolata a runtime (task 13.x).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PairResolvability {
    /// Coppia `(GD_Version, Target_Platform)` interessata.
    #[serde(with = "target_pair_serde")]
    pub pair: TargetPair,
    /// `true` sse il binding di `pair` è verificato (`resolved sse verificato`).
    pub resolvable: bool,
}

/// Riferimento di provenienza di un `API_Element` per una coppia (Req 8).
///
/// Espone **per riferimento** (senza ricomputazione) la completezza del
/// `Provenance_Record` della `Catalog_Entry` per la coppia. Popolato a runtime
/// (task 13.x).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ProvenanceRef {
    /// Coppia `(GD_Version, Target_Platform)` interessata.
    #[serde(with = "target_pair_serde")]
    pub pair: TargetPair,
    /// `true` sse cross-check e prologo sono documentati e completi (Req 8.2).
    pub complete: bool,
}

// ---------------------------------------------------------------------------
// 5.1 — Serializzazione serde di TargetPair come stringa "major.minor/platform".
// ---------------------------------------------------------------------------

/// Shim serde per [`TargetPair`] (Req 10.2).
///
/// `TargetPair` vive nella `bindings-pipeline` (`cli/src/bindings/`) e **non**
/// deriva serde; questa fase **non deve modificare** quel crate condiviso.
/// Anziché un mirror serializzabile, mappiamo la coppia su/da la sua forma
/// testuale canonica `"{major}.{minor}/{platform-id}"` — la stessa di
/// [`TargetPair::Display`](crate::bindings::TargetPair) e del segmento di
/// percorso `.pbind` — riusando [`GdVersion`] e
/// [`TargetPlatform::platform_id`]/[`TargetPlatform::from_platform_id`]. La
/// rappresentazione è quindi **stabile, leggibile e diffabile** e resta
/// indipendente dal linguaggio.
///
/// In pratica i vettori `resolvability`/`provenance` sono **vuoti** in
/// compilazione (popolati a runtime in 13.x), ma lo shim è implementato in modo
/// completo e con round-trip esatto per quando lo saranno.
mod target_pair_serde {
    use super::{GdVersion, TargetPair, TargetPlatform};
    use serde::{Deserialize, Deserializer, Serializer};

    /// Serializza la coppia come stringa `"{major}.{minor}/{platform-id}"`.
    pub fn serialize<S>(pair: &TargetPair, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        serializer.serialize_str(&pair.to_string())
    }

    /// Deserializza la coppia dalla forma testuale canonica, rifiutando in
    /// chiusura ogni stringa malformata o piattaforma fuori dall'insieme finito.
    pub fn deserialize<'de, D>(deserializer: D) -> Result<TargetPair, D::Error>
    where
        D: Deserializer<'de>,
    {
        let raw = String::deserialize(deserializer)?;
        parse_pair(&raw).ok_or_else(|| {
            serde::de::Error::custom(format!("Target_Pair non valida: {raw:?}"))
        })
    }

    /// Analizza `"{major}.{minor}/{platform-id}"` in una [`TargetPair`].
    ///
    /// Il separatore `'/'` divide versione e piattaforma; il `platform-id`
    /// contiene `'-'` ma mai `'/'`, quindi lo split sul primo `'/'` è univoco.
    fn parse_pair(raw: &str) -> Option<TargetPair> {
        let (version, platform_id) = raw.split_once('/')?;
        let (major, minor) = version.split_once('.')?;
        let major: u32 = major.parse().ok()?;
        let minor: u32 = minor.parse().ok()?;
        let platform = TargetPlatform::from_platform_id(platform_id)?;
        Some(TargetPair::new(GdVersion::new(major, minor), platform))
    }
}

// ---------------------------------------------------------------------------
// 5.1 — Ordinamento canonico, serializzazione JSON e scrittura atomica su disco.
// ---------------------------------------------------------------------------

/// Nome del file della `Surface_IR` serializzata sotto `out_root/surface/`.
pub const SURFACE_IR_FILE_NAME: &str = "surface.ir.json";

impl SurfaceIr {
    /// Restituisce una copia della IR con **ordinamento canonico** (Req 10.2):
    ///
    /// - le [`SurfaceIr::classes`] sono ordinate per [`ClassBinding::name`];
    /// - i [`ClassBinding::methods`] di ciascuna classe sono ordinati per
    ///   [`SymbolId`];
    /// - gli [`SurfaceIr::elements`] sono ordinati per
    ///   [`SurfaceSymbol::canonical`] (il `SymbolId`).
    ///
    /// I [`SurfaceIr::type_overrides`] conservano l'ordine del manifest (sono
    /// dati di configurazione, non parte dell'identità ordinabile della
    /// superficie). L'ordinamento è **idempotente** e **totale** (i `SymbolId`
    /// sono univoci nella superficie, Req 2.4), perciò
    /// `ir.sorted() == ir.sorted().sorted()` e la serializzazione di una IR
    /// ordinata è **byte-identica** fra due esecuzioni a parità di IR.
    pub fn sorted(&self) -> SurfaceIr {
        let mut classes = self.classes.clone();
        for class in &mut classes {
            class.methods.sort();
        }
        classes.sort_by(|a, b| a.name.cmp(&b.name));

        let mut elements = self.elements.clone();
        elements.sort_by(|a, b| a.symbol.canonical.cmp(&b.symbol.canonical));

        SurfaceIr {
            classes,
            elements,
            type_overrides: self.type_overrides.clone(),
        }
    }
}

/// Serializza la [`SurfaceIr`] in JSON **deterministico** (pretty), applicando
/// prima l'[ordinamento canonico](SurfaceIr::sorted) (Req 10.2).
///
/// È una funzione **pura**: a parità di IR produce sempre la stessa stringa
/// (l'ordine dei campi delle struct e degli elementi dei `Vec` è preservato da
/// `serde_json`, e non vi sono mappe a chiavi non ordinate). Il JSON contiene
/// **solo** simboli, firme a **tipi GD**, priorità e — quando popolata —
/// risolvibilità/provenienza per coppia: **nessun tipo C++** (gli override sono
/// trasportati come dati e applicati solo dal `Cpp_Generator`).
pub fn to_json(ir: &SurfaceIr) -> Result<String, SurfaceError> {
    let sorted = ir.sorted();
    serde_json::to_string_pretty(&sorted).map_err(|source| SurfaceError::SerializeIr { source })
}

/// Costruisce il percorso canonico del `surface.ir.json`:
/// `out_root/surface/surface.ir.json`.
pub fn ir_path(out_root: &Path) -> PathBuf {
    out_root.join("surface").join(SURFACE_IR_FILE_NAME)
}

/// Scrive la [`SurfaceIr`] in `out_root/surface/surface.ir.json` in modo
/// **atomico** e **deterministico** (Req 10.2), restituendo il percorso scritto.
///
/// La IR è serializzata con [`to_json`] (ordinata, pretty) e promossa con un
/// `rename` atomico nella stessa directory, nella stessa disciplina fail-closed
/// del `Cpp_Generator`/pipeline (Req 3.6): su errore l'eventuale
/// `surface.ir.json` precedente resta intatto byte-per-byte e nessun file
/// temporaneo parziale è lasciato a terra. La directory `surface/` è creata se
/// assente.
pub fn write_ir(ir: &SurfaceIr, out_root: &Path) -> Result<PathBuf, SurfaceError> {
    let json = to_json(ir)?;
    let path = ir_path(out_root);

    let parent = path.parent().unwrap_or_else(|| Path::new("."));
    std::fs::create_dir_all(parent).map_err(|source| SurfaceError::WriteIr {
        path: parent.to_path_buf(),
        source,
    })?;

    atomic_write(&path, json.as_bytes())?;
    Ok(path)
}

/// Scrive `content` in `path` in modo **atomico**: il contenuto è prima scritto
/// in un file temporaneo nella **stessa directory** (così il `rename` resta sullo
/// stesso filesystem ed è atomico), poi promosso con [`std::fs::rename`]. Un
/// consumatore osserva sempre o il vecchio file completo o il nuovo, mai un file
/// parziale. Rispecchia `atomic_write` del `bindings::generator` (Req 3.6).
fn atomic_write(path: &Path, content: &[u8]) -> Result<(), SurfaceError> {
    use std::sync::atomic::{AtomicU64, Ordering};

    /// Contatore monotòno di processo per nomi di file temporaneo univoci.
    static TMP_COUNTER: AtomicU64 = AtomicU64::new(0);

    let io_err = |source| SurfaceError::WriteIr {
        path: path.to_path_buf(),
        source,
    };

    let parent = path.parent().unwrap_or_else(|| Path::new("."));
    let file_name = path
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or("surface.ir.json");
    let unique = TMP_COUNTER.fetch_add(1, Ordering::Relaxed);
    let tmp_path = parent.join(format!(".{file_name}.{}.{unique}.tmp", std::process::id()));

    // 1) Scrive l'intero contenuto nel file temporaneo.
    if let Err(source) = std::fs::write(&tmp_path, content) {
        let _ = std::fs::remove_file(&tmp_path);
        return Err(io_err(source));
    }

    // 2) Promuove atomicamente il temporaneo a destinazione finale.
    if let Err(source) = std::fs::rename(&tmp_path, path) {
        let _ = std::fs::remove_file(&tmp_path);
        return Err(io_err(source));
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bindings::{GdVersion, TargetPlatform};

    /// Costruisce un `ApiElement` minimale (senza risolvibilità/provenienza) per
    /// i test di serializzazione/ordinamento.
    fn element(canonical: &str, class: &str, ret: &str, params: &[&str]) -> ApiElement {
        ApiElement {
            symbol: SurfaceSymbol::from_canonical(SymbolId::new(canonical)).unwrap(),
            class_name: class.to_owned(),
            signature: CanonicalSignature {
                return_gd: ret.to_owned(),
                param_gds: params.iter().map(|s| (*s).to_owned()).collect(),
            },
            priority: 0,
            is_hook_point: false,
            resolvability: Vec::new(),
            provenance: Vec::new(),
        }
    }

    /// IR di esempio con classi/elementi forniti in ordine **non** canonico, così
    /// da provare che `sorted()` riordina davvero.
    fn sample_ir_unsorted() -> SurfaceIr {
        SurfaceIr {
            classes: vec![
                ClassBinding {
                    name: "PlayLayer".to_owned(),
                    methods: vec![
                        SymbolId::new("PlayLayer::update"),
                        SymbolId::new("PlayLayer::init"),
                    ],
                },
                ClassBinding {
                    name: "MenuLayer".to_owned(),
                    methods: vec![SymbolId::new("MenuLayer::init")],
                },
            ],
            elements: vec![
                element("PlayLayer::update", "PlayLayer", "void", &["PlayLayer*", "float"]),
                element("MenuLayer::init", "MenuLayer", "bool", &["MenuLayer*"]),
                element("PlayLayer::init", "PlayLayer", "bool", &["PlayLayer*"]),
            ],
            type_overrides: Vec::new(),
        }
    }

    #[test]
    fn sorted_orders_classes_by_name_and_methods_by_symbol() {
        let sorted = sample_ir_unsorted().sorted();

        // Classi per nome.
        let names: Vec<&str> = sorted.classes.iter().map(|c| c.name.as_str()).collect();
        assert_eq!(names, vec!["MenuLayer", "PlayLayer"]);

        // Metodi di ogni classe per SymbolId.
        let play = sorted.classes.iter().find(|c| c.name == "PlayLayer").unwrap();
        assert_eq!(
            play.methods,
            vec![SymbolId::new("PlayLayer::init"), SymbolId::new("PlayLayer::update")]
        );
    }

    #[test]
    fn sorted_orders_elements_by_symbol_id() {
        let sorted = sample_ir_unsorted().sorted();
        let canon: Vec<&str> = sorted
            .elements
            .iter()
            .map(|e| e.symbol.canonical.as_str())
            .collect();
        assert_eq!(
            canon,
            vec!["MenuLayer::init", "PlayLayer::init", "PlayLayer::update"]
        );
    }

    #[test]
    fn sorted_is_idempotent() {
        let once = sample_ir_unsorted().sorted();
        let twice = once.sorted();
        assert_eq!(once, twice);
    }

    #[test]
    fn to_json_is_byte_identical_across_serializations() {
        // Determinismo: serializzare la stessa IR due volte dà JSON byte-identico.
        let ir = sample_ir_unsorted();
        let a = to_json(&ir).unwrap();
        let b = to_json(&ir).unwrap();
        assert_eq!(a, b);
    }

    #[test]
    fn to_json_is_invariant_to_input_ordering() {
        // Due IR con lo stesso contenuto ma ordine d'inserimento diverso
        // producono lo stesso JSON canonico (l'ordinamento è applicato prima).
        let unsorted = sample_ir_unsorted();
        let pre_sorted = unsorted.sorted();
        assert_eq!(to_json(&unsorted).unwrap(), to_json(&pre_sorted).unwrap());
    }

    #[test]
    fn json_round_trips_to_the_sorted_ir() {
        // Round-trip: deserializzare il JSON ridà esattamente la IR ordinata.
        let ir = sample_ir_unsorted();
        let json = to_json(&ir).unwrap();
        let back: SurfaceIr = serde_json::from_str(&json).unwrap();
        assert_eq!(back, ir.sorted());
    }

    #[test]
    fn json_contains_gd_types_and_no_cpp_mapping() {
        // Language-agnostic (Req 10.2): il JSON porta i TIPI GD del catalogo
        // (es. "MenuLayer*") e NESSUN tipo C++ mappato (es. "pulse::gd::...").
        let ir = sample_ir_unsorted();
        let json = to_json(&ir).unwrap();
        assert!(json.contains("MenuLayer*"), "deve contenere il tipo GD this-first");
        assert!(json.contains("PlayLayer*"));
        assert!(
            !json.contains("pulse::gd::"),
            "la IR non deve contenere alcun tipo C++ mappato"
        );
    }

    #[test]
    fn type_overrides_are_carried_as_data_without_cpp_mapping_applied() {
        // Gli override sono trasportati come DATI (gd/cpp) ma la IR non li applica:
        // le firme degli elementi restano a tipi GD.
        let mut ir = sample_ir_unsorted();
        ir.type_overrides = vec![TypeMapRule {
            gd: "cocos2d::CCObject*".to_owned(),
            cpp: "pulse::gd::CCObject*".to_owned(),
        }];
        let json = to_json(&ir).unwrap();

        // L'override è presente come dato.
        assert!(json.contains("cocos2d::CCObject*"));
        assert!(json.contains("pulse::gd::CCObject*"));

        // Ma nessuna firma di elemento è stata mappata a C++: i param restano GD.
        let menu = ir
            .elements
            .iter()
            .find(|e| e.symbol.canonical == SymbolId::new("MenuLayer::init"))
            .unwrap();
        assert_eq!(menu.signature.param_gds, vec!["MenuLayer*".to_owned()]);
    }

    #[test]
    fn pair_serde_round_trips_through_canonical_string() {
        // Lo shim TargetPair↔stringa fa round-trip esatto quando i vettori sono
        // popolati (qui simulato a mano, dato che in compilazione sono vuoti).
        let pair = TargetPair::new(GdVersion::new(2, 2081), TargetPlatform::MacosArm64);
        let mut ir = sample_ir_unsorted();
        ir.elements[0].resolvability = vec![PairResolvability {
            pair,
            resolvable: true,
        }];
        ir.elements[0].provenance = vec![ProvenanceRef { pair, complete: false }];

        let json = to_json(&ir).unwrap();
        // La coppia è serializzata nella forma testuale canonica.
        assert!(json.contains("2.2081/macos-arm64"));

        let back: SurfaceIr = serde_json::from_str(&json).unwrap();
        assert_eq!(back, ir.sorted());
    }

    #[test]
    fn write_ir_writes_to_canonical_path_and_is_deterministic() {
        // Scrive su una directory temporanea, verifica il percorso canonico e
        // che due scritture diano contenuti byte-identici (determinismo), senza
        // lasciare file temporanei a terra.
        let root = std::env::temp_dir().join(format!(
            "pulse-surface-ir-{}-{}",
            std::process::id(),
            SymbolId::new("x").as_str().len()
        ));
        let _ = std::fs::remove_dir_all(&root);

        let ir = sample_ir_unsorted();
        let path = write_ir(&ir, &root).unwrap();
        assert_eq!(path, root.join("surface").join("surface.ir.json"));

        let first = std::fs::read(&path).unwrap();
        // Il contenuto on-disk coincide con to_json (ordinato, pretty).
        assert_eq!(String::from_utf8(first.clone()).unwrap(), to_json(&ir).unwrap());

        // Seconda scrittura: byte-identica e nessun residuo `.tmp`.
        let path2 = write_ir(&ir, &root).unwrap();
        let second = std::fs::read(&path2).unwrap();
        assert_eq!(first, second);

        let leftovers: Vec<_> = std::fs::read_dir(root.join("surface"))
            .unwrap()
            .filter_map(|e| e.ok())
            .filter(|e| e.path().extension().and_then(|x| x.to_str()) == Some("tmp"))
            .collect();
        assert!(leftovers.is_empty(), "nessun file .tmp deve restare: {leftovers:?}");

        let _ = std::fs::remove_dir_all(&root);
    }
}
