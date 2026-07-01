//! Modello del menu della TUI: i 10 comandi raggruppati in 4 sezioni.
//!
//! Ogni comando dichiara etichetta, descrizione, pubblico di riferimento,
//! l'invocazione `pulse …` sottostante e uno [`Status`]. I comandi `Real` sono
//! effettivamente eseguibili (mappati sui gestori esistenti in `cli.rs`); i
//! `ComingSoon` sono mostrati ma non eseguibili.

/// Stato di un comando nel menu.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Status {
    /// Comando cablato e realmente eseguibile.
    Real,
    /// Comando pianificato ma non ancora implementato (mostrato, disabilitato).
    ComingSoon,
}

/// Azione concreta che un comando `Real` innesca nella TUI.
///
/// Ogni variante corrisponde a un gestore pubblico riusato da `cli.rs`, così la
/// TUI non duplica la logica né lancia un secondo processo `pulse`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Action {
    /// `pulse new <name>` → `scaffold::scaffold_new`.
    New,
    /// `pulse build` → `builder::build` nella directory corrente.
    Build,
    /// `pulse install` → `installer::install`/`install_native`.
    Install,
    /// `pulse uninstall` → `installer::uninstall`.
    Uninstall,
    /// Comando non ancora disponibile.
    ComingSoon,
}

/// Una singola voce di comando del menu.
#[derive(Debug, Clone, Copy)]
pub struct Command {
    /// Etichetta mostrata nella lista, es. `new <name>`.
    pub label: &'static str,
    /// Descrizione di una riga.
    pub description: &'static str,
    /// Pubblico di riferimento (devs / players / everyone …).
    pub audience: &'static str,
    /// Invocazione `pulse …` che verrà realmente eseguita.
    pub invocation: &'static str,
    /// Stato (Real / ComingSoon).
    pub status: Status,
    /// Azione innescata da Enter su un comando `Real`.
    pub action: Action,
}

impl Command {
    /// True se il comando è eseguibile (Enter lo esegue).
    pub fn is_runnable(&self) -> bool {
        matches!(self.status, Status::Real)
    }
}

/// Un gruppo di comandi con un'intestazione (non selezionabile).
#[derive(Debug, Clone, Copy)]
pub struct Group {
    /// Titolo del gruppo mostrato come intestazione dim.
    pub title: &'static str,
    /// Comandi del gruppo.
    pub commands: &'static [Command],
}

/// I 10 comandi in 4 gruppi, nell'ordine di visualizzazione.
pub const GROUPS: &[Group] = &[
    Group {
        title: "Core & Workflow",
        commands: &[
            Command {
                label: "new <name>",
                description: "Scaffold a new mod project (folders + pulse.toml).",
                audience: "devs",
                invocation: "pulse new <name>",
                status: Status::Real,
                action: Action::New,
            },
            Command {
                label: "build",
                description: "Compile the local mod into a ready .pulse.",
                audience: "devs",
                invocation: "pulse build",
                status: Status::Real,
                action: Action::Build,
            },
            Command {
                label: "install",
                description: "Install the built mod into Geometry Dash for testing.",
                audience: "devs & players",
                invocation: "pulse install --gd <path> --artifact <path> [--native]",
                status: Status::Real,
                action: Action::Install,
            },
            Command {
                label: "uninstall",
                description: "Cleanly remove an installed mod.",
                audience: "devs & players",
                invocation: "pulse uninstall --gd <path>",
                status: Status::Real,
                action: Action::Uninstall,
            },
        ],
    },
    Group {
        title: "Debug & Diagnostics",
        commands: &[
            Command {
                label: "doctor",
                description: "Check the dev environment (GD version, C++ toolchain) and flag problems.",
                audience: "devs",
                invocation: "pulse doctor",
                status: Status::ComingSoon,
                action: Action::ComingSoon,
            },
            Command {
                label: "logs",
                description: "Launch GD and stream its console logs live.",
                audience: "devs",
                invocation: "pulse logs",
                status: Status::ComingSoon,
                action: Action::ComingSoon,
            },
        ],
    },
    Group {
        title: "Reverse Engineering (Bindings hunt)",
        commands: &[
            Command {
                label: "siggen <offset>",
                description: "Generate a stable byte signature (AOB) for an offset.",
                audience: "reverse engineers",
                invocation: "pulse siggen <offset>",
                status: Status::ComingSoon,
                action: Action::ComingSoon,
            },
            Command {
                label: "check-offsets",
                description: "Validate the offsets in pulse.toml against the installed GD binary.",
                audience: "reverse engineers",
                invocation: "pulse check-offsets",
                status: Status::ComingSoon,
                action: Action::ComingSoon,
            },
        ],
    },
    Group {
        title: "Community & Publishing",
        commands: &[
            Command {
                label: "submit",
                description: "Interactive wizard to submit a mod to the Index (with support tags).",
                audience: "everyone",
                invocation: "pulse submit",
                status: Status::ComingSoon,
                action: Action::ComingSoon,
            },
            Command {
                label: "upload",
                description: "Upload the mod binaries + metadata to the index.",
                audience: "everyone",
                invocation: "pulse upload",
                status: Status::ComingSoon,
                action: Action::ComingSoon,
            },
        ],
    },
];

/// Una riga renderizzabile nella lista di sinistra.
#[derive(Debug, Clone, Copy)]
pub enum Row {
    /// Intestazione di gruppo (non selezionabile).
    Header(&'static str),
    /// Comando (selezionabile), con indice nella lista piatta dei comandi.
    Item {
        /// Indice progressivo del comando fra tutti i comandi.
        command_index: usize,
        /// Il comando.
        command: Command,
    },
}

/// Appiattisce [`GROUPS`] in righe (intestazioni + comandi) per il rendering,
/// assegnando a ogni comando un indice progressivo stabile.
pub fn build_rows() -> Vec<Row> {
    let mut rows = Vec::new();
    let mut command_index = 0usize;
    for group in GROUPS {
        rows.push(Row::Header(group.title));
        for command in group.commands {
            rows.push(Row::Item {
                command_index,
                command: *command,
            });
            command_index += 1;
        }
    }
    rows
}

/// Lista piatta di tutti i comandi, nell'ordine di visualizzazione.
pub fn all_commands() -> Vec<Command> {
    GROUPS
        .iter()
        .flat_map(|g| g.commands.iter().copied())
        .collect()
}
