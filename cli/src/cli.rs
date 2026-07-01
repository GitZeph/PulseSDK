//! Definizione dei sottocomandi `clap` della Pulse CLI (Req 14).
//!
//! Lo scaffold del task 14.1 definisce la struttura dei comandi
//! `new` / `build` / `publish`; le implementazioni complete sono nei task
//! 14.2 (`new`), 14.3 (`build`) e 14.4 (`publish`). Qui i gestori sono stub
//! che restituiscono un errore "non ancora implementato" in modo che la CLI
//! compili e mostri l'help corretto.

use std::path::PathBuf;

use clap::{Parser, Subcommand, ValueEnum};

use crate::bindings::{self, GdVersion, Signature, SymbolId, TargetPair, TargetPlatform};
use crate::builder::{self, BuildError};
use crate::installer::{self, InstallError};
use crate::publisher::{self, PublishError, UploadError};
use crate::scaffold;
use crate::surface;

/// Pulse CLI — scaffold, build e publish delle mod Pulse.
#[derive(Debug, Parser)]
#[command(name = "pulse", version, about, long_about = None)]
pub struct Cli {
    #[command(subcommand)]
    pub command: Option<Command>,
}

/// Sottocomandi della CLI (Req 14: new/build/publish).
#[derive(Debug, Subcommand)]
pub enum Command {
    /// Genera lo scaffold di una nuova mod (Req 14.1, 14.2) — task 14.2.
    New {
        /// Directory di destinazione della nuova mod.
        path: PathBuf,
        /// Identificatore della mod (es. com.example.mymod).
        #[arg(long)]
        id: Option<String>,
    },
    /// Compila la mod e produce un Pulse Package `.pulse` (Req 14.3-14.5) — task 14.3.
    Build {
        /// Directory del progetto da compilare (default: directory corrente).
        #[arg(default_value = ".")]
        path: PathBuf,
    },
    /// Pubblica il Pulse Package sul Marketplace (Req 14.6-14.8) — task 14.4.
    Publish {
        /// Directory del progetto da pubblicare (default: directory corrente).
        #[arg(default_value = ".")]
        path: PathBuf,
    },
    /// Installa il Pulse_Loader in una `GD_Installation` (Req 7).
    Install {
        /// Path to the Geometry Dash .app bundle root
        #[arg(long = "gd")]
        gd: PathBuf,
        /// Path to the built libpulse_loader.dylib artifact
        #[arg(long = "artifact")]
        artifact: PathBuf,
        /// Use the real native macOS injection (LC_LOAD_DYLIB + ad-hoc resign) instead of the host-testable stub
        #[arg(long = "native", default_value_t = false)]
        native: bool,
    },
    /// Disinstalla il Pulse_Loader da una `GD_Installation` (Req 7.8).
    Uninstall {
        #[arg(long = "gd")]
        gd: PathBuf,
    },
    /// Strumenti della Bindings_Pipeline build-time (Req 1/3/6/10/11) — task 17.1.
    ///
    /// Raggruppa i sottocomandi che inoltrano ai componenti della pipeline:
    /// `Contribution_Flow` (`add`/`set-offset`/`set-signature`),
    /// `Observational_Cross_Check` + `Geode_Firewall` (`crosscheck`),
    /// `Prologue_Verification` (`verify-prologue`), `Binding_Generator`
    /// (`generate`), `Pbind_Linter` (`lint`), `Validation_Stage` (`validate`) e
    /// la lettura del `Provenance_Record` (`provenance`).
    Bindings {
        #[command(subcommand)]
        action: BindingsCommand,
    },
    /// Strumenti della GD API Surface build-time (Req 1/3/10) — task 11.1.
    ///
    /// Raggruppa i sottocomandi che inoltrano ai componenti **nuovi** della
    /// superficie, costruita **sopra** la `bindings-pipeline` senza
    /// re-implementarne i pezzi:
    ///
    /// - `compile` — `load_manifest` + `load_catalog` (riuso) → `compile_surface`
    ///   → scrive la `Surface_IR` (`surface.ir.json`);
    /// - `generate` — il flusso end-to-end **compile → generate**: `compile_surface`
    ///   poi `generate_cpp`, emettendo l'API SDK C++
    ///   `sdk/include/pulse/gd/{types,bindings,hooks}.gen.hpp` (Req 10.1) dalla
    ///   `Surface_IR` language-agnostic (Req 10.2);
    /// - `lint` — `Surface_Linter` (duplicati, simboli mancanti, schema);
    /// - `validate` — `Surface_Validator` (gate di completezza della provenienza).
    ///
    /// Tutti i gestori sono **fail-closed**: su errore di caricamento/compilazione
    /// non viene prodotto alcun artefatto e la causa è propagata all'utente.
    Surface {
        #[command(subcommand)]
        action: SurfaceCommand,
    },
    /// Analizza il `Dev_Environment` e segnala/risolve i problemi (Req 1).
    Doctor,
    /// Avvia Geometry Dash e fa streaming live del `Log_Stream` (Req 2).
    Logs {
        /// Percorso della `GD_Installation`; se assente, scoperta localmente.
        #[arg(long = "gd")]
        gd: Option<PathBuf>,
    },
    /// Genera una `Byte_Signature` stabile per un `Offset` (Req 3).
    Siggen {
        /// Offset decimale (`4096`) o esadecimale (`0x1000`).
        offset: String,
        /// Percorso della `GD_Installation`; se assente, scoperta localmente.
        #[arg(long = "gd")]
        gd: Option<PathBuf>,
    },
    /// Valida le `Offset_Declaration` del `pulse.toml` contro il `GD_Binary` (Req 4).
    CheckOffsets {
        /// Directory del progetto (default: directory corrente).
        #[arg(default_value = ".")]
        path: PathBuf,
        /// Percorso della `GD_Installation`; se assente, scoperta localmente.
        #[arg(long = "gd")]
        gd: Option<PathBuf>,
    },
    /// Procedura guidata interattiva di submission verso l'`Index` (Req 5).
    Submit {
        /// Directory del progetto (default: directory corrente).
        #[arg(default_value = ".")]
        path: PathBuf,
    },
    /// Upload di artefatti + metadati verso l'`Index_Endpoint` (Req 6).
    Upload {
        /// Directory del progetto (default: directory corrente).
        #[arg(default_value = ".")]
        path: PathBuf,
    },
}

impl Cli {
    /// Esegue il comando selezionato.
    ///
    /// Se nessun sottocomando è stato fornito (`command == None`), avvia la
    /// TUI full-screen interattiva ([`crate::tui::launch`]); altrimenti esegue
    /// il sottocomando come di consueto.
    pub fn run(self) -> anyhow::Result<()> {
        let command = match self.command {
            Some(command) => command,
            None => return crate::tui::launch(),
        };
        match command {
            Command::New { path, id } => run_new(&path, id.as_deref()),
            Command::Build { path } => run_build(&path),
            Command::Publish { path } => run_publish(&path),
            Command::Install {
                gd,
                artifact,
                native,
            } => run_install(&gd, &artifact, native),
            Command::Uninstall { gd } => run_uninstall(&gd),
            Command::Bindings { action } => action.run(),
            Command::Surface { action } => action.run(),
            Command::Doctor => run_doctor(),
            Command::Logs { gd } => run_logs(gd.as_deref()),
            Command::Siggen { offset, gd } => run_siggen(&offset, gd.as_deref()),
            Command::CheckOffsets { path, gd } => run_check_offsets(&path, gd.as_deref()),
            Command::Submit { path } => run_submit(&path),
            Command::Upload { path } => run_upload(&path),
        }
    }
}

/// Esegue `pulse new`: genera lo scaffold di una nuova mod (Req 14.1, 14.2).
///
/// Estratto come funzione pubblica così che la TUI possa invocare la stessa
/// logica dei sottocomandi senza duplicarla né lanciare un secondo processo.
pub fn run_new(path: &std::path::Path, id: Option<&str>) -> anyhow::Result<()> {
    let outcome = scaffold::scaffold_new(path, id)?;
    println!(
        "Creata nuova mod Pulse '{}' in {}",
        outcome.mod_id,
        outcome.project_dir.display()
    );
    for file in &outcome.created_files {
        println!("  + {file}");
    }
    Ok(())
}

/// Esegue `pulse build`: valida il manifest, compila e produce il `.pulse`.
///
/// Su manifest non valido (Req 14.4) o compilazione fallita (Req 14.5) NON
/// viene prodotto alcun pacchetto e l'errore riporta, rispettivamente, l'elenco
/// dei campi non conformi o l'elenco delle cause.
pub fn run_build(path: &std::path::Path) -> anyhow::Result<()> {
    match builder::build(path) {
        Ok(outcome) => {
            println!(
                "Build completata: mod '{}' impacchettata in {}",
                outcome.mod_id,
                outcome.package_path.display()
            );
            for entry in &outcome.entries {
                println!("  + {entry}");
            }
            Ok(())
        }
        Err(BuildError::InvalidManifest(violations)) => {
            // Req 14.4: nessun pacchetto, elenco completo dei campi non conformi.
            let mut msg = String::from(
                "build interrotta: il Manifest non e' valido, nessun Pulse Package prodotto.\n\
                 Campi del Manifest non validi:",
            );
            for v in &violations {
                msg.push_str(&format!("\n  - {}: {}", v.field, v.message));
            }
            anyhow::bail!(msg)
        }
        Err(BuildError::CompilationFailed(causes)) => {
            // Req 14.5: nessun pacchetto, elenco delle cause della compilazione.
            let mut msg = String::from(
                "build interrotta: compilazione del codice sorgente fallita, \
                 nessun Pulse Package prodotto.\nCause:",
            );
            for c in &causes {
                msg.push_str(&format!("\n  - {c}"));
            }
            anyhow::bail!(msg)
        }
        Err(other) => Err(anyhow::Error::new(other)),
    }
}

/// Esegue `pulse publish`: valida il manifest, costruisce il `.pulse` e lo
/// carica sul Marketplace tramite il client di default.
///
/// Su manifest non valido (Req 14.7) la pubblicazione viene annullata SENZA
/// caricare nulla e l'errore riporta l'elenco completo dei campi non conformi.
/// Su Marketplace non disponibile o errore di rete (Req 14.8) la pubblicazione
/// viene interrotta SENZA lasciare alcun pacchetto parziale nel Marketplace.
fn run_publish(path: &std::path::Path) -> anyhow::Result<()> {
    match publisher::publish(path) {
        Ok(outcome) => {
            println!(
                "Pubblicazione completata: mod '{}' v{} caricata su {}",
                outcome.mod_id, outcome.version, outcome.receipt.location
            );
            Ok(())
        }
        Err(PublishError::InvalidManifest(violations)) => {
            // Req 14.7: nessun caricamento, elenco completo dei campi non conformi.
            let mut msg = String::from(
                "pubblicazione annullata: il Manifest non e' valido, nessun Pulse Package \
                 caricato nel Marketplace.\nCampi del Manifest non validi:",
            );
            for v in &violations {
                msg.push_str(&format!("\n  - {}: {}", v.field, v.message));
            }
            anyhow::bail!(msg)
        }
        Err(PublishError::Upload(upload_err)) => {
            // Req 14.8: nessun pacchetto parziale nel Marketplace, fallimento segnalato.
            let cause = match &upload_err {
                UploadError::Unavailable(_) => "Marketplace non disponibile",
                UploadError::Network(_) => "errore di rete",
            };
            anyhow::bail!(
                "pubblicazione interrotta: caricamento nel Marketplace fallito ({cause}), \
                 nessun Pulse Package parziale lasciato nel Marketplace.\nCausa: {upload_err}"
            )
        }
        Err(other) => Err(anyhow::Error::new(other)),
    }
}

