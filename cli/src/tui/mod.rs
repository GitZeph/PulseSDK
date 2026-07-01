//! TUI full-screen interattiva della Pulse CLI.
//!
//! Avviata quando `pulse` è invocato **senza sottocomando**. Mostra il banner
//! Pulse, una lista di comandi raggruppati navigabile con le frecce e un
//! pannello di dettaglio; i comandi `Real` sono eseguiti riusando gli stessi
//! gestori dei sottocomandi (`crate::cli`), senza duplicare la logica né
//! lanciare un secondo processo `pulse`.
//!
//! ## Guardia TTY (fail-safe)
//! `ratatui` richiede un vero terminale. Se `stdout` NON è un TTY (pipe/CI) la
//! TUI **non** entra in raw mode: stampa il banner + la lista comandi in testo
//! semplice ed esce con codice 0 ([`launch`]). Così non blocca né rompe le
//! shell non interattive.
//!
//! ## Ripristino del terminale (sempre)
//! L'ingresso in raw mode + schermo alternato è protetto da [`TerminalGuard`]:
//! il suo `Drop` ripristina lo stato del terminale anche in caso di errore, e
//! un panic hook dedicato lo ripristina prima di stampare il panic. In questo
//! modo un fallimento non lascia mai il terminale dell'utente corrotto.

pub mod banner;
pub mod menu;

use std::io::{self, IsTerminal, Stdout, Write};
use std::path::Path;
use std::time::{Duration, Instant};

use crossterm::event::{self, Event, KeyCode, KeyEvent, KeyEventKind, KeyModifiers};
use crossterm::terminal::{
    disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen,
};
use crossterm::{cursor, execute};
use ratatui::backend::CrosstermBackend;
use ratatui::layout::{Alignment, Constraint, Direction, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span, Text};
use ratatui::widgets::{
    Block, BorderType, Borders, Clear, List, ListItem, ListState, Paragraph, Wrap,
};
use ratatui::{Frame, Terminal};

use crate::cli;
use menu::{Action, Command, Row, Status};

/// Verde Pulse — accento principale della TUI.
const PULSE_GREEN: Color = Color::Rgb(0, 230, 118);
/// Verde attenuato per intestazioni/testi secondari.
const PULSE_GREEN_DIM: Color = Color::Rgb(0, 140, 78);

/// Alias del terminale ratatui su `crossterm` + `stdout`.
type Tui = Terminal<CrosstermBackend<Stdout>>;

/// Punto di ingresso della TUI (`pulse` senza sottocomando).
///
/// Guardia TTY: se `stdout` non è un terminale interattivo, stampa il banner e
/// la lista comandi in testo semplice ed esce con successo, senza entrare in
/// raw mode (adatto a pipe/CI). Altrimenti avvia la TUI full-screen.
pub fn launch() -> anyhow::Result<()> {
    if !io::stdout().is_terminal() {
        print_plain_fallback();
        return Ok(());
    }
    run_interactive()
}

/// Fallback non-TTY: banner + tagline + lista comandi raggruppati in testo
/// semplice su stdout.
fn print_plain_fallback() {
    println!("{}", banner::PULSE_BANNER);
    println!();
    println!("{}", banner::PULSE_TAGLINE);
    println!();
    println!("Pulse CLI — available commands:");
    for group in menu::GROUPS {
        println!();
        println!("  {}", group.title);
        for cmd in group.commands {
            let badge = match cmd.status {
                Status::Real => "",
                Status::ComingSoon => "  (soon)",
            };
            println!("    {:<16} {}{}", cmd.label, cmd.description, badge);
        }
    }
    println!();
    println!("Run `pulse <command> --help` for details, or `pulse` in a terminal for the interactive UI.");
}

/// Avvia la TUI interattiva con ripristino garantito del terminale.
fn run_interactive() -> anyhow::Result<()> {
    install_panic_hook();
    let _guard = TerminalGuard::enter()?;

    let backend = CrosstermBackend::new(io::stdout());
    let mut terminal = Terminal::new(backend)?;
    terminal.clear()?;

    let mut app = App::new();
    let result = app.run(&mut terminal);

    // `_guard` ripristina il terminale al `Drop`, anche in caso di errore.
    result
}

