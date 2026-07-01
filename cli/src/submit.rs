//! `pulse submit` — modello dati della submission verso l'`Index` (Req 5.2,
//! 5.3, 5.4).
//!
//! Questo modulo definisce il **data model** del comando `submit`:
//!
//! - l'insieme finito dei [`SUPPORT_TAGS`] selezionabili, definito dalla CLI
//!   (Req 5.2);
//! - il [`SubmissionDescriptor`] con i metadati della mod e i `Support_Tag`
//!   selezionati, serializzato/deserializzato in TOML come `submission.toml`
//!   con round-trip stabile (`parse(serialize(d)) == d`, Req 5.4);
//! - il gate dei metadati obbligatori [`SubmissionDescriptor::missing_required`]
//!   usato dal wizard per bloccare la conferma quando mancano campi obbligatori
//!   (Req 5.3).
//!
//! Il wizard interattivo (`crossterm`) è implementato in questo stesso modulo
//! ([`run_wizard`]); il gestore `run_submit` che lo invoca è nel task 11.3.
//!
//! Formato canonico `submission.toml`:
//!
//! ```toml
//! schema_version = 1
//! mod_id   = "com.example.mymod"     # prefill dal Mod_Manifest
//! name     = "My Mod"                # prefill dal Mod_Manifest
//! version  = "1.0.0"                 # prefill dal Mod_Manifest
//! summary  = "…"                     # metadato obbligatorio raccolto dal wizard
//! support_tags = ["🌱 Learning Project"]  # 0..N Support_Tag selezionati (Req 5.2)
//!
//! [[artifacts]]                      # riferimenti agli Upload_Artifact
//! path = "mymod.pulse"
//! ```

use std::io::{self, Write};
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::builder::MANIFEST_FILE;
use crate::manifest::Manifest;

/// Versione di schema di default del `submission.toml`.
const DEFAULT_SCHEMA_VERSION: i32 = 1;

/// Nome del file di submission scritto nella radice del progetto (Req 5.4).
pub const SUBMISSION_FILE: &str = "submission.toml";

/// Insieme finito dei `Support_Tag` selezionabili in `pulse submit`, definito
/// dalla CLI (Req 5.2). L'utente può selezionarne da zero fino all'intero
/// insieme; nessun tag esterno a questo elenco è ammesso.
pub const SUPPORT_TAGS: &[&str] = &[
    "🌱 Learning Project",
    "🐛 Bug Reports Welcome",
    "🤝 Looking for Collaborators",
    "🧪 Experimental",
    "✅ Stable",
    "📚 Well Documented",
    "🎨 Cosmetic",
    "⚡ Performance",
];

/// Riferimento a un `Upload_Artifact` (binario o metadato) da caricare
/// sull'`Index`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ArtifactRef {
    /// Percorso dell'artefatto, relativo alla radice del progetto.
    pub path: String,
}

/// Il `Submission_Descriptor` prodotto da `pulse submit`: metadati della mod +
/// `Support_Tag` selezionati + riferimenti agli artefatti (Req 5.4).
///
/// Serializzato/deserializzato in TOML come `submission.toml`. Il round-trip è
/// stabile: `parse(serialize(d)) == d`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SubmissionDescriptor {
    /// Versione di schema del descrittore (default 1).
    #[serde(default = "default_schema_version")]
    pub schema_version: i32,
    /// Identificatore della mod (prefill da `Mod_Manifest`). Obbligatorio.
    pub mod_id: String,
    /// Nome leggibile della mod (prefill da `Mod_Manifest`). Obbligatorio.
    pub name: String,
    /// Versione della mod (prefill da `Mod_Manifest`). Obbligatorio.
    pub version: String,
    /// Sintesi/descrizione breve raccolta dal wizard. Obbligatorio.
    pub summary: String,
    /// `Support_Tag` selezionati (da 0 fino all'intero [`SUPPORT_TAGS`]).
    #[serde(default)]
    pub support_tags: Vec<String>,
    /// Riferimenti agli `Upload_Artifact` (binari + metadati).
    #[serde(default)]
    pub artifacts: Vec<ArtifactRef>,
}

