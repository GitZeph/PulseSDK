//! Modello del `Manifest` (`pulse.toml`) con parser/serializer basati su
//! `serde`/`toml` (Req 16.1, 16.5).
//!
//! Il formato canonico rispecchia quello emesso dal loader C++
//! (`loader/lifecycle/manifest.hpp`):
//!
//! ```toml
//! schema_version = 1
//!
//! [mod]
//! id = "com.example.mymod"
//! version = "1.2.3"
//! name = "My Mod"
//! type = "native"
//!
//! [[entry_points]]
//! kind = "init"
//! symbol = "pulse_mod_init"
//!
//! [[dependencies]]
//! id = "com.example.lib"
//! version = ">=1.0.0 <2.0.0"
//!
//! [permissions]
//! required = ["network", "storage"]
//!
//! [[settings]]
//! name = "max_items"
//! type = "int"
//! default = 5
//! ```
//!
//! Req 16.5 (round-trip): per ogni `Manifest` valido,
//! `parse(serialize(m)) == m`.

use std::fmt;
use std::str::FromStr;

use serde::de;
use serde::{Deserialize, Deserializer, Serialize, Serializer};

/// Errore di analisi/serializzazione del manifest.
#[derive(Debug, thiserror::Error)]
pub enum ManifestError {
    #[error("pulse.toml non valido: {0}")]
    Parse(#[from] toml::de::Error),
    #[error("serializzazione del manifest fallita: {0}")]
    Serialize(#[from] toml::ser::Error),
}

// ---------------------------------------------------------------------------
// SemVer — versione MAJOR.MINOR.PATCH (Req 16.1). Serializzata come stringa.
// ---------------------------------------------------------------------------

/// Versione semantica `MAJOR.MINOR.PATCH`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct SemVer {
    pub major: u32,
    pub minor: u32,
    pub patch: u32,
}

impl SemVer {
    pub fn new(major: u32, minor: u32, patch: u32) -> Self {
        Self {
            major,
            minor,
            patch,
        }
    }
}

impl fmt::Display for SemVer {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}.{}.{}", self.major, self.minor, self.patch)
    }
}

impl FromStr for SemVer {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let mut parts = s.split('.');
        let mut next = |which: &str| -> Result<u32, String> {
            let tok = parts
                .next()
                .ok_or_else(|| format!("SemVer malformata '{s}': manca il campo {which}"))?;
            tok.parse::<u32>()
                .map_err(|_| format!("SemVer malformata '{s}': campo {which} non numerico"))
        };
        let major = next("major")?;
        let minor = next("minor")?;
        let patch = next("patch")?;
        if parts.next().is_some() {
            return Err(format!("SemVer malformata '{s}': troppi campi"));
        }
        Ok(SemVer::new(major, minor, patch))
    }
}

impl Serialize for SemVer {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_str(&self.to_string())
    }
}

impl<'de> Deserialize<'de> for SemVer {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let s = String::deserialize(deserializer)?;
        SemVer::from_str(&s).map_err(de::Error::custom)
    }
}

// ---------------------------------------------------------------------------
// VersionConstraint — ">=MIN" oppure ">=MIN <MAXEXCL". Serializzato come stringa.
// ---------------------------------------------------------------------------

/// Vincolo di versione di una dipendenza: minimo incluso e massimo esclusivo
/// opzionale. Forma canonica: `>=MIN` o `>=MIN <MAXEXCL`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VersionConstraint {
    pub min: SemVer,
    pub max_exclusive: Option<SemVer>,
}

impl VersionConstraint {
    pub fn at_least(min: SemVer) -> Self {
        Self {
            min,
            max_exclusive: None,
        }
    }

    pub fn range(min: SemVer, max_exclusive: SemVer) -> Self {
        Self {
            min,
            max_exclusive: Some(max_exclusive),
        }
    }
}

impl fmt::Display for VersionConstraint {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, ">={}", self.min)?;
        if let Some(max) = &self.max_exclusive {
            write!(f, " <{max}")?;
        }
        Ok(())
    }
}

