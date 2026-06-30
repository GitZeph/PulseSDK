//! Cpp_Generator — emissione **header-only deterministica** dell'API SDK C++
//! a partire dalla [`SurfaceIr`](crate::surface::ir::SurfaceIr) (Fase D).
//!
//! Questo è l'**unico** punto in cui i tipi GD della superficie vengono mappati
//! sui tipi C++ (Req 3.3): la `Surface_IR` resta language-agnostic (porta solo
//! tipi GD), e la mappatura vive **qui**, così che un futuro generatore per un
//! altro linguaggio applichi la **propria** mappatura sulla stessa IR (Req 10.2,
//! 10.3). Il generatore consuma la IR ed emette **tre** header-only:
//!
//! - **`pulse/gd/types.gen.hpp`** — una forward-declaration per ogni *pointee*
//!   opaco osservato nelle firme (`namespace pulse::gd { struct MenuLayer; }`),
//!   **ordinate e deduplicate** (Req 3.3, 3.4).
//! - **`pulse/gd/bindings.gen.hpp`** — per ogni `API_Element` una specializzazione
//!   `BindingTraits<FixedString("Class_method")>` il cui `using Fn =
//!   Ret(ClassPtr, Params…)` è **derivato** dalla [`CanonicalSignature`] con il
//!   puntatore alla classe ricevente come **primo** parametro (this-first), e il
//!   marcatore d'appartenenza `in_surface<token>()` / `kPulseGdInSurface_<token>`
//!   emesso **se e solo se** il token è in superficie (Req 3.1, 3.2, 5.3, 5.4).
//! - **`pulse/gd/hooks.gen.hpp`** — la macro ergonomica `PULSE_GD_HOOK(Class,
//!   Method, Ret, Params)` (più la variante con priorità) che ponte fra il
//!   `cpp_token` (chiave dei `BindingTraits`, verifica di firma a compile-time)
//!   e il **simbolo canonico** (registrazione runtime), riusando i primitivi
//!   dello SDK (`FixedString`, `SignatureMatches`, `register_hook`,
//!   `HookRegistration`, `callOriginal`) (Req 4.x, 5.x).
//!
//! Ogni header è una **funzione pura** `SurfaceIr → String` con elementi
//! **ordinati per `SymbolId`** (via [`SurfaceIr::sorted`]), **byte-identica** fra
//! due esecuzioni a parità di IR (Req 3.5). La scrittura su disco è **atomica**
//! (temp + `rename`), nella stessa disciplina fail-closed di `ir.rs`/`generator.rs`
//! (Req 10.1): su errore l'eventuale `.gen.hpp` precedente resta intatto
//! byte-per-byte e nessun file temporaneo parziale è lasciato a terra.
//!
//! ## Mappatura della doppia identità (token ↔ canonico)
//!
//! La macro generata `PULSE_GD_HOOK(Class, Method, …)` **non** richiama
//! `PULSE_HOOK` dello SDK così com'è: `PULSE_HOOK` lega il **medesimo**
//! identificatore sia alla chiave di verifica della firma a compile-time sia
//! alla stringa di registrazione runtime. La superficie GD ha invece una
//! **doppia identità** (vedi [`crate::surface::SurfaceSymbol`]):
//!
//! - il **`cpp_token`** (`"MenuLayer_init"`) indicizza `BindingTraits` e alimenta
//!   `SignatureMatches` (verifica di firma a compile-time, Req 5.1, 5.2) e
//!   `in_surface` (appartenenza alla superficie, Req 5.3);
//! - il **simbolo canonico** (`"MenuLayer::init"`) è quello registrato presso il
//!   registro globale con `register_hook` e risolto a runtime contro il `.pbind`
//!   (Req 4.1) — esattamente come `examples/allhooks-mod`.
//!
//! Perciò la macro **replica** la struttura di `PULSE_HOOK` (namespace dedicato,
//! slot del trampolino, `callOriginal`, registrazione `used` a prova di
//! dead-strip) **riusando i primitivi dello SDK**, ma separa le due identità:
//! verifica la firma sul token e registra sul canonico. Così la firma **non** è
//! mai duplicata fuori dal `Binding_Catalog` (Req 5.4).
//!
//! _Requisiti: 3.1, 3.2, 3.3, 3.4, 3.5, 4.1, 4.2, 4.4, 4.5, 4.6, 5.1, 5.2, 5.3,
//! 5.4, 10.1._

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

use super::ir::SurfaceIr;
use super::manifest::TypeMapRule;
use super::SurfaceError;

// ---------------------------------------------------------------------------
// 7.1 — CppType e map_type (mappatura totale GD → C++).
// ---------------------------------------------------------------------------