fn default_schema_version() -> i32 {
    DEFAULT_SCHEMA_VERSION
}

/// Nomi dei metadati obbligatori del `Submission_Descriptor`, nell'ordine di
/// segnalazione. Sono i campi che il gate del wizard richiede non vuoti prima
/// di consentire la conferma (Req 5.3).
const REQUIRED_FIELDS: &[&str] = &["mod_id", "name", "version", "summary"];

impl SubmissionDescriptor {
    /// Analizza una stringa `submission.toml` nel modello (Req 5.4).
    pub fn parse(text: &str) -> anyhow::Result<Self> {
        Ok(toml::from_str(text)?)
    }

    /// Produce la forma canonica `submission.toml` del descrittore (Req 5.4).
    ///
    /// Il round-trip è stabile: `parse(serialize(d)) == d`.
    pub fn serialize(&self) -> anyhow::Result<String> {
        Ok(toml::to_string(self)?)
    }

    /// Elenca i metadati obbligatori assenti o vuoti (solo spazi conta come
    /// vuoto), nell'ordine di [`REQUIRED_FIELDS`]. Gate del wizard: se non
    /// vuoto, la conferma è bloccata e nessun descrittore va prodotto (Req 5.3).
    pub fn missing_required(&self) -> Vec<&'static str> {
        REQUIRED_FIELDS
            .iter()
            .copied()
            .filter(|field| {
                let value = match *field {
                    "mod_id" => &self.mod_id,
                    "name" => &self.name,
                    "version" => &self.version,
                    "summary" => &self.summary,
                    _ => unreachable!("campo obbligatorio non gestito: {field}"),
                };
                value.trim().is_empty()
            })
            .collect()
    }
}

// ===========================================================================
// Wizard interattivo `run_wizard` (crossterm) — Req 5.1, 5.2, 5.3, 5.4, 5.5, 5.6
//
// La logica con side-effect (I/O terminale) è isolata dietro un sottile driver
// [`run_wizard`], mentre le parti pure e host-testabili sono estratte:
//
//   - [`load_prefill`]           — carica+valida il Mod_Manifest, prefill dei
//                                   campi (Req 5.1, 5.5);
//   - [`WizardState::handle_key`] — macchina a stati pura del wizard, guidabile
//                                   senza terminale (navigazione, testo, toggle
//                                   dei Support_Tag, gate della conferma);
//   - [`WizardState::build_descriptor`] — costruisce il descrittore dai campi
//                                   raccolti (Req 5.2);
//   - [`write_submission_atomic`] — scrittura atomica (temp + rename) del
//                                   `submission.toml` (Req 5.4).
//
// Il gate dei metadati obbligatori riusa [`SubmissionDescriptor::missing_required`]
// (Req 5.3). L'annullamento non tocca il filesystem perché la scrittura avviene
// SOLO dopo una conferma valida (Req 5.6).
// ===========================================================================

use crossterm::event::{self, Event, KeyCode, KeyEvent, KeyEventKind, KeyModifiers};
use crossterm::terminal::{disable_raw_mode, enable_raw_mode, Clear, ClearType};
use crossterm::{cursor, execute};

/// Campi precompilati dal `Mod_Manifest` (Req 5.1): identità della mod usata per
/// inizializzare il `Submission_Descriptor` prima della raccolta interattiva.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WizardPrefill {
    pub mod_id: String,
    pub name: String,
    pub version: String,
}