/// Installa un panic hook che ripristina il terminale PRIMA di stampare il
/// panic, così un crash non lascia lo schermo alternato/raw mode attivi.
fn install_panic_hook() {
    let original = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        let _ = restore_terminal();
        original(info);
    }));
}

/// Ripristina lo stato del terminale (raw mode off + schermo normale + cursore).
fn restore_terminal() -> io::Result<()> {
    let _ = disable_raw_mode();
    execute!(io::stdout(), LeaveAlternateScreen, cursor::Show)
}

/// Guardia RAII: entra in raw mode + schermo alternato alla creazione e
/// ripristina il terminale al `Drop` (anche su errore/unwind).
struct TerminalGuard;

impl TerminalGuard {
    fn enter() -> io::Result<Self> {
        enable_raw_mode()?;
        execute!(io::stdout(), EnterAlternateScreen, cursor::Hide)?;
        Ok(Self)
    }
}

impl Drop for TerminalGuard {
    fn drop(&mut self) {
        let _ = restore_terminal();
    }
}

// ===========================================================================
// Stato dell'applicazione
// ===========================================================================

/// Tipo di campo di un prompt di input.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum FieldKind {
    /// Campo di testo libero.
    Text,
    /// Interruttore booleano (yes/no) commutabile con spazio/frecce.
    Toggle,
}

/// Un campo di un prompt di input.
#[derive(Debug, Clone)]
struct Field {
    label: String,
    value: String,
    kind: FieldKind,
    /// True se il campo di testo è obbligatorio (non può restare vuoto).
    required: bool,
}

/// Modale di raccolta argomenti prima di eseguire un comando `Real`.
#[derive(Debug, Clone)]
struct Prompt {
    action: Action,
    title: String,
    fields: Vec<Field>,
    active: usize,
}

impl Prompt {
    /// Costruisce il prompt appropriato per l'azione, o `None` se il comando non
    /// richiede argomenti (es. `build`).
    fn for_action(action: Action) -> Option<Self> {
        match action {
            Action::Build | Action::ComingSoon => None,
            Action::New => Some(Prompt {
                action,
                title: "pulse new — scaffold a new mod".to_string(),
                fields: vec![Field {
                    label: "Project name / path".to_string(),
                    value: String::new(),
                    kind: FieldKind::Text,
                    required: true,
                }],
                active: 0,
            }),
            Action::Install => Some(Prompt {
                action,
                title: "pulse install — install into Geometry Dash".to_string(),
                fields: vec![
                    Field {
                        label: "GD .app path (--gd)".to_string(),
                        value: String::new(),
                        kind: FieldKind::Text,
                        required: true,
                    },
                    Field {
                        label: "Built artifact path (--artifact)".to_string(),
                        value: String::new(),
                        kind: FieldKind::Text,
                        required: true,
                    },
                    Field {
                        label: "Native injection (--native)".to_string(),
                        value: "no".to_string(),
                        kind: FieldKind::Toggle,
                        required: false,
                    },
                ],
                active: 0,
            }),
            Action::Uninstall => Some(Prompt {
                action,
                title: "pulse uninstall — remove an installed mod".to_string(),
                fields: vec![Field {
                    label: "GD .app path (--gd)".to_string(),
                    value: String::new(),
                    kind: FieldKind::Text,
                    required: true,
                }],
                active: 0,
            }),
        }
    }
}

/// Stato della TUI.
struct App {
    rows: Vec<Row>,
    /// Indice della riga selezionata (sempre una `Row::Item`).
    selected_row: usize,
    show_help: bool,
    prompt: Option<Prompt>,
    /// Messaggio transitorio (es. "coming soon"); un tasto qualsiasi lo chiude.
    toast: Option<String>,
    /// Istante d'avvio: la fase del battito del banner deriva da
    /// `start.elapsed()`, così l'animazione è liscia e legata al tempo di
    /// parete (non al conteggio dei frame).
    start: Instant,
}

impl App {
    fn new() -> Self {
        let rows = menu::build_rows();
        let selected_row = rows
            .iter()
            .position(|r| matches!(r, Row::Item { .. }))
            .unwrap_or(0);
        App {
            rows,
            selected_row,
            show_help: false,
            prompt: None,
            toast: None,
            start: Instant::now(),
        }
    }

