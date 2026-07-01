//! `pulse logs` — avvio di Geometry Dash e streaming live del Log_Stream (Req 2).
//!
//! Mac-first: il comando avvia l'eseguibile Mach-O della `GD_Installation`
//! (`GdInstallation::executable()`, dentro il bundle `.app`) come **processo
//! figlio** con `std::process::Command`, reindirizzando `stdout`/`stderr` su
//! pipe, e ne **inoltra ogni riga in ordine** sul terminale (Req 2.1, 2.2).
//!
//! Il modulo isola la logica **pura e host-testabile** dagli effetti collaterali
//! (avvio del processo, I/O sul terminale, gestione di Ctrl-C):
//!
//!   - [`multiplex`] è un **multiplexer puro**: fonde le due sorgenti (stdout,
//!     stderr) — ciascuna già etichettata con un numero di sequenza globale di
//!     arrivo — in un unico canale ordinato `(seq, line)` **senza scartare
//!     righe** e **preservando l'ordine relativo** di ciascuna sorgente
//!     (Req 2.2). A runtime lo stesso invariante è garantito da un canale
//!     `mpsc` alimentato da due thread lettori, ciascuno FIFO per la propria
//!     sorgente.
//!   - [`classify_exit`] è una **funzione pura** che mappa lo stato di
//!     terminazione del processo: codice `0` ⇒ [`LogsExit::Normal`], qualunque
//!     codice ≠ 0 o terminazione da segnale ⇒ [`LogsExit::Crashed`] (Req 2.3,
//!     2.4).
//!
//! [`stream_logs`] orchestra il tutto: risolve la `GD_Installation` (fornita o
//! scoperta localmente; assente/non avviabile ⇒ **errore con causa**, nessun
//! `Log_Stream`, **nessun binario ridistribuito** — Req 2.5), avvia il figlio,
//! **svuota le righe residue** all'uscita del processo prima di terminare
//! (Req 2.3, 2.4) e, su **Ctrl-C** (riuso di `crossterm`, nessuna nuova
//! dipendenza), cessa la stampa, **termina il processo figlio** e poi esce
//! (Req 2.6).

use std::io::{self, BufRead, BufReader, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc::{self, RecvTimeoutError, Sender};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::Duration;

use anyhow::{anyhow, Context};
use crossterm::event::{self, Event, KeyCode, KeyEvent, KeyEventKind, KeyModifiers};
use crossterm::terminal::{disable_raw_mode, enable_raw_mode};

use crate::installer::{self, GdInstallation};

/// Esito dell'esecuzione di `pulse logs` (Req 2.3, 2.4, 2.6).
///
/// - [`LogsExit::Normal`] — Geometry Dash è terminato normalmente (codice 0):
///   esito di successo.
/// - [`LogsExit::Crashed`] — Geometry Dash è andato in crash (codice ≠ 0 o
///   terminazione da segnale): il codice riportato è ≠ 0.
/// - [`LogsExit::Interrupted`] — l'utente ha interrotto il comando (Ctrl-C): il
///   processo figlio è stato terminato prima dell'uscita.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LogsExit {
    /// Uscita normale (codice 0).
    Normal,
    /// Crash: codice di uscita ≠ 0 oppure terminazione da segnale. Il valore è
    /// un codice ≠ 0 idoneo alla propagazione (per i segnali: `128 + signal`).
    Crashed(i32),
    /// Interruzione da parte dell'utente (Ctrl-C): il figlio è stato terminato.
    Interrupted,
}

/// Intervallo di poll del canale/eventi: tiene reattivo sia il drenaggio delle
/// righe sia la rilevazione di Ctrl-C, senza busy-loop.
const POLL_INTERVAL: Duration = Duration::from_millis(100);