/// Esegue `pulse install`: installa il Pulse_Loader in una `GD_Installation`.
///
/// Con `native` true usa l'iniezione nativa macOS reale
/// ([`installer::install_native`]); altrimenti lo stub host-testabile
/// ([`installer::install`]). In caso di successo stampa l'elenco completo dei
/// file modificati (Req 7.7); su [`InstallError`] interrompe con un messaggio
/// chiaro (la `Display` dell'errore è già descrittiva).
pub fn run_install(gd: &std::path::Path, artifact: &std::path::Path, native: bool) -> anyhow::Result<()> {
    let result = if native {
        installer::install_native(gd, artifact)
    } else {
        installer::install(gd, artifact)
    };
    match result {
        Ok(modified) => {
            println!(
                "Installazione completata: Pulse_Loader installato in {}",
                gd.display()
            );
            for file in &modified.0 {
                println!("  ~ {}", file.display());
            }
            Ok(())
        }
        Err(err) => map_install_err(err),
    }
}

/// Esegue `pulse uninstall`: disinstalla il Pulse_Loader da una
/// `GD_Installation`, riportando l'albero allo stato pre-installazione.
///
/// In caso di successo stampa l'elenco completo dei file ripristinati/eliminati
/// (Req 7.7); su [`InstallError`] interrompe con un messaggio chiaro.
pub fn run_uninstall(gd: &std::path::Path) -> anyhow::Result<()> {
    match installer::uninstall(gd) {
        Ok(restored) => {
            println!(
                "Disinstallazione completata: Pulse_Loader rimosso da {}",
                gd.display()
            );
            for file in &restored.0 {
                println!("  ~ {}", file.display());
            }
            Ok(())
        }
        Err(err) => map_install_err(err),
    }
}

/// Mappa un [`InstallError`] su `anyhow::bail!` con un messaggio chiaro.
/// La `Display` di [`InstallError`] è già descrittiva.
fn map_install_err(err: InstallError) -> anyhow::Result<()> {
    anyhow::bail!("operazione installer interrotta: {err}")
}

// ===========================================================================
// CLI Command Suite — i sei gestori `run_*` (stesso pattern di `run_build`,
// `run_install`, …), richiamati sia dalla linea di comando sia dalla TUI
// in-process, senza duplicare la logica (Req 7.1).
// ===========================================================================

/// Esegue `pulse doctor`: costruisce il registry dei controlli built-in, esegue
/// tutti i controlli, stampa l'`Environment_Report` **completo** e termina
/// secondo la exit policy — report pronto (o con soli `warning`) ⇒ successo;
/// almeno un `problema` ⇒ errore `anyhow` con codice ≠ 0, dopo aver comunque
/// stampato tutti i controlli (Req 1.5, 1.6, 1.7, 7.1).
pub fn run_doctor() -> anyhow::Result<()> {
    let registry = crate::doctor::default_registry();
    let report = crate::doctor::run_all(&registry);

    // Stampa l'intero report (tutte le voci) in ogni caso (Req 1.6).
    print!("{}", report.render());

    if crate::doctor::report::exit_code(&report) == 0 {
        Ok(())
    } else {
        // Req 1.6: almeno un `problema` ⇒ codice ≠ 0 dopo il report completo.
        anyhow::bail!(
            "pulse doctor: rilevati problemi che bloccano il Dev_Environment; \
             risolvili con le azioni indicate nel report"
        )
    }
}

/// Esegue `pulse logs`: avvia Geometry Dash e fa streaming live del
/// `Log_Stream`, traducendo l'esito nella convenzione `anyhow` (Req 2.1, 2.3,
/// 2.4, 2.5, 7.1).
///
/// Uscita normale ⇒ successo; crash (codice ≠ 0 / segnale) ⇒ errore con codice
/// ≠ 0; interruzione dell'utente (Ctrl-C) ⇒ uscita pulita dopo la terminazione
/// del processo figlio. `GD` non individuabile/non avviabile propaga l'errore
/// con causa da [`crate::logs::stream_logs`] (Req 2.5).
pub fn run_logs(gd: Option<&std::path::Path>) -> anyhow::Result<()> {
    match crate::logs::stream_logs(gd)? {
        // Uscita normale o interruzione volontaria dell'utente ⇒ uscita pulita.
        crate::logs::LogsExit::Normal | crate::logs::LogsExit::Interrupted => Ok(()),
        // Crash: codice ≠ 0 / segnale ⇒ errore con causa (Req 2.4).
        crate::logs::LogsExit::Crashed(code) => anyhow::bail!(
            "pulse logs: Geometry Dash è terminato in modo anomalo (codice {code})"
        ),
    }
}

/// Esegue `pulse siggen <offset>`: risolve la `GD_Installation`, interpreta
/// l'offset, legge il `GD_Binary` e genera una `Byte_Signature` verificata,
/// emettendola su stdout **solo** su successo (fail-closed, Req 3).
///
/// - `GD` non fornita né scopribile ⇒ errore con causa, **nessun binario
///   ridistribuito** (Req 3.7).
/// - Offset in formato non valido ⇒ errore con il formato atteso, **nessuna
///   firma** (Req 3.2, 3.3).
/// - Offset fuori dai limiti / binario illeggibile / firma non a corrispondenza
///   unica ⇒ errore, **nessuna firma** (Req 3.5, 3.6).
pub fn run_siggen(offset: &str, gd: Option<&std::path::Path>) -> anyhow::Result<()> {
    // Risolve la GD_Installation (fornita o scoperta; assente ⇒ errore, nessun
    // binario ridistribuito) riusando la stessa logica di `pulse logs` (Req 3.7).
    let install = crate::logs::resolve_installation(gd)?;

    // Interpreta l'offset con l'helper condiviso: formato non valido ⇒ errore
    // col formato atteso, nessuna firma (Req 3.2, 3.3).
    let offset = parse_offset(offset)?;

    // Legge i byte dell'eseguibile Mach-O del bundle (GD_Binary), in sola
    // lettura; illeggibile ⇒ errore, nessuna firma (Req 3.5).
    let executable = install.executable();
    let binary = std::fs::read(&executable).map_err(|err| {
        anyhow::anyhow!(
            "pulse siggen: lettura del GD_Binary '{}' fallita: {err}",
            executable.display()
        )
    })?;

    // Genera e verifica la firma (fail-closed): emette SOLO su successo (Req 3.1).
    let signature = crate::siggen::generate(&binary, offset)?;
    println!("{}", signature.render());
    Ok(())
}

/// Esegue `pulse check-offsets`: carica il `Mod_Manifest`, risolve la
/// `GD_Installation`, apre il `GD_Binary` in sola lettura, classifica **ogni**
/// `Offset_Declaration` e riporta **tutti** gli `Offset_Verdict` (Req 4).
///
/// - Manifest assente / senza alcuna `Offset_Declaration` ⇒ errore, codice ≠ 0,
///   **nessun output parziale** di verdetti (Req 4.6).
/// - `GD` non fornita né scopribile ⇒ errore, codice ≠ 0, **nessun binario
///   ridistribuito** (Req 4.7).
/// - Tutti `Valid` ⇒ codice 0; almeno un `Invalid`/`Shifted`/`Undeterminable`
///   ⇒ codice ≠ 0 **dopo** aver riportato tutti i verdetti (Req 4.8, 4.9).
pub fn run_check_offsets(
    project_dir: &std::path::Path,
    gd: Option<&std::path::Path>,
) -> anyhow::Result<()> {
    use crate::checkoffsets::Verdict;

    // 1. Carica il Mod_Manifest. Assente/vuoto/senza [[offsets]] ⇒ errore,
    //    codice ≠ 0, nessun output parziale di verdetti (Req 4.6).
    let manifest_path = project_dir.join(crate::builder::MANIFEST_FILE);
    if !manifest_path.is_file() {
        anyhow::bail!(
            "pulse check-offsets: Mod_Manifest assente ('{}' non trovato); \
             nessun verdetto prodotto",
            manifest_path.display()
        );
    }
    let text = std::fs::read_to_string(&manifest_path).map_err(|err| {
        anyhow::anyhow!(
            "pulse check-offsets: lettura del Mod_Manifest '{}' fallita: {err}",
            manifest_path.display()
        )
    })?;
    let manifest = crate::manifest::Manifest::parse(&text).map_err(|err| {
        anyhow::anyhow!("pulse check-offsets: Mod_Manifest non analizzabile: {err}")
    })?;
    if manifest.offsets.is_empty() {
        anyhow::bail!(
            "pulse check-offsets: il Mod_Manifest non contiene alcuna Offset_Declaration \
             (tabella `[[offsets]]` assente o vuota); nessun verdetto prodotto"
        );
    }

    // 2. Risolve la GD_Installation (assente ⇒ errore, codice ≠ 0, nessun
    //    binario ridistribuito) riusando la logica di `pulse logs` (Req 4.7).
    let install = crate::logs::resolve_installation(gd)?;

    // 3. Apre il GD_Binary in sola lettura (Req 4.1).
    let executable = install.executable();
    let binary = std::fs::read(&executable).map_err(|err| {
        anyhow::anyhow!(
            "pulse check-offsets: lettura del GD_Binary '{}' fallita: {err}",
            executable.display()
        )
    })?;

    // 4. Classifica tutte le dichiarazioni e riporta TUTTI i verdetti (Req 4.9).
    let verdicts = crate::checkoffsets::check_all(&binary, &manifest.offsets);
    println!("pulse check-offsets — verifica delle Offset_Declaration contro il GD_Binary");
    for v in &verdicts {
        match &v.verdict {
            Verdict::Valid => {
                println!("  [valid]         {} @ 0x{:X}", v.name, v.declared);
            }
            Verdict::Shifted { detected, delta } => {
                println!(
                    "  [shifted]       {} dichiarato 0x{:X} → rilevato 0x{:X} (delta {:+})",
                    v.name, v.declared, detected, delta
                );
            }
            Verdict::Invalid => {
                println!("  [invalid]       {} @ 0x{:X}", v.name, v.declared);
            }
            Verdict::Undeterminable { reason } => {
                println!(
                    "  [undeterminable] {} @ 0x{:X}: {reason}",
                    v.name, v.declared
                );
            }
        }
    }

    // 5. Exit policy: tutti Valid ⇒ successo; altrimenti errore dopo il report
    //    completo (Req 4.8, 4.9).
    let all_valid = verdicts.iter().all(|v| v.verdict == Verdict::Valid);
    if all_valid {
        Ok(())
    } else {
        anyhow::bail!(
            "pulse check-offsets: uno o più Offset_Verdict non sono `Valid` \
             (Invalid/Shifted/Undeterminable); vedi il report sopra"
        )
    }
}

/// Esegue `pulse submit`: avvia la procedura guidata interattiva di submission
/// e traduce l'esito nella convenzione `anyhow` (Req 5.1, 7.1).
///
/// Conferma valida ⇒ successo con conferma della scrittura del
/// `submission.toml`; annullamento ⇒ uscita pulita senza produrre nulla;
/// `Mod_Manifest` assente/non valido ⇒ errore con causa da
/// [`crate::submit::run_wizard`] (Req 5.5).
pub fn run_submit(project_dir: &std::path::Path) -> anyhow::Result<()> {
    match crate::submit::run_wizard(project_dir)? {
        Some(descriptor) => {
            println!(
                "pulse submit: submission preparata per '{}' — '{}' scritto in {}",
                descriptor.mod_id,
                crate::submit::SUBMISSION_FILE,
                project_dir.display()
            );
            Ok(())
        }
        None => {
            // Req 5.6: annullamento ⇒ uscita pulita, nessun output parziale.
            println!("pulse submit: procedura annullata, nessuna submission prodotta");
            Ok(())
        }
    }
}