/// Il tipo C++ risultante dalla mappatura di un tipo GD (Req 3.3).
///
/// È **chiuso** su due forme:
/// - [`CppType::Primitive`] — un tipo primitivo reso per **identità** (es.
///   `bool`, `int`, `void`, `float`, `double`, `void*`), trasportato verbatim;
/// - [`CppType::Opaque`] — un tipo GD non primitivo reso come **puntatore a un
///   tipo incompleto** forward-declared nel namespace `pulse::gd` (es.
///   `MenuLayer*` → `pulse::gd::MenuLayer*`, con `struct MenuLayer;` in
///   `types.gen.hpp`). Il `pointee` è il **nome foglia** (qualificatori di
///   namespace rimossi), così che il forward-declare viva sotto l'unico
///   namespace `pulse::gd` dello SDK.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CppType {
    /// Tipo primitivo reso per identità (es. `"bool"`, `"int"`, `"void"`).
    Primitive(String),
    /// Tipo opaco: puntatore (o valore) a un tipo incompleto forward-declared
    /// nel namespace `pulse::gd`.
    Opaque {
        /// Nome foglia del tipo (senza qualificatori di namespace), es.
        /// `"MenuLayer"`. Il forward-declare emesso è `struct {pointee};`.
        pointee: String,
        /// `true` se il tipo GD era un puntatore (terminava con `*`).
        is_pointer: bool,
    },
}

impl CppType {
    /// Rende il [`CppType`] come stringa di tipo C++ utilizzabile in una firma.
    ///
    /// - [`CppType::Primitive`] → la stringa verbatim (es. `"bool"`, `"void*"`);
    /// - [`CppType::Opaque`] → `pulse::gd::{pointee}` con un `*` finale se
    ///   puntatore (es. `pulse::gd::MenuLayer*`).
    pub fn render(&self) -> String {
        match self {
            CppType::Primitive(name) => name.clone(),
            CppType::Opaque {
                pointee,
                is_pointer,
            } => {
                if *is_pointer {
                    format!("pulse::gd::{pointee}*")
                } else {
                    format!("pulse::gd::{pointee}")
                }
            }
        }
    }
}

/// Mappa un tipo GD della [`CanonicalSignature`] su un [`CppType`] (Req 3.3).
///
/// È una funzione **totale**: per *qualsiasi* stringa in ingresso restituisce un
/// [`CppType`] (mai indefinita, mai panica). La precedenza è:
///
/// 1. **Override del manifest** (trasportati nella IR come dati, Req 10.2):
///    se esiste una [`TypeMapRule`] con `rule.gd == gd_type`, il tipo è derivato
///    da `rule.cpp` (autorevole). Se la base di `rule.cpp` è un primitivo noto,
///    il valore è trasportato verbatim ([`CppType::Primitive`]); altrimenti è un
///    [`CppType::Opaque`] sul nome foglia di `rule.cpp`.
/// 2. **Primitivi noti** per identità: `bool`, `int`, `float`, `void`, `double`,
///    `char`, `unsigned int`, … (vedi [`is_known_primitive`]). Il primitivo è
///    reso verbatim, preservando un eventuale `*` (es. `void*`).
/// 3. **Tipo opaco** altrimenti: un tipo come `MenuLayer*` o `cocos2d::CCObject*`
///    diventa un **puntatore a tipo incompleto** forward-declared nel namespace
///    `pulse::gd`, sul **nome foglia** (qualificatori `::` rimossi).
///
/// **Regola di rilevamento del puntatore e dei qualificatori (documentata):** un
/// `*` finale (eventualmente preceduto da spazi) marca `is_pointer = true`; la
/// base è il tipo senza i `*` finali. Per i tipi opachi, il `pointee` è il
/// segmento dopo l'ultimo `::` (nome foglia), così `cocos2d::CCObject` e
/// `pulse::gd::CCObject` collassano entrambi su `CCObject` sotto `pulse::gd`.
pub fn map_type(gd_type: &str, overrides: &[TypeMapRule]) -> CppType {
    // (1) Override del manifest applicati per primi (Req 3.3, 10.2).
    if let Some(rule) = overrides.iter().find(|rule| rule.gd == gd_type) {
        let (base, is_pointer) = split_pointer(&rule.cpp);
        if is_known_primitive(base) {
            // Es. override verso un primitivo: trasportato verbatim.
            return CppType::Primitive(rule.cpp.trim().to_owned());
        }
        return CppType::Opaque {
            pointee: leaf_name(base).to_owned(),
            is_pointer,
        };
    }

    // (2) Primitivi noti per identità (verbatim, preservando un eventuale `*`).
    let (base, is_pointer) = split_pointer(gd_type);
    if is_known_primitive(base) {
        return CppType::Primitive(gd_type.trim().to_owned());
    }

    // (3) Altrimenti: tipo opaco, puntatore a incompleto forward-declared.
    CppType::Opaque {
        pointee: leaf_name(base).to_owned(),
        is_pointer,
    }
}

/// Divide un tipo nella coppia `(base, is_pointer)`: rimuove i `*` finali
/// (e gli spazi attorno) e segnala se almeno un `*` era presente.
fn split_pointer(ty: &str) -> (&str, bool) {
    let trimmed = ty.trim();
    let base = trimmed.trim_end_matches('*').trim();
    let is_pointer = base.len() != trimmed.len();
    (base, is_pointer)
}

