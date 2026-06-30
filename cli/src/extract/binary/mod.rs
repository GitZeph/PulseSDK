//! `SourceBinary` / `BinaryIdentity` — loader format-agnostico dei binari
//! first-party di Geometry Dash forniti dall'utente (Req 9.4, 10).
//!
//! Il loader legge **solo** da percorsi locali forniti dall'utente, calcola la
//! [`BinaryIdentity`] come hash crittografico del **solo contenuto** del file e
//! usa la crate [`object`] per rilevare formato e architettura. Fallisce in
//! chiusura su percorso assente/illeggibile/non-locale o su un formato non
//! corrispondente all'atteso, **senza** interrompere gli altri percorsi forniti
//! (il chiamante raccoglie i `Result` per-percorso). **Nessuna** richiesta di
//! rete in uscita.
//!
//! I byte grezzi del [`SourceBinary`] (campo privato `data`) non sono **mai**
//! re-emessi nell'output: l'estrattore emette solo identificatori, firme, valori
//! numerici e l'hash della [`BinaryIdentity`] (Req 10.3).
//!
//! _Requisiti: 9.4, 10.1, 10.2, 10.3, 10.4._

use std::path::Path;

use sha2::{Digest, Sha256};

use object::Object;

use super::{
    is_supported_platform, Architecture, BinaryFormat, ExtractError, GdVersion, TargetPair,
    TargetPlatform,
};

/// Identità di un `Source_Binary`, **senza** riprodurne il contenuto (Req 10.3).
///
/// È il riferimento di tracciabilità inserito nel `Provenance_Record` di ogni
/// offset derivato da quel binario (Req 9.3, 9.4).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BinaryIdentity {
    /// `GD_Version` dalla coppia parametro (Req 9.4).
    pub gd_version: GdVersion,
    /// `Target_Platform` dalla coppia parametro (Req 9.4).
    pub platform: TargetPlatform,
    /// Hash crittografico (SHA-256) del **solo contenuto** del file (Req 9.4, 10.3).
    pub content_hash: [u8; 32],
}

impl BinaryIdentity {
    /// Identità tracciabile in forma testuale `"<platform>:<hash-prefix>"`,
    /// adatta ai messaggi d'errore che devono ricondurre un problema al file
    /// **senza** re-emetterne il contenuto (Req 10.3).
    ///
    /// Il prefisso dell'hash (primi 8 byte in esadecimale) è sufficiente a
    /// distinguere i binari in diagnostica pur restando un identificatore opaco.
    pub fn traceable_id(&self) -> String {
        let mut hex = String::with_capacity(16);
        for byte in &self.content_hash[..8] {
            use std::fmt::Write as _;
            let _ = write!(hex, "{byte:02x}");
        }
        format!("{}:{}", self.platform.platform_id(), hex)
    }
}

/// Un `Source_Binary` aperto: la sua identità, il formato e l'architettura
/// rilevati, e i byte letti dal percorso locale.
///
/// Il campo `data` è **privato** e non viene **mai** re-emesso nell'output
/// (invariante di privacy / no-redistribuzione, Req 10.3). I livelli superiori
/// accedono ai byte solo per derivare identificatori/firme/valori numerici.
#[derive(Debug, Clone)]
pub struct SourceBinary {
    /// Identità tracciabile del binario (Req 9.4).
    pub identity: BinaryIdentity,
    /// Formato eseguibile rilevato (ELF / Mach-O / PE).
    pub format: BinaryFormat,
    /// Architettura CPU rilevata, rilevante per il `Prologue_Sanity_Check` (Req 5).
    pub architecture: Architecture,
    /// Byte letti dal percorso locale; **mai** re-emessi nell'output (Req 10.3).
    data: Vec<u8>,
}

impl SourceBinary {
    /// Accesso in sola lettura ai byte del binario per i livelli di parsing
    /// interni (tabella simboli, RTTI/vtable, prologo).
    ///
    /// È volutamente l'**unico** punto di accesso ai byte grezzi: l'output del
    /// `Binding_Extractor` non contiene mai questi byte (Req 10.3).
    pub fn bytes(&self) -> &[u8] {
        &self.data
    }

    /// Numero di byte letti dal binario (utile a logging/diagnostica senza
    /// esporne il contenuto).
    pub fn byte_len(&self) -> usize {
        self.data.len()
    }
}