/// Esegue `pulse upload`: costruisce il [`crate::upload::DefaultIndexClient`] e
/// delega a [`crate::upload::run`], propagando l'esito con la stessa convenzione
/// `anyhow` degli altri gestori (Req 6.1, 7.1).
///
/// `Index_Endpoint` non configurato ⇒ dry-run a zero rete (successo); endpoint
/// configurato ⇒ upload reale con verifica di raggiungibilità e timeout.
pub fn run_upload(project_dir: &std::path::Path) -> anyhow::Result<()> {
    let client = crate::upload::DefaultIndexClient::new();
    crate::upload::run(project_dir, &client)
}

// ===========================================================================
// `pulse bindings` — sottocomandi della Bindings_Pipeline (task 17.1).
// ===========================================================================

/// Metodo della `Prologue_Verification` selezionabile da CLI (`--method`),
/// mappato su [`bindings::prologue::PrologueMethod`] (Req 5.5).
#[derive(Debug, Clone, Copy, PartialEq, Eq, ValueEnum)]
pub enum PrologueMethodArg {
    /// Disassemblamento manuale via `otool` sul binario reale (Phase E).
    #[value(name = "otool-manual")]
    OtoolManual,
    /// Confronto programmatico dei byte di prologo (host-testabile).
    #[value(name = "automatic-bytes")]
    AutomaticBytes,
}

impl From<PrologueMethodArg> for bindings::prologue::PrologueMethod {
    fn from(value: PrologueMethodArg) -> Self {
        match value {
            PrologueMethodArg::OtoolManual => bindings::prologue::PrologueMethod::OtoolManual,
            PrologueMethodArg::AutomaticBytes => {
                bindings::prologue::PrologueMethod::AutomaticBytes
            }
        }
    }
}

/// Sottocomandi di `pulse bindings`, ognuno **inoltra a un componente** della
/// Bindings_Pipeline (design Phase I, Req 1/3/6/10/11).
///
/// Il flusso end-to-end della pipeline è
/// `load_catalog → crosscheck → verify-prologue → generate → lint → validate`:
///
/// - `add`/`set-offset`/`set-signature` popolano il `Binding_Catalog`
///   (`Contribution_Flow`, Req 2);
/// - `crosscheck` confronta un indirizzo candidato col `Geode_Reference`
///   passando dal `Geode_Firewall` (Req 4);
/// - `verify-prologue` esegue la `Prologue_Verification` su un offset (Req 5);
/// - `generate` carica il catalogo (`load_catalog`) ed **emette i `.pbind`**
///   (`Binding_Generator`, Req 3); su un catalogo che fallisce il caricamento
///   **non produce alcun output derivato** e riporta la causa (Req 3.7, 11.5);
/// - `lint` controlla schema/valori di un `.pbind` (`Pbind_Linter`, Req 7);
/// - `validate` applica la semantica "resolved sse verificato"
///   (`Validation_Stage`, Req 6);
/// - `provenance` **legge** il `Provenance_Record` di un offset senza
///   rieseguire cross-check/prologo (Req 10.4).
///
/// Tutti i gestori propagano gli errori dei componenti come `anyhow` con un
/// messaggio chiaro, in modalità **fail-closed** (Req 11.5).
#[derive(Debug, Subcommand)]
pub enum BindingsCommand {
    /// Aggiunge una `Catalog_Entry` (simbolo + firma) con offset sentinel per le
    /// coppie indicate (`Contribution_Flow`, Req 2.1).
    Add {
        /// Radice del catalogo (`mod-index/catalog`).
        #[arg(long = "catalog-root")]
        catalog_root: PathBuf,
        /// Identificatore di simbolo, es. `MenuLayer::init`.
        #[arg(long = "symbol")]
        symbol: String,
        /// Tipo di ritorno della firma, es. `bool`.
        #[arg(long = "return", value_name = "RETURN_TYPE")]
        return_type: String,
        /// Tipi dei parametri (separati da virgola), es. `MenuLayer*` o
        /// `PlayLayer*,float`. Il `this` è il primo parametro per i metodi.
        #[arg(long = "params", value_delimiter = ',', default_value = "")]
        params: Vec<String>,
        /// Versione GD `<major>.<minor>` (es. `2.2081`) per le coppie sentinel.
        #[arg(long = "gd")]
        gd: Option<String>,
        /// Piattaforme target (`macos-arm64`, …) abbinate alla `--gd` indicata.
        #[arg(long = "platform")]
        platform: Vec<String>,
    },
    /// Associa un offset (`rva`) a una coppia e registra il `Contributor`
    /// (`Contribution_Flow`, Req 2.2/2.4).
    SetOffset {
        #[arg(long = "catalog-root")]
        catalog_root: PathBuf,
        #[arg(long = "symbol")]
        symbol: String,
        #[arg(long = "gd")]
        gd: String,
        #[arg(long = "platform")]
        platform: String,
        /// RVA dell'offset (decimale oppure esadecimale `0x…`).
        #[arg(long = "rva")]
        rva: String,
        /// Identità del `Contributor`, registrata come fonte dell'`Address_Data`.
        #[arg(long = "contributor")]
        contributor: String,
    },
    /// Aggiorna la firma di una voce e riporta a `false` tutti i suoi
    /// `Verified_Flag` (`Contribution_Flow`, Req 2.5).
    SetSignature {
        #[arg(long = "catalog-root")]
        catalog_root: PathBuf,
        #[arg(long = "symbol")]
        symbol: String,
        #[arg(long = "return", value_name = "RETURN_TYPE")]
        return_type: String,
        #[arg(long = "params", value_delimiter = ',', default_value = "")]
        params: Vec<String>,
    },
    /// Esegue l'`Observational_Cross_Check` di un indirizzo candidato contro il
    /// `Geode_Reference` passando dal `Geode_Firewall` (Req 4).
    Crosscheck {
        /// File `Geode_Reference` numerico
        /// (`mod-index/catalog/geode-reference/{version}/{platform}.toml`).
        #[arg(long = "reference")]
        reference: PathBuf,
        /// Simbolo da confrontare (chiave nella tabella di riferimento).
        #[arg(long = "symbol")]
        symbol: String,
        /// Indirizzo candidato (decimale oppure esadecimale `0x…`).
        #[arg(long = "rva")]
        rva: String,
        #[arg(long = "gd")]
        gd: String,
        #[arg(long = "platform")]
        platform: String,
    },
    /// Esegue la `Prologue_Verification` su un offset del catalogo (Req 5).
    VerifyPrologue {
        #[arg(long = "catalog-root")]
        catalog_root: PathBuf,
        #[arg(long = "symbol")]
        symbol: String,
        #[arg(long = "gd")]
        gd: String,
        #[arg(long = "platform")]
        platform: String,
        /// Metodo di verifica del prologo.
        #[arg(long = "method", value_enum)]
        method: PrologueMethodArg,
        /// Byte di prologo osservati all'offset, esadecimale (es. `626f6f6c`).
        /// Se assenti, i byte sono trattati come illeggibili (fail-closed).
        #[arg(long = "bytes")]
        bytes: Option<String>,
    },
    /// Carica il catalogo ed **emette i `.pbind`** (`Binding_Generator`, Req 3).
    Generate {
        /// Radice del catalogo (`mod-index/catalog`).
        #[arg(long = "catalog-root")]
        catalog_root: PathBuf,
        /// Radice di output (`mod-index/`): i `.pbind` finiscono sotto
        /// `out-root/bindings/{version}/{platform}.pbind`.
        #[arg(long = "out-root")]
        out_root: PathBuf,
    },
    /// Controlla schema e valori di un `.pbind` (`Pbind_Linter`, Req 7).
    Lint {
        /// Percorso del `.pbind` da analizzare.
        path: PathBuf,
    },
    /// Applica la semantica "resolved sse verificato" alle voci del catalogo,
    /// per coppia (`Validation_Stage`, Req 6).
    Validate {
        #[arg(long = "catalog-root")]
        catalog_root: PathBuf,
    },
    /// **Legge** il `Provenance_Record` di un offset senza ricalcolarlo
    /// (Req 10.4).
    Provenance {
        #[arg(long = "catalog-root")]
        catalog_root: PathBuf,
        #[arg(long = "symbol")]
        symbol: String,
        #[arg(long = "gd")]
        gd: String,
        #[arg(long = "platform")]
        platform: String,
    },
    /// **Deriva automaticamente** `Catalog_Entry` (nomi, firme, offset,
    /// provenienza) dai binari first-party di Geometry Dash (`Binding_Extractor`,
    /// Req 8, 10). Parametrizzato per coppia `(GD_Version, Target_Platform)`:
    /// `open_source_binary → elf/demangle → rtti/vtable → match → prologue →
    /// tier/geode → write`. I binari sono **locali e forniti dall'utente**;
    /// nessuna richiesta di rete in uscita (Req 10.1, 10.2).
    Extract {
        /// Versione GD `<major>.<minor>` (es. `2.2081`).
        #[arg(long = "gd")]
        gd: String,
        /// `Target_Platform` (`macos-arm64` | `macos-x64` | `windows-x64` |
        /// `android-arm64`).
        #[arg(long = "platform")]
        platform: String,
        /// `Android_Symbol_Source` ELF (master di nomi e firme).
        #[arg(long = "elf")]
        elf: Option<PathBuf>,
        /// `Source_Binary` Mach-O di destinazione (RTTI/vtable per macOS).
        #[arg(long = "macho", conflicts_with = "pe")]
        macho: Option<PathBuf>,
        /// `Source_Binary` PE di destinazione (RTTI/vtable per Windows).
        #[arg(long = "pe")]
        pe: Option<PathBuf>,
        /// `Geode_Reference_Table` opzionale (numerico-only) per il cross-check.
        #[arg(long = "geode-ref")]
        geode_ref: Option<PathBuf>,
        /// Radice del catalogo (default `mod-index/catalog`).
        #[arg(long = "catalog-root", default_value = "mod-index/catalog")]
        catalog_root: PathBuf,
    },
}

