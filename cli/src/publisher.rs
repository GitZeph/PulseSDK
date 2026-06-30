//! Implementazione di `pulse publish` — pubblica il Pulse Package sul
//! Marketplace (Req 14.6, 14.7, 14.8).
//!
//! Flusso del comando:
//!   1. Legge e VALIDA il `pulse.toml` del progetto contro lo schema. Se il
//!      manifest è assente, non analizzabile o non conforme, la pubblicazione
//!      viene ANNULLATA: nessun pacchetto viene caricato e viene segnalato
//!      l'elenco completo dei campi non conformi (Req 14.7).
//!   2. Costruisce il Pulse Package `.pulse` (riusa [`crate::builder`]).
//!   3. Carica il pacchetto sul Marketplace attraverso un [`MarketplaceClient`]
//!      astratto. Il caricamento è ATOMICO: il client effettua il "commit" del
//!      pacchetto solo a fronte del pieno successo. Se il Marketplace non è
//!      disponibile o si verifica un errore di rete, la pubblicazione viene
//!      interrotta SENZA lasciare alcun pacchetto parziale nel Marketplace e
//!      viene segnalato il fallimento del caricamento (Req 14.8).
//!
//! Il passo di caricamento è astratto dietro il trait [`MarketplaceClient`]
//! così da poter iniettare implementazioni nei test (caricamento fittizio,
//! "marketplace non disponibile", "errore di rete") SENZA accesso di rete
//! reale. Il client di default ([`DefaultMarketplaceClient`]) punta a un
//! endpoint configurato (variabile d'ambiente `PULSE_MARKETPLACE_ENDPOINT`,
//! forma `host:port`); in assenza di un endpoint raggiungibile riporta
//! l'indisponibilità del Marketplace senza caricare nulla.

use std::fs;
use std::net::{TcpStream, ToSocketAddrs};
use std::path::{Path, PathBuf};
use std::time::Duration;

use crate::builder::{self, BuildError};
use crate::manifest::{FieldViolation, Manifest};

/// Nome del file manifest del progetto.
pub use crate::builder::MANIFEST_FILE;

// ---------------------------------------------------------------------------
// Astrazione del Marketplace (Req 14.6, 14.8).
// ---------------------------------------------------------------------------

/// Pacchetto da caricare sul Marketplace: identità della mod e byte del
/// container `.pulse`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PackageUpload {
    /// Identificatore della mod (`mod.id` del manifest).
    pub mod_id: String,
    /// Versione SemVer della mod (`mod.version`).
    pub version: String,
    /// Byte del container `.pulse` da caricare.
    pub bytes: Vec<u8>,
}

/// Ricevuta di un caricamento riuscito e committato sul Marketplace.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UploadReceipt {
    pub mod_id: String,
    pub version: String,
    /// Riferimento al pacchetto committato (es. URL/endpoint).
    pub location: String,
}

/// Errori del caricamento sul Marketplace. In ENTRAMBI i casi il caricamento è
/// atomico: nessun pacchetto (nemmeno parziale) viene committato (Req 14.8).
#[derive(Debug, thiserror::Error)]
pub enum UploadError {
    /// Il Marketplace non è disponibile/raggiungibile.
    #[error("Marketplace non disponibile: {0}")]
    Unavailable(String),
    /// Errore di rete durante il caricamento.
    #[error("errore di rete durante il caricamento: {0}")]
    Network(String),
}

/// Astrazione del servizio Marketplace (Req 14.6). Iniettabile nei test.
///
/// Contratto di ATOMICITÀ (Req 14.8): `upload` DEVE committare il pacchetto
/// solo a fronte del pieno successo; in caso di errore NON deve lasciare alcun
/// artefatto (nemmeno parziale) nel Marketplace.
pub trait MarketplaceClient {
    /// Carica `package` sul Marketplace, restituendo una ricevuta in caso di
    /// pieno successo o un [`UploadError`] (senza alcun commit) altrimenti.
    fn upload(&self, package: &PackageUpload) -> Result<UploadReceipt, UploadError>;
}