/// Carica e valida il `Mod_Manifest` del progetto, restituendo i campi
/// precompilati (Req 5.1). Fail-closed (Req 5.5): manifest assente, non
/// analizzabile o non conforme allo schema ⇒ `Err` con causa e **nessun**
/// descrittore prodotto a valle.
///
/// Funzione pura rispetto al terminale (unico side-effect: lettura del file),
/// così il ramo di manifest assente/non valido è host-testabile.
pub fn load_prefill(project_dir: &Path) -> anyhow::Result<WizardPrefill> {
    let manifest_path = project_dir.join(MANIFEST_FILE);
    if !manifest_path.is_file() {
        anyhow::bail!(
            "submit interrotto: Mod_Manifest assente ('{}' non trovato); nessun \
             Submission_Descriptor prodotto",
            manifest_path.display()
        );
    }
    let text = std::fs::read_to_string(&manifest_path).map_err(|err| {
        anyhow::anyhow!(
            "submit interrotto: lettura del Mod_Manifest '{}' fallita: {err}",
            manifest_path.display()
        )
    })?;
    let manifest = Manifest::parse(&text).map_err(|err| {
        anyhow::anyhow!("submit interrotto: Mod_Manifest non analizzabile: {err}")
    })?;
    let validation = manifest.validate();
    if !validation.ok() {
        anyhow::bail!(
            "submit interrotto: Mod_Manifest non valido; campi non conformi: {}",
            validation.field_names().join(", ")
        );
    }
    Ok(WizardPrefill {
        mod_id: manifest.mod_info.id,
        name: manifest.mod_info.name,
        version: manifest.mod_info.version.to_string(),
    })
}

/// Scrive il `Submission_Descriptor` come `submission.toml` nella radice del
/// progetto in modo **atomico** (Req 5.4): il contenuto è prima riversato in un
/// file temporaneo nella **stessa directory** (così il `rename` resta sullo
/// stesso filesystem ed è atomico), poi promosso con [`std::fs::rename`]. Su
/// fallimento nessun file parziale è lasciato a terra.
pub fn write_submission_atomic(
    project_dir: &Path,
    descriptor: &SubmissionDescriptor,
) -> anyhow::Result<PathBuf> {
    use std::sync::atomic::{AtomicU64, Ordering};

    /// Contatore monotòno di processo per nomi di temporaneo univoci.
    static TMP_COUNTER: AtomicU64 = AtomicU64::new(0);

    let content = descriptor.serialize()?;
    let path = project_dir.join(SUBMISSION_FILE);
    let parent = path.parent().unwrap_or_else(|| Path::new("."));
    let unique = TMP_COUNTER.fetch_add(1, Ordering::Relaxed);
    let tmp_path = parent.join(format!(
        ".{SUBMISSION_FILE}.{}.{unique}.tmp",
        std::process::id()
    ));

    // 1) Scrive l'intero contenuto nel file temporaneo.
    if let Err(source) = std::fs::write(&tmp_path, content.as_bytes()) {
        let _ = std::fs::remove_file(&tmp_path);
        return Err(anyhow::anyhow!(
            "submit interrotto: scrittura del submission.toml temporaneo fallita: {source}"
        ));
    }

    // 2) Promuove atomicamente il temporaneo a destinazione finale.
    if let Err(source) = std::fs::rename(&tmp_path, &path) {
        let _ = std::fs::remove_file(&tmp_path);
        return Err(anyhow::anyhow!(
            "submit interrotto: promozione atomica del submission.toml fallita: {source}"
        ));
    }

    Ok(path)
}

/// Tasto logico del wizard, astrazione host-testabile dell'evento `crossterm`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum WizardKey {
    Char(char),
    Backspace,
    Up,
    Down,
    Space,
    Enter,
    Tab,
    /// Annullamento (Esc o Ctrl-C).
    Cancel,
}

/// Passo corrente del wizard.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum WizardStep {
    /// Raccolta del `summary` (unico metadato obbligatorio non precompilato).
    Summary,
    /// Selezione dei `Support_Tag` (da 0 all'intero insieme).
    Tags,
    /// Riepilogo e conferma con gate dei metadati obbligatori.
    Confirm,
}

/// Esito di una singola pressione di tasto.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum StepOutcome {
    /// Il wizard prosegue: ridisegna e attende il prossimo tasto.
    Continue,
    /// Conferma valida: costruisci il descrittore e scrivilo.
    Confirm,
    /// Annullamento: nessun descrittore, nessuna scrittura (Req 5.6).
    Cancel,
}