    /// Comando attualmente evidenziato.
    fn selected_command(&self) -> Option<Command> {
        match self.rows.get(self.selected_row) {
            Some(Row::Item { command, .. }) => Some(*command),
            _ => None,
        }
    }

    /// Sposta la selezione al prossimo `Row::Item` nella direzione indicata,
    /// saltando le intestazioni di gruppo.
    fn move_selection(&mut self, delta: isize) {
        let n = self.rows.len() as isize;
        let mut idx = self.selected_row as isize;
        for _ in 0..n {
            idx += delta;
            if idx < 0 {
                idx = n - 1;
            } else if idx >= n {
                idx = 0;
            }
            if matches!(self.rows[idx as usize], Row::Item { .. }) {
                self.selected_row = idx as usize;
                return;
            }
        }
    }

    /// Ciclo principale: disegna a ~20 FPS, gestisce gli eventi appena
    /// disponibili e fa avanzare l'animazione del battito in base al tempo.
    ///
    /// A differenza di un `event::read()` bloccante, qui si usa
    /// `event::poll(POLL_INTERVAL)`: se un evento è pronto entro l'intervallo lo
    /// si legge e gestisce **esattamente come prima** (nessun cambiamento nella
    /// gestione dei tasti, quindi q/Esc/Ctrl+C restano istantanei); altrimenti,
    /// allo scadere del timeout, il loop ridisegna comunque così il banner
    /// pulsa in modo fluido. ~50ms mantiene la CPU bassa senza busy-loop.
    fn run(&mut self, terminal: &mut Tui) -> anyhow::Result<()> {
        const POLL_INTERVAL: Duration = Duration::from_millis(50);
        loop {
            terminal.draw(|f| ui(f, self))?;
            if event::poll(POLL_INTERVAL)? {
                let Event::Key(key) = event::read()? else {
                    continue;
                };
                if key.kind != KeyEventKind::Press {
                    continue;
                }
                if self.handle_key(key, terminal)? {
                    return Ok(());
                }
            }
        }
    }

    /// Gestisce un tasto. Restituisce `true` se la TUI deve uscire.
    fn handle_key(&mut self, key: KeyEvent, terminal: &mut Tui) -> anyhow::Result<bool> {
        // Ctrl+C esce sempre.
        if key.modifiers.contains(KeyModifiers::CONTROL) && key.code == KeyCode::Char('c') {
            return Ok(true);
        }

        // Un toast attivo intercetta il primo tasto (lo chiude).
        if self.toast.is_some() {
            self.toast = None;
            return Ok(false);
        }

        // Prompt attivo: instrada gli input al prompt.
        if self.prompt.is_some() {
            self.handle_prompt_key(key, terminal)?;
            return Ok(false);
        }

        match key.code {
            KeyCode::Char('q') | KeyCode::Esc => return Ok(true),
            KeyCode::Down | KeyCode::Char('j') => self.move_selection(1),
            KeyCode::Up | KeyCode::Char('k') => self.move_selection(-1),
            KeyCode::Char('?') => self.show_help = !self.show_help,
            KeyCode::Enter => self.activate_selected(terminal)?,
            _ => {}
        }
        Ok(false)
    }

    /// Attiva il comando evidenziato (Enter dalla lista).
    fn activate_selected(&mut self, terminal: &mut Tui) -> anyhow::Result<()> {
        let Some(cmd) = self.selected_command() else {
            return Ok(());
        };
        if !cmd.is_runnable() {
            self.toast = Some(format!(
                "`{}` isn't built yet — help wanted!\n\nThis command is on the roadmap. \
                 See CONTRIBUTING.md to lend a hand.\n\n(press any key to go back)",
                cmd.label
            ));
            return Ok(());
        }
        match Prompt::for_action(cmd.action) {
            Some(prompt) => self.prompt = Some(prompt),
            // Nessun argomento richiesto (es. build): esegui subito.
            None => self.run_action(cmd.action, &[], terminal)?,
        }
        Ok(())
    }

