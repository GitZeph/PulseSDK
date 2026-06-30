//! Bindings Pipeline — strumentazione build-time/contributore della Pulse_CLI.
//!
//! Questo modulo ospita la pipeline che genera, verifica e distribuisce i file
//! `.pbind` a partire dal Binding_Catalog. Vive **interamente** nella Pulse_CLI
//! (Rust): il loader runtime (C++) resta un puro *consumatore* dei `.pbind`
//! prodotti, attraverso `EmbeddedBindingsProvider`/`OnlineBindingsProvider`. La
//! pipeline **riusa** i componenti del loader, non li re-implementa, e ne
//! rispecchia le convenzioni.
//!
//! Questo file (`mod.rs`) definisce i **tipi condivisi** della pipeline:
//! `SymbolId`, `Signature`, `GdVersion`, `TargetPlatform`, `TargetPair` e il
//! `Sentinel_Value`. I tipi rispecchiano numericamente e per convenzione le
//! controparti C++ del loader:
//!
//! - [`Signature`] ⇄ `pulse::loader::bindings::Signature` (`return` + lista
//!   ordinata di `params`, con la convenzione "il `this` è il primo parametro").
//! - [`GdVersion`] ⇄ `pulse::loader::GdVersion` (`{ major, minor }`).
//! - [`TargetPlatform`] è un insieme **finito** chiuso; il suo
//!   [`TargetPlatform::platform_id`] testuale coincide con quello prodotto da
//!   `version_detector`/`runtime_context` (`platform_id`) e usato nel percorso
//!   `mod-index/bindings/{version}/{platform}.pbind`.
//! - [`SENTINEL_VALUE`] è allineato numericamente a `kPlaceholderSentinel = ~0`
//!   di `loader/bindings/binding_verifier.hpp`.
//!
//! _Requisiti: 1.1, 1.2._

use std::fmt;

use serde::{Deserialize, Serialize};

pub mod catalog;
pub mod contribution;
pub mod crosscheck;
pub mod generator;
pub mod linter;
pub mod prologue;
pub mod provenance;
pub mod validation;

/// Valore placeholder/sentinel che marca un offset non ancora verificato.
///
/// Allineato numericamente al runtime `kPlaceholderSentinel = ~0` di
/// `loader/bindings/binding_verifier.hpp` (tutti i bit a 1). Un offset uguale
/// al `Sentinel_Value` non è **mai** risolvibile: il generatore lo emette
/// sempre con `verified = false` (fail-closed).
pub const SENTINEL_VALUE: u64 = u64::MAX;

// ---------------------------------------------------------------------------
// SymbolId — identificatore univoco di simbolo (Req 1.1).
// ---------------------------------------------------------------------------

/// Identificatore univoco di un simbolo del gioco, es. `"MenuLayer::init"`.
///
/// È l'identità di una `Catalog_Entry`: deve essere univoco per coppia
/// `(GD_Version, Target_Platform)` nel catalogo. La risoluzione lato loader è
/// a corrispondenza **esatta** della stringa (nessun fuzzy-match), coerente con
/// `pulse::loader::bindings::BindingSet::resolve`.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub struct SymbolId(pub String);

impl SymbolId {
    /// Crea un `SymbolId` da qualsiasi sorgente convertibile in `String`.
    pub fn new(symbol: impl Into<String>) -> Self {
        Self(symbol.into())
    }

    /// Restituisce il simbolo come `&str`.
    pub fn as_str(&self) -> &str {
        &self.0
    }
}

impl fmt::Display for SymbolId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl From<&str> for SymbolId {
    fn from(value: &str) -> Self {
        Self(value.to_owned())
    }
}

impl From<String> for SymbolId {
    fn from(value: String) -> Self {
        Self(value)
    }
}

// ---------------------------------------------------------------------------
// Signature — firma di una funzione del gioco (mirror di Signature C++).
// ---------------------------------------------------------------------------