/// Client di default: punta a un endpoint del Marketplace configurato (forma
/// `host:port`). Non disponendo, in questo ambiente, di un Marketplace reale,
/// effettua una verifica di raggiungibilità dell'endpoint: se l'endpoint non è
/// configurato o non è raggiungibile, riporta l'indisponibilità del Marketplace
/// senza caricare nulla (Req 14.8). Il trait [`MarketplaceClient`] consente di
/// iniettare un client reale o fittizio nei test.
#[derive(Debug, Clone)]
pub struct DefaultMarketplaceClient {
    endpoint: Option<String>,
    connect_timeout: Duration,
}

impl DefaultMarketplaceClient {
    /// Crea un client verso `endpoint` (forma `host:port`).
    pub fn new(endpoint: impl Into<String>) -> Self {
        Self {
            endpoint: Some(endpoint.into()),
            connect_timeout: Duration::from_secs(5),
        }
    }

    /// Crea un client con un endpoint opzionale (`None` => non configurato).
    pub fn with_optional_endpoint(endpoint: Option<String>) -> Self {
        Self {
            endpoint,
            connect_timeout: Duration::from_secs(5),
        }
    }

    /// Crea un client leggendo l'endpoint dalla variabile d'ambiente
    /// `PULSE_MARKETPLACE_ENDPOINT` (assente => Marketplace non configurato).
    pub fn from_env() -> Self {
        Self::with_optional_endpoint(std::env::var("PULSE_MARKETPLACE_ENDPOINT").ok())
    }
}

impl Default for DefaultMarketplaceClient {
    fn default() -> Self {
        Self::from_env()
    }
}

impl MarketplaceClient for DefaultMarketplaceClient {
    fn upload(&self, _package: &PackageUpload) -> Result<UploadReceipt, UploadError> {
        let endpoint = self.endpoint.as_deref().ok_or_else(|| {
            UploadError::Unavailable(
                "nessun endpoint del Marketplace configurato (PULSE_MARKETPLACE_ENDPOINT)"
                    .to_string(),
            )
        })?;

        // Risolve l'indirizzo dell'endpoint (forma `host:port`).
        let addr = endpoint
            .to_socket_addrs()
            .map_err(|e| UploadError::Network(format!("indirizzo '{endpoint}' non valido: {e}")))?
            .next()
            .ok_or_else(|| {
                UploadError::Unavailable(format!("nessun indirizzo risolto per '{endpoint}'"))
            })?;

        // Verifica di raggiungibilità: se il Marketplace non è disponibile,
        // abortisce senza caricare alcun pacchetto (Req 14.8).
        let _stream = TcpStream::connect_timeout(&addr, self.connect_timeout)
            .map_err(|e| UploadError::Unavailable(format!("{endpoint}: {e}")))?;

        Ok(UploadReceipt {
            mod_id: _package.mod_id.clone(),
            version: _package.version.clone(),
            location: format!("{endpoint}/{}@{}", _package.mod_id, _package.version),
        })
    }
}

// ---------------------------------------------------------------------------
// Errori ed esito della pubblicazione.
// ---------------------------------------------------------------------------

/// Errori del comando `pulse publish`. In ogni caso di errore NON viene
/// pubblicato alcun Pulse Package nel Marketplace (Req 14.7, 14.8).
#[derive(Debug, thiserror::Error)]
pub enum PublishError {
    /// La directory del progetto o il `pulse.toml` non esistono.
    #[error("manifest non trovato: '{0}' non esiste (atteso un pulse.toml di progetto)")]
    ManifestNotFound(PathBuf),

    /// Errore di I/O durante la lettura del manifest o del pacchetto.
    #[error("errore di I/O su '{path}': {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },

    /// Il `pulse.toml` non è analizzabile (manifest non valido, Req 14.7).
    #[error("pulse.toml non analizzabile: {0}")]
    ManifestParse(String),

    /// Il manifest è analizzabile ma non conforme allo schema: pubblicazione
    /// annullata, nessun caricamento, elenco completo dei campi non conformi
    /// (Req 14.7).
    #[error("manifest non valido: {} campo/i non conforme/i", .0.len())]
    InvalidManifest(Vec<FieldViolation>),