impl BindingsCommand {
    /// Esegue il sottocomando `bindings` selezionato, inoltrando al componente.
    pub fn run(self) -> anyhow::Result<()> {
        match self {
            BindingsCommand::Add {
                catalog_root,
                symbol,
                return_type,
                params,
                gd,
                platform,
            } => run_bindings_add(&catalog_root, symbol, return_type, params, gd, platform),
            BindingsCommand::SetOffset {
                catalog_root,
                symbol,
                gd,
                platform,
                rva,
                contributor,
            } => run_bindings_set_offset(&catalog_root, &symbol, &gd, &platform, &rva, &contributor),
            BindingsCommand::SetSignature {
                catalog_root,
                symbol,
                return_type,
                params,
            } => run_bindings_set_signature(&catalog_root, &symbol, return_type, params),
            BindingsCommand::Crosscheck {
                reference,
                symbol,
                rva,
                gd,
                platform,
            } => run_bindings_crosscheck(&reference, &symbol, &rva, &gd, &platform),
            BindingsCommand::VerifyPrologue {
                catalog_root,
                symbol,
                gd,
                platform,
                method,
                bytes,
            } => run_bindings_verify_prologue(
                &catalog_root,
                &symbol,
                &gd,
                &platform,
                method,
                bytes.as_deref(),
            ),
            BindingsCommand::Generate {
                catalog_root,
                out_root,
            } => run_bindings_generate(&catalog_root, &out_root),
            BindingsCommand::Lint { path } => run_bindings_lint(&path),
            BindingsCommand::Validate { catalog_root } => run_bindings_validate(&catalog_root),
            BindingsCommand::Provenance {
                catalog_root,
                symbol,
                gd,
                platform,
            } => run_bindings_provenance(&catalog_root, &symbol, &gd, &platform),
            BindingsCommand::Extract {
                gd,
                platform,
                elf,
                macho,
                pe,
                geode_ref,
                catalog_root,
            } => run_bindings_extract(
                &gd,
                &platform,
                elf.as_deref(),
                macho.as_deref(),
                pe.as_deref(),
                geode_ref.as_deref(),
                &catalog_root,
            ),
        }
    }
}

// --- Helper di parsing condivisi dai sottocomandi ---------------------------

/// Interpreta `"<major>.<minor>"` come [`GdVersion`] (es. `"2.2081"`).
fn parse_gd_version(value: &str) -> anyhow::Result<GdVersion> {
    let (major, minor) = value
        .split_once('.')
        .ok_or_else(|| anyhow::anyhow!("--gd malformata {value:?}: atteso \"<major>.<minor>\""))?;
    let major: u32 = major
        .parse()
        .map_err(|_| anyhow::anyhow!("--gd malformata {value:?}: major non intero"))?;
    let minor: u32 = minor
        .parse()
        .map_err(|_| anyhow::anyhow!("--gd malformata {value:?}: minor non intero"))?;
    Ok(GdVersion::new(major, minor))
}

/// Interpreta una stringa di piattaforma (`macos-arm64`, …) come
/// [`TargetPlatform`] dall'insieme finito chiuso (`from_platform_id`).
fn parse_platform(value: &str) -> anyhow::Result<TargetPlatform> {
    TargetPlatform::from_platform_id(value).ok_or_else(|| {
        anyhow::anyhow!(
            "--platform non supportata {value:?}: attesa una tra macos-arm64, macos-x64, \
             windows-x64, android-arm64, ios-arm64"
        )
    })
}

/// Interpreta una coppia da `--gd`/`--platform`.
fn parse_pair(gd: &str, platform: &str) -> anyhow::Result<TargetPair> {
    Ok(TargetPair::new(parse_gd_version(gd)?, parse_platform(platform)?))
}

/// Interpreta un offset come intero non negativo espresso in forma decimale
/// (es. `4096`) oppure esadecimale con prefisso `0x`/`0X` (es. `0x1000`), come
/// `u64`.
///
/// Fonte unica dell'accettazione decimale/esadecimale condivisa da tutti i
/// comandi che leggono un offset (`bindings set-offset`, `crosscheck`,
/// `run_siggen`, `run_check_offsets`), così che usino la **stessa** notazione.
/// Su formato non valido restituisce un errore che indica il formato atteso
/// senza produrre alcun valore.
pub(crate) fn parse_offset(value: &str) -> anyhow::Result<u64> {
    let parsed = if let Some(hex) = value.strip_prefix("0x").or_else(|| value.strip_prefix("0X")) {
        u64::from_str_radix(hex, 16)
    } else {
        value.parse::<u64>()
    };
    parsed.map_err(|_| {
        anyhow::anyhow!(
            "offset malformato {value:?}: atteso intero non negativo decimale (es. 4096) \
             oppure esadecimale con prefisso 0x (es. 0x1000)"
        )
    })
}

/// Interpreta una stringa esadecimale (lunghezza pari) come vettore di byte.
fn parse_hex_bytes(value: &str) -> anyhow::Result<Vec<u8>> {
    let trimmed = value.trim();
    if trimmed.len() % 2 != 0 {
        anyhow::bail!("--bytes malformato {value:?}: numero dispari di cifre esadecimali");
    }
    (0..trimmed.len())
        .step_by(2)
        .map(|i| {
            u8::from_str_radix(&trimmed[i..i + 2], 16)
                .map_err(|_| anyhow::anyhow!("--bytes malformato {value:?}: cifra non esadecimale"))
        })
        .collect()
}

/// Costruisce e valida una [`Signature`] da `--return`/`--params`, scartando i
/// parametri vuoti (artefatto del `default_value = ""`).
fn build_signature(return_type: String, params: Vec<String>) -> Signature {
    let params = params.into_iter().filter(|p| !p.trim().is_empty()).collect();
    Signature::new(return_type, params)
}

// --- Gestori dei sottocomandi (inoltro ai componenti) -----------------------

/// `bindings add`: inoltra a [`bindings::contribution::add`] (Req 2.1).
fn run_bindings_add(
    catalog_root: &std::path::Path,
    symbol: String,
    return_type: String,
    params: Vec<String>,
    gd: Option<String>,
    platform: Vec<String>,
) -> anyhow::Result<()> {
    let signature = build_signature(return_type, params);

    // Coppie sentinel: ogni `--platform` è abbinata alla `--gd` indicata.
    let pairs: Vec<TargetPair> = match (&gd, platform.is_empty()) {
        (_, true) => Vec::new(),
        (Some(gd), false) => platform
            .iter()
            .map(|p| parse_pair(gd, p))
            .collect::<anyhow::Result<_>>()?,
        (None, false) => anyhow::bail!("--platform richiede una --gd associata"),
    };

    let symbol = SymbolId::new(symbol);
    match bindings::contribution::add(catalog_root, symbol, signature, &pairs) {
        Ok(path) => {
            println!("Catalog_Entry creata: {}", path.display());
            Ok(())
        }
        Err(err) => anyhow::bail!("contribuzione `add` interrotta: {err}"),
    }
}

/// `bindings set-offset`: inoltra a [`bindings::contribution::set_offset`]
/// (Req 2.2/2.4).
fn run_bindings_set_offset(
    catalog_root: &std::path::Path,
    symbol: &str,
    gd: &str,
    platform: &str,
    rva: &str,
    contributor: &str,
) -> anyhow::Result<()> {
    let pair = parse_pair(gd, platform)?;
    let rva = parse_offset(rva)?;
    let symbol = SymbolId::new(symbol);
    match bindings::contribution::set_offset(catalog_root, &symbol, pair, rva, contributor) {
        Ok(path) => {
            println!(
                "Offset registrato per {symbol} su {pair} (verified=false): {}",
                path.display()
            );
            Ok(())
        }
        Err(err) => anyhow::bail!("contribuzione `set-offset` interrotta: {err}"),
    }
}

/// `bindings set-signature`: inoltra a [`bindings::contribution::set_signature`]
/// (Req 2.5).
fn run_bindings_set_signature(
    catalog_root: &std::path::Path,
    symbol: &str,
    return_type: String,
    params: Vec<String>,
) -> anyhow::Result<()> {
    let signature = build_signature(return_type, params);
    let symbol = SymbolId::new(symbol);
    match bindings::contribution::set_signature(catalog_root, &symbol, signature) {
        Ok(path) => {
            println!(
                "Firma aggiornata per {symbol} (tutti i Verified_Flag a false): {}",
                path.display()
            );
            Ok(())
        }
        Err(err) => anyhow::bail!("contribuzione `set-signature` interrotta: {err}"),
    }
}

/// `bindings crosscheck`: carica il `Geode_Reference` (fail-closed sul firewall)
/// ed esegue l'`Observational_Cross_Check` (Req 4).
fn run_bindings_crosscheck(
    reference: &std::path::Path,
    symbol: &str,
    rva: &str,
    gd: &str,
    platform: &str,
) -> anyhow::Result<()> {
    let pair = parse_pair(gd, platform)?;
    let candidate = parse_offset(rva)?;

    // Geode_Firewall: un contenuto non puramente numerico è rifiutato in
    // chiusura senza incorporarne alcuna parte (Req 4.2/4.5).
    let table = match bindings::crosscheck::load_geode_reference(reference) {
        Ok(table) => table,
        Err(err) => anyhow::bail!("Geode_Firewall: riferimento rifiutato, nessun confronto eseguito: {err}"),
    };

    let outcome = bindings::crosscheck::cross_check(candidate, &table, pair, symbol);
    println!(
        "Observational_Cross_Check per {symbol} su {pair}: {outcome:?} \
         (forza verified=false: {}, no_reuse: {})",
        outcome.forces_verified_false(),
        outcome.no_reuse()
    );
    Ok(())
}

/// `bindings verify-prologue`: carica il catalogo, trova la voce ed esegue la
/// `Prologue_Verification` sull'offset della coppia (Req 5).
fn run_bindings_verify_prologue(
    catalog_root: &std::path::Path,
    symbol: &str,
    gd: &str,
    platform: &str,
    method: PrologueMethodArg,
    bytes: Option<&str>,
) -> anyhow::Result<()> {
    let pair = parse_pair(gd, platform)?;
    let symbol = SymbolId::new(symbol);

    // Catalogo illeggibile/malformato/vuoto → fail-closed, nessun output (Req 3.7).
    let catalog = bindings::catalog::load_catalog(catalog_root)
        .map_err(|err| anyhow::anyhow!("caricamento del Binding_Catalog fallito: {err}"))?;

    let entry = catalog
        .entries
        .iter()
        .find(|e| e.symbol == symbol)
        .ok_or_else(|| anyhow::anyhow!("nessuna Catalog_Entry per il simbolo {symbol}"))?;

    let prologue_bytes = match bytes {
        Some(hex) => Some(parse_hex_bytes(hex)?),
        None => None,
    };

    match bindings::prologue::evaluate_prologue(
        entry,
        pair,
        method.into(),
        prologue_bytes.as_deref(),
    ) {
        Ok(verification) => {
            println!(
                "Prologue_Verification per {symbol} su {pair}: esito={}, metodo={}, verified={}",
                verification.outcome, verification.method, verification.verified
            );
            Ok(())
        }
        // Fail-closed: il Verified_Flag resta false; l'errore identifica
        // simbolo/coppia/causa (Req 5.3, 5.6).
        Err(err) => anyhow::bail!("Prologue_Verification fallita (verified resta false): {err}"),
    }
}

/// `bindings generate`: `load_catalog` poi [`bindings::generator::generate`].
///
/// Su un catalogo che fallisce il caricamento (illeggibile/malformato/vuoto,
/// firme in conflitto o simboli duplicati) **non viene prodotto alcun output
/// derivato** e l'errore riporta la causa (Req 3.7, 1.1, 11.5).
fn run_bindings_generate(
    catalog_root: &std::path::Path,
    out_root: &std::path::Path,
) -> anyhow::Result<()> {
    // load_catalog è fail-closed: su qualsiasi anomalia ritorna un errore con
    // causa e NON produciamo alcun `.pbind` (Req 3.7, 11.5).
    let catalog = bindings::catalog::load_catalog(catalog_root).map_err(|err| {
        anyhow::anyhow!(
            "caricamento del Binding_Catalog fallito: nessun output derivato prodotto. Causa: {err}"
        )
    })?;

    match bindings::generator::generate(&catalog, out_root) {
        Ok(report) => {
            println!("Generazione completata: {} Binding_Set_File scritti", report.written.len());
            for path in &report.written {
                println!("  + {}", path.display());
            }
            if !report.unresolved.is_empty() {
                println!(
                    "Voci non risolte emesse con verified=false (fail-closed, Req 8.6): {}",
                    report.unresolved.len()
                );
                for (pair, symbol) in &report.unresolved {
                    println!("  ! {symbol} su {pair}");
                }
            }
            Ok(())
        }
        Err(err) => anyhow::bail!("generazione interrotta: {err}"),
    }
}