/// Multiplexer **puro**: fonde due sorgenti di righe — `stdout` e `stderr`,
/// ciascuna già etichettata con il numero di sequenza globale di arrivo — in un
/// unico canale ordinato `(seq, line)` (Req 2.2).
///
/// Invarianti garantiti (Property 8):
///   - **nessuna riga persa**: l'output contiene esattamente tutte le righe
///     delle due sorgenti (`stdout.len() + stderr.len()` elementi);
///   - **ordine relativo preservato**: le righe di ciascuna sorgente compaiono
///     nell'output nello stesso ordine in cui vi appaiono in ingresso.
///
/// Presuppone che ciascuna sorgente sia ordinata per `seq` crescente — è
/// l'invariante prodotto a runtime dall'assegnazione monotòna del `seq` globale
/// nell'ordine di lettura di ogni thread. Il merge è stabile: a parità di `seq`
/// (che a runtime non accade, essendo il `seq` globale univoco) la riga di
/// `stdout` precede quella di `stderr`.
pub fn multiplex(stdout: &[(u64, String)], stderr: &[(u64, String)]) -> Vec<(u64, String)> {
    let mut merged = Vec::with_capacity(stdout.len() + stderr.len());
    let (mut i, mut j) = (0usize, 0usize);
    while i < stdout.len() && j < stderr.len() {
        if stdout[i].0 <= stderr[j].0 {
            merged.push(stdout[i].clone());
            i += 1;
        } else {
            merged.push(stderr[j].clone());
            j += 1;
        }
    }
    merged.extend_from_slice(&stdout[i..]);
    merged.extend_from_slice(&stderr[j..]);
    merged
}

/// Classifica lo stato di terminazione del processo di Geometry Dash (Req 2.3,
/// 2.4). Funzione **pura**: codice `0` ⇒ [`LogsExit::Normal`]; qualunque codice
/// ≠ 0 ⇒ [`LogsExit::Crashed(code)`]; terminazione da **segnale** (nessun
/// codice) ⇒ [`LogsExit::Crashed(128 + signal)`], secondo la convenzione POSIX.
pub fn classify_exit(status: std::process::ExitStatus) -> LogsExit {
    match status.code() {
        Some(0) => LogsExit::Normal,
        Some(code) => LogsExit::Crashed(code),
        None => {
            // Terminazione da segnale (nessun codice di uscita): mappa a un
            // codice ≠ 0 secondo la convenzione POSIX `128 + signal`.
            #[cfg(unix)]
            {
                use std::os::unix::process::ExitStatusExt;
                let signal = status.signal().unwrap_or(0);
                LogsExit::Crashed(128 + signal)
            }
            #[cfg(not(unix))]
            {
                LogsExit::Crashed(1)
            }
        }
    }
}