/// Restituisce il **nome foglia** di un tipo qualificato: il segmento dopo
/// l'ultimo separatore di scope `::` (es. `cocos2d::CCObject` → `CCObject`).
fn leaf_name(base: &str) -> &str {
    match base.rfind("::") {
        Some(idx) => &base[idx + 2..],
        None => base,
    }
}

/// `true` se `base` è un tipo primitivo C++ noto, reso per identità (Req 3.3).
///
/// Insieme volutamente conservativo ma sensato (include `void`): ciò che non vi
/// compare è trattato come tipo opaco forward-declared.
fn is_known_primitive(base: &str) -> bool {
    matches!(
        base,
        "void"
            | "bool"
            | "char"
            | "signed char"
            | "unsigned char"
            | "short"
            | "unsigned short"
            | "int"
            | "unsigned int"
            | "unsigned"
            | "long"
            | "unsigned long"
            | "long long"
            | "unsigned long long"
            | "float"
            | "double"
            | "long double"
            | "wchar_t"
            | "char8_t"
            | "char16_t"
            | "char32_t"
            | "int8_t"
            | "int16_t"
            | "int32_t"
            | "int64_t"
            | "uint8_t"
            | "uint16_t"
            | "uint32_t"
            | "uint64_t"
            | "size_t"
            | "ssize_t"
            | "intptr_t"
            | "uintptr_t"
            | "ptrdiff_t"
            | "std::size_t"
            | "std::int32_t"
            | "std::int64_t"
            | "std::uint32_t"
            | "std::uint64_t"
    )
}

// ---------------------------------------------------------------------------
// Helpers di firma: deriva i tipi C++ di un API_Element dalla CanonicalSignature.
// ---------------------------------------------------------------------------

/// Rende la firma C++ `Ret(Param0, Param1, …)` di un `API_Element` derivandola
/// **esclusivamente** dalla [`CanonicalSignature`] del catalogo via [`map_type`]
/// (Req 5.4). La firma del catalogo è già **this-first** (il puntatore alla
/// classe ricevente è il primo parametro), perciò i parametri sono mappati
/// nell'ordine dato, senza riordinare né iniettare alcun `this` aggiuntivo.
fn render_fn_type(signature: &super::ir::CanonicalSignature, overrides: &[TypeMapRule]) -> String {
    let ret = map_type(&signature.return_gd, overrides).render();
    let params: Vec<String> = signature
        .param_gds
        .iter()
        .map(|gd| map_type(gd, overrides).render())
        .collect();
    format!("{ret}({})", params.join(", "))
}

// ---------------------------------------------------------------------------
// 7.2 — types.gen.hpp (forward-declaration dei tipi opachi).
// ---------------------------------------------------------------------------

/// Raccoglie, **ordinati e deduplicati**, i nomi foglia di tutti i *pointee*
/// opachi osservati nelle firme (ritorno + parametri) di tutti gli elementi.
fn collect_opaque_pointees(ir: &SurfaceIr) -> BTreeSet<String> {
    let mut pointees = BTreeSet::new();
    for element in &ir.elements {
        let mut consider = |gd: &str| {
            if let CppType::Opaque { pointee, .. } = map_type(gd, &ir.type_overrides) {
                if !pointee.is_empty() {
                    pointees.insert(pointee);
                }
            }
        };
        consider(&element.signature.return_gd);
        for gd in &element.signature.param_gds {
            consider(gd);
        }
    }
    pointees
}

/// Emette `pulse/gd/types.gen.hpp` (Req 3.3, 3.4): header-only, `#pragma once`,
/// con una forward-declaration per ogni tipo opaco osservato, **ordinate e
/// deduplicate**, sotto `namespace pulse::gd`. Funzione **pura** della IR
/// (ordinata internamente per determinismo, Req 3.5).
pub fn render_types_header(ir: &SurfaceIr) -> String {
    let ir = ir.sorted();
    let pointees = collect_opaque_pointees(&ir);

    let mut out = String::new();
    out.push_str(&gen_banner("types.gen.hpp"));
    out.push_str(
        "// Forward-declaration dei tipi GD opachi osservati nelle firme della\n\
         // superficie (Req 3.3, 3.4). Header-only: nessuna definizione completa\n\
         // dei tipi GD — il Developer ne usa solo il puntatore opaco.\n",
    );
    out.push_str("#pragma once\n\n");
    out.push_str("namespace pulse::gd {\n");
    for pointee in &pointees {
        out.push_str(&format!("struct {pointee};\n"));
    }
    out.push_str("}  // namespace pulse::gd\n");
    out
}

// ---------------------------------------------------------------------------
// 7.3 — bindings.gen.hpp (BindingTraits this-first + marcatore d'appartenenza).
// ---------------------------------------------------------------------------

