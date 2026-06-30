//! Binario `pulse` — entry point della Pulse CLI (Req 14).
//!
//! La CLI è un tool standalone in Rust, NON linkato nel processo di Geometry
//! Dash (design, Req 26.6). Parsea gli argomenti con `clap` e delega
//! l'esecuzione ai sottocomandi definiti in `cli`.

use clap::Parser;

use pulse_cli::cli::Cli;

fn main() -> anyhow::Result<()> {
    let cli = Cli::parse();
    cli.run()
}