/// `bindings lint`: legge un `.pbind` e inoltra a [`bindings::linter::lint_result`]
/// (Req 7). Esito di successo distinto dall'errore (Req 7.6); fail-closed se
/// vengono rilevate violazioni.
fn run_bindings_lint(path: &std::path::Path) -> anyhow::Result<()> {
    let content = std::fs::read_to_string(path)
        .map_err(|err| anyhow::anyhow!("lettura del .pbind {} fallita: {err}", path.display()))?;

    let report = bindings::linter::lint_result(path, &content);
    if report.is_distributable() {
        println!("Lint OK: {} — {}", path.display(), report.summary());
        Ok(())
    } else {
        let mut msg = format!("Lint fallito: {} — {}", path.display(), report.summary());
        for finding in report.findings() {
            msg.push_str(&format!(
                "\n  - riga {}: {:?}: {}",
                finding.line, finding.kind, finding.message
            ));
        }
        anyhow::bail!(msg)
    }
}

/// `bindings validate`: carica il catalogo, pivota per coppia e applica
/// [`bindings::validation::validate_set`] (semantica "resolved sse verificato",
/// Req 6). Fail-closed: una qualsiasi incoerenza rifiuta la coppia.
fn run_bindings_validate(catalog_root: &std::path::Path) -> anyhow::Result<()> {
    use bindings::validation::{validate_set, PbindFunction, PbindSet};
    use std::collections::BTreeSet;

    let catalog = bindings::catalog::load_catalog(catalog_root).map_err(|err| {
        anyhow::anyhow!("caricamento del Binding_Catalog fallito: nessuna validazione eseguita. Causa: {err}")
    })?;

    // Insieme deterministico delle coppie presenti nel catalogo.
    let mut pairs: BTreeSet<TargetPair> = BTreeSet::new();
    for entry in &catalog.entries {
        for offset in &entry.offsets {
            pairs.insert(offset.pair);
        }
    }

    if pairs.is_empty() {
        anyhow::bail!("catalogo privo di coppie: nessuna voce da validare");
    }

    for pair in pairs {
        // Pivot per coppia: una PbindFunction per ogni voce con offset su `pair`.
        let mut functions: Vec<PbindFunction> = Vec::new();
        for entry in &catalog.entries {
            if let Some(offset) = entry.offsets.iter().find(|o| o.pair == pair) {
                functions.push(PbindFunction::new(
                    entry.symbol.clone(),
                    offset.effective_rva(),
                    entry.signature.clone(),
                    offset.verified,
                ));
            }
        }
        let set = PbindSet::new(pair, functions);
        match validate_set(&set) {
            Ok(report) => {
                println!(
                    "Validazione coppia {pair}: superata, {}/{} voci risolvibili",
                    report.resolvable_count(),
                    report.resolvable.len()
                );
            }
            // Fail-closed (Req 6.2/6.4): nessuna voce della coppia è distribuita.
            Err(err) => anyhow::bail!("validazione interrotta per la coppia {pair}: {err}"),
        }
    }
    Ok(())
}

/// `bindings provenance`: **legge** il `Provenance_Record` di un offset dallo
/// store costruito dal catalogo, senza ricalcoli (Req 10.4).
fn run_bindings_provenance(
    catalog_root: &std::path::Path,
    symbol: &str,
    gd: &str,
    platform: &str,
) -> anyhow::Result<()> {
    let pair = parse_pair(gd, platform)?;
    let symbol = SymbolId::new(symbol);

    let catalog = bindings::catalog::load_catalog(catalog_root)
        .map_err(|err| anyhow::anyhow!("caricamento del Binding_Catalog fallito: {err}"))?;

    let store = bindings::provenance::ProvenanceStore::from_catalog(&catalog);
    match store.get(&symbol, pair) {
        Some(record) => {
            println!("Provenance_Record per {symbol} su {pair}:");
            println!("  address_source       = {:?}", record.address_source);
            println!("  cross_check          = {:?}", record.cross_check);
            println!("  cross_check_no_reuse = {}", record.cross_check_no_reuse);
            println!("  prologue_method      = {:?}", record.prologue_method);
            println!("  prologue_outcome     = {:?}", record.prologue_outcome);
            Ok(())
        }
        None => anyhow::bail!("nessun Provenance_Record per {symbol} su {pair}"),
    }
}