/// Emette `pulse/gd/bindings.gen.hpp` (Req 3.1, 3.2, 5.3, 5.4): per ogni
/// `API_Element` una specializzazione `BindingTraits<FixedString("token")>` il
/// cui `using Fn` è **derivato** dalla [`CanonicalSignature`] (this-first), più
/// il marcatore d'appartenenza `in_surface<token>()`/`kPulseGdInSurface_<token>`
/// emesso **solo** per i token presenti nella superficie (così un hook su un
/// simbolo non in superficie fallisce il `Build_Check`, Req 5.3). Funzione
/// **pura** e deterministica (elementi ordinati per `SymbolId`).
pub fn render_bindings_header(ir: &SurfaceIr) -> String {
    let ir = ir.sorted();

    let mut out = String::new();
    out.push_str(&gen_banner("bindings.gen.hpp"));
    out.push_str(
        "// Specializzazioni `BindingTraits` (firma canonica this-first, derivata\n\
         // dal Binding_Catalog, Req 3.1/3.2/5.4) e marcatori d'appartenenza alla\n\
         // superficie per il Build_Check (Req 5.3).\n",
    );
    out.push_str("#pragma once\n\n");
    out.push_str("#include <pulse/hooks.hpp>\n\n");
    out.push_str("#include \"types.gen.hpp\"\n\n");

    // (a) Specializzazioni BindingTraits: `using Fn = Ret(ClassPtr, Params…)`.
    out.push_str("// --- BindingTraits: firma canonica per token (this-first) ---\n");
    for element in &ir.elements {
        let token = &element.symbol.cpp_token;
        let fn_type = render_fn_type(&element.signature, &ir.type_overrides);
        out.push_str(&format!(
            "template <>\n\
             struct pulse::hooks::BindingTraits<pulse::hooks::FixedString(\"{token}\")> {{\n\
             {INDENT}using Fn = {fn_type};\n\
             }};\n",
            token = token,
            fn_type = fn_type,
            INDENT = "    ",
        ));
    }
    out.push('\n');

    // (b) Marcatore d'appartenenza: `in_surface<token>()` true SOLO per i token
    //     in superficie; il template primario è false (Req 5.3).
    out.push_str("// --- Marcatore d'appartenenza alla superficie (Build_Check, Req 5.3) ---\n");
    out.push_str("namespace pulse::gd::detail {\n");
    out.push_str(
        "// Template primario: un token NON in superficie è `false` (fail-closed).\n\
         template <::pulse::hooks::FixedString Token>\n\
         consteval bool in_surface() { return false; }\n",
    );
    for element in &ir.elements {
        let token = &element.symbol.cpp_token;
        out.push_str(&format!(
            "template <>\n\
             consteval bool in_surface<::pulse::hooks::FixedString(\"{token}\")>() {{ return true; }}\n",
        ));
    }
    out.push_str("}  // namespace pulse::gd::detail\n\n");

    // (c) Costanti d'appartenenza leggibili (una per token in superficie).
    out.push_str("// --- Costanti d'appartenenza (una per token in superficie) ---\n");
    for element in &ir.elements {
        let token = &element.symbol.cpp_token;
        out.push_str(&format!(
            "inline constexpr bool kPulseGdInSurface_{token} = true;\n"
        ));
    }
    out
}

// ---------------------------------------------------------------------------
// 7.4 — hooks.gen.hpp (macro PULSE_GD_HOOK, registrazione canonica, Build_Check).
// ---------------------------------------------------------------------------

/// Emette `pulse/gd/hooks.gen.hpp` (Req 4.1, 4.2, 4.4, 4.5, 4.6, 5.1, 5.2, 5.3):
/// la macro ergonomica `PULSE_GD_HOOK(Class, Method, Ret, Params)` e la variante
/// `PULSE_GD_HOOK_PRIORITY(Class, Method, Priority, Ret, Params)`.
///
/// La macro **ponte** fra le due identità (vedi doc di modulo): verifica la firma
/// a compile-time sul `cpp_token` (`SignatureMatches`, Req 5.1/5.2) e
/// l'appartenenza alla superficie (`in_surface`, Req 5.3), poi **registra il
/// simbolo canonico** `"Class::method"` con `register_hook` (percorso esplicito
/// come `allhooks`, **nessun letterale di indirizzo**, Req 4.1), preservando
/// detour, slot del trampolino e `callOriginal` con il valore di ritorno
/// (Req 4.2, 4.4) e la priorità di catena (default 500, esplicita con la variante
/// `_PRIORITY`, Req 4.5/4.6).
///
/// Il corpo della macro è **costante** (indipendente dalla IR); gli `Hook_Point`
/// della superficie sono elencati come commenti di documentazione **ordinati per
/// `SymbolId`** con il loro simbolo canonico e la firma derivata, per determinismo
/// e auditabilità.
pub fn render_hooks_header(ir: &SurfaceIr) -> String {
    let ir = ir.sorted();

    let mut out = String::new();
    out.push_str(&gen_banner("hooks.gen.hpp"));
    out.push_str(
        "// Macro ergonomica `PULSE_GD_HOOK` (Req 4.x, 5.x). Ponte fra il\n\
         // `cpp_token` (chiave dei BindingTraits, verifica di firma a\n\
         // compile-time) e il simbolo canonico `Class::method` (registrazione\n\
         // runtime via register_hook, come examples/allhooks-mod).\n",
    );
    out.push_str("#pragma once\n\n");
    out.push_str("#include <utility>\n\n");
    out.push_str("#include <pulse/hooks.hpp>\n\n");
    out.push_str("#include \"bindings.gen.hpp\"\n\n");

    out.push_str(MACRO_DEFINITION);
    out.push('\n');

    // Elenco deterministico degli Hook_Point disponibili (solo documentazione).
    out.push_str("// --- Hook_Point disponibili nella superficie (ordinati per SymbolId) ---\n");
    let mut any_hook = false;
    for element in &ir.elements {
        if !element.is_hook_point {
            continue;
        }
        any_hook = true;
        let canonical = element.symbol.canonical.as_str();
        let fn_type = render_fn_type(&element.signature, &ir.type_overrides);
        out.push_str(&format!(
            "// PULSE_GD_HOOK({class_name}, …, {fn_type}) -> registra \"{canonical}\"\n",
            class_name = element.class_name,
            fn_type = fn_type,
            canonical = canonical,
        ));
    }
    if !any_hook {
        out.push_str("// (nessun Hook_Point dichiarato nella superficie)\n");
    }
    out
}