/// Stato del wizard: campi raccolti + navigazione. La transizione di stato è
/// una funzione pura ([`WizardState::handle_key`]) guidabile senza terminale.
struct WizardState {
    prefill: WizardPrefill,
    /// Metadato obbligatorio raccolto interattivamente (Req 5.3).
    summary: String,
    /// Flag di selezione per ciascun `Support_Tag`, allineato a [`SUPPORT_TAGS`].
    tag_selected: Vec<bool>,
    /// Cursore nella lista dei `Support_Tag`.
    tag_cursor: usize,
    step: WizardStep,
    /// Messaggio transitorio (es. gate dei metadati obbligatori, Req 5.3).
    notice: Option<String>,
}

impl WizardState {
    fn new(prefill: WizardPrefill) -> Self {
        Self {
            prefill,
            summary: String::new(),
            tag_selected: vec![false; SUPPORT_TAGS.len()],
            tag_cursor: 0,
            step: WizardStep::Summary,
            notice: None,
        }
    }

    /// Costruisce il `Submission_Descriptor` dai campi raccolti (Req 5.2): i
    /// `Support_Tag` selezionati compaiono **esattamente** (nell'ordine di
    /// [`SUPPORT_TAGS`]), senza tag inventati né persi.
    fn build_descriptor(&self) -> SubmissionDescriptor {
        let support_tags = SUPPORT_TAGS
            .iter()
            .zip(self.tag_selected.iter())
            .filter_map(|(tag, &selected)| selected.then(|| (*tag).to_string()))
            .collect();
        SubmissionDescriptor {
            schema_version: DEFAULT_SCHEMA_VERSION,
            mod_id: self.prefill.mod_id.clone(),
            name: self.prefill.name.clone(),
            version: self.prefill.version.clone(),
            summary: self.summary.trim().to_string(),
            support_tags,
            artifacts: Vec::new(),
        }
    }

    /// Applica un tasto e restituisce l'esito. Pura e testabile: nessun I/O.
    fn handle_key(&mut self, key: WizardKey) -> StepOutcome {
        if key == WizardKey::Cancel {
            return StepOutcome::Cancel;
        }
        match self.step {
            WizardStep::Summary => match key {
                WizardKey::Char(c) => {
                    self.notice = None;
                    self.summary.push(c);
                }
                WizardKey::Backspace => {
                    self.summary.pop();
                }
                WizardKey::Enter | WizardKey::Tab | WizardKey::Down => {
                    self.notice = None;
                    self.step = WizardStep::Tags;
                }
                _ => {}
            },
            WizardStep::Tags => match key {
                WizardKey::Up => {
                    if self.tag_cursor == 0 {
                        // Risalendo dal primo tag si torna al campo summary.
                        self.step = WizardStep::Summary;
                    } else {
                        self.tag_cursor -= 1;
                    }
                }
                WizardKey::Down => {
                    if self.tag_cursor + 1 < SUPPORT_TAGS.len() {
                        self.tag_cursor += 1;
                    }
                }
                WizardKey::Space => {
                    let sel = &mut self.tag_selected[self.tag_cursor];
                    *sel = !*sel;
                }
                WizardKey::Enter | WizardKey::Tab => {
                    self.step = WizardStep::Confirm;
                }
                _ => {}
            },
            WizardStep::Confirm => match key {
                WizardKey::Up | WizardKey::Tab => {
                    self.step = WizardStep::Tags;
                }
                WizardKey::Enter => {
                    // Gate dei metadati obbligatori (Req 5.3): se manca qualcosa
                    // la conferma è BLOCCATA, si indicano i mancanti e nulla è
                    // prodotto; si torna al campo summary per correggere.
                    let descriptor = self.build_descriptor();
                    let missing = descriptor.missing_required();
                    if missing.is_empty() {
                        return StepOutcome::Confirm;
                    }
                    self.notice = Some(format!(
                        "Conferma bloccata: metadati obbligatori mancanti: {}",
                        missing.join(", ")
                    ));
                    self.step = WizardStep::Summary;
                }
                _ => {}
            },
        }
        StepOutcome::Continue
    }
}