/// Avvia Geometry Dash e fa streaming live del suo Log_Stream (Req 2).
///
/// Passi:
///   1. Risolve la `GD_Installation`: se `gd` è fornito lo si riconosce con
///      [`installer::recognize_gd`]; altrimenti si tenta la scoperta locale. In
///      assenza di una copia individuabile ⇒ errore con causa, **nessun**
///      `Log_Stream`, **nessun binario ridistribuito** (Req 2.5).
///   2. Avvia l'eseguibile Mach-O del bundle come processo figlio con
///      `stdout`/`stderr` su pipe (Req 2.1). Fallimento di avvio ⇒ errore con
///      causa (Req 2.5).
///   3. Due thread lettori spingono `(seq, line)` in un canale ordinato; il
///      thread principale stampa in ordine di ricezione, senza scartare righe
///      (Req 2.2).
///   4. All'uscita del processo, **svuota le righe residue** prima di terminare
///      e classifica l'esito con [`classify_exit`] (Req 2.3, 2.4).
///   5. Su **Ctrl-C** (watcher `crossterm`) cessa la stampa, **termina il
///      figlio** e ritorna [`LogsExit::Interrupted`] (Req 2.6).
pub fn stream_logs(gd: Option<&Path>) -> anyhow::Result<LogsExit> {
    let install = resolve_installation(gd)?;
    let executable = install.executable();

    // (2) Avvio del processo figlio con stdout/stderr su pipe (Req 2.1).
    let mut child = Command::new(&executable)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .with_context(|| {
            format!(
                "impossibile avviare Geometry Dash da '{}'",
                executable.display()
            )
        })?;

    // Canale ordinato (seq, line) alimentato dai due thread lettori. Il seq
    // globale monotòno assegnato all'atto della lettura preserva l'ordine
    // relativo di ciascuna sorgente (Req 2.2).
    let seq = Arc::new(AtomicU64::new(0));
    let (tx, rx) = mpsc::channel::<(u64, String)>();

    let stdout = child
        .stdout
        .take()
        .ok_or_else(|| anyhow!("stdout di Geometry Dash non disponibile"))?;
    let stderr = child
        .stderr
        .take()
        .ok_or_else(|| anyhow!("stderr di Geometry Dash non disponibile"))?;

    let stdout_reader = spawn_reader(stdout, seq.clone(), tx.clone());
    let stderr_reader = spawn_reader(stderr, seq.clone(), tx);
    // Nota: entrambe le copie di `tx` sono ora possedute dai lettori; il canale
    // si chiude (Disconnected) solo quando entrambi i lettori raggiungono EOF.

    // (5) Watcher Ctrl-C: riuso di `crossterm`, nessuna nuova dipendenza.
    let interrupted = Arc::new(AtomicBool::new(false));
    let watcher_done = Arc::new(AtomicBool::new(false));
    let watcher = spawn_ctrl_c_watcher(interrupted.clone(), watcher_done.clone());

    // (3)+(4) Loop di stampa: consuma il canale in ordine di ricezione e svuota
    // le righe residue fino alla chiusura del canale, salvo interruzione.
    let mut stdout_handle = io::stdout();
    let outcome = loop {
        if interrupted.load(Ordering::SeqCst) {
            break StreamOutcome::Interrupted;
        }
        match rx.recv_timeout(POLL_INTERVAL) {
            Ok((_seq, line)) => {
                // Best-effort: un errore di scrittura sul terminale non deve
                // lasciare processi orfani; interrompiamo lo streaming.
                if print_line(&mut stdout_handle, &line).is_err() {
                    break StreamOutcome::Interrupted;
                }
            }
            Err(RecvTimeoutError::Timeout) => continue,
            // Entrambi i lettori hanno raggiunto EOF: tutte le righe residue
            // sono state emesse (Req 2.3, 2.4).
            Err(RecvTimeoutError::Disconnected) => break StreamOutcome::Drained,
        }
    };

    // Ferma e ripristina il watcher (uscita pulita del terminale).
    watcher_done.store(true, Ordering::SeqCst);
    let _ = watcher.join();

    match outcome {
        StreamOutcome::Interrupted => {
            // (5) Ctrl-C: termina il processo figlio avviato da `logs` e poi
            // esce, senza lasciare processi orfani (Req 2.6).
            let _ = child.kill();
            let _ = child.wait();
            // I lettori termineranno alla chiusura delle pipe del figlio.
            let _ = stdout_reader.join();
            let _ = stderr_reader.join();
            Ok(LogsExit::Interrupted)
        }
        StreamOutcome::Drained => {
            let _ = stdout_reader.join();
            let _ = stderr_reader.join();
            // (4) Classificazione dell'uscita del processo (Req 2.3, 2.4).
            let status = child
                .wait()
                .context("attesa della terminazione di Geometry Dash fallita")?;
            Ok(classify_exit(status))
        }
    }
}

/// Esito interno del loop di streaming, prima della classificazione finale.
enum StreamOutcome {
    /// Il canale si è chiuso: tutte le righe sono state emesse.
    Drained,
    /// L'utente ha interrotto (Ctrl-C) o la scrittura sul terminale è fallita.
    Interrupted,
}