/// Apre un `Source_Binary` da un percorso **locale** fornito dall'utente,
/// calcola la [`BinaryIdentity`] (hash del contenuto) e rileva formato e
/// architettura via crate [`object`].
///
/// Fallisce in chiusura, senza interrompere gli altri percorsi forniti, quando:
///
/// - la `Target_Platform` della coppia è fuori dall'insieme supportato
///   ([`ExtractError::UnsupportedPair`], Req 8.3);
/// - il percorso non è locale / non è un file regolare
///   ([`ExtractError::NonLocalPath`], Req 10.1, 10.2, 10.4);
/// - il file è assente o illeggibile ([`ExtractError::Io`], Req 10.4);
/// - il contenuto non è un formato eseguibile riconoscibile
///   ([`ExtractError::UnrecognizedFormat`]);
/// - il formato rilevato non corrisponde all'atteso
///   ([`ExtractError::FormatMismatch`], Req 2.7, 3.6);
/// - l'architettura non è supportata
///   ([`ExtractError::UnsupportedArchitecture`]).
///
/// **Nessuna** richiesta di rete viene effettuata: legge solo il contenuto
/// locale (Req 10.1, 10.2).
pub fn open_source_binary(
    path: &Path,
    pair: TargetPair,
    expected: BinaryFormat,
) -> Result<SourceBinary, ExtractError> {
    // 0. La coppia deve appartenere all'insieme finito e chiuso supportato (Req 8.3).
    if !is_supported_platform(pair.platform) {
        return Err(ExtractError::UnsupportedPair {
            pair: pair.to_string(),
            reason: format!(
                "Target_Platform {} fuori dall'insieme supportato {{macos-arm64, macos-x64, windows-x64, android-arm64}}",
                pair.platform.platform_id()
            ),
        });
    }

    // 1. Solo percorsi locali: rifiuta gli schemi non locali (es. URL di rete)
    //    (Req 10.1, 10.2, 10.4). Nessuna richiesta di rete viene mai effettuata.
    if let Some(text) = path.to_str() {
        if text.contains("://") {
            return Err(ExtractError::NonLocalPath {
                path: path.to_path_buf(),
                reason: "percorso con schema non locale; atteso un file locale".to_owned(),
            });
        }
    }

    // 2. Deve essere un file regolare leggibile localmente (Req 10.4).
    let metadata = std::fs::metadata(path).map_err(|source| ExtractError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    if !metadata.is_file() {
        return Err(ExtractError::NonLocalPath {
            path: path.to_path_buf(),
            reason: "il percorso non è un file regolare locale".to_owned(),
        });
    }

    // 3. Legge SOLO il contenuto locale (Req 10.1): nessun accesso di rete.
    let data = std::fs::read(path).map_err(|source| ExtractError::Io {
        path: path.to_path_buf(),
        source,
    })?;

    // 4. BinaryIdentity = hash crittografico del SOLO contenuto del file (Req 9.4, 10.3).
    let content_hash = sha256_content(&data);

    // 5. Rileva formato e architettura via crate `object`.
    let parsed = object::File::parse(data.as_slice()).map_err(|err| {
        ExtractError::UnrecognizedFormat {
            path: path.to_path_buf(),
            reason: err.to_string(),
        }
    })?;

    let found = map_format(parsed.format()).ok_or_else(|| {
        ExtractError::UnrecognizedFormat {
            path: path.to_path_buf(),
            reason: format!("formato eseguibile non supportato: {:?}", parsed.format()),
        }
    })?;

    // 6. Il formato rilevato deve corrispondere all'atteso (Req 2.7, 3.6).
    if found != expected {
        return Err(ExtractError::FormatMismatch {
            path: path.to_path_buf(),
            expected,
            found,
        });
    }

    // 7. L'architettura deve essere fra quelle supportate (arm64 / x86-64).
    let architecture =
        map_architecture(parsed.architecture()).ok_or_else(|| {
            ExtractError::UnsupportedArchitecture {
                path: path.to_path_buf(),
                reason: format!(
                    "architettura non supportata: {:?}",
                    parsed.architecture()
                ),
            }
        })?;

    Ok(SourceBinary {
        identity: BinaryIdentity {
            gd_version: pair.gd,
            platform: pair.platform,
            content_hash,
        },
        format: found,
        architecture,
        data,
    })
}

/// Calcola l'hash crittografico (SHA-256) del **solo contenuto** del file
/// (Req 9.4): contenuti uguali ⇒ hash uguale, contenuti diversi ⇒ hash diverso.
fn sha256_content(data: &[u8]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(data);
    hasher.finalize().into()
}

/// Mappa il formato rilevato dalla crate `object` sul [`BinaryFormat`]
/// dell'estrattore. Restituisce `None` per i formati fuori scope.
fn map_format(format: object::BinaryFormat) -> Option<BinaryFormat> {
    match format {
        object::BinaryFormat::Elf => Some(BinaryFormat::Elf),
        object::BinaryFormat::MachO => Some(BinaryFormat::MachO),
        object::BinaryFormat::Pe => Some(BinaryFormat::Pe),
        _ => None,
    }
}

/// Mappa l'architettura rilevata dalla crate `object` sull'[`Architecture`]
/// dell'estrattore (arm64 / x86-64). Restituisce `None` per le architetture
/// fuori scope.
fn map_architecture(arch: object::Architecture) -> Option<Architecture> {
    match arch {
        object::Architecture::Aarch64 => Some(Architecture::Arm64),
        object::Architecture::X86_64 => Some(Architecture::X86_64),
        _ => None,
    }
}