/// Firma di una funzione del gioco: tipo di ritorno + tipi dei parametri.
///
/// Mirror di `pulse::loader::bindings::Signature` (`bindings.hpp`): un
/// `return_type` più una lista **ordinata** di `param_types`. Vale la
/// convenzione "il `this` è il primo parametro" per i metodi membro
/// (es. `MenuLayer::init` → `return_type = "bool"`,
/// `param_types = ["MenuLayer*"]`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Signature {
    /// Tipo di ritorno, es. `"bool"`.
    pub return_type: String,
    /// Tipi dei parametri in ordine; il `this` è il primo per i metodi membro.
    pub param_types: Vec<String>,
}

impl Signature {
    /// Costruisce una `Signature` da tipo di ritorno e tipi dei parametri.
    pub fn new(return_type: impl Into<String>, param_types: Vec<String>) -> Self {
        Self {
            return_type: return_type.into(),
            param_types,
        }
    }
}

// ---------------------------------------------------------------------------
// GdVersion — versione del binario di Geometry Dash (mirror di GdVersion C++).
// ---------------------------------------------------------------------------

/// Versione specifica del binario di Geometry Dash, es. `2.2081` → `{2, 2081}`.
///
/// Mirror di `pulse::loader::GdVersion` (`runtime_context.hpp`): `major`/`minor`
/// con `u32`, uguaglianza esatta campo-per-campo.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub struct GdVersion {
    pub major: u32,
    pub minor: u32,
}

impl GdVersion {
    /// Costruisce una `GdVersion` da `major` e `minor`.
    pub fn new(major: u32, minor: u32) -> Self {
        Self { major, minor }
    }
}

impl fmt::Display for GdVersion {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // Identificatore di versione usato nel percorso `.pbind`
        // (`mod-index/bindings/{version}/...`), es. "2.2081".
        write!(f, "{}.{}", self.major, self.minor)
    }
}

// ---------------------------------------------------------------------------
// TargetPlatform — insieme FINITO chiuso delle piattaforme target (Req 1.1).
// ---------------------------------------------------------------------------

/// Piattaforma target supportata: insieme **finito** chiuso.
///
/// Lo [`TargetPlatform::platform_id`] testuale coincide con quello prodotto da
/// `version_detector`/`runtime_context` (`platform_id`), così la coppia del
/// file `.pbind` combacia con quella del `RuntimeContext`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum TargetPlatform {
    MacosArm64,
    MacosX64,
    WindowsX64,
    AndroidArm64,
    IosArm64,
}

impl TargetPlatform {
    /// Elenco completo delle piattaforme target (insieme finito chiuso).
    pub const ALL: [TargetPlatform; 5] = [
        TargetPlatform::MacosArm64,
        TargetPlatform::MacosX64,
        TargetPlatform::WindowsX64,
        TargetPlatform::AndroidArm64,
        TargetPlatform::IosArm64,
    ];

    /// Identificatore testuale stabile `"<os>-<arch>"`, coincidente con quello
    /// di `version_detector`/`runtime_context` (`platform_id`).
    pub fn platform_id(self) -> &'static str {
        match self {
            TargetPlatform::MacosArm64 => "macos-arm64",
            TargetPlatform::MacosX64 => "macos-x64",
            TargetPlatform::WindowsX64 => "windows-x64",
            TargetPlatform::AndroidArm64 => "android-arm64",
            TargetPlatform::IosArm64 => "ios-arm64",
        }
    }

    /// Inverso di [`TargetPlatform::platform_id`]: dalla stringa del percorso
    /// `.pbind` alla variante. Restituisce `None` per identificatori non
    /// appartenenti all'insieme finito chiuso.
    pub fn from_platform_id(id: &str) -> Option<TargetPlatform> {
        TargetPlatform::ALL
            .iter()
            .copied()
            .find(|platform| platform.platform_id() == id)
    }
}

impl fmt::Display for TargetPlatform {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.platform_id())
    }
}