    /// Gestisce un tasto mentre un prompt è attivo.
    fn handle_prompt_key(&mut self, key: KeyEvent, terminal: &mut Tui) -> anyhow::Result<()> {
        let Some(prompt) = self.prompt.as_mut() else {
            return Ok(());
        };
        match key.code {
            KeyCode::Esc => {
                self.prompt = None;
            }
            KeyCode::Tab | KeyCode::Down => {
                prompt.active = (prompt.active + 1) % prompt.fields.len();
            }
            KeyCode::BackTab | KeyCode::Up => {
                prompt.active =
                    (prompt.active + prompt.fields.len() - 1) % prompt.fields.len();
            }
            KeyCode::Enter => {
                if prompt.active + 1 < prompt.fields.len() {
                    prompt.active += 1;
                } else {
                    // Ultimo campo: valida ed esegui.
                    let missing = prompt
                        .fields
                        .iter()
                        .find(|f| f.required && f.value.trim().is_empty())
                        .map(|f| f.label.clone());
                    if let Some(label) = missing {
                        self.toast = Some(format!(
                            "\"{label}\" is required.\n\n(press any key to go back)"
                        ));
                        return Ok(());
                    }
                    let action = prompt.action;
                    let fields = prompt.fields.clone();
                    self.prompt = None;
                    self.run_action(action, &fields, terminal)?;
                }
            }
            KeyCode::Char(' ') if prompt.fields[prompt.active].kind == FieldKind::Toggle => {
                let f = &mut prompt.fields[prompt.active];
                f.value = if f.value == "yes" { "no" } else { "yes" }.to_string();
            }
            KeyCode::Left | KeyCode::Right
                if prompt.fields[prompt.active].kind == FieldKind::Toggle =>
            {
                let f = &mut prompt.fields[prompt.active];
                f.value = if f.value == "yes" { "no" } else { "yes" }.to_string();
            }
            KeyCode::Backspace => {
                let f = &mut prompt.fields[prompt.active];
                if f.kind == FieldKind::Text {
                    f.value.pop();
                }
            }
            KeyCode::Char(c) => {
                let f = &mut prompt.fields[prompt.active];
                if f.kind == FieldKind::Text {
                    f.value.push(c);
                }
            }
            _ => {}
        }
        Ok(())
    }

    /// Esegue un'azione `Real` riusando i gestori di `crate::cli`.
    ///
    /// Sospende la TUI (esce dallo schermo alternato + raw mode), esegue il
    /// gestore, mostra output/errore, attende un tasto e ripristina la TUI.
    /// Gli errori sono gestiti con grazia: vengono stampati, senza far
    /// crashare la TUI.
    fn run_action(
        &mut self,
        action: Action,
        fields: &[Field],
        terminal: &mut Tui,
    ) -> anyhow::Result<()> {
        suspend_tui(terminal)?;

        let invocation = describe_invocation(action, fields);
        println!("\n\x1b[38;2;0;230;118m▶ running: {invocation}\x1b[0m\n");
        let _ = io::stdout().flush();

        let result: anyhow::Result<()> = match action {
            Action::New => {
                let name = fields[0].value.trim();
                cli::run_new(Path::new(name), None)
            }
            Action::Build => cli::run_build(Path::new(".")),
            Action::Install => {
                let gd = fields[0].value.trim();
                let artifact = fields[1].value.trim();
                let native = fields[2].value == "yes";
                cli::run_install(Path::new(gd), Path::new(artifact), native)
            }
            Action::Uninstall => {
                let gd = fields[0].value.trim();
                cli::run_uninstall(Path::new(gd))
            }
            Action::ComingSoon => Ok(()),
        };

        match result {
            Ok(()) => println!("\n\x1b[38;2;0;230;118m✔ done.\x1b[0m"),
            Err(err) => {
                // Errore gestito con grazia: mostrato, non fa crashare la TUI.
                eprintln!("\n\x1b[38;2;255;95;95m✖ error:\x1b[0m {err:#}");
            }
        }

        println!("\n\x1b[2mPress any key to return to the menu…\x1b[0m");
        let _ = io::stdout().flush();
        wait_for_keypress()?;

        resume_tui(terminal)?;
        Ok(())
    }
}