/// Risolve la `GD_Installation` da usare (Req 2.1, 2.5).
///
/// Se `gd` è fornito lo si riconosce con [`installer::recognize_gd`]; altrimenti
/// si tenta la **scoperta locale**. In assenza di una copia individuabile
/// restituisce un errore con causa, senza mai richiedere né ridistribuire un
/// binario di Geometry Dash (Req 2.5, 9.5).
///
/// Esposto `pub(crate)` così che i comandi che leggono il `GD_Binary`
/// (`siggen`, `check-offsets`) riusino la **stessa** logica di risoluzione
/// (fornita o scoperta localmente) senza duplicarla (Req 3.7, 4.7, 9.5).
pub(crate) fn resolve_installation(gd: Option<&Path>) -> anyhow::Result<GdInstallation> {
    match gd {
        Some(path) => installer::recognize_gd(path).map_err(|err| {
            anyhow!(
                "GD_Installation non individuabile in '{}': {err}",
                path.display()
            )
        }),
        None => discover_gd_installation().ok_or_else(|| {
            anyhow!(
                "nessuna GD_Installation fornita né scopribile localmente; \
                 indica il percorso del bundle con --gd. \
                 Pulse non richiede né ridistribuisce alcun binario di Geometry Dash."
            )
        }),
    }
}

/// Tenta la scoperta locale di una `GD_Installation` in posizioni note (macOS,
/// installazione Steam). Restituisce la prima copia **riconosciuta** da
/// [`installer::recognize_gd`], oppure `None` se nessuna è individuabile — mai
/// un binario ridistribuito dal progetto (Req 2.5).
fn discover_gd_installation() -> Option<GdInstallation> {
    for candidate in candidate_install_paths() {
        if let Ok(install) = installer::recognize_gd(&candidate) {
            return Some(install);
        }
    }
    None
}

/// Percorsi candidati per la scoperta locale della `GD_Installation` (macOS).
fn candidate_install_paths() -> Vec<PathBuf> {
    let mut candidates = Vec::new();
    if let Some(home) = std::env::var_os("HOME") {
        let home = PathBuf::from(home);
        candidates.push(home.join(
            "Library/Application Support/Steam/steamapps/common/Geometry Dash/Geometry Dash.app",
        ));
    }
    candidates
}

/// Avvia un thread lettore che legge `reader` riga per riga e spinge ogni riga
/// nel canale con un numero di sequenza globale monotòno (assegnato all'atto
/// della lettura), preservando così l'ordine relativo della sorgente (Req 2.2).
fn spawn_reader<R: Read + Send + 'static>(
    reader: R,
    seq: Arc<AtomicU64>,
    tx: Sender<(u64, String)>,
) -> JoinHandle<()> {
    thread::spawn(move || {
        let buffered = BufReader::new(reader);
        for line in buffered.lines() {
            match line {
                Ok(text) => {
                    let s = seq.fetch_add(1, Ordering::SeqCst);
                    // Se il ricevitore è andato via, non c'è più nulla da
                    // stampare: il lettore termina.
                    if tx.send((s, text)).is_err() {
                        break;
                    }
                }
                // Errore di lettura o chiusura della pipe: EOF effettivo.
                Err(_) => break,
            }
        }
    })
}

/// Avvia il watcher di Ctrl-C basato su `crossterm` (riuso, nessuna nuova
/// dipendenza — Req 2.6).
///
/// Abilita la modalità raw per intercettare Ctrl-C come evento di tastiera e
/// poll-a gli eventi finché non rileva `Ctrl-C` (⇒ imposta il flag
/// `interrupted`) o finché il chiamante non segnala `done` (fine dello
/// streaming). In entrambi i casi ripristina la modalità raw prima di uscire,
/// così il terminale resta pulito.
fn spawn_ctrl_c_watcher(
    interrupted: Arc<AtomicBool>,
    done: Arc<AtomicBool>,
) -> JoinHandle<()> {
    thread::spawn(move || {
        // Se non è disponibile un terminale (raw mode non abilitabile), il
        // watcher esce senza interferire: SIGINT resta gestito dal driver del
        // terminale, che termina comunque il gruppo di processi.
        if enable_raw_mode().is_err() {
            return;
        }
        loop {
            if done.load(Ordering::SeqCst) {
                break;
            }
            match event::poll(POLL_INTERVAL) {
                Ok(true) => {
                    if let Ok(Event::Key(KeyEvent {
                        code: KeyCode::Char('c'),
                        modifiers,
                        kind: KeyEventKind::Press,
                        ..
                    })) = event::read()
                    {
                        if modifiers.contains(KeyModifiers::CONTROL) {
                            interrupted.store(true, Ordering::SeqCst);
                            break;
                        }
                    }
                }
                Ok(false) => continue,
                Err(_) => break,
            }
        }
        let _ = disable_raw_mode();
    })
}