/// `bindings extract`: **Binding_Extractor** end-to-end (Req 8, 10).
///
/// Cabla il flusso del design `open_source_binary → elf/demangle → rtti/vtable →
/// match → prologue → tier/geode → write`, parametrizzato per coppia
/// `(GD_Version, Target_Platform)` senza cablare staticamente alcuna coppia
/// (Req 8.2). I binari sono **locali e forniti dall'utente**: nessuna richiesta
/// di rete in uscita (Req 10.1, 10.2). Ogni errore è **fail-closed** e propagato
/// all'utente.
///
/// ## Master di nomi/firme: sempre l'ELF Android (Req 2)
///
/// L'`--elf` è l'`Android_Symbol_Source`: nome, firma (this-first) e — per il
/// target `android-arm64` — l'offset provengono dalla sua tabella dei simboli
/// (tier `symbol-table`). Mach-O/PE sono privi dei nomi di GD: forniscono solo
/// RTTI/vtable per la **cross-derivazione** dell'offset per piattaforma.
///
/// ## Crux d'integrazione risolto: l'ordinamento di vtable Android (cross-derivazione)
///
/// Il `Cross_Platform_Match` ordinale
/// ([`match_method_at`](crate::extract::matcher::match_method_at)) richiede i
/// metodi virtuali della classe **ordinati secondo la posizione di vtable
/// Android**. Quella posizione **non** è codificata nella tabella dei simboli
/// ELF, ma lo è nelle vtable Itanium dell'ELF (`_ZTV<class>`): l'ELF Vtable
/// Reconstructor (Phase G2,
/// [`reconstruct_android_vtables`](crate::extract::elf_vtable::reconstruct_android_vtables))
/// le ricostruisce e mappa l'RVA di ogni slot al `DerivedSymbol` con quell'RVA,
/// recuperando l'ordinamento dei metodi virtuali per classe **esclusivamente dal
/// binario first-party**. Con quell'ordine sano, lo slot Mach-O/PE alla stessa
/// posizione ordinale produce l'offset `cross-derived` **verificato** (prologo
/// permettendo): la cross-derivazione su host **non** è più universalmente
/// fail-closed. Resta fail-closed (sentinel, `verified = false`) solo per le
/// classi prive di vtable Android ricostruibile, per i metodi che restano buchi
/// e per gli esiti `NoMatch`/`Ambiguous` — mai un offset indovinato. La conferma
/// a campione degli offset derivati resta affidata al **GD reale** (Phase H).
#[allow(clippy::too_many_arguments)]
fn run_bindings_extract(
    gd: &str,
    platform: &str,
    elf: Option<&std::path::Path>,
    macho: Option<&std::path::Path>,
    pe: Option<&std::path::Path>,
    geode_ref: Option<&std::path::Path>,
    catalog_root: &std::path::Path,
) -> anyhow::Result<()> {
    use std::collections::{BTreeMap, HashSet};

    use crate::extract::binary::open_source_binary;
    use crate::extract::demangle::{demangle, DerivedSymbol};
    use crate::extract::elf_vtable::reconstruct_android_vtables;
    use crate::extract::matcher::{match_method_at, MatchOutcome};
    use crate::extract::prologue::{check_prologue, verified_flag};
    use crate::extract::provenance::{
        assign_tier, emit_offset, geode_concordance, Derivation, ExtractionProvenance,
    };
    use crate::extract::writer::{write_catalog_entries, ExtractedEntry, ExtractedOffset};
    use crate::extract::{self, Architecture, BinaryFormat};

    // Coppia target. Una piattaforma fuori dall'insieme supportato dall'estrattore
    // → nessun output, errore che identifica la coppia (Req 8.3).
    let target_pair = parse_pair(gd, platform)?;
    if !extract::is_supported_platform(target_pair.platform) {
        anyhow::bail!(
            "coppia non supportata dall'estrattore {target_pair}: Target_Platform fuori \
             dall'insieme {{macos-arm64, macos-x64, windows-x64, android-arm64}} (Req 8.3)"
        );
    }
    let target_arch = Architecture::from_platform(target_pair.platform)
        .ok_or_else(|| anyhow::anyhow!("architettura non derivabile per la coppia {target_pair}"))?;

    // L'ELF Android è il master di nomi/firme: richiesto (Req 2).
    let elf_path = elf.ok_or_else(|| {
        anyhow::anyhow!(
            "--elf è richiesto: l'Android_Symbol_Source ELF è il master di nomi e firme \
             (ogni identità di simbolo e firma deriva esclusivamente da lì)"
        )
    })?;
    // L'identità dell'ELF usa la propria coppia (android-arm64), non quella target.
    let android_pair = TargetPair::new(target_pair.gd, TargetPlatform::AndroidArm64);
    let elf_bin = open_source_binary(elf_path, android_pair, BinaryFormat::Elf)
        .map_err(|err| anyhow::anyhow!("apertura dell'ELF Android fallita (fail-closed): {err}"))?;

    // Lista master: demangla ogni Mangled_Symbol; gli indecodificabili/privi di
    // indirizzo sono esclusi e l'elaborazione prosegue (Req 2.5, 2.6).
    let raw_symbols = extract::elf::read_elf_symbols(&elf_bin)
        .map_err(|err| anyhow::anyhow!("lettura dei simboli ELF fallita (fail-closed): {err}"))?;
    let mut symbols: Vec<DerivedSymbol> = Vec::new();
    let mut seen_ids: HashSet<String> = HashSet::new();
    let mut demangle_excluded = 0usize;
    for raw in &raw_symbols {
        match demangle(raw) {
            Ok(sym) => {
                if seen_ids.insert(sym.symbol_id()) {
                    symbols.push(sym);
                }
            }
            Err(err) => {
                demangle_excluded += 1;
                eprintln!("[extract] simbolo escluso al demangling (Req 2.5): {err}");
            }
        }
    }

    // Geode_Reference opzionale, instradata dall'UNICO Geode_Firewall riusato:
    // un contenuto non puramente numerico è rifiutato in chiusura (Req 1.4, 1.5).
    let geode_table = match geode_ref {
        Some(path) => Some(bindings::crosscheck::load_geode_reference(path).map_err(|err| {
            anyhow::anyhow!("Geode_Firewall: riferimento rifiutato, nessun cross-check eseguito: {err}")
        })?),
        None => None,
    };

    // Helper: offset sentinel (non risolvibile) per la coppia target (Req 11.3).
    let sentinel = |pair: TargetPair| ExtractedOffset {
        pair,
        rva: None,
        verified: false,
        provenance: None,
    };

    let mut entries: Vec<ExtractedEntry> = Vec::new();
    let mut auditability_errors = 0usize;
    let mut notes: Vec<String> = Vec::new();

    if target_pair.platform == TargetPlatform::AndroidArm64 {
        // Tier `symbol-table`: offset first-party direttamente dall'ELF (Req 2.4).
        for sym in &symbols {
            let symbol_id = SymbolId::new(sym.symbol_id());
            let derivation = Derivation::SymbolTable { rva: sym.rva };
            let outcome = check_prologue(&elf_bin, sym.rva, target_arch);
            let requested_verified = verified_flag(sym.rva, &outcome);
            let offset = match assign_tier(&derivation, &outcome) {
                Some(tier) => {
                    let concordance = geode_table
                        .as_ref()
                        .and_then(|t| geode_concordance(sym.rva, t, &symbol_id));
                    let prov = ExtractionProvenance::new(
                        tier,
                        &derivation,
                        vec![elf_bin.identity.clone()],
                        Some(outcome),
                        concordance,
                    );
                    let (emitted, audit_err) =
                        emit_offset(symbol_id.clone(), target_pair, sym.rva, requested_verified, prov);
                    if audit_err.is_some() {
                        auditability_errors += 1;
                    }
                    ExtractedOffset {
                        pair: target_pair,
                        rva: Some(emitted.offset),
                        verified: emitted.verified,
                        provenance: Some(emitted.provenance),
                    }
                }
                None => sentinel(target_pair),
            };
            entries.push(ExtractedEntry {
                symbol: symbol_id,
                signature: sym.signature.clone(),
                offsets: vec![offset],
            });
        }
    } else {
        // Tier `cross-derived`: nome/firma dall'ELF, offset dallo slot di vtable
        // del Source_Binary di destinazione (Mach-O/PE) abbinato per classe+ordine.
        let target_bin = match target_pair.platform {
            TargetPlatform::WindowsX64 => {
                let path = pe.ok_or_else(|| {
                    anyhow::anyhow!(
                        "--pe è richiesto per la coppia {target_pair} (Source_Binary PE di destinazione)"
                    )
                })?;
                open_source_binary(path, target_pair, BinaryFormat::Pe)
                    .map_err(|err| anyhow::anyhow!("apertura del PE di destinazione fallita: {err}"))?
            }
            _ => {
                let path = macho.ok_or_else(|| {
                    anyhow::anyhow!(
                        "--macho è richiesto per la coppia {target_pair} (Source_Binary Mach-O di destinazione)"
                    )
                })?;
                open_source_binary(path, target_pair, BinaryFormat::MachO).map_err(|err| {
                    anyhow::anyhow!("apertura del Mach-O di destinazione fallita: {err}")
                })?
            }
        };

        let rtti = extract::rtti::parse_rtti(&target_bin)
            .map_err(|err| anyhow::anyhow!("parsing RTTI del binario di destinazione fallito: {err}"))?;
        let vtables = extract::vtable::reconstruct_vtables(&target_bin, &rtti).map_err(|err| {
            anyhow::anyhow!("ricostruzione vtable del binario di destinazione fallita: {err}")
        })?;

        // Phase G2: recupera l'ordinamento di vtable Android (l'ordine dei
        // metodi virtuali per classe) ricostruendo le vtable Itanium dell'ELF e
        // mappando l'RVA di ogni slot al DerivedSymbol con quell'RVA. È
        // l'ordinamento sano che abilita la cross-derivazione ordinale reale.
        let android_order = reconstruct_android_vtables(&elf_bin, &symbols).map_err(|err| {
            anyhow::anyhow!("ricostruzione delle vtable Android fallita (fail-closed): {err}")
        })?;

        // SymbolId → ordinale di vtable Android (posizione assoluta, buchi
        // inclusi nel conteggio). `None` ⇒ ambiguo o non un metodo virtuale
        // recuperabile: il chiamante resta fail-closed (mai indovinare).
        let ordinal_by_symbol = android_order.symbol_ordinals();
        let recovered_classes = android_order.class_count();

        // Raggruppa i DerivedSymbol Android per classe (ordine deterministico).
        // Ogni simbolo riceve comunque una Catalog_Entry: cross-derived dove
        // l'ordinale Android è recuperato e lo slot di destinazione combacia,
        // altrimenti sentinel fail-closed.
        let mut by_class: BTreeMap<String, Vec<DerivedSymbol>> = BTreeMap::new();
        for sym in &symbols {
            by_class.entry(sym.class.clone()).or_default().push(sym.clone());
        }

        let mut cross_derived_methods = 0usize;
        let mut fail_closed_methods = 0usize;
        for (_class, mut methods) in by_class {
            methods.sort_by(|a, b| a.symbol_id().cmp(&b.symbol_id()));
            for sym in &methods {
                let symbol_id = SymbolId::new(sym.symbol_id());

                // L'ordinale assoluto proviene dall'ordine di vtable Android: è
                // ciò che lega il nome Android allo slot Mach-O/PE della stessa
                // posizione. `match_method_at` applica l'iff del Req 4.1
                // (classe coincidente + slot unico a quell'ordinale).
                let outcome = match ordinal_by_symbol.get(&sym.symbol_id()).copied().flatten() {
                    Some(ordinal) => match_method_at(sym, ordinal, &vtables),
                    // Nessun ordine di vtable Android recuperabile per questo
                    // metodo (buco, ambiguo, o non virtuale): fail-closed.
                    None => MatchOutcome::NoMatch,
                };

                let offset = match &outcome {
                    // Percorso reale (host): match unico ⇒ offset cross-derived,
                    // verificato se il prologo è plausibile.
                    MatchOutcome::Matched { slot } => {
                        let rva = slot.rva;
                        let derivation = Derivation::CrossDerived { rva };
                        let prologue = check_prologue(&target_bin, rva, target_arch);
                        let requested_verified = verified_flag(rva, &prologue);
                        match assign_tier(&derivation, &prologue) {
                            Some(tier) => {
                                cross_derived_methods += 1;
                                let concordance = geode_table
                                    .as_ref()
                                    .and_then(|t| geode_concordance(rva, t, &symbol_id));
                                let prov = ExtractionProvenance::new(
                                    tier,
                                    &derivation,
                                    vec![elf_bin.identity.clone(), target_bin.identity.clone()],
                                    Some(prologue),
                                    concordance,
                                );
                                let (emitted, audit_err) = emit_offset(
                                    symbol_id.clone(),
                                    target_pair,
                                    rva,
                                    requested_verified,
                                    prov,
                                );
                                if audit_err.is_some() {
                                    auditability_errors += 1;
                                }
                                ExtractedOffset {
                                    pair: target_pair,
                                    rva: Some(emitted.offset),
                                    verified: emitted.verified,
                                    provenance: Some(emitted.provenance),
                                }
                            }
                            None => {
                                fail_closed_methods += 1;
                                sentinel(target_pair)
                            }
                        }
                    }
                    // Fail-closed: NoMatch o Ambiguous ⇒ sentinel, verified=false
                    // (Req 4.4, 4.6, 11.3): mai indovinare un offset.
                    MatchOutcome::NoMatch | MatchOutcome::Ambiguous { .. } => {
                        fail_closed_methods += 1;
                        sentinel(target_pair)
                    }
                };
                entries.push(ExtractedEntry {
                    symbol: symbol_id,
                    signature: sym.signature.clone(),
                    offsets: vec![offset],
                });
            }
        }

        notes.push(format!(
            "Phase G2: ordinamento di vtable Android recuperato per {recovered_classes} classe/i \
             dalle vtable Itanium dell'ELF (`_ZTV…`). La cross-derivazione su host non è più \
             universalmente fail-closed: {cross_derived_methods} offset cross-derived emessi \
             (verified secondo il Prologue_Sanity_Check), {fail_closed_methods} metodo/i restano \
             fail-closed (sentinel, verified=false) per classe senza vtable Android ricostruibile, \
             buco, o match NoMatch/Ambiguous. La conferma a campione degli offset derivati resta \
             affidata al GD reale (Phase H)."
        ));
    }

    // Scrittura deterministica/atomica/fail-closed nel formato del catalogo
    // esistente; conflitti per la stessa coppia lasciano invariata la voce
    // esistente (incl. il seed) e fanno fallire l'estrazione (Req 11.4).
    let report = write_catalog_entries(&entries, catalog_root)
        .map_err(|err| anyhow::anyhow!("scrittura del catalogo interrotta (fail-closed): {err}"))?;

    println!(
        "Estrazione completata per la coppia {target_pair}: {} offset risolti emessi",
        report.total_offsets
    );
    for (tier, count) in &report.by_tier {
        println!("  {tier} = {count}");
    }
    if demangle_excluded > 0 {
        println!("Simboli esclusi al demangling (Req 2.5): {demangle_excluded}");
    }
    if auditability_errors > 0 {
        println!(
            "Offset declassati a verified=false per provenienza incompleta (Req 6.5): {auditability_errors}"
        );
    }
    for note in &notes {
        println!("Nota: {note}");
    }
    Ok(())
}

// ===========================================================================
// `pulse surface` — sottocomandi della GD API Surface (task 11.1).
// ===========================================================================

/// Default del percorso del `Surface_Manifest` (`mod-index/surface/surface.toml`).
const DEFAULT_SURFACE_MANIFEST: &str = "mod-index/surface/surface.toml";
/// Default della radice del catalogo (`mod-index/catalog`).
const DEFAULT_CATALOG_ROOT: &str = "mod-index/catalog";
/// Default della radice di output (`mod-index`).
const DEFAULT_SURFACE_OUT_ROOT: &str = "mod-index";