/// Descrive l'invocazione `pulse …` effettiva a partire dai campi raccolti.
fn describe_invocation(action: Action, fields: &[Field]) -> String {
    match action {
        Action::New => format!("pulse new {}", fields[0].value.trim()),
        Action::Build => "pulse build".to_string(),
        Action::Install => {
            let native = if fields[2].value == "yes" { " --native" } else { "" };
            format!(
                "pulse install --gd {} --artifact {}{}",
                fields[0].value.trim(),
                fields[1].value.trim(),
                native
            )
        }
        Action::Uninstall => format!("pulse uninstall --gd {}", fields[0].value.trim()),
        Action::ComingSoon => "pulse".to_string(),
    }
}

/// Sospende la TUI: torna allo schermo normale e disattiva il raw mode.
fn suspend_tui(terminal: &mut Tui) -> io::Result<()> {
    disable_raw_mode()?;
    execute!(terminal.backend_mut(), LeaveAlternateScreen, cursor::Show)?;
    Ok(())
}

/// Ripristina la TUI: rientra nello schermo alternato + raw mode e pulisce.
fn resume_tui(terminal: &mut Tui) -> io::Result<()> {
    enable_raw_mode()?;
    execute!(terminal.backend_mut(), EnterAlternateScreen, cursor::Hide)?;
    terminal.clear()?;
    Ok(())
}

/// Attende la pressione di un singolo tasto (in raw mode temporaneo).
fn wait_for_keypress() -> io::Result<()> {
    enable_raw_mode()?;
    let outcome = loop {
        match event::read() {
            Ok(Event::Key(k)) if k.kind == KeyEventKind::Press => break Ok(()),
            Ok(_) => continue,
            Err(e) => break Err(e),
        }
    };
    disable_raw_mode()?;
    outcome
}

// ===========================================================================
// Rendering
// ===========================================================================

/// Disegna l'intera interfaccia.
fn ui(f: &mut Frame, app: &App) {
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(9),                                       // banner
            Constraint::Min(6),                                          // corpo
            Constraint::Length(if app.show_help { 6 } else { 3 }),       // footer
        ])
        .split(f.area());

    render_banner(f, chunks[0], app);
    render_body(f, chunks[1], app);
    render_footer(f, chunks[2], app);

    if let Some(prompt) = &app.prompt {
        render_prompt(f, f.area(), prompt);
    }
    if let Some(toast) = &app.toast {
        render_toast(f, f.area(), toast);
    }
}

/// Banner ASCII con animazione a battito + tagline.
///
/// Ogni glifo del banner diventa uno `Span` colorato per colonna in base
/// all'onda di luce che scorre (vedi [`banner::pulse_char_rgb`]): verde profondo
/// a riposo → verde Pulse brillante → cresta menta al picco. La fase deriva da
/// `app.start.elapsed()`, quindi lo sweep è liscio e legato al tempo di parete.
/// La tagline resta in verde attenuato corsivo (calma, non distrae).
fn render_banner(f: &mut Frame, area: Rect, app: &App) {
    let elapsed = app.start.elapsed();
    // Larghezza di riferimento dell'onda: la riga più larga del banner, così la
    // fronte d'onda è coerente su tutte le righe.
    let banner_width = banner::PULSE_BANNER
        .lines()
        .map(|l| l.chars().count())
        .max()
        .unwrap_or(1);

    let mut lines: Vec<Line> = banner::PULSE_BANNER
        .lines()
        .enumerate()
        .map(|(row, text)| {
            let spans: Vec<Span> = text
                .chars()
                .enumerate()
                .map(|(col, ch)| {
                    let (r, g, b) = banner::pulse_char_rgb(row, col, banner_width, elapsed);
                    Span::styled(
                        ch.to_string(),
                        Style::default()
                            .fg(Color::Rgb(r, g, b))
                            .add_modifier(Modifier::BOLD),
                    )
                })
                .collect();
            Line::from(spans)
        })
        .collect();

    lines.push(Line::styled(
        banner::PULSE_TAGLINE,
        Style::default().fg(PULSE_GREEN_DIM).add_modifier(Modifier::ITALIC),
    ));
    let banner = Paragraph::new(lines).alignment(Alignment::Center);
    f.render_widget(banner, area);
}

/// Corpo: lista comandi a sinistra, dettaglio a destra.
fn render_body(f: &mut Frame, area: Rect, app: &App) {
    let cols = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Percentage(42), Constraint::Percentage(58)])
        .split(area);

    render_command_list(f, cols[0], app);
    render_detail(f, cols[1], app);
}