impl FromStr for VersionConstraint {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let mut min: Option<SemVer> = None;
        let mut max_exclusive: Option<SemVer> = None;

        for token in s.split_whitespace() {
            if let Some(rest) = token.strip_prefix(">=") {
                min = Some(SemVer::from_str(rest)?);
            } else if let Some(rest) = token.strip_prefix('<') {
                max_exclusive = Some(SemVer::from_str(rest)?);
            } else if let Some(rest) = token.strip_prefix('>') {
                // ">" nudo trattato come ">=" minimale.
                min = Some(SemVer::from_str(rest)?);
            } else {
                // Versione nuda => minimo.
                min = Some(SemVer::from_str(token)?);
            }
        }

        let min = min.ok_or_else(|| {
            format!("vincolo di versione '{s}': deve fissare almeno il minimo (>=X.Y.Z)")
        })?;
        Ok(VersionConstraint { min, max_exclusive })
    }
}

impl Serialize for VersionConstraint {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_str(&self.to_string())
    }
}

impl<'de> Deserialize<'de> for VersionConstraint {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let s = String::deserialize(deserializer)?;
        VersionConstraint::from_str(&s).map_err(de::Error::custom)
    }
}

// ---------------------------------------------------------------------------
// ModType — tipo della mod (`type` in `[mod]`).
// ---------------------------------------------------------------------------

/// Tipo della mod dichiarato nel manifest.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ModType {
    Native,
    Script,
}

impl Default for ModType {
    fn default() -> Self {
        ModType::Native
    }
}

// ---------------------------------------------------------------------------
// Sezione [mod].
// ---------------------------------------------------------------------------

/// Tabella `[mod]`: identità, versione, nome e tipo della mod.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ModInfo {
    /// Identificatore univoco non vuoto, <=256 caratteri (Req 16.1).
    pub id: String,
    /// Versione SemVer (Req 16.1).
    pub version: SemVer,
    /// Nome leggibile della mod.
    pub name: String,
    /// Tipo della mod (native|script).
    #[serde(rename = "type")]
    pub mod_type: ModType,
}

// ---------------------------------------------------------------------------
// EntryPoint — un punto di ingresso (Req 16.1, almeno uno).
// ---------------------------------------------------------------------------

/// Punto di ingresso della mod: `kind` (es. "init") + `symbol` esportato.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct EntryPoint {
    pub kind: String,
    pub symbol: String,
}

// ---------------------------------------------------------------------------
// Dependency — id + vincolo di versione (Req 16.1).
// ---------------------------------------------------------------------------

/// Dipendenza dichiarata: id della mod e vincolo di versione.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Dependency {
    pub id: String,
    pub version: VersionConstraint,
}

// ---------------------------------------------------------------------------
// Permissions — sezione [permissions] con l'array `required`.
// ---------------------------------------------------------------------------

/// Tabella `[permissions]`: elenco dei permessi richiesti dalla mod.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Permissions {
    #[serde(default)]
    pub required: Vec<String>,
}

// ---------------------------------------------------------------------------
// SettingDecl — dichiarazione di un'impostazione (nome, tipo, default).
// ---------------------------------------------------------------------------

/// Valore di default tipizzato di una `SettingDecl`. La variante attiva è
/// coerente con il campo `type` dell'impostazione.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(untagged)]
pub enum SettingValue {
    // Bool prima di Int per evitare ogni ambiguità sul valore TOML `true`/`false`.
    Bool(bool),
    Int(i64),
    Float(f64),
    Str(String),
}

/// Dichiarazione di un'impostazione: nome, tipo e valore di default.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct SettingDecl {
    pub name: String,
    /// "int" | "float" | "bool" | "string".
    #[serde(rename = "type")]
    pub setting_type: String,
    pub default: SettingValue,
}