/// Stampa una riga di log sul terminale. Usa `\r\n` perché durante lo streaming
/// il watcher tiene attiva la modalità raw (in cui `\n` non implica il ritorno
/// a capo); in modalità cotta l'eventuale `\r` extra è innocuo.
fn print_line(out: &mut impl Write, line: &str) -> io::Result<()> {
    out.write_all(line.as_bytes())?;
    out.write_all(b"\r\n")?;
    out.flush()
}

#[cfg(test)]
mod tests {
    use super::*;

    // --- multiplexer puro (Req 2.2) ---------------------------------------

    #[test]
    fn multiplex_preserves_all_lines_and_relative_order() {
        // stdout riceve seq pari, stderr seq dispari (interleaving realistico).
        let stdout = vec![
            (0u64, "out-0".to_string()),
            (2, "out-1".to_string()),
            (4, "out-2".to_string()),
        ];
        let stderr = vec![(1u64, "err-0".to_string()), (3, "err-1".to_string())];

        let merged = multiplex(&stdout, &stderr);

        // Nessuna riga persa.
        assert_eq!(merged.len(), stdout.len() + stderr.len());

        // Ordine globale per seq.
        let seqs: Vec<u64> = merged.iter().map(|(s, _)| *s).collect();
        assert_eq!(seqs, vec![0, 1, 2, 3, 4]);

        // Ordine relativo di ciascuna sorgente preservato.
        let out_lines: Vec<&str> = merged
            .iter()
            .filter(|(_, l)| l.starts_with("out-"))
            .map(|(_, l)| l.as_str())
            .collect();
        assert_eq!(out_lines, vec!["out-0", "out-1", "out-2"]);

        let err_lines: Vec<&str> = merged
            .iter()
            .filter(|(_, l)| l.starts_with("err-"))
            .map(|(_, l)| l.as_str())
            .collect();
        assert_eq!(err_lines, vec!["err-0", "err-1"]);
    }

    #[test]
    fn multiplex_handles_empty_sources() {
        let out = vec![(0u64, "a".to_string()), (1, "b".to_string())];
        let empty: Vec<(u64, String)> = Vec::new();
        assert_eq!(multiplex(&out, &empty), out);
        assert_eq!(multiplex(&empty, &out), out);
        assert!(multiplex(&empty, &empty).is_empty());
    }

    // --- classify_exit puro (Req 2.3, 2.4) --------------------------------

    #[cfg(unix)]
    #[test]
    fn classify_exit_maps_zero_to_normal_and_nonzero_to_crashed() {
        use std::os::unix::process::ExitStatusExt;
        use std::process::ExitStatus;

        assert_eq!(classify_exit(ExitStatus::from_raw(0)), LogsExit::Normal);

        // Codice di uscita 1 (wait status: code << 8 su Unix).
        let code_one = ExitStatus::from_raw(1 << 8);
        assert_eq!(classify_exit(code_one), LogsExit::Crashed(1));

        // Terminazione da segnale (es. SIGSEGV = 11): codice ≠ 0 (128 + 11).
        let signaled = ExitStatus::from_raw(11);
        assert_eq!(classify_exit(signaled), LogsExit::Crashed(128 + 11));
    }

    // --- resolve_installation (Req 2.5) -----------------------------------

    #[test]
    fn resolve_installation_errors_when_path_is_not_gd() {
        // Un percorso inesistente non è una GD_Installation: errore, nessun
        // Log_Stream, nessun binario ridistribuito (Req 2.5).
        let bogus = PathBuf::from("/percorso/che/non/esiste/Geometry Dash.app");
        let err = resolve_installation(Some(&bogus)).unwrap_err();
        assert!(
            err.to_string().contains("non individuabile"),
            "messaggio inatteso: {err}"
        );
    }
}