/// Corpo costante delle macro `PULSE_GD_HOOK` / `PULSE_GD_HOOK_PRIORITY`.
///
/// Replica la struttura di `PULSE_HOOK` dello SDK **riusandone i primitivi**
/// (`FixedString`, `SignatureMatches`, `register_hook`, `HookRegistration`,
/// `PULSE_HOOK_USED`, lo schema `callOriginal`/trampolino), ma separa le due
/// identità: verifica la firma sul **token** e registra sul **canonico**.
const MACRO_DEFINITION: &str = r##"// Variante con priorità di catena esplicita (Req 4.6).
#define PULSE_GD_HOOK_PRIORITY(Class, Method, Priority, Ret, Params)            \
    namespace pulse_gd_hook_##Class##_##Method {                               \
        /* Tipo della funzione bersaglio (i nomi dei parametri sono ignorati). */ \
        using PulseFn = Ret Params;                                            \
        /* Appartenenza alla superficie: fallisce se il token non è in          \
           superficie (Req 5.3). */                                            \
        static_assert(                                                         \
            ::pulse::gd::detail::in_surface<                                   \
                ::pulse::hooks::FixedString(#Class "_" #Method)>(),            \
            "PULSE_GD_HOOK(" #Class "::" #Method "): API_Element assente "      \
            "dalla GD_API_Surface (simbolo non in superficie).");             \
        /* Verifica della firma a compile-time contro il binding (Req 5.1/5.2). */ \
        static_assert(                                                         \
            ::pulse::hooks::SignatureMatches<                                  \
                ::pulse::hooks::FixedString(#Class "_" #Method), PulseFn>,     \
            "PULSE_GD_HOOK(" #Class "::" #Method "): la firma dichiarata è "    \
            "incompatibile con quella del Binding_Catalog.");                 \
        /* Slot del trampolino: cablato dall'Hooking Engine dopo install(). */  \
        inline PulseFn* pulse_original = nullptr;                              \
        /* Dichiarazione anticipata del detour; il corpo segue la macro. */     \
        Ret pulse_detour Params;                                               \
        /* Invoca l'originale preservando parametri e valore di ritorno (Req 4.2). */ \
        template <class... PulseArgs>                                          \
        inline Ret callOriginal(PulseArgs&&... pulse_args) {                   \
            return pulse_original(std::forward<PulseArgs>(pulse_args)...);     \
        }                                                                      \
        /* Registrazione SUL SIMBOLO CANONICO (Req 4.1), come allhooks: nessun  \
           letterale di indirizzo. `used` impedisce il dead-strip (Req 4.4). */ \
        PULSE_HOOK_USED                                                        \
        inline const ::pulse::hooks::HookRegistration pulse_registration =     \
            ::pulse::hooks::register_hook(                                     \
                #Class "::" #Method,                                          \
                reinterpret_cast<void*>(&pulse_detour),                        \
                reinterpret_cast<void**>(&pulse_original), (Priority));        \
    }                                                                          \
    Ret pulse_gd_hook_##Class##_##Method::pulse_detour Params

// Forma minimale: priorità di catena di default (500, Req 4.5).
#define PULSE_GD_HOOK(Class, Method, Ret, Params)                              \
    PULSE_GD_HOOK_PRIORITY(Class, Method, 500, Ret, Params)
"##;

// ---------------------------------------------------------------------------
// 7.5 — Orchestrazione, determinismo e scrittura atomica.
// ---------------------------------------------------------------------------

/// Esito della generazione: i percorsi degli header scritti su disco.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GenReport {
    /// Percorsi dei `.gen.hpp` scritti, nell'ordine `types`, `bindings`, `hooks`.
    pub written: Vec<PathBuf>,
}

/// Percorso della directory degli header generati: `out_root/sdk/include/pulse/gd`.
fn gd_include_dir(out_root: &Path) -> PathBuf {
    out_root
        .join("sdk")
        .join("include")
        .join("pulse")
        .join("gd")
}