// ---------------------------------------------------------------------------
// Manifest — modello logico del `pulse.toml` (Req 16.1).
// ---------------------------------------------------------------------------

/// Modello completo del `pulse.toml`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Manifest {
    #[serde(default = "default_schema_version")]
    pub schema_version: i32,
    #[serde(rename = "mod")]
    pub mod_info: ModInfo,
    /// Almeno un punto di ingresso (Req 16.1; validato nel task 13.2/14.x).
    #[serde(default, rename = "entry_points")]
    pub entry_points: Vec<EntryPoint>,
    #[serde(default)]
    pub dependencies: Vec<Dependency>,
    #[serde(default)]
    pub permissions: Permissions,
    #[serde(default)]
    pub settings: Vec<SettingDecl>,
}

fn default_schema_version() -> i32 {
    1
}

impl Manifest {
    /// Analizza una stringa `pulse.toml` nel modello `Manifest` (Req 16.5).
    pub fn parse(text: &str) -> Result<Self, ManifestError> {
        Ok(toml::from_str(text)?)
    }

    /// Produce la forma canonica `pulse.toml` del manifest (Req 16.5).
    pub fn serialize(&self) -> Result<String, ManifestError> {
        Ok(toml::to_string(self)?)
    }

    /// Valida il manifest contro lo schema, raccogliendo TUTTE le violazioni
    /// (Req 16.2, 16.4). Funzione pura senza side-effect, usata come gate dal
    /// comando `pulse build` prima di compilare/impacchettare (Req 14.4).
    pub fn validate(&self) -> ValidationResult {
        validate(self)
    }
}

// ---------------------------------------------------------------------------
// Validazione del Manifest contro lo schema (Req 16.2, 16.4).
//
// Rispecchia 1:1 le regole del loader C++
// (`loader/lifecycle/manifest_validation.hpp`):
//   - schema_version           : intero >= 1;
//   - mod.id                    : non vuoto, <= 256 caratteri;
//   - entry_points              : almeno uno; ciascuno con kind/symbol non vuoti;
//   - dependencies[i].id        : non vuoto (il vincolo di versione è tipizzato);
//   - permissions.required[i]   : non vuoto e riconosciuto
//                                 {network, filesystem, hooking, ui, events};
//   - settings[i]               : name non vuoto, type in {int,float,bool,string}
//                                 e default coerente col type dichiarato.
//
// La validazione NON si ferma alla prima violazione: accumula l'elenco completo
// dei campi non conformi (Req 16.4).
// ---------------------------------------------------------------------------

/// Lunghezza massima dell'identificatore della mod (Req 16.1).
pub const MAX_ID_LENGTH: usize = 256;

/// Insieme dei permessi riconosciuti dal sistema (Req 17.1), coerente con
/// l'enum `Permission { Network, FileSystem, Hooking, UI, Events }` del design.
pub const RECOGNIZED_PERMISSIONS: [&str; 5] =
    ["network", "filesystem", "hooking", "ui", "events"];

/// Tipi di setting riconosciuti.
pub const RECOGNIZED_SETTING_TYPES: [&str; 4] = ["int", "float", "bool", "string"];

/// Una singola non conformità: il percorso del campo (es. `mod.id`,
/// `entry_points[1].symbol`, `permissions.required[2]`) e un messaggio
/// leggibile che descrive la regola violata.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FieldViolation {
    pub field: String,
    pub message: String,
}

impl fmt::Display for FieldViolation {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {}", self.field, self.message)
    }
}

/// Esito della validazione del manifest.
///
/// - `valid == true`  → nessuna violazione, il Manifest è conforme allo schema.
/// - `valid == false` → `violations` contiene l'ELENCO COMPLETO dei campi non
///   conformi (Req 16.4).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ValidationResult {
    pub valid: bool,
    pub violations: Vec<FieldViolation>,
}

impl ValidationResult {
    /// `true` se il manifest è conforme allo schema.
    pub fn ok(&self) -> bool {
        self.valid
    }