/// Lista dei comandi raggruppati (intestazioni dim, comandi selezionabili).
fn render_command_list(f: &mut Frame, area: Rect, app: &App) {
    let items: Vec<ListItem> = app
        .rows
        .iter()
        .map(|row| match row {
            Row::Header(title) => ListItem::new(Line::styled(
                format!("  {title}"),
                Style::default()
                    .fg(PULSE_GREEN_DIM)
                    .add_modifier(Modifier::BOLD | Modifier::DIM),
            )),
            Row::Item { command, .. } => {
                let spans = match command.status {
                    Status::Real => vec![
                        Span::styled("  ● ", Style::default().fg(PULSE_GREEN)),
                        Span::styled(command.label, Style::default().fg(Color::White)),
                    ],
                    Status::ComingSoon => vec![
                        Span::styled("  ○ ", Style::default().fg(Color::DarkGray)),
                        Span::styled(command.label, Style::default().fg(Color::DarkGray)),
                        Span::styled(
                            "  soon",
                            Style::default()
                                .fg(Color::DarkGray)
                                .add_modifier(Modifier::ITALIC | Modifier::DIM),
                        ),
                    ],
                };
                ListItem::new(Line::from(spans))
            }
        })
        .collect();

    let block = Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(Style::default().fg(PULSE_GREEN_DIM))
        .title(Span::styled(
            " Commands ",
            Style::default().fg(PULSE_GREEN).add_modifier(Modifier::BOLD),
        ));

    let list = List::new(items).block(block).highlight_style(
        Style::default()
            .bg(PULSE_GREEN)
            .fg(Color::Black)
            .add_modifier(Modifier::BOLD),
    );

    let mut state = ListState::default();
    state.select(Some(app.selected_row));
    f.render_stateful_widget(list, area, &mut state);
}

/// Pannello di dettaglio del comando evidenziato.
fn render_detail(f: &mut Frame, area: Rect, app: &App) {
    let block = Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(Style::default().fg(PULSE_GREEN_DIM))
        .title(Span::styled(
            " Details ",
            Style::default().fg(PULSE_GREEN).add_modifier(Modifier::BOLD),
        ));

    let text: Text = match app.selected_command() {
        Some(cmd) => {
            let status_line = match cmd.status {
                Status::Real => Line::from(vec![
                    Span::styled("status   ", Style::default().fg(Color::DarkGray)),
                    Span::styled(
                        "● ready",
                        Style::default().fg(PULSE_GREEN).add_modifier(Modifier::BOLD),
                    ),
                ]),
                Status::ComingSoon => Line::from(vec![
                    Span::styled("status   ", Style::default().fg(Color::DarkGray)),
                    Span::styled(
                        "○ coming soon",
                        Style::default().fg(Color::DarkGray).add_modifier(Modifier::ITALIC),
                    ),
                ]),
            };
            Text::from(vec![
                Line::from(Span::styled(
                    cmd.label,
                    Style::default().fg(PULSE_GREEN).add_modifier(Modifier::BOLD),
                )),
                Line::from(""),
                Line::from(Span::styled(cmd.description, Style::default().fg(Color::White))),
                Line::from(""),
                Line::from(vec![
                    Span::styled("audience ", Style::default().fg(Color::DarkGray)),
                    Span::styled(cmd.audience, Style::default().fg(Color::Gray)),
                ]),
                status_line,
                Line::from(""),
                Line::from(Span::styled("runs", Style::default().fg(Color::DarkGray))),
                Line::from(Span::styled(
                    format!("  $ {}", cmd.invocation),
                    Style::default().fg(PULSE_GREEN_DIM),
                )),
            ])
        }
        None => Text::from("Select a command."),
    };

    let detail = Paragraph::new(text).block(block).wrap(Wrap { trim: false });
    f.render_widget(detail, area);
}