/// Traduce un evento tastiera `crossterm` nel tasto logico del wizard. Ritorna
/// `None` per gli eventi da ignorare (rilasci di tasto, tasti non mappati).
fn translate_key(key: KeyEvent) -> Option<WizardKey> {
    if key.kind != KeyEventKind::Press {
        return None;
    }
    if key.modifiers.contains(KeyModifiers::CONTROL) && key.code == KeyCode::Char('c') {
        return Some(WizardKey::Cancel);
    }
    match key.code {
        KeyCode::Esc => Some(WizardKey::Cancel),
        KeyCode::Enter => Some(WizardKey::Enter),
        KeyCode::Tab => Some(WizardKey::Tab),
        KeyCode::Up => Some(WizardKey::Up),
        KeyCode::Down => Some(WizardKey::Down),
        KeyCode::Backspace => Some(WizardKey::Backspace),
        KeyCode::Char(' ') => Some(WizardKey::Space),
        KeyCode::Char(c) => Some(WizardKey::Char(c)),
        _ => None,
    }
}

/// Rende la schermata del wizard come testo (righe separate da `\r\n`, adatte
/// al raw mode). Pura: nessun I/O, usata dal driver per il disegno.
fn render_screen(state: &WizardState) -> String {
    let mut lines: Vec<String> = Vec::new();
    lines.push("╭─ pulse submit ─ procedura guidata di submission".to_string());
    lines.push(String::new());
    lines.push(format!("  mod_id  : {}", state.prefill.mod_id));
    lines.push(format!("  name    : {}", state.prefill.name));
    lines.push(format!("  version : {}", state.prefill.version));
    lines.push(String::new());

    let marker = |active: bool| if active { "▸" } else { " " };

    // Campo summary.
    lines.push(format!(
        "{} summary : {}{}",
        marker(state.step == WizardStep::Summary),
        state.summary,
        if state.step == WizardStep::Summary { "_" } else { "" }
    ));
    lines.push(String::new());

    // Lista Support_Tag.
    lines.push("  Support_Tag (Spazio per selezionare):".to_string());
    for (i, tag) in SUPPORT_TAGS.iter().enumerate() {
        let checked = if state.tag_selected[i] { "[x]" } else { "[ ]" };
        let cursor = if state.step == WizardStep::Tags && state.tag_cursor == i {
            "▸"
        } else {
            " "
        };
        lines.push(format!("   {cursor} {checked} {tag}"));
    }
    lines.push(String::new());

    // Riepilogo/conferma.
    let confirm_marker = marker(state.step == WizardStep::Confirm);
    lines.push(format!("{confirm_marker} [ Conferma ]  (Invio conferma · Esc annulla)"));
    lines.push(String::new());

    if let Some(notice) = &state.notice {
        lines.push(format!("  ⚠ {notice}"));
    }
    lines.push(
        "  Naviga: ↑/↓ · Invio avanza/conferma · Tab passo successivo · Esc/Ctrl-C annulla"
            .to_string(),
    );

    lines.join("\r\n")
}

/// Guardia RAII del raw mode: lo attiva all'ingresso e lo ripristina all'uscita,
/// anche su `?`/panico, così il terminale non resta in uno stato inconsistente.
struct RawModeGuard;

impl RawModeGuard {
    fn enter() -> io::Result<Self> {
        enable_raw_mode()?;
        Ok(Self)
    }
}

impl Drop for RawModeGuard {
    fn drop(&mut self) {
        let _ = disable_raw_mode();
    }
}

