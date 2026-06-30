//! Pulse CLI — libreria interna.
//!
//! Espone il modello del `Manifest` (`pulse.toml`) con parser/serializer
//! basati su `serde`/`toml` (Req 16.5: proprietà di round-trip) e la
//! definizione dei sottocomandi `clap` della CLI (Req 14: new/build/publish).
//!
//! La CLI è un tool standalone in Rust, NON linkato nel processo di Geometry
//! Dash (design, Req 26.6). Il modello del manifest rispecchia lo schema
//! definito lato loader in `loader/lifecycle/manifest.hpp`.

pub mod bindings;
pub mod builder;
pub mod cli;
pub mod extract;
pub mod installer;
pub mod manifest;
pub mod native_compiler;
pub mod publisher;
pub mod scaffold;
pub mod surface;