/// Sottocomandi di `pulse surface`, ognuno **inoltra a un componente** nuovo
/// della GD API Surface (design Phase F, Req 1/3/10).
///
/// Il flusso end-to-end è `load_manifest + load_catalog → compile_surface →
/// (write_ir) → generate_cpp`:
///
/// - `compile` carica il manifest (`load_manifest`) e il catalogo
///   (`load_catalog`, **riuso** della pipeline), li fonde con `compile_surface`
///   e scrive la `Surface_IR` in `out-root/surface/surface.ir.json`;
/// - `generate` esegue il flusso completo **compile → generate**: dopo la
///   compilazione scrive la `Surface_IR` ed emette i tre header SDK C++
///   (`generate_cpp`) sotto `out-root/sdk/include/pulse/gd/` (Req 10.1);
/// - `lint` fa emergere le anomalie del manifest/superficie (`Surface_Linter`);
/// - `validate` applica il gate di completezza della provenienza
///   (`Surface_Validator`).
///
/// **Gestione delle diagnostiche per-elemento (Req 1.5).** `compile_surface`
/// **esclude e prosegue** sui simboli privi di `Catalog_Entry`, raccogliendoli
/// in `CompiledSurface::diagnostics`. Coerentemente con questa disciplina di
/// "exclude-and-continue", `compile`/`generate` **riportano** le diagnostiche e
/// **proseguono** generando la superficie sopravvissuta. Il flag `--strict`
/// rende l'operazione **fail-closed** su qualsiasi diagnostica: nessun artefatto
/// è prodotto e la causa è riportata. Gli errori **globali** di
/// caricamento/compilazione (manifest/catalogo illeggibili o malformati,
/// `cpp_token` non derivabile) sono **sempre** fail-closed.
#[derive(Debug, Subcommand)]
pub enum SurfaceCommand {
    /// Compila la `Surface_IR` (`load_manifest` + `load_catalog` →
    /// `compile_surface` → `surface.ir.json`).
    Compile {
        /// Percorso del `Surface_Manifest` (`mod-index/surface/surface.toml`).
        #[arg(long = "manifest", default_value = DEFAULT_SURFACE_MANIFEST)]
        manifest: PathBuf,
        /// Radice del catalogo (`mod-index/catalog`).
        #[arg(long = "catalog-root", default_value = DEFAULT_CATALOG_ROOT)]
        catalog_root: PathBuf,
        /// Radice di output (`mod-index`): la IR finisce sotto
        /// `out-root/surface/surface.ir.json`.
        #[arg(long = "out-root", default_value = DEFAULT_SURFACE_OUT_ROOT)]
        out_root: PathBuf,
        /// Fail-closed su qualsiasi diagnostica per-elemento: nessun artefatto.
        #[arg(long = "strict", default_value_t = false)]
        strict: bool,
    },
    /// Genera l'API SDK C++ end-to-end (**compile → generate**): scrive la
    /// `Surface_IR` ed emette `sdk/include/pulse/gd/{types,bindings,hooks}.gen.hpp`.
    Generate {
        #[arg(long = "manifest", default_value = DEFAULT_SURFACE_MANIFEST)]
        manifest: PathBuf,
        #[arg(long = "catalog-root", default_value = DEFAULT_CATALOG_ROOT)]
        catalog_root: PathBuf,
        /// Radice di output (`mod-index`): gli header finiscono sotto
        /// `out-root/sdk/include/pulse/gd/`, la IR sotto `out-root/surface/`.
        #[arg(long = "out-root", default_value = DEFAULT_SURFACE_OUT_ROOT)]
        out_root: PathBuf,
        /// Fail-closed su qualsiasi diagnostica per-elemento: nessun header.
        #[arg(long = "strict", default_value_t = false)]
        strict: bool,
    },
    /// Fa emergere le anomalie della superficie (`Surface_Linter`): duplicati,
    /// simboli mancanti, schema non supportato.
    Lint {
        #[arg(long = "manifest", default_value = DEFAULT_SURFACE_MANIFEST)]
        manifest: PathBuf,
        #[arg(long = "catalog-root", default_value = DEFAULT_CATALOG_ROOT)]
        catalog_root: PathBuf,
    },
    /// Applica il gate di completezza della provenienza per coppia
    /// (`Surface_Validator`, Req 8).
    Validate {
        #[arg(long = "manifest", default_value = DEFAULT_SURFACE_MANIFEST)]
        manifest: PathBuf,
        #[arg(long = "catalog-root", default_value = DEFAULT_CATALOG_ROOT)]
        catalog_root: PathBuf,
    },
}

impl SurfaceCommand {
    /// Esegue il sottocomando `surface` selezionato, inoltrando al componente.
    pub fn run(self) -> anyhow::Result<()> {
        match self {
            SurfaceCommand::Compile {
                manifest,
                catalog_root,
                out_root,
                strict,
            } => run_surface_compile(&manifest, &catalog_root, &out_root, strict),
            SurfaceCommand::Generate {
                manifest,
                catalog_root,
                out_root,
                strict,
            } => run_surface_generate(&manifest, &catalog_root, &out_root, strict),
            SurfaceCommand::Lint {
                manifest,
                catalog_root,
            } => run_surface_lint(&manifest, &catalog_root),
            SurfaceCommand::Validate {
                manifest,
                catalog_root,
            } => run_surface_validate(&manifest, &catalog_root),
        }
    }
}

/// Carica il `Surface_Manifest` e il `Binding_Catalog` in modo **fail-closed**:
/// su qualsiasi anomalia di caricamento ritorna un errore con causa e nessun
/// artefatto è prodotto (Req 1.1, 11.5 ereditato).
fn load_surface_inputs(
    manifest_path: &std::path::Path,
    catalog_root: &std::path::Path,
) -> anyhow::Result<(surface::manifest::SurfaceManifest, bindings::catalog::BindingCatalog)> {
    let manifest = surface::manifest::load_manifest(manifest_path).map_err(|err| {
        anyhow::anyhow!(
            "caricamento del Surface_Manifest fallito: nessun artefatto prodotto. Causa: {err}"
        )
    })?;
    let catalog = bindings::catalog::load_catalog(catalog_root).map_err(|err| {
        anyhow::anyhow!(
            "caricamento del Binding_Catalog fallito: nessun artefatto prodotto. Causa: {err}"
        )
    })?;
    Ok((manifest, catalog))
}

/// Stampa le diagnostiche per-elemento raccolte da `compile_surface` (Req 1.5).
fn report_surface_diagnostics(diagnostics: &[surface::SurfaceError]) {
    println!(
        "Diagnostiche di compilazione ({} elemento/i escluso/i, exclude-and-continue, Req 1.5):",
        diagnostics.len()
    );
    for diagnostic in diagnostics {
        println!("  ! {diagnostic}");
    }
}

/// Aggrega le diagnostiche per-elemento in un messaggio fail-closed `--strict`.
fn strict_diagnostics_bail(diagnostics: &[surface::SurfaceError]) -> anyhow::Error {
    let mut msg = String::from(
        "operazione interrotta (--strict): la superficie presenta diagnostiche, \
         nessun artefatto prodotto.\nDiagnostiche:",
    );
    for diagnostic in diagnostics {
        msg.push_str(&format!("\n  - {diagnostic}"));
    }
    anyhow::anyhow!(msg)
}

/// `surface compile`: `load_manifest` + `load_catalog` → `compile_surface` →
/// scrive la `Surface_IR` in `out-root/surface/surface.ir.json`.
///
/// Fail-closed sul caricamento (manifest/catalogo) e sugli errori globali di
/// compilazione. Le diagnostiche per-elemento (simboli mancanti, Req 1.5) sono
/// riportate e la compilazione **prosegue** con la superficie sopravvissuta;
/// con `--strict` qualsiasi diagnostica interrompe senza produrre la IR.
fn run_surface_compile(
    manifest_path: &std::path::Path,
    catalog_root: &std::path::Path,
    out_root: &std::path::Path,
    strict: bool,
) -> anyhow::Result<()> {
    let (manifest, catalog) = load_surface_inputs(manifest_path, catalog_root)?;

    let compiled = surface::compiler::compile_surface(&manifest, &catalog).map_err(|err| {
        anyhow::anyhow!(
            "compilazione della superficie interrotta: nessun artefatto prodotto. Causa: {err}"
        )
    })?;

    if !compiled.diagnostics.is_empty() {
        if strict {
            return Err(strict_diagnostics_bail(&compiled.diagnostics));
        }
        report_surface_diagnostics(&compiled.diagnostics);
    }

    let path = surface::ir::write_ir(&compiled.ir, out_root)
        .map_err(|err| anyhow::anyhow!("scrittura della Surface_IR fallita: {err}"))?;
    println!(
        "Compilazione completata: {} API_Element esposti, Surface_IR scritta in {}",
        compiled.ir.elements.len(),
        path.display()
    );
    Ok(())
}

/// `surface generate`: flusso end-to-end **compile → generate**.
///
/// `load_manifest` + `load_catalog` → `compile_surface` → scrive la `Surface_IR`
/// → `generate_cpp` emette i tre header SDK C++ sotto
/// `out-root/sdk/include/pulse/gd/` (Req 10.1) dalla `Surface_IR`
/// language-agnostic (Req 10.2). Fail-closed su caricamento/compilazione e
/// scrittura: su anomalia nessun header è prodotto e la causa è propagata. Le
/// diagnostiche per-elemento sono riportate (exclude-and-continue) salvo
/// `--strict`, che interrompe senza generare alcun header.
fn run_surface_generate(
    manifest_path: &std::path::Path,
    catalog_root: &std::path::Path,
    out_root: &std::path::Path,
    strict: bool,
) -> anyhow::Result<()> {
    let (manifest, catalog) = load_surface_inputs(manifest_path, catalog_root)?;

    // Fase compile.
    let compiled = surface::compiler::compile_surface(&manifest, &catalog).map_err(|err| {
        anyhow::anyhow!(
            "compilazione della superficie interrotta: nessun header prodotto. Causa: {err}"
        )
    })?;

    if !compiled.diagnostics.is_empty() {
        if strict {
            return Err(strict_diagnostics_bail(&compiled.diagnostics));
        }
        report_surface_diagnostics(&compiled.diagnostics);
    }

    // Persistiamo anche la Surface_IR language-agnostic accanto agli header,
    // così l'artefatto intermedio del flusso resta ispezionabile (Req 10.2).
    let ir_path = surface::ir::write_ir(&compiled.ir, out_root)
        .map_err(|err| anyhow::anyhow!("scrittura della Surface_IR fallita: {err}"))?;
    println!("Surface_IR scritta in {}", ir_path.display());

    // Fase generate: emissione header-only deterministica dell'API SDK C++.
    let report = surface::cppgen::generate_cpp(&compiled.ir, out_root)
        .map_err(|err| anyhow::anyhow!("generazione dell'API C++ interrotta: {err}"))?;

    println!(
        "Generazione completata: {} header SDK C++ scritti",
        report.written.len()
    );
    for path in &report.written {
        println!("  + {}", path.display());
    }
    Ok(())
}

/// `surface lint`: legge il contenuto del manifest, carica il catalogo (riuso) e
/// inoltra a [`surface::linter::lint_surface_source`] (Req 1.5, 2.4). Esito di
/// successo distinto dall'errore; fail-closed se vengono rilevate anomalie.
fn run_surface_lint(
    manifest_path: &std::path::Path,
    catalog_root: &std::path::Path,
) -> anyhow::Result<()> {
    let content = std::fs::read_to_string(manifest_path).map_err(|err| {
        anyhow::anyhow!(
            "lettura del Surface_Manifest {} fallita: {err}",
            manifest_path.display()
        )
    })?;

    let catalog = bindings::catalog::load_catalog(catalog_root)
        .map_err(|err| anyhow::anyhow!("caricamento del Binding_Catalog fallito: {err}"))?;

    let report = surface::linter::lint_surface_source(&content, manifest_path, &catalog);
    if report.is_clean() {
        println!(
            "Lint OK: {} — nessuna anomalia nella superficie",
            manifest_path.display()
        );
        Ok(())
    } else {
        let mut msg = format!(
            "Lint fallito: {} — {} anomalia/e rilevata/e, nessun artefatto da produrre",
            manifest_path.display(),
            report.len()
        );
        for finding in &report.findings {
            msg.push_str(&format!("\n  - {finding}"));
        }
        anyhow::bail!(msg)
    }
}