impl Serialize for TargetPlatform {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        serializer.serialize_str(self.platform_id())
    }
}

impl<'de> Deserialize<'de> for TargetPlatform {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let id = String::deserialize(deserializer)?;
        TargetPlatform::from_platform_id(&id).ok_or_else(|| {
            serde::de::Error::custom(format!("Target_Platform non supportata: {id:?}"))
        })
    }
}

// ---------------------------------------------------------------------------
// TargetPair — coppia (GD_Version, Target_Platform) (Req 1.1).
// ---------------------------------------------------------------------------

/// Coppia univoca `(GD_Version, Target_Platform)` che indicizza gli offset del
/// catalogo ed è la chiave del percorso `mod-index/bindings/{version}/{platform}.pbind`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct TargetPair {
    pub gd: GdVersion,
    pub platform: TargetPlatform,
}

impl TargetPair {
    /// Costruisce una `TargetPair` da `GD_Version` e `Target_Platform`.
    pub fn new(gd: GdVersion, platform: TargetPlatform) -> Self {
        Self { gd, platform }
    }
}

impl fmt::Display for TargetPair {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // Coerente col percorso `.pbind`: "{version}/{platform}".
        write!(f, "{}/{}", self.gd, self.platform.platform_id())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sentinel_value_is_all_ones_like_cpp_placeholder() {
        // Allineato a kPlaceholderSentinel = ~0 di binding_verifier.hpp.
        assert_eq!(SENTINEL_VALUE, u64::MAX);
        assert_eq!(SENTINEL_VALUE, !0u64);
    }

    #[test]
    fn platform_id_matches_version_detector_strings() {
        assert_eq!(TargetPlatform::MacosArm64.platform_id(), "macos-arm64");
        assert_eq!(TargetPlatform::MacosX64.platform_id(), "macos-x64");
        assert_eq!(TargetPlatform::WindowsX64.platform_id(), "windows-x64");
        assert_eq!(TargetPlatform::AndroidArm64.platform_id(), "android-arm64");
        assert_eq!(TargetPlatform::IosArm64.platform_id(), "ios-arm64");
    }

    #[test]
    fn from_platform_id_round_trips_for_every_variant() {
        for platform in TargetPlatform::ALL {
            let id = platform.platform_id();
            assert_eq!(TargetPlatform::from_platform_id(id), Some(platform));
        }
        assert_eq!(TargetPlatform::from_platform_id("linux-x64"), None);
        assert_eq!(TargetPlatform::from_platform_id(""), None);
    }

    #[test]
    fn target_platform_is_a_closed_finite_set_of_five() {
        assert_eq!(TargetPlatform::ALL.len(), 5);
    }

    #[test]
    fn gd_version_display_uses_major_dot_minor() {
        assert_eq!(GdVersion::new(2, 2081).to_string(), "2.2081");
    }

    #[test]
    fn target_pair_display_matches_pbind_path_segment() {
        let pair = TargetPair::new(GdVersion::new(2, 2081), TargetPlatform::MacosArm64);
        assert_eq!(pair.to_string(), "2.2081/macos-arm64");
    }

    #[test]
    fn signature_mirrors_cpp_this_first_convention() {
        // MenuLayer::init → return "bool", params ["MenuLayer*"].
        let sig = Signature::new("bool", vec!["MenuLayer*".to_owned()]);
        assert_eq!(sig.return_type, "bool");
        assert_eq!(sig.param_types, vec!["MenuLayer*".to_owned()]);
    }

    #[test]
    fn target_platform_serde_uses_platform_id() {
        let toml_str = toml::to_string(&Wrap {
            platform: TargetPlatform::MacosArm64,
        })
        .unwrap();
        assert!(toml_str.contains("macos-arm64"));
        let back: Wrap = toml::from_str(&toml_str).unwrap();
        assert_eq!(back.platform, TargetPlatform::MacosArm64);
    }

    #[derive(Serialize, Deserialize)]
    struct Wrap {
        platform: TargetPlatform,
    }
}