/// Genera i tre header header-only sotto `out_root/sdk/include/pulse/gd/`
/// (Req 3.4, 3.5, 10.1).
///
/// Ogni header è prodotto da una **funzione pura** `SurfaceIr → String` con
/// elementi ordinati per `SymbolId` (determinismo, output byte-identico fra due
/// esecuzioni a parità di IR). La scrittura è **atomica** (temp + `rename`): su
/// errore il `.gen.hpp` precedente resta intatto byte-per-byte e nessun file
/// temporaneo parziale è lasciato a terra (fail-closed). La consegna dello SDK
/// C++ avviene esclusivamente tramite questi header (Req 10.1).
pub fn generate_cpp(ir: &SurfaceIr, out_root: &Path) -> Result<GenReport, SurfaceError> {
    let dir = gd_include_dir(out_root);
    std::fs::create_dir_all(&dir).map_err(|source| SurfaceError::GenerateCpp {
        path: dir.clone(),
        source,
    })?;

    let artifacts = [
        ("types.gen.hpp", render_types_header(ir)),
        ("bindings.gen.hpp", render_bindings_header(ir)),
        ("hooks.gen.hpp", render_hooks_header(ir)),
    ];

    let mut written = Vec::with_capacity(artifacts.len());
    for (name, content) in &artifacts {
        let path = dir.join(name);
        atomic_write(&path, content.as_bytes())?;
        written.push(path);
    }

    Ok(GenReport { written })
}

/// Intestazione comune di un header generato (segnala l'origine automatica e che
/// è header-only). È **costante** e non dipende dal tempo, così da preservare il
/// determinismo byte-identico (Req 3.5).
fn gen_banner(file_name: &str) -> String {
    format!(
        "// {file_name} — GENERATO da `pulse surface generate` (Cpp_Generator).\n\
         // NON modificare a mano: rigenerato deterministicamente dalla Surface_IR.\n\
         // Header-only, parte dell'API SDK C++ di Pulse (Req 3.4, 10.1).\n",
    )
}