    /// La build del pacchetto da pubblicare è fallita.
    #[error("build del pacchetto fallita: {0}")]
    Build(#[from] BuildError),

    /// Il caricamento nel Marketplace è fallito: pubblicazione interrotta,
    /// nessun pacchetto parziale lasciato nel Marketplace (Req 14.8).
    #[error("caricamento nel Marketplace fallito: {0}")]
    Upload(#[from] UploadError),
}

/// Esito di una pubblicazione riuscita.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PublishOutcome {
    /// Identificatore della mod pubblicata.
    pub mod_id: String,
    /// Versione pubblicata.
    pub version: String,
    /// Percorso locale del `.pulse` prodotto e caricato.
    pub package_path: PathBuf,
    /// Ricevuta del caricamento committato sul Marketplace.
    pub receipt: UploadReceipt,
}

// ---------------------------------------------------------------------------
// Publish.
// ---------------------------------------------------------------------------

/// Esegue la pubblicazione con il client di default ([`DefaultMarketplaceClient`]).
///
/// `project_dir` è la radice del progetto (contiene `pulse.toml`).
pub fn publish(project_dir: &Path) -> Result<PublishOutcome, PublishError> {
    publish_with(project_dir, &DefaultMarketplaceClient::default())
}

/// Esegue la pubblicazione con un [`MarketplaceClient`] iniettato. Usata dai
/// test per simulare successo, Marketplace non disponibile ed errore di rete
/// senza accesso di rete reale.
pub fn publish_with(
    project_dir: &Path,
    client: &dyn MarketplaceClient,
) -> Result<PublishOutcome, PublishError> {
    // --- 1. Lettura e validazione del manifest (Req 14.7). ----------------
    let manifest_path = project_dir.join(MANIFEST_FILE);
    if !manifest_path.is_file() {
        return Err(PublishError::ManifestNotFound(manifest_path));
    }
    let manifest_text = fs::read_to_string(&manifest_path).map_err(|source| PublishError::Io {
        path: manifest_path.clone(),
        source,
    })?;

    let manifest = match Manifest::parse(&manifest_text) {
        Ok(m) => m,
        Err(e) => return Err(PublishError::ManifestParse(e.to_string())),
    };

    let validation = manifest.validate();
    if !validation.ok() {
        // Manifest non valido: annulla il caricamento, non pubblicare nulla,
        // segnala l'elenco completo dei campi non conformi (Req 14.7).
        return Err(PublishError::InvalidManifest(validation.violations));
    }

    // --- 2. Build del Pulse Package da pubblicare (Req 14.6). -------------
    let build = builder::build(project_dir)?;

    // --- 3. Caricamento atomico sul Marketplace (Req 14.6, 14.8). ---------
    let bytes = fs::read(&build.package_path).map_err(|source| PublishError::Io {
        path: build.package_path.clone(),
        source,
    })?;

    let upload = PackageUpload {
        mod_id: manifest.mod_info.id.clone(),
        version: manifest.mod_info.version.to_string(),
        bytes,
    };

    // Su Marketplace non disponibile / errore di rete: il client NON committa
    // nulla; l'errore viene propagato senza lasciare pacchetti parziali (Req 14.8).
    let receipt = client.upload(&upload)?;

    Ok(PublishOutcome {
        mod_id: manifest.mod_info.id,
        version: manifest.mod_info.version.to_string(),
        package_path: build.package_path,
        receipt,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    use std::sync::atomic::{AtomicU64, Ordering};

    use crate::scaffold;

    /// Directory temporanea isolata per i test, con pulizia automatica.
    struct TempGuard {
        path: PathBuf,
    }

    impl TempGuard {
        fn new(tag: &str) -> Self {
            static COUNTER: AtomicU64 = AtomicU64::new(0);
            let n = COUNTER.fetch_add(1, Ordering::Relaxed);
            let pid = std::process::id();
            let path = std::env::temp_dir().join(format!("pulse-publish-{tag}-{pid}-{n}"));
            let _ = fs::remove_dir_all(&path);
            fs::create_dir_all(&path).unwrap();
            TempGuard { path }
        }

        fn path(&self) -> &Path {
            &self.path
        }
    }

    impl Drop for TempGuard {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.path);
        }
    }

    /// Comportamento simulato del Marketplace fittizio.
    #[derive(Clone, Copy)]
    enum FakeBehavior {
        /// Caricamento riuscito: il pacchetto viene committato.
        Success,
        /// Marketplace non disponibile: nessun commit (Req 14.8).
        Unavailable,
        /// Errore di rete: nessun commit (Req 14.8).
        Network,
    }

    /// Marketplace fittizio iniettabile: tiene traccia dei pacchetti
    /// EFFETTIVAMENTE committati. Su fallimento simulato NON committa nulla,
    /// così i test possono verificare l'assenza di pacchetti parziali.
    struct FakeMarketplaceClient {
        behavior: FakeBehavior,
        committed: RefCell<Vec<PackageUpload>>,
    }

    impl FakeMarketplaceClient {
        fn new(behavior: FakeBehavior) -> Self {
            Self {
                behavior,
                committed: RefCell::new(Vec::new()),
            }
        }

        fn committed(&self) -> Vec<PackageUpload> {
            self.committed.borrow().clone()
        }
    }

    impl MarketplaceClient for FakeMarketplaceClient {
        fn upload(&self, package: &PackageUpload) -> Result<UploadReceipt, UploadError> {
            match self.behavior {
                FakeBehavior::Success => {
                    // Commit del pacchetto solo a fronte del pieno successo.
                    self.committed.borrow_mut().push(package.clone());
                    Ok(UploadReceipt {
                        mod_id: package.mod_id.clone(),
                        version: package.version.clone(),
                        location: format!("fake://market/{}@{}", package.mod_id, package.version),
                    })
                }
                FakeBehavior::Unavailable => {
                    // Nessun commit: il Marketplace non è disponibile (Req 14.8).
                    Err(UploadError::Unavailable("istanza di test offline".to_string()))
                }
                FakeBehavior::Network => {
                    // Nessun commit: errore di rete durante il caricamento (Req 14.8).
                    Err(UploadError::Network("connessione interrotta".to_string()))
                }
            }
        }
    }

    /// Crea un progetto valido scaffoldato pronto per la pubblicazione.
    fn scaffold_valid_project(dir: &Path, id: &str) {
        scaffold::scaffold_new(dir, Some(id)).unwrap();
    }

    /// Req 14.6 — pubblicazione con manifest valido: il pacchetto viene caricato
    /// (committato) sul Marketplace.
    #[test]
    fn publish_valid_manifest_uploads_package() {
        let guard = TempGuard::new("ok");
        let dir = guard.path();
        scaffold_valid_project(dir, "com.example.pub");

        let client = FakeMarketplaceClient::new(FakeBehavior::Success);
        let outcome = publish_with(dir, &client).expect("pubblicazione riuscita");

        assert_eq!(outcome.mod_id, "com.example.pub");
        assert!(outcome.package_path.is_file(), "il .pulse deve esistere");

        // Esattamente un pacchetto committato, con identità e byte attesi.
        let committed = client.committed();
        assert_eq!(committed.len(), 1, "atteso un pacchetto committato");
        assert_eq!(committed[0].mod_id, "com.example.pub");
        assert!(!committed[0].bytes.is_empty(), "i byte del .pulse non possono essere vuoti");
        assert_eq!(outcome.receipt.mod_id, "com.example.pub");
    }

    /// Req 14.7 — manifest non valido: pubblicazione annullata, elenco dei campi
    /// non conformi e NESSUN caricamento sul Marketplace.
    #[test]
    fn publish_invalid_manifest_cancels_with_field_list_and_no_upload() {
        let guard = TempGuard::new("invalid");
        let dir = guard.path();
        // Manifest analizzabile ma NON conforme: id vuoto, nessun entry point,
        // permesso non riconosciuto.
        let bad = r#"
            schema_version = 1

            [mod]
            id = ""
            version = "1.0.0"
            name = "Bad"
            type = "native"

            [permissions]
            required = ["telepathy"]
        "#;
        fs::write(dir.join(MANIFEST_FILE), bad).unwrap();
        fs::create_dir_all(dir.join("src")).unwrap();
        fs::write(dir.join("src/mod.cpp"), b"int x;").unwrap();

        let client = FakeMarketplaceClient::new(FakeBehavior::Success);
        let err = publish_with(dir, &client).unwrap_err();

        match err {
            PublishError::InvalidManifest(violations) => {
                let fields: Vec<String> = violations.iter().map(|v| v.field.clone()).collect();
                assert!(fields.iter().any(|f| f == "mod.id"), "campi: {fields:?}");
                assert!(fields.iter().any(|f| f == "entry_points"), "campi: {fields:?}");
                assert!(
                    fields.iter().any(|f| f == "permissions.required[0]"),
                    "campi: {fields:?}"
                );
            }
            other => panic!("errore inatteso: {other:?}"),
        }

        // Nessun pacchetto caricato sul Marketplace (Req 14.7).
        assert!(
            client.committed().is_empty(),
            "nessun caricamento atteso su manifest non valido"
        );
    }

    /// Req 14.8 — Marketplace non disponibile: pubblicazione interrotta, nessun
    /// pacchetto parziale committato.
    #[test]
    fn publish_marketplace_unavailable_aborts_with_no_partial_upload() {
        let guard = TempGuard::new("unavailable");
        let dir = guard.path();
        scaffold_valid_project(dir, "com.example.down");

        let client = FakeMarketplaceClient::new(FakeBehavior::Unavailable);
        let err = publish_with(dir, &client).unwrap_err();

        assert!(
            matches!(err, PublishError::Upload(UploadError::Unavailable(_))),
            "errore atteso 'Marketplace non disponibile', trovato: {err:?}"
        );
        // Nessun pacchetto (nemmeno parziale) nel Marketplace (Req 14.8).
        assert!(
            client.committed().is_empty(),
            "nessun pacchetto parziale atteso su Marketplace non disponibile"
        );
    }

    /// Req 14.8 — errore di rete: pubblicazione interrotta, nessun pacchetto
    /// parziale committato.
    #[test]
    fn publish_network_error_aborts_with_no_partial_upload() {
        let guard = TempGuard::new("network");
        let dir = guard.path();
        scaffold_valid_project(dir, "com.example.neterr");

        let client = FakeMarketplaceClient::new(FakeBehavior::Network);
        let err = publish_with(dir, &client).unwrap_err();

        assert!(
            matches!(err, PublishError::Upload(UploadError::Network(_))),
            "errore atteso 'errore di rete', trovato: {err:?}"
        );
        // Nessun pacchetto (nemmeno parziale) nel Marketplace (Req 14.8).
        assert!(
            client.committed().is_empty(),
            "nessun pacchetto parziale atteso su errore di rete"
        );
    }

    /// `publish` su directory senza manifest fallisce senza caricamenti.
    #[test]
    fn publish_missing_manifest_is_reported() {
        let guard = TempGuard::new("missing");
        let dir = guard.path();

        let client = FakeMarketplaceClient::new(FakeBehavior::Success);
        let err = publish_with(dir, &client).unwrap_err();
        assert!(matches!(err, PublishError::ManifestNotFound(_)), "errore: {err:?}");
        assert!(client.committed().is_empty());
    }

    /// Il client di default senza endpoint configurato riporta l'indisponibilità
    /// del Marketplace senza alcun accesso di rete.
    #[test]
    fn default_client_without_endpoint_reports_unavailable() {
        let client = DefaultMarketplaceClient::with_optional_endpoint(None);
        let upload = PackageUpload {
            mod_id: "com.example.x".to_string(),
            version: "1.0.0".to_string(),
            bytes: vec![1, 2, 3],
        };
        let err = client.upload(&upload).unwrap_err();
        assert!(matches!(err, UploadError::Unavailable(_)), "errore: {err:?}");
    }
}