/// Wizard interattivo di submission (Req 5.1–5.6).
///
/// Ritorna `Ok(Some(descriptor))` su conferma valida (con `submission.toml`
/// scritto atomicamente), `Ok(None)` su annullamento (nessuna scrittura,
/// Req 5.6), `Err` su Mod_Manifest assente/non valido (nessun descrittore,
/// Req 5.5).
pub fn run_wizard(project_dir: &Path) -> anyhow::Result<Option<SubmissionDescriptor>> {
    // Fail-closed sul manifest PRIMA di toccare il terminale (Req 5.1, 5.5).
    let prefill = load_prefill(project_dir)?;
    let mut state = WizardState::new(prefill);

    // Ciclo interattivo confinato allo scope del raw mode.
    let outcome = {
        let _guard = RawModeGuard::enter()?;
        let mut stdout = io::stdout();
        loop {
            execute!(
                stdout,
                Clear(ClearType::All),
                cursor::MoveTo(0, 0)
            )?;
            write!(stdout, "{}", render_screen(&state))?;
            stdout.flush()?;

            let Event::Key(key_event) = event::read()? else {
                continue;
            };
            let Some(key) = translate_key(key_event) else {
                continue;
            };
            match state.handle_key(key) {
                StepOutcome::Continue => continue,
                other => break other,
            }
        }
    };
    // Raw mode ripristinato qui (guardia rilasciata). Pulisce la riga corrente.
    let mut stdout = io::stdout();
    let _ = write!(stdout, "\r\n");
    let _ = stdout.flush();

    match outcome {
        StepOutcome::Cancel => Ok(None), // Req 5.6: nessuna scrittura sul filesystem.
        StepOutcome::Confirm => {
            let descriptor = state.build_descriptor();
            write_submission_atomic(project_dir, &descriptor)?; // Req 5.4.
            Ok(Some(descriptor))
        }
        StepOutcome::Continue => unreachable!("il ciclo termina solo su Confirm/Cancel"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_descriptor() -> SubmissionDescriptor {
        SubmissionDescriptor {
            schema_version: 1,
            mod_id: "com.example.mymod".to_string(),
            name: "My Mod".to_string(),
            version: "1.0.0".to_string(),
            summary: "Una mod di esempio.".to_string(),
            support_tags: vec![
                "🌱 Learning Project".to_string(),
                "✅ Stable".to_string(),
            ],
            artifacts: vec![ArtifactRef {
                path: "mymod.pulse".to_string(),
            }],
        }
    }

    /// Req 5.4 — round-trip: parse(serialize(d)) == d.
    #[test]
    fn descriptor_round_trip() {
        let d = sample_descriptor();
        let text = d.serialize().expect("serialize");
        let parsed = SubmissionDescriptor::parse(&text).expect("parse");
        assert_eq!(parsed, d);

        // Stabilità: una seconda passata produce lo stesso testo.
        let text2 = parsed.serialize().expect("serialize 2");
        assert_eq!(text, text2);
    }

    /// Req 5.4 — un descrittore senza tag né artefatti fa round-trip (default).
    #[test]
    fn descriptor_round_trip_empty_collections() {
        let d = SubmissionDescriptor {
            schema_version: 1,
            mod_id: "com.example.min".to_string(),
            name: "Min".to_string(),
            version: "0.1.0".to_string(),
            summary: "Minimal.".to_string(),
            support_tags: Vec::new(),
            artifacts: Vec::new(),
        };
        let text = d.serialize().unwrap();
        assert_eq!(SubmissionDescriptor::parse(&text).unwrap(), d);
    }

    /// Req 5.4 — schema_version assente nel TOML usa il default (1).
    #[test]
    fn parse_defaults_schema_version() {
        let text = r#"
            mod_id = "com.example.mymod"
            name = "My Mod"
            version = "1.0.0"
            summary = "x"
        "#;
        let d = SubmissionDescriptor::parse(text).unwrap();
        assert_eq!(d.schema_version, DEFAULT_SCHEMA_VERSION);
        assert!(d.support_tags.is_empty());
        assert!(d.artifacts.is_empty());
    }

    /// Req 5.3 — nessun obbligatorio mancante quando tutti sono presenti.
    #[test]
    fn missing_required_empty_when_complete() {
        let d = sample_descriptor();
        assert!(d.missing_required().is_empty());
    }

    /// Req 5.3 — i campi obbligatori assenti o vuoti (anche solo spazi) sono
    /// segnalati, nell'ordine canonico.
    #[test]
    fn missing_required_reports_empty_fields() {
        let d = SubmissionDescriptor {
            schema_version: 1,
            mod_id: String::new(),
            name: "   ".to_string(), // solo spazi ⇒ vuoto
            version: "1.0.0".to_string(),
            summary: String::new(),
            support_tags: Vec::new(),
            artifacts: Vec::new(),
        };
        assert_eq!(d.missing_required(), vec!["mod_id", "name", "summary"]);
    }

    /// Req 5.2 — l'insieme dei Support_Tag è finito, non vuoto e senza duplicati.
    #[test]
    fn support_tags_are_unique_and_non_empty() {
        assert!(!SUPPORT_TAGS.is_empty());
        for tag in SUPPORT_TAGS {
            assert!(!tag.trim().is_empty());
        }
        let mut seen = std::collections::HashSet::new();
        for tag in SUPPORT_TAGS {
            assert!(seen.insert(*tag), "Support_Tag duplicato: {tag}");
        }
    }

    // -----------------------------------------------------------------------
    // Wizard `run_wizard` — logica pura host-testabile (Req 5.1–5.6).
    // -----------------------------------------------------------------------

    use std::sync::atomic::{AtomicU64, Ordering};

    /// Directory temporanea isolata con pulizia automatica.
    struct TempGuard {
        path: std::path::PathBuf,
    }

    impl TempGuard {
        fn new(tag: &str) -> Self {
            static COUNTER: AtomicU64 = AtomicU64::new(0);
            let n = COUNTER.fetch_add(1, Ordering::Relaxed);
            let pid = std::process::id();
            let path = std::env::temp_dir().join(format!("pulse-submit-{tag}-{pid}-{n}"));
            let _ = std::fs::remove_dir_all(&path);
            std::fs::create_dir_all(&path).unwrap();
            Self { path }
        }
        fn path(&self) -> &std::path::Path {
            &self.path
        }
    }

    impl Drop for TempGuard {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.path);
        }
    }

    const VALID_MANIFEST: &str = r#"
        schema_version = 1

        [mod]
        id = "com.example.mymod"
        version = "1.2.3"
        name = "My Mod"
        type = "native"

        [[entry_points]]
        kind = "init"
        symbol = "pulse_mod_init"
    "#;

    fn prefill() -> WizardPrefill {
        WizardPrefill {
            mod_id: "com.example.mymod".to_string(),
            name: "My Mod".to_string(),
            version: "1.2.3".to_string(),
        }
    }

    /// Req 5.1 — i campi sono precompilati dal Mod_Manifest.
    #[test]
    fn load_prefill_fills_fields_from_manifest() {
        let guard = TempGuard::new("prefill");
        std::fs::write(guard.path().join(MANIFEST_FILE), VALID_MANIFEST).unwrap();

        let p = load_prefill(guard.path()).expect("prefill");
        assert_eq!(p.mod_id, "com.example.mymod");
        assert_eq!(p.name, "My Mod");
        assert_eq!(p.version, "1.2.3");
    }

    /// Req 5.5 — Mod_Manifest assente ⇒ Err, nessun descrittore.
    #[test]
    fn load_prefill_errors_when_manifest_absent() {
        let guard = TempGuard::new("absent");
        let err = load_prefill(guard.path()).unwrap_err();
        assert!(err.to_string().contains("assente"), "err: {err}");
    }

    /// Req 5.5 — Mod_Manifest non analizzabile ⇒ Err, nessun descrittore.
    #[test]
    fn load_prefill_errors_when_manifest_unparseable() {
        let guard = TempGuard::new("unparseable");
        std::fs::write(guard.path().join(MANIFEST_FILE), b"= = not toml [[[").unwrap();
        assert!(load_prefill(guard.path()).is_err());
    }

    /// Req 5.5 — Mod_Manifest non conforme allo schema ⇒ Err, nessun descrittore.
    #[test]
    fn load_prefill_errors_when_manifest_invalid() {
        // Manifest analizzabile ma senza entry_points ⇒ validate() fallisce.
        let text = r#"
            schema_version = 1

            [mod]
            id = "com.example.mymod"
            version = "1.2.3"
            name = "My Mod"
            type = "native"
        "#;
        let guard = TempGuard::new("invalid");
        std::fs::write(guard.path().join(MANIFEST_FILE), text).unwrap();
        let err = load_prefill(guard.path()).unwrap_err();
        assert!(err.to_string().contains("non valido"), "err: {err}");
    }

    /// Req 5.2 — i Support_Tag selezionati compaiono esattamente nel descrittore.
    #[test]
    fn build_descriptor_selects_exactly_the_toggled_tags() {
        let mut state = WizardState::new(prefill());
        state.summary = "Una mod.".to_string();
        // Seleziona il primo e il terzo tag.
        state.tag_selected[0] = true;
        state.tag_selected[2] = true;

        let d = state.build_descriptor();
        assert_eq!(
            d.support_tags,
            vec![SUPPORT_TAGS[0].to_string(), SUPPORT_TAGS[2].to_string()]
        );
        assert_eq!(d.mod_id, "com.example.mymod");
        assert_eq!(d.summary, "Una mod.");
    }

    /// Req 5.3 — il gate blocca la conferma quando mancano obbligatori (summary
    /// vuoto) e nessun descrittore/scrittura è prodotto; con summary presente la
    /// conferma passa.
    #[test]
    fn confirm_gate_blocks_when_summary_missing() {
        let mut state = WizardState::new(prefill());
        state.step = WizardStep::Confirm;
        // summary vuoto ⇒ conferma bloccata.
        let outcome = state.handle_key(WizardKey::Enter);
        assert_eq!(outcome, StepOutcome::Continue);
        assert!(state.notice.as_ref().unwrap().contains("summary"));

        // Con summary presente ⇒ conferma consentita.
        state.summary = "descrizione".to_string();
        state.step = WizardStep::Confirm;
        assert_eq!(state.handle_key(WizardKey::Enter), StepOutcome::Confirm);
    }

    /// Req 5.6 — Esc/Ctrl-C in qualunque passo annulla.
    #[test]
    fn cancel_key_aborts_from_any_step() {
        for step in [WizardStep::Summary, WizardStep::Tags, WizardStep::Confirm] {
            let mut state = WizardState::new(prefill());
            state.step = step;
            assert_eq!(state.handle_key(WizardKey::Cancel), StepOutcome::Cancel);
        }
    }

    /// La navigazione con Spazio commuta il tag sotto il cursore.
    #[test]
    fn space_toggles_tag_under_cursor() {
        let mut state = WizardState::new(prefill());
        state.step = WizardStep::Tags;
        state.tag_cursor = 1;
        assert!(!state.tag_selected[1]);
        state.handle_key(WizardKey::Space);
        assert!(state.tag_selected[1]);
        state.handle_key(WizardKey::Space);
        assert!(!state.tag_selected[1]);
    }

    /// Req 5.4 — la scrittura è atomica e produce submission.toml round-trippabile.
    #[test]
    fn write_submission_atomic_writes_round_trippable_file() {
        let guard = TempGuard::new("write");
        let d = sample_descriptor();
        let path = write_submission_atomic(guard.path(), &d).expect("write");
        assert_eq!(path, guard.path().join(SUBMISSION_FILE));
        assert!(path.is_file());
        // Nessun file temporaneo lasciato a terra.
        let leftovers: Vec<_> = std::fs::read_dir(guard.path())
            .unwrap()
            .filter_map(|e| e.ok())
            .filter(|e| e.file_name().to_string_lossy().ends_with(".tmp"))
            .collect();
        assert!(leftovers.is_empty(), "temporanei residui: {leftovers:?}");

        let text = std::fs::read_to_string(&path).unwrap();
        assert_eq!(SubmissionDescriptor::parse(&text).unwrap(), d);
    }
}