/// Scrive `content` in `path` in modo **atomico** (temp + `rename`), nella stessa
/// disciplina fail-closed di `ir.rs`/`generator.rs` (Req 10.1): il contenuto è
/// scritto in un file temporaneo nella **stessa directory** (così il `rename` è
/// sullo stesso filesystem ed è atomico), poi promosso. Un consumatore osserva
/// sempre o il vecchio file completo o il nuovo, mai un file parziale.
fn atomic_write(path: &Path, content: &[u8]) -> Result<(), SurfaceError> {
    use std::sync::atomic::{AtomicU64, Ordering};

    /// Contatore monotòno di processo per nomi di file temporaneo univoci.
    static TMP_COUNTER: AtomicU64 = AtomicU64::new(0);

    let io_err = |source| SurfaceError::GenerateCpp {
        path: path.to_path_buf(),
        source,
    };

    let parent = path.parent().unwrap_or_else(|| Path::new("."));
    let file_name = path
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or("header.gen.hpp");
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
    use crate::surface::ir::{ApiElement, CanonicalSignature, ClassBinding};
    use crate::surface::{SurfaceSymbol, SymbolId};

    // -- Costruttori di supporto -------------------------------------------------

    /// Costruisce un `ApiElement` minimale per i test del generatore.
    fn element(canonical: &str, class: &str, ret: &str, params: &[&str], hook: bool) -> ApiElement {
        ApiElement {
            symbol: SurfaceSymbol::from_canonical(SymbolId::new(canonical)).unwrap(),
            class_name: class.to_owned(),
            signature: CanonicalSignature {
                return_gd: ret.to_owned(),
                param_gds: params.iter().map(|s| (*s).to_owned()).collect(),
            },
            priority: 100,
            is_hook_point: hook,
            resolvability: Vec::new(),
            provenance: Vec::new(),
        }
    }

    /// IR seed (`MenuLayer::init`) per il Prioritized_Target.
    fn seed_ir() -> SurfaceIr {
        SurfaceIr {
            classes: vec![ClassBinding {
                name: "MenuLayer".to_owned(),
                methods: vec![SymbolId::new("MenuLayer::init")],
            }],
            elements: vec![element(
                "MenuLayer::init",
                "MenuLayer",
                "bool",
                &["MenuLayer*"],
                true,
            )],
            type_overrides: Vec::new(),
        }
    }

    // -- 7.1: map_type totalità (override / primitivo / opaco) -------------------

    #[test]
    fn map_type_applies_manifest_override_first() {
        let overrides = vec![TypeMapRule {
            gd: "cocos2d::CCObject*".to_owned(),
            cpp: "pulse::gd::CCObject*".to_owned(),
        }];
        let mapped = map_type("cocos2d::CCObject*", &overrides);
        assert_eq!(
            mapped,
            CppType::Opaque {
                pointee: "CCObject".to_owned(),
                is_pointer: true,
            }
        );
        // L'override del design produce esattamente `pulse::gd::CCObject*`.
        assert_eq!(mapped.render(), "pulse::gd::CCObject*");
    }

    #[test]
    fn map_type_override_to_primitive_is_verbatim() {
        let overrides = vec![TypeMapRule {
            gd: "SomeEnum".to_owned(),
            cpp: "int".to_owned(),
        }];
        assert_eq!(
            map_type("SomeEnum", &overrides),
            CppType::Primitive("int".to_owned())
        );
    }

    #[test]
    fn map_type_identity_for_known_primitives() {
        for prim in ["bool", "int", "float", "void", "double", "char", "unsigned int"] {
            assert_eq!(
                map_type(prim, &[]),
                CppType::Primitive(prim.to_owned()),
                "il primitivo noto {prim} deve mappare per identità"
            );
            assert_eq!(map_type(prim, &[]).render(), prim);
        }
    }

    #[test]
    fn map_type_pointer_to_primitive_kept_verbatim() {
        assert_eq!(
            map_type("void*", &[]),
            CppType::Primitive("void*".to_owned())
        );
    }

    #[test]
    fn map_type_opaque_pointer_detects_pointer_and_strips_namespace() {
        // `MenuLayer*` → pointee `MenuLayer`, is_pointer true.
        assert_eq!(
            map_type("MenuLayer*", &[]),
            CppType::Opaque {
                pointee: "MenuLayer".to_owned(),
                is_pointer: true,
            }
        );
        assert_eq!(map_type("MenuLayer*", &[]).render(), "pulse::gd::MenuLayer*");

        // Qualificatori di namespace ridotti al nome foglia.
        assert_eq!(
            map_type("cocos2d::CCNode*", &[]),
            CppType::Opaque {
                pointee: "CCNode".to_owned(),
                is_pointer: true,
            }
        );
    }

    #[test]
    fn map_type_is_total_never_panics() {
        // Funzione totale: qualunque input produce un CppType (mai panico).
        for input in ["", "   ", "*", "Foo", "a::b::c", "weird type with spaces", "T**"] {
            let _ = map_type(input, &[]);
        }
    }

    // -- 7.2: types.gen.hpp dedup + sorted ---------------------------------------

    #[test]
    fn types_header_is_deduped_and_sorted_header_only() {
        let ir = SurfaceIr {
            classes: vec![],
            elements: vec![
                // PlayLayer compare due volte (ret + param) → dedup.
                element("PlayLayer::update", "PlayLayer", "PlayLayer*", &["PlayLayer*", "float"], false),
                element("MenuLayer::init", "MenuLayer", "bool", &["MenuLayer*"], true),
            ],
            type_overrides: Vec::new(),
        };
        let header = render_types_header(&ir);

        assert!(header.contains("#pragma once"), "deve essere header-only");
        assert!(header.contains("namespace pulse::gd {"));
        assert!(header.contains("struct MenuLayer;"));
        assert!(header.contains("struct PlayLayer;"));

        // Dedup: una sola forward-declaration di PlayLayer.
        assert_eq!(header.matches("struct PlayLayer;").count(), 1);

        // Ordinate: MenuLayer prima di PlayLayer.
        let menu = header.find("struct MenuLayer;").unwrap();
        let play = header.find("struct PlayLayer;").unwrap();
        assert!(menu < play, "le forward-declaration devono essere ordinate");

        // I primitivi non generano forward-declaration.
        assert!(!header.contains("struct float;"));
        assert!(!header.contains("struct bool;"));
    }

    // -- 7.3: BindingTraits this-first + derivato; marcatore solo se in superficie

    #[test]
    fn bindings_header_fn_is_this_first_and_derived() {
        let header = render_bindings_header(&seed_ir());

        // Specializzazione sul cpp_token, Fn derivato this-first dalla firma.
        assert!(header.contains(
            "struct pulse::hooks::BindingTraits<pulse::hooks::FixedString(\"MenuLayer_init\")>"
        ));
        assert!(
            header.contains("using Fn = bool(pulse::gd::MenuLayer*);"),
            "Fn deve essere bool(pulse::gd::MenuLayer*) (this-first, derivato): {header}"
        );
    }

    #[test]
    fn bindings_header_emits_in_surface_marker_only_for_surface_tokens() {
        let header = render_bindings_header(&seed_ir());

        // Template primario presente (token non in superficie → false).
        assert!(header.contains("consteval bool in_surface() { return false; }"));

        // Marcatore presente SOLO per il token in superficie.
        assert!(header.contains(
            "consteval bool in_surface<::pulse::hooks::FixedString(\"MenuLayer_init\")>() { return true; }"
        ));
        assert!(header.contains("inline constexpr bool kPulseGdInSurface_MenuLayer_init = true;"));

        // Un token NON in superficie non ha né specializzazione né costante.
        assert!(!header.contains("PlayLayer_update"));
        assert!(!header.contains("kPulseGdInSurface_PlayLayer_update"));
    }

    // -- 7.4: PULSE_GD_HOOK: canonico, niente indirizzo, in_surface static_assert

    #[test]
    fn hooks_header_macro_uses_canonical_string_and_in_surface_assert() {
        let header = render_hooks_header(&seed_ir());

        // La macro è definita.
        assert!(header.contains("#define PULSE_GD_HOOK(Class, Method, Ret, Params)"));
        assert!(header
            .contains("#define PULSE_GD_HOOK_PRIORITY(Class, Method, Priority, Ret, Params)"));

        // static_assert d'appartenenza alla superficie (Req 5.3).
        assert!(header.contains("::pulse::gd::detail::in_surface<"));

        // Verifica di firma a compile-time (Req 5.1/5.2).
        assert!(header.contains("::pulse::hooks::SignatureMatches<"));

        // Registrazione sul SIMBOLO CANONICO `Class::method` (Req 4.1).
        assert!(
            header.contains("#Class \"::\" #Method"),
            "la macro deve registrare il simbolo canonico Class::method"
        );
        assert!(header.contains("::pulse::hooks::register_hook("));

        // callOriginal preservato (Req 4.2).
        assert!(header.contains("callOriginal"));

        // Priorità di default 500 quando non specificata (Req 4.5).
        assert!(header.contains("PULSE_GD_HOOK_PRIORITY(Class, Method, 500, Ret, Params)"));

        // Nessun letterale di indirizzo di memoria (Req 4.1): niente `0x…`.
        assert!(
            !header.to_lowercase().contains("0x"),
            "l'espansione non deve contenere alcun letterale di indirizzo"
        );

        // Il seed elenca l'Hook_Point col suo simbolo canonico.
        assert!(header.contains("registra \"MenuLayer::init\""));
    }

    #[test]
    fn hooks_header_lists_only_hook_points() {
        let ir = SurfaceIr {
            classes: vec![],
            elements: vec![
                element("MenuLayer::init", "MenuLayer", "bool", &["MenuLayer*"], true),
                // Non hook → non elencato fra gli Hook_Point.
                element("PlayLayer::update", "PlayLayer", "void", &["PlayLayer*", "float"], false),
            ],
            type_overrides: Vec::new(),
        };
        let header = render_hooks_header(&ir);
        assert!(header.contains("registra \"MenuLayer::init\""));
        assert!(!header.contains("registra \"PlayLayer::update\""));
    }

    // -- 7.5: determinismo + scrittura atomica -----------------------------------

    #[test]
    fn render_functions_are_byte_identical_across_runs() {
        let ir = seed_ir();
        assert_eq!(render_types_header(&ir), render_types_header(&ir));
        assert_eq!(render_bindings_header(&ir), render_bindings_header(&ir));
        assert_eq!(render_hooks_header(&ir), render_hooks_header(&ir));
    }

    #[test]
    fn render_is_invariant_to_input_element_ordering() {
        // Stessi elementi, ordine d'inserimento diverso → output identico
        // (le funzioni ordinano per SymbolId internamente, Req 3.5).
        let a = SurfaceIr {
            classes: vec![],
            elements: vec![
                element("MenuLayer::init", "MenuLayer", "bool", &["MenuLayer*"], true),
                element("PlayLayer::update", "PlayLayer", "void", &["PlayLayer*", "float"], true),
            ],
            type_overrides: Vec::new(),
        };
        let b = SurfaceIr {
            classes: vec![],
            elements: vec![
                element("PlayLayer::update", "PlayLayer", "void", &["PlayLayer*", "float"], true),
                element("MenuLayer::init", "MenuLayer", "bool", &["MenuLayer*"], true),
            ],
            type_overrides: Vec::new(),
        };
        assert_eq!(render_bindings_header(&a), render_bindings_header(&b));
        assert_eq!(render_hooks_header(&a), render_hooks_header(&b));
        assert_eq!(render_types_header(&a), render_types_header(&b));
    }

    #[test]
    fn generate_cpp_writes_three_headers_and_is_deterministic() {
        let root = std::env::temp_dir().join(format!(
            "pulse-cppgen-{}-{}",
            std::process::id(),
            "seed".len()
        ));
        let _ = std::fs::remove_dir_all(&root);

        let ir = seed_ir();
        let report = generate_cpp(&ir, &root).unwrap();
        assert_eq!(report.written.len(), 3);

        let dir = root
            .join("sdk")
            .join("include")
            .join("pulse")
            .join("gd");
        assert_eq!(report.written[0], dir.join("types.gen.hpp"));
        assert_eq!(report.written[1], dir.join("bindings.gen.hpp"));
        assert_eq!(report.written[2], dir.join("hooks.gen.hpp"));

        // Contenuto on-disk == funzione pura.
        let bindings_first = std::fs::read(&report.written[1]).unwrap();
        assert_eq!(
            String::from_utf8(bindings_first.clone()).unwrap(),
            render_bindings_header(&ir)
        );

        // Seconda esecuzione: byte-identica e nessun residuo `.tmp`.
        let _ = generate_cpp(&ir, &root).unwrap();
        let bindings_second = std::fs::read(dir.join("bindings.gen.hpp")).unwrap();
        assert_eq!(bindings_first, bindings_second);

        let leftovers: Vec<_> = std::fs::read_dir(&dir)
            .unwrap()
            .filter_map(|e| e.ok())
            .filter(|e| e.path().extension().and_then(|x| x.to_str()) == Some("tmp"))
            .collect();
        assert!(leftovers.is_empty(), "nessun file .tmp deve restare: {leftovers:?}");

        let _ = std::fs::remove_dir_all(&root);
    }
}