/// `surface validate`: `load_manifest` + `load_catalog` → `compile_surface` →
/// [`surface::validation::validate_surface`] (gate di completezza della
/// provenienza, Req 8). Stampa le classificazioni risolvibili/non risolvibili e
/// fallisce in chiusura se vi sono errori di auditabilità (simbolo + coppia).
fn run_surface_validate(
    manifest_path: &std::path::Path,
    catalog_root: &std::path::Path,
) -> anyhow::Result<()> {
    let (manifest, catalog) = load_surface_inputs(manifest_path, catalog_root)?;

    let compiled = surface::compiler::compile_surface(&manifest, &catalog).map_err(|err| {
        anyhow::anyhow!("compilazione della superficie interrotta: {err}")
    })?;

    let outcome = surface::validation::validate_surface(&compiled.ir, &catalog);

    // Classificazioni per coppia: risolvibile/non risolvibile (Req 7/8).
    println!(
        "Validazione superficie: {} classificazione/i per coppia",
        outcome.classifications.len()
    );
    for c in &outcome.classifications {
        let status = if c.resolvable {
            "risolvibile"
        } else {
            "non risolvibile"
        };
        println!("  - {} su {}: {status}", c.symbol, c.pair);
    }

    // Gate di auditabilità (Req 8.2): fail-closed se ci sono provenienze incomplete.
    if outcome.auditability_errors.is_empty() {
        println!("Validazione superata: ogni elemento risolvibile ha provenienza completa");
        Ok(())
    } else {
        let mut msg = format!(
            "Validazione fallita: {} errore/i di auditabilità (provenienza incompleta), \
             elementi declassati a non risolvibili",
            outcome.auditability_errors.len()
        );
        for err in &outcome.auditability_errors {
            msg.push_str(&format!("\n  - {err}"));
        }
        anyhow::bail!(msg)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use clap::CommandFactory;

    #[test]
    fn cli_definition_is_valid() {
        // Verifica che la definizione clap sia coerente (debug_assert interno).
        Cli::command().debug_assert();
    }

    #[test]
    fn parses_new_subcommand() {
        let cli = Cli::try_parse_from(["pulse", "new", "mymod", "--id", "com.example.mymod"])
            .expect("new dovrebbe parsare");
        match cli.command.unwrap() {
            Command::New { path, id } => {
                assert_eq!(path.to_str().unwrap(), "mymod");
                assert_eq!(id.as_deref(), Some("com.example.mymod"));
            }
            other => panic!("comando inatteso: {other:?}"),
        }
    }

    #[test]
    fn parses_build_and_publish_defaults() {
        let build = Cli::try_parse_from(["pulse", "build"]).unwrap();
        assert!(matches!(build.command, Some(Command::Build { .. })));

        let publish = Cli::try_parse_from(["pulse", "publish"]).unwrap();
        assert!(matches!(publish.command, Some(Command::Publish { .. })));
    }

    #[test]
    fn parses_install_subcommand() {
        let cli = Cli::try_parse_from([
            "pulse",
            "install",
            "--gd",
            "/x.app",
            "--artifact",
            "/y.dylib",
        ])
        .expect("install dovrebbe parsare");
        match cli.command.unwrap() {
            Command::Install {
                gd,
                artifact,
                native,
            } => {
                assert_eq!(gd.to_str().unwrap(), "/x.app");
                assert_eq!(artifact.to_str().unwrap(), "/y.dylib");
                assert!(!native, "native deve essere false di default");
            }
            other => panic!("comando inatteso: {other:?}"),
        }
    }

    #[test]
    fn parses_install_subcommand_native() {
        let cli = Cli::try_parse_from([
            "pulse",
            "install",
            "--gd",
            "/x.app",
            "--artifact",
            "/y.dylib",
            "--native",
        ])
        .expect("install --native dovrebbe parsare");
        match cli.command.unwrap() {
            Command::Install {
                gd,
                artifact,
                native,
            } => {
                assert_eq!(gd.to_str().unwrap(), "/x.app");
                assert_eq!(artifact.to_str().unwrap(), "/y.dylib");
                assert!(native, "native deve essere true con --native");
            }
            other => panic!("comando inatteso: {other:?}"),
        }
    }

    #[test]
    fn parses_uninstall_subcommand() {
        let cli = Cli::try_parse_from(["pulse", "uninstall", "--gd", "/x.app"])
            .expect("uninstall dovrebbe parsare");
        match cli.command.unwrap() {
            Command::Uninstall { gd } => {
                assert_eq!(gd.to_str().unwrap(), "/x.app");
            }
            other => panic!("comando inatteso: {other:?}"),
        }
    }

    // -----------------------------------------------------------------------
    // `pulse bindings` — parsing dei sottocomandi (task 17.1).
    // -----------------------------------------------------------------------

    #[test]
    fn parses_bindings_generate_subcommand() {
        let cli = Cli::try_parse_from([
            "pulse",
            "bindings",
            "generate",
            "--catalog-root",
            "/mod-index/catalog",
            "--out-root",
            "/mod-index",
        ])
        .expect("bindings generate dovrebbe parsare");
        match cli.command.unwrap() {
            Command::Bindings {
                action:
                    BindingsCommand::Generate {
                        catalog_root,
                        out_root,
                    },
            } => {
                assert_eq!(catalog_root.to_str().unwrap(), "/mod-index/catalog");
                assert_eq!(out_root.to_str().unwrap(), "/mod-index");
            }
            other => panic!("comando inatteso: {other:?}"),
        }
    }

    #[test]
    fn parses_bindings_verify_prologue_both_methods() {
        for (raw, expected) in [
            ("otool-manual", PrologueMethodArg::OtoolManual),
            ("automatic-bytes", PrologueMethodArg::AutomaticBytes),
        ] {
            let cli = Cli::try_parse_from([
                "pulse",
                "bindings",
                "verify-prologue",
                "--catalog-root",
                "/c",
                "--symbol",
                "MenuLayer::init",
                "--gd",
                "2.2081",
                "--platform",
                "macos-arm64",
                "--method",
                raw,
            ])
            .unwrap_or_else(|e| panic!("verify-prologue --method {raw} dovrebbe parsare: {e}"));
            match cli.command.unwrap() {
                Command::Bindings {
                    action: BindingsCommand::VerifyPrologue { method, .. },
                } => assert_eq!(method, expected),
                other => panic!("comando inatteso: {other:?}"),
            }
        }
    }

    #[test]
    fn parses_bindings_add_with_pairs_and_params() {
        let cli = Cli::try_parse_from([
            "pulse",
            "bindings",
            "add",
            "--catalog-root",
            "/c",
            "--symbol",
            "PlayLayer::update",
            "--return",
            "void",
            "--params",
            "PlayLayer*,float",
            "--gd",
            "2.2081",
            "--platform",
            "macos-arm64",
        ])
        .expect("bindings add dovrebbe parsare");
        match cli.command.unwrap() {
            Command::Bindings {
                action:
                    BindingsCommand::Add {
                        symbol,
                        return_type,
                        params,
                        gd,
                        platform,
                        ..
                    },
            } => {
                assert_eq!(symbol, "PlayLayer::update");
                assert_eq!(return_type, "void");
                assert_eq!(params, vec!["PlayLayer*".to_owned(), "float".to_owned()]);
                assert_eq!(gd.as_deref(), Some("2.2081"));
                assert_eq!(platform, vec!["macos-arm64".to_owned()]);
            }
            other => panic!("comando inatteso: {other:?}"),
        }
    }

    #[test]
    fn parses_bindings_lint_and_provenance() {
        let lint = Cli::try_parse_from(["pulse", "bindings", "lint", "/p.pbind"]).unwrap();
        assert!(matches!(
            lint.command,
            Some(Command::Bindings {
                action: BindingsCommand::Lint { .. }
            })
        ));

        let prov = Cli::try_parse_from([
            "pulse",
            "bindings",
            "provenance",
            "--catalog-root",
            "/c",
            "--symbol",
            "MenuLayer::init",
            "--gd",
            "2.2081",
            "--platform",
            "macos-arm64",
        ])
        .unwrap();
        assert!(matches!(
            prov.command,
            Some(Command::Bindings {
                action: BindingsCommand::Provenance { .. }
            })
        ));
    }

    #[test]
    fn parse_helpers_handle_gd_platform_rva_and_hex() {
        assert_eq!(parse_gd_version("2.2081").unwrap(), GdVersion::new(2, 2081));
        assert!(parse_gd_version("garbage").is_err());

        assert_eq!(
            parse_platform("macos-arm64").unwrap(),
            TargetPlatform::MacosArm64
        );
        assert!(parse_platform("linux-x64").is_err());

        assert_eq!(parse_offset("0x316688").unwrap(), 0x316688);
        assert_eq!(parse_offset("42").unwrap(), 42);
        assert!(parse_offset("0xZZ").is_err());

        assert_eq!(parse_hex_bytes("626f6f6c").unwrap(), b"bool".to_vec());
        assert!(parse_hex_bytes("abc").is_err());
    }

    // -----------------------------------------------------------------------
    // `pulse surface` — parsing dei sottocomandi (task 11.1).
    // -----------------------------------------------------------------------

    #[test]
    fn parses_surface_generate_with_defaults() {
        let cli = Cli::try_parse_from(["pulse", "surface", "generate"])
            .expect("surface generate dovrebbe parsare con i default");
        match cli.command.unwrap() {
            Command::Surface {
                action:
                    SurfaceCommand::Generate {
                        manifest,
                        catalog_root,
                        out_root,
                        strict,
                    },
            } => {
                assert_eq!(
                    manifest.to_str().unwrap(),
                    "mod-index/surface/surface.toml"
                );
                assert_eq!(catalog_root.to_str().unwrap(), "mod-index/catalog");
                assert_eq!(out_root.to_str().unwrap(), "mod-index");
                assert!(!strict, "strict deve essere false di default");
            }
            other => panic!("comando inatteso: {other:?}"),
        }
    }

    #[test]
    fn parses_surface_generate_with_explicit_args_and_strict() {
        let cli = Cli::try_parse_from([
            "pulse",
            "surface",
            "generate",
            "--manifest",
            "/m/surface.toml",
            "--catalog-root",
            "/m/catalog",
            "--out-root",
            "/out",
            "--strict",
        ])
        .expect("surface generate dovrebbe parsare con argomenti espliciti");
        match cli.command.unwrap() {
            Command::Surface {
                action:
                    SurfaceCommand::Generate {
                        manifest,
                        catalog_root,
                        out_root,
                        strict,
                    },
            } => {
                assert_eq!(manifest.to_str().unwrap(), "/m/surface.toml");
                assert_eq!(catalog_root.to_str().unwrap(), "/m/catalog");
                assert_eq!(out_root.to_str().unwrap(), "/out");
                assert!(strict, "strict deve essere true con --strict");
            }
            other => panic!("comando inatteso: {other:?}"),
        }
    }

    #[test]
    fn parses_surface_compile_with_defaults() {
        let cli = Cli::try_parse_from(["pulse", "surface", "compile"])
            .expect("surface compile dovrebbe parsare con i default");
        match cli.command.unwrap() {
            Command::Surface {
                action:
                    SurfaceCommand::Compile {
                        manifest,
                        catalog_root,
                        out_root,
                        strict,
                    },
            } => {
                assert_eq!(
                    manifest.to_str().unwrap(),
                    "mod-index/surface/surface.toml"
                );
                assert_eq!(catalog_root.to_str().unwrap(), "mod-index/catalog");
                assert_eq!(out_root.to_str().unwrap(), "mod-index");
                assert!(!strict);
            }
            other => panic!("comando inatteso: {other:?}"),
        }
    }

    #[test]
    fn parses_surface_lint_and_validate() {
        let lint = Cli::try_parse_from(["pulse", "surface", "lint"])
            .expect("surface lint dovrebbe parsare con i default");
        match lint.command.unwrap() {
            Command::Surface {
                action:
                    SurfaceCommand::Lint {
                        manifest,
                        catalog_root,
                    },
            } => {
                assert_eq!(
                    manifest.to_str().unwrap(),
                    "mod-index/surface/surface.toml"
                );
                assert_eq!(catalog_root.to_str().unwrap(), "mod-index/catalog");
            }
            other => panic!("comando inatteso: {other:?}"),
        }

        let validate = Cli::try_parse_from([
            "pulse",
            "surface",
            "validate",
            "--manifest",
            "/m/surface.toml",
            "--catalog-root",
            "/m/catalog",
        ])
        .expect("surface validate dovrebbe parsare");
        assert!(matches!(
            validate.command,
            Some(Command::Surface {
                action: SurfaceCommand::Validate { .. }
            })
        ));
    }
}