/// Footer con i keybinding (esteso quando l'help è attivo).
fn render_footer(f: &mut Frame, area: Rect, app: &App) {
    let block = Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(Style::default().fg(PULSE_GREEN_DIM));

    let key = |k: &'static str| Span::styled(k, Style::default().fg(PULSE_GREEN).add_modifier(Modifier::BOLD));
    let sep = || Span::styled("  ·  ", Style::default().fg(Color::DarkGray));
    let lbl = |s: &'static str| Span::styled(s, Style::default().fg(Color::Gray));

    let mut lines = vec![Line::from(vec![
        key(" ↑/↓ j/k "),
        lbl("move"),
        sep(),
        key(" ↵ "),
        lbl("run"),
        sep(),
        key(" ? "),
        lbl("help"),
        sep(),
        key(" q/Esc "),
        lbl("quit"),
    ])];

    if app.show_help {
        lines.push(Line::from(Span::styled(
            " Real commands run in-process (no second `pulse` spawned); coming-soon items are disabled.",
            Style::default().fg(Color::DarkGray),
        )));
        lines.push(Line::from(Span::styled(
            " In prompts: Tab/↑/↓ switch fields · Space toggles flags · ↵ confirm · Esc cancel.",
            Style::default().fg(Color::DarkGray),
        )));
    }

    let footer = Paragraph::new(lines).block(block).alignment(Alignment::Center);
    f.render_widget(footer, area);
}

/// Modale centrata di raccolta argomenti.
fn render_prompt(f: &mut Frame, area: Rect, prompt: &Prompt) {
    let popup = centered_rect(64, 40, area);
    f.render_widget(Clear, popup);

    let block = Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(Style::default().fg(PULSE_GREEN))
        .title(Span::styled(
            format!(" {} ", prompt.title),
            Style::default().fg(PULSE_GREEN).add_modifier(Modifier::BOLD),
        ));

    let mut lines: Vec<Line> = Vec::new();
    lines.push(Line::from(""));
    for (i, field) in prompt.fields.iter().enumerate() {
        let active = i == prompt.active;
        let marker = if active { "▶ " } else { "  " };
        let label_style = if active {
            Style::default().fg(PULSE_GREEN).add_modifier(Modifier::BOLD)
        } else {
            Style::default().fg(Color::Gray)
        };
        lines.push(Line::from(vec![
            Span::styled(marker, Style::default().fg(PULSE_GREEN)),
            Span::styled(field.label.clone(), label_style),
        ]));

        let value_display = match field.kind {
            FieldKind::Text => {
                let cursor = if active { "▏" } else { "" };
                format!("    {}{}", field.value, cursor)
            }
            FieldKind::Toggle => {
                let on = field.value == "yes";
                format!("    [{}] {}", if on { "x" } else { " " }, if on { "native" } else { "host stub" })
            }
        };
        lines.push(Line::from(Span::styled(
            value_display,
            Style::default().fg(Color::White),
        )));
        lines.push(Line::from(""));
    }
    lines.push(Line::from(Span::styled(
        "Tab/↑/↓ field · Space toggle · ↵ confirm · Esc cancel",
        Style::default().fg(Color::DarkGray),
    )));

    let paragraph = Paragraph::new(lines).block(block).wrap(Wrap { trim: false });
    f.render_widget(paragraph, popup);
}

/// Modale centrata per un messaggio transitorio (es. coming-soon).
fn render_toast(f: &mut Frame, area: Rect, message: &str) {
    let popup = centered_rect(56, 34, area);
    f.render_widget(Clear, popup);

    let block = Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(Style::default().fg(PULSE_GREEN))
        .title(Span::styled(
            " Heads up ",
            Style::default().fg(PULSE_GREEN).add_modifier(Modifier::BOLD),
        ));

    let paragraph = Paragraph::new(message)
        .block(block)
        .style(Style::default().fg(Color::White))
        .alignment(Alignment::Center)
        .wrap(Wrap { trim: true });
    f.render_widget(paragraph, popup);
}

/// Calcola un rettangolo centrato con percentuali di larghezza/altezza.
fn centered_rect(percent_x: u16, percent_y: u16, area: Rect) -> Rect {
    let vertical = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Percentage((100 - percent_y) / 2),
            Constraint::Percentage(percent_y),
            Constraint::Percentage((100 - percent_y) / 2),
        ])
        .split(area);
    Layout::default()
        .direction(Direction::Horizontal)
        .constraints([
            Constraint::Percentage((100 - percent_x) / 2),
            Constraint::Percentage(percent_x),
            Constraint::Percentage((100 - percent_x) / 2),
        ])
        .split(vertical[1])[1]
}