    /// Elenco dei soli percorsi dei campi non conformi.
    pub fn field_names(&self) -> Vec<String> {
        self.violations.iter().map(|v| v.field.clone()).collect()
    }
}

fn is_recognized_permission(p: &str) -> bool {
    RECOGNIZED_PERMISSIONS.contains(&p)
}

fn is_recognized_setting_type(t: &str) -> bool {
    RECOGNIZED_SETTING_TYPES.contains(&t)
}

/// Verifica la coerenza tra il `type` dichiarato di un setting e la variante
/// effettivamente attiva nel suo valore di default.
fn setting_default_matches_type(s: &SettingDecl) -> bool {
    match s.setting_type.as_str() {
        "int" => matches!(s.default, SettingValue::Int(_)),
        "float" => matches!(s.default, SettingValue::Float(_)),
        "bool" => matches!(s.default, SettingValue::Bool(_)),
        "string" => matches!(s.default, SettingValue::Str(_)),
        _ => false,
    }
}

/// Valida un `Manifest` contro lo schema, raccogliendo TUTTE le violazioni
/// (Req 16.2, 16.4).
pub fn validate(m: &Manifest) -> ValidationResult {
    let mut violations: Vec<FieldViolation> = Vec::new();
    let mut add = |field: &str, message: String| {
        violations.push(FieldViolation {
            field: field.to_string(),
            message,
        });
    };

    // schema_version: intero >= 1.
    if m.schema_version < 1 {
        add(
            "schema_version",
            format!(
                "schema_version deve essere un intero >= 1 (trovato {})",
                m.schema_version
            ),
        );
    }

    // mod.id: non vuoto, <= 256 caratteri (Req 16.1).
    if m.mod_info.id.is_empty() {
        add("mod.id", "l'identificatore della Mod non puo' essere vuoto".to_string());
    } else if m.mod_info.id.chars().count() > MAX_ID_LENGTH {
        add(
            "mod.id",
            format!(
                "l'identificatore della Mod supera i 256 caratteri (lunghezza {})",
                m.mod_info.id.chars().count()
            ),
        );
    }

    // entry_points: almeno uno (Req 16.1), ciascuno con kind e symbol non vuoti.
    if m.entry_points.is_empty() {
        add(
            "entry_points",
            "il Manifest deve dichiarare almeno un punto di ingresso".to_string(),
        );
    } else {
        for (i, ep) in m.entry_points.iter().enumerate() {
            if ep.kind.is_empty() {
                add(
                    &format!("entry_points[{i}].kind"),
                    "il tipo del punto di ingresso non puo' essere vuoto".to_string(),
                );
            }
            if ep.symbol.is_empty() {
                add(
                    &format!("entry_points[{i}].symbol"),
                    "il simbolo del punto di ingresso non puo' essere vuoto".to_string(),
                );
            }
        }
    }

    // dependencies[i].id: non vuoto (il vincolo di versione è tipizzato).
    for (i, d) in m.dependencies.iter().enumerate() {
        if d.id.is_empty() {
            add(
                &format!("dependencies[{i}].id"),
                "l'id della dipendenza non puo' essere vuoto".to_string(),
            );
        }
    }

    // permissions.required[i]: non vuoto e riconosciuto (Req 17.1).
    for (i, p) in m.permissions.required.iter().enumerate() {
        let field = format!("permissions.required[{i}]");
        if p.is_empty() {
            add(&field, "il permesso dichiarato non puo' essere una stringa vuota".to_string());
        } else if !is_recognized_permission(p) {
            add(
                &field,
                format!(
                    "permesso non riconosciuto: '{p}' (attesi: network, filesystem, hooking, ui, events)"
                ),
            );
        }
    }

    // settings[i]: name non vuoto, type riconosciuto, default coerente col type.
    for (i, s) in m.settings.iter().enumerate() {
        let base = format!("settings[{i}]");
        if s.name.is_empty() {
            add(&format!("{base}.name"), "il nome del setting non puo' essere vuoto".to_string());
        }
        if !is_recognized_setting_type(&s.setting_type) {
            add(
                &format!("{base}.type"),
                format!(
                    "tipo di setting non riconosciuto: '{}' (attesi: int, float, bool, string)",
                    s.setting_type
                ),
            );
        } else if !setting_default_matches_type(s) {
            add(
                &format!("{base}.default"),
                format!(
                    "il valore di default non e' coerente col tipo dichiarato '{}'",
                    s.setting_type
                ),
            );
        }
    }

    ValidationResult {
        valid: violations.is_empty(),
        violations,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_manifest() -> Manifest {
        Manifest {
            schema_version: 1,
            mod_info: ModInfo {
                id: "com.example.mymod".to_string(),
                version: SemVer::new(1, 2, 3),
                name: "My Mod".to_string(),
                mod_type: ModType::Native,
            },
            entry_points: vec![EntryPoint {
                kind: "init".to_string(),
                symbol: "pulse_mod_init".to_string(),
            }],
            dependencies: vec![
                Dependency {
                    id: "com.example.lib".to_string(),
                    version: VersionConstraint::range(SemVer::new(1, 0, 0), SemVer::new(2, 0, 0)),
                },
                Dependency {
                    id: "com.example.core".to_string(),
                    version: VersionConstraint::at_least(SemVer::new(0, 5, 0)),
                },
            ],
            permissions: Permissions {
                required: vec!["network".to_string(), "storage".to_string()],
            },
            settings: vec![
                SettingDecl {
                    name: "max_items".to_string(),
                    setting_type: "int".to_string(),
                    default: SettingValue::Int(5),
                },
                SettingDecl {
                    name: "ratio".to_string(),
                    setting_type: "float".to_string(),
                    default: SettingValue::Float(0.75),
                },
                SettingDecl {
                    name: "enabled".to_string(),
                    setting_type: "bool".to_string(),
                    default: SettingValue::Bool(true),
                },
                SettingDecl {
                    name: "label".to_string(),
                    setting_type: "string".to_string(),
                    default: SettingValue::Str("hello".to_string()),
                },
            ],
        }
    }

    #[test]
    fn semver_display_and_parse_round_trip() {
        let v = SemVer::new(3, 14, 159);
        assert_eq!(v.to_string(), "3.14.159");
        assert_eq!(SemVer::from_str("3.14.159").unwrap(), v);
    }

    #[test]
    fn version_constraint_canonical_forms() {
        let only_min = VersionConstraint::at_least(SemVer::new(1, 0, 0));
        assert_eq!(only_min.to_string(), ">=1.0.0");
        assert_eq!(
            VersionConstraint::from_str(">=1.0.0").unwrap(),
            only_min
        );

        let range = VersionConstraint::range(SemVer::new(1, 0, 0), SemVer::new(2, 0, 0));
        assert_eq!(range.to_string(), ">=1.0.0 <2.0.0");
        assert_eq!(
            VersionConstraint::from_str(">=1.0.0 <2.0.0").unwrap(),
            range
        );
    }

    /// Req 16.5 — round-trip: parse(serialize(m)) == m.
    #[test]
    fn manifest_round_trip() {
        let m = sample_manifest();
        let text = m.serialize().expect("serialize");
        let parsed = Manifest::parse(&text).expect("parse");
        assert_eq!(parsed, m);

        // Stabilità: una seconda passata produce lo stesso testo.
        let text2 = parsed.serialize().expect("serialize 2");
        assert_eq!(text, text2);
    }

    /// Req 16.5 — round-trip a partire dal testo: parse(serialize(parse(s))) == parse(s).
    #[test]
    fn manifest_text_round_trip() {
        let original = sample_manifest();
        let text = original.serialize().unwrap();
        let parsed_once = Manifest::parse(&text).unwrap();
        let reserialized = parsed_once.serialize().unwrap();
        let parsed_twice = Manifest::parse(&reserialized).unwrap();
        assert_eq!(parsed_once, parsed_twice);
    }

    #[test]
    fn parse_minimal_manifest_uses_defaults() {
        let text = r#"
            schema_version = 1

            [mod]
            id = "com.example.min"
            version = "0.1.0"
            name = "Minimal"
            type = "script"

            [[entry_points]]
            kind = "init"
            symbol = "init"
        "#;
        let m = Manifest::parse(text).unwrap();
        assert_eq!(m.mod_info.id, "com.example.min");
        assert_eq!(m.mod_info.mod_type, ModType::Script);
        assert_eq!(m.entry_points.len(), 1);
        assert!(m.dependencies.is_empty());
        assert!(m.permissions.required.is_empty());
        assert!(m.settings.is_empty());
    }

    #[test]
    fn parse_rejects_malformed_semver() {
        let text = r#"
            [mod]
            id = "x"
            version = "1.2"
            name = "X"
            type = "native"
        "#;
        assert!(Manifest::parse(text).is_err());
    }

    // -----------------------------------------------------------------------
    // Validazione del manifest (Req 16.2, 16.4).
    // -----------------------------------------------------------------------

    #[test]
    fn validate_accepts_well_formed_manifest() {
        let mut m = sample_manifest();
        // sample_manifest usa "storage" (non riconosciuto); usa permessi validi.
        m.permissions.required = vec!["network".to_string(), "filesystem".to_string()];
        let res = m.validate();
        assert!(res.ok(), "violazioni inattese: {:?}", res.violations);
        assert!(res.violations.is_empty());
    }

    #[test]
    fn validate_collects_all_violations() {
        // Manifest con molteplici non conformità simultanee.
        let m = Manifest {
            schema_version: 0, // < 1
            mod_info: ModInfo {
                id: String::new(), // vuoto
                version: SemVer::new(1, 0, 0),
                name: "X".to_string(),
                mod_type: ModType::Native,
            },
            entry_points: vec![EntryPoint {
                kind: String::new(),   // vuoto
                symbol: String::new(), // vuoto
            }],
            dependencies: vec![Dependency {
                id: String::new(), // vuoto
                version: VersionConstraint::at_least(SemVer::new(1, 0, 0)),
            }],
            permissions: Permissions {
                required: vec!["network".to_string(), "telepathy".to_string()],
            },
            settings: vec![SettingDecl {
                name: "n".to_string(),
                setting_type: "int".to_string(),
                default: SettingValue::Str("nope".to_string()), // default incoerente
            }],
        };
        let res = m.validate();
        assert!(!res.ok());
        let fields = res.field_names();
        // Tutte le violazioni attese sono presenti (elenco COMPLETO, Req 16.4).
        for expected in [
            "schema_version",
            "mod.id",
            "entry_points[0].kind",
            "entry_points[0].symbol",
            "dependencies[0].id",
            "permissions.required[1]",
            "settings[0].default",
        ] {
            assert!(
                fields.iter().any(|f| f == expected),
                "campo non conforme atteso assente: {expected}; trovati: {fields:?}"
            );
        }
    }

    #[test]
    fn validate_rejects_missing_entry_points() {
        let mut m = sample_manifest();
        m.entry_points.clear();
        let res = m.validate();
        assert!(!res.ok());
        assert!(res.field_names().iter().any(|f| f == "entry_points"));
    }

    #[test]
    fn validate_rejects_id_over_256_chars() {
        let mut m = sample_manifest();
        m.mod_info.id = "a".repeat(257);
        let res = m.validate();
        assert!(!res.ok());
        assert!(res.field_names().iter().any(|f| f == "mod.id"));
    }

    #[test]
    fn validate_rejects_unrecognized_permission() {
        let mut m = sample_manifest();
        m.permissions.required = vec!["filesystem".to_string(), "bogus".to_string()];
        let res = m.validate();
        assert!(!res.ok());
        assert!(res.field_names().iter().any(|f| f == "permissions.required[1]"));
    }
}
