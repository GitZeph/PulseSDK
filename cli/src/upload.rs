//! `pulse upload` — contratto lato client verso l'`Index` (Req 6).
//!
//! Questo modulo definisce il **contratto client** dell'upload verso l'`Index`,
//! senza presupporre l'esistenza di un backend server. Le responsabilità sono:
//!
//! - il modello [`UploadArtifact`] (nome + byte) degli artefatti da caricare
//!   (binari + metadati);
//! - il seam di test [`IndexClient`]: l'HTTP reale vive **solo** dietro questo
//!   trait (in `DefaultIndexClient`, task 14.2), così che dry-run e tutti i rami
//!   d'errore siano host-testabili **senza rete** iniettando un client fake;
//! - la risoluzione dell'`Index_Endpoint` [`resolve_endpoint`] con precedenza
//!   env → file di config; assente in entrambi ⇒ `None` ⇒ dry-run (Req 6.3);
//! - l'orchestrazione [`run`]:
//!   - manca il `Submission_Descriptor` o un `Upload_Artifact` ⇒ errore,
//!     **nessuna richiesta di rete** (Req 6.6);
//!   - `Index_Endpoint` **non configurato** ⇒ **dry-run** che elenca **tutti**
//!     gli `Upload_Artifact` (binari + metadati), **zero rete**, successo
//!     (Req 6.3);
//!   - endpoint configurato ⇒ `reachable` (10 s) poi `upload` (300 s
//!     complessivi); successo ⇒ stampa l'**id univoco** della risorsa creata
//!     (Req 6.1, 6.2);
//!   - non raggiungibile / risposta d'errore / fallimento dopo l'inizio ⇒
//!     errore con causa; il client non riporta successo su upload incompleto
//!     (Req 6.4, 6.5).
//!
//! `DefaultIndexClient` (implementazione reale via `ureq`) è nel task 14.2; qui
//! il flusso è completamente esercitabile con un [`IndexClient`] fake.

use std::fs;
use std::path::{Path, PathBuf};
use std::time::Duration;

use crate::submit::SubmissionDescriptor;

/// Nome canonico del `Submission_Descriptor` nella radice del progetto.
pub const SUBMISSION_FILE: &str = "submission.toml";

/// Variabile d'ambiente che, se presente e non vuota, ha la precedenza nella
/// risoluzione dell'`Index_Endpoint` (Req 6.3).
pub const ENDPOINT_ENV: &str = "PULSE_INDEX_ENDPOINT";

/// Percorso del file di config della CLI, relativo alla home dell'utente,
/// consultato quando la variabile d'ambiente non è impostata.
const CONFIG_REL_PATH: &str = ".config/pulse/config.toml";

/// Chiave del file di config che contiene l'`Index_Endpoint`.
const CONFIG_ENDPOINT_KEY: &str = "index_endpoint";

// ---------------------------------------------------------------------------
// Modello degli artefatti e seam di rete.
// ---------------------------------------------------------------------------

/// Un `Upload_Artifact`: nome logico + byte del contenuto. L'insieme completo
/// comprende sia i binari referenziati dal `Submission_Descriptor` sia i
/// metadati (il `submission.toml` stesso).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UploadArtifact {
    /// Nome logico dell'artefatto (percorso relativo per i binari,
    /// [`SUBMISSION_FILE`] per i metadati).
    pub name: String,
    /// Byte del contenuto dell'artefatto.
    pub bytes: Vec<u8>,
}

/// Seam di test dell'upload: l'HTTP reale vive **solo** dietro questo trait, i
/// test iniettano un client fake senza toccare la rete.
///
/// Contratto: `upload` riporta successo **solo** a fronte del pieno
/// caricamento di **tutti** gli artefatti; su fallimento (anche dopo l'inizio
/// del trasferimento) restituisce `Err` e non lascia artefatti parziali
/// sull'`Index` (Req 6.5).
pub trait IndexClient {
    /// Verifica di raggiungibilità dell'`Index_Endpoint` entro 10 s
    /// (Req 6.1, 6.4). `Ok(())` se raggiungibile, `Err` altrimenti.
    fn reachable(&self, endpoint: &str) -> anyhow::Result<()>;

    /// Carica **tutti** gli `artifacts` verso `endpoint` entro un timeout
    /// complessivo di 300 s; in caso di pieno successo restituisce
    /// l'identificatore univoco della risorsa creata sull'`Index`
    /// (Req 6.1, 6.2).
    fn upload(&self, endpoint: &str, artifacts: &[UploadArtifact]) -> anyhow::Result<String>;
}

// ---------------------------------------------------------------------------
// Errori del comando.
// ---------------------------------------------------------------------------

/// Errori del comando `pulse upload`. In ogni caso di errore non viene lasciato
/// alcun `Upload_Artifact` parziale sull'`Index` (Req 6.4, 6.5, 6.6).
#[derive(Debug, thiserror::Error)]
pub enum UploadError {
    /// Il `Submission_Descriptor` (`submission.toml`) non esiste: nessuna
    /// richiesta di rete (Req 6.6).
    #[error("Submission_Descriptor non trovato: '{0}' non esiste (esegui prima `pulse submit`)")]
    DescriptorNotFound(PathBuf),

    /// Il `submission.toml` non è analizzabile.
    #[error("submission.toml non analizzabile: {0}")]
    DescriptorParse(String),

    /// Uno degli `Upload_Artifact` referenziati non è disponibile: nessuna
    /// richiesta di rete (Req 6.6).
    #[error("Upload_Artifact non disponibile: '{0}' non esiste")]
    ArtifactNotFound(PathBuf),

    /// Errore di I/O durante la lettura del descrittore o di un artefatto.
    #[error("errore di I/O su '{path}': {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },

    /// L'`Index_Endpoint` è configurato ma non raggiungibile entro 10 s o
    /// restituisce una risposta d'errore (Req 6.4).
    #[error("Index_Endpoint non raggiungibile: {0}")]
    Unreachable(String),

    /// Il caricamento è fallito o è stato interrotto dopo l'inizio del
    /// trasferimento: nessun artefatto parziale sull'`Index` (Req 6.5).
    #[error("upload verso l'Index fallito: {0}")]
    Failed(String),
}

// ---------------------------------------------------------------------------
// Risoluzione dell'Index_Endpoint (Req 6.3).
// ---------------------------------------------------------------------------

/// Risolve l'`Index_Endpoint` configurato, in ordine di precedenza:
///
///   1. variabile d'ambiente [`ENDPOINT_ENV`] (`PULSE_INDEX_ENDPOINT`);
///   2. chiave `index_endpoint` in `~/.config/pulse/config.toml`.
///
/// Assente in entrambi (o vuoto) ⇒ `None` ⇒ il comando degrada a dry-run senza
/// alcuna richiesta di rete (Req 6.3). Nessun endpoint di default è cablato.
pub fn resolve_endpoint() -> Option<String> {
    // 1. Variabile d'ambiente (precedenza massima).
    if let Ok(value) = std::env::var(ENDPOINT_ENV) {
        let trimmed = value.trim();
        if !trimmed.is_empty() {
            return Some(trimmed.to_string());
        }
    }

    // 2. File di config della CLI sotto la home dell'utente.
    let home = std::env::var_os("HOME")?;
    let config_path = Path::new(&home).join(CONFIG_REL_PATH);
    let text = fs::read_to_string(&config_path).ok()?;
    let value: toml::Value = toml::from_str(&text).ok()?;
    let endpoint = value.get(CONFIG_ENDPOINT_KEY)?.as_str()?.trim();
    if endpoint.is_empty() {
        None
    } else {
        Some(endpoint.to_string())
    }
}

// ---------------------------------------------------------------------------
// Orchestrazione dell'upload.
// ---------------------------------------------------------------------------

/// Esegue `pulse upload` risolvendo l'`Index_Endpoint` dall'ambiente/config
/// ([`resolve_endpoint`]).
///
/// `project_dir` è la radice del progetto (contiene `submission.toml`). Il seam
/// [`IndexClient`] isola la rete: il flusso completo è host-testabile con un
/// client fake iniettato.
pub fn run(project_dir: &Path, client: &dyn IndexClient) -> anyhow::Result<()> {
    run_with_endpoint(project_dir, client, resolve_endpoint())
}

/// Come [`run`], ma con l'`Index_Endpoint` iniettato esplicitamente. Usata dai
/// test per esercitare dry-run e rami d'errore senza dipendere da ambiente o
/// filesystem di config.
pub(crate) fn run_with_endpoint(
    project_dir: &Path,
    client: &dyn IndexClient,
    endpoint: Option<String>,
) -> anyhow::Result<()> {
    // --- 1. Carica il Submission_Descriptor (assente ⇒ errore, no rete). ---
    let descriptor_path = project_dir.join(SUBMISSION_FILE);
    if !descriptor_path.is_file() {
        return Err(UploadError::DescriptorNotFound(descriptor_path).into());
    }
    let descriptor_text = fs::read_to_string(&descriptor_path).map_err(|source| UploadError::Io {
        path: descriptor_path.clone(),
        source,
    })?;
    let descriptor = SubmissionDescriptor::parse(&descriptor_text)
        .map_err(|e| UploadError::DescriptorParse(e.to_string()))?;

    // --- 2. Raccogli tutti gli Upload_Artifact (binari + metadati). --------
    // Un artefatto mancante ⇒ errore PRIMA di qualunque richiesta di rete
    // (Req 6.6).
    let artifacts = collect_artifacts(project_dir, &descriptor, &descriptor_text)?;

    // --- 3. Endpoint non configurato ⇒ dry-run a zero rete (Req 6.3). ------
    let Some(endpoint) = endpoint else {
        println!(
            "Index_Endpoint non configurato: dry-run (nessuna richiesta di rete).\n\
             Verrebbero caricati {} Upload_Artifact:",
            artifacts.len()
        );
        for artifact in &artifacts {
            println!("  - {} ({} byte)", artifact.name, artifact.bytes.len());
        }
        return Ok(());
    };

    // --- 4. Endpoint configurato: reachable (10 s) poi upload (300 s). -----
    client
        .reachable(&endpoint)
        .map_err(|e| UploadError::Unreachable(e.to_string()))?;

    let resource_id = client
        .upload(&endpoint, &artifacts)
        .map_err(|e| UploadError::Failed(e.to_string()))?;

    // Successo: riporta l'id univoco della risorsa creata (Req 6.2).
    println!("upload completato: risorsa creata sull'Index con id {resource_id}");
    Ok(())
}

/// Raccoglie l'insieme completo degli `Upload_Artifact`: i binari referenziati
/// dal `Submission_Descriptor` (nell'ordine di dichiarazione) più il metadato
/// `submission.toml`. Un binario referenziato ma assente ⇒ errore, così la
/// verifica di disponibilità avviene **prima** di qualunque richiesta di rete
/// (Req 6.6).
fn collect_artifacts(
    project_dir: &Path,
    descriptor: &SubmissionDescriptor,
    descriptor_text: &str,
) -> Result<Vec<UploadArtifact>, UploadError> {
    let mut artifacts = Vec::with_capacity(descriptor.artifacts.len() + 1);

    // Binari referenziati (nell'ordine di dichiarazione).
    for artifact_ref in &descriptor.artifacts {
        let artifact_path = project_dir.join(&artifact_ref.path);
        if !artifact_path.is_file() {
            return Err(UploadError::ArtifactNotFound(artifact_path));
        }
        let bytes = fs::read(&artifact_path).map_err(|source| UploadError::Io {
            path: artifact_path.clone(),
            source,
        })?;
        artifacts.push(UploadArtifact {
            name: artifact_ref.path.clone(),
            bytes,
        });
    }

    // Metadato: il Submission_Descriptor stesso.
    artifacts.push(UploadArtifact {
        name: SUBMISSION_FILE.to_string(),
        bytes: descriptor_text.as_bytes().to_vec(),
    });

    Ok(artifacts)
}

// ---------------------------------------------------------------------------
// Implementazione reale via `ureq` (confinata SOLO qui).
// ---------------------------------------------------------------------------

/// Timeout della verifica di raggiungibilità dell'`Index_Endpoint`: 10 s
/// (Req 6.1, 6.4).
const REACHABLE_TIMEOUT: Duration = Duration::from_secs(10);

/// Timeout complessivo dell'upload di **tutti** gli `Upload_Artifact`: 300 s
/// (Req 6.1).
const UPLOAD_TIMEOUT: Duration = Duration::from_secs(300);

/// Boundary del corpo `multipart/form-data` usato per il POST degli artefatti.
const MULTIPART_BOUNDARY: &str = "----pulse-upload-boundary-7Nq3Xr2Zk";

/// Implementazione reale di [`IndexClient`] basata su **`ureq`** (client HTTP
/// sincrono). Questo è l'**unico** punto del progetto in cui vive l'HTTP reale:
/// `ureq` è confinato dietro il trait [`IndexClient`], così che dry-run e tutti
/// i rami d'errore restino host-testabili senza rete tramite un client fake.
///
/// Nessun `Index_Endpoint` di default è cablato qui: l'endpoint arriva sempre
/// dalla risoluzione env/config ([`resolve_endpoint`]). La verifica end-to-end
/// contro un vero server è nel task manuale finale.
#[derive(Debug, Default, Clone, Copy)]
pub struct DefaultIndexClient;

impl DefaultIndexClient {
    /// Crea un nuovo client HTTP reale verso l'`Index`.
    pub fn new() -> Self {
        DefaultIndexClient
    }
}

impl IndexClient for DefaultIndexClient {
    /// Verifica leggera di raggiungibilità: una `GET` sull'`Index_Endpoint` con
    /// timeout **complessivo** di 10 s (connessione + risposta). Se il server
    /// risponde in qualunque modo — anche con uno stato d'errore HTTP — è
    /// considerato raggiungibile; l'errore applicativo verrà eventualmente
    /// rilevato durante l'`upload`. Un fallimento di trasporto (connessione
    /// rifiutata, DNS, timeout) ⇒ `Err`, endpoint non raggiungibile
    /// (Req 6.1, 6.4).
    fn reachable(&self, endpoint: &str) -> anyhow::Result<()> {
        let agent = ureq::AgentBuilder::new()
            .timeout(REACHABLE_TIMEOUT)
            .build();

        match agent.get(endpoint).call() {
            // Il server ha risposto: raggiungibile a livello di rete.
            Ok(_) => Ok(()),
            // Ha risposto con uno stato HTTP d'errore: comunque raggiungibile.
            Err(ureq::Error::Status(_, _)) => Ok(()),
            // Fallimento di trasporto: connessione/timeout ⇒ non raggiungibile.
            Err(ureq::Error::Transport(transport)) => {
                Err(anyhow::anyhow!("{endpoint}: {transport}"))
            }
        }
    }

    /// Carica **tutti** gli `artifacts` con una singola `POST`
    /// `multipart/form-data` verso l'`Index_Endpoint`, entro un timeout
    /// **complessivo** di 300 s. In caso di pieno successo (stato 2xx) estrae e
    /// restituisce l'identificatore univoco della risorsa creata dal corpo
    /// della risposta; qualunque stato d'errore o fallimento di trasporto ⇒
    /// `Err` (nessun successo su upload incompleto) (Req 6.1, 6.2).
    fn upload(&self, endpoint: &str, artifacts: &[UploadArtifact]) -> anyhow::Result<String> {
        let body = build_multipart_body(artifacts);
        let content_type = format!("multipart/form-data; boundary={MULTIPART_BOUNDARY}");

        let agent = ureq::AgentBuilder::new()
            .timeout(UPLOAD_TIMEOUT)
            .build();

        let response = match agent
            .post(endpoint)
            .set("Content-Type", &content_type)
            .send_bytes(&body)
        {
            Ok(response) => response,
            Err(ureq::Error::Status(code, response)) => {
                let detail = response
                    .into_string()
                    .unwrap_or_else(|_| "<corpo della risposta illeggibile>".to_string());
                return Err(anyhow::anyhow!(
                    "l'Index ha risposto con stato {code}: {}",
                    detail.trim()
                ));
            }
            Err(ureq::Error::Transport(transport)) => {
                return Err(anyhow::anyhow!("trasferimento fallito: {transport}"));
            }
        };

        let body = response
            .into_string()
            .map_err(|e| anyhow::anyhow!("corpo della risposta dell'Index illeggibile: {e}"))?;

        extract_resource_id(&body)
    }
}

/// Costruisce il corpo `multipart/form-data` che impacchetta **tutti** gli
/// `Upload_Artifact` (binari + metadati) in un'unica richiesta. Ogni artefatto
/// diventa una parte `file` con il proprio nome logico.
fn build_multipart_body(artifacts: &[UploadArtifact]) -> Vec<u8> {
    let mut body = Vec::new();
    for artifact in artifacts {
        body.extend_from_slice(format!("--{MULTIPART_BOUNDARY}\r\n").as_bytes());
        body.extend_from_slice(
            format!(
                "Content-Disposition: form-data; name=\"artifacts\"; filename=\"{}\"\r\n",
                artifact.name
            )
            .as_bytes(),
        );
        body.extend_from_slice(b"Content-Type: application/octet-stream\r\n\r\n");
        body.extend_from_slice(&artifact.bytes);
        body.extend_from_slice(b"\r\n");
    }
    body.extend_from_slice(format!("--{MULTIPART_BOUNDARY}--\r\n").as_bytes());
    body
}

/// Estrae l'identificatore univoco della risorsa creata dal corpo della
/// risposta dell'`Index`. Prova prima a interpretarlo come JSON con un campo
/// `id` (stringa o numero); in mancanza ricade sul corpo grezzo ripulito. Un
/// corpo vuoto o privo di identificatore ⇒ `Err` (il client non riporta
/// successo senza un id di risorsa) (Req 6.2).
fn extract_resource_id(body: &str) -> anyhow::Result<String> {
    if let Ok(value) = serde_json::from_str::<serde_json::Value>(body) {
        if let Some(id) = value.get("id") {
            if let Some(s) = id.as_str() {
                if !s.trim().is_empty() {
                    return Ok(s.trim().to_string());
                }
            } else if id.is_number() {
                return Ok(id.to_string());
            }
        }
    }

    let trimmed = body.trim();
    if trimmed.is_empty() {
        return Err(anyhow::anyhow!(
            "risposta dell'Index priva dell'identificatore della risorsa creata"
        ));
    }
    Ok(trimmed.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    use std::sync::atomic::{AtomicU64, Ordering};

    use crate::submit::{ArtifactRef, SubmissionDescriptor};

    /// Directory temporanea isolata per i test, con pulizia automatica.
    struct TempGuard {
        path: PathBuf,
    }

    impl TempGuard {
        fn new(tag: &str) -> Self {
            static COUNTER: AtomicU64 = AtomicU64::new(0);
            let n = COUNTER.fetch_add(1, Ordering::Relaxed);
            let pid = std::process::id();
            let path = std::env::temp_dir().join(format!("pulse-upload-{tag}-{pid}-{n}"));
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

    /// Comportamento simulato del client Index fittizio.
    #[derive(Clone, Copy)]
    enum FakeBehavior {
        /// reachable Ok, upload Ok con id di risorsa.
        Success,
        /// reachable Err (endpoint non raggiungibile, Req 6.4).
        Unreachable,
        /// reachable Ok ma upload Err (fallimento dopo l'inizio, Req 6.5).
        UploadFails,
    }

    /// `IndexClient` fittizio iniettabile che registra le invocazioni di rete,
    /// così i test possono verificare l'assenza di richieste (dry-run / errori
    /// pre-rete) e gli artefatti effettivamente caricati.
    struct FakeIndexClient {
        behavior: FakeBehavior,
        reachable_calls: RefCell<u32>,
        uploaded: RefCell<Vec<UploadArtifact>>,
    }

    impl FakeIndexClient {
        fn new(behavior: FakeBehavior) -> Self {
            Self {
                behavior,
                reachable_calls: RefCell::new(0),
                uploaded: RefCell::new(Vec::new()),
            }
        }
    }

    impl IndexClient for FakeIndexClient {
        fn reachable(&self, _endpoint: &str) -> anyhow::Result<()> {
            *self.reachable_calls.borrow_mut() += 1;
            match self.behavior {
                FakeBehavior::Unreachable => anyhow::bail!("connessione rifiutata (simulata)"),
                FakeBehavior::Success | FakeBehavior::UploadFails => Ok(()),
            }
        }

        fn upload(&self, _endpoint: &str, artifacts: &[UploadArtifact]) -> anyhow::Result<String> {
            match self.behavior {
                FakeBehavior::UploadFails => {
                    anyhow::bail!("interruzione del trasferimento (simulata)")
                }
                FakeBehavior::Success => {
                    *self.uploaded.borrow_mut() = artifacts.to_vec();
                    Ok("res-abc123".to_string())
                }
                FakeBehavior::Unreachable => {
                    unreachable!("upload non deve essere invocato se reachable fallisce")
                }
            }
        }
    }

    /// Scrive un `submission.toml` valido con `artifact_paths` file binari
    /// (creati con contenuto noto) nella radice del progetto.
    fn write_project(dir: &Path, artifact_paths: &[&str]) {
        for p in artifact_paths {
            fs::write(dir.join(p), format!("bytes-of-{p}")).unwrap();
        }
        let descriptor = SubmissionDescriptor {
            schema_version: 1,
            mod_id: "com.example.mymod".to_string(),
            name: "My Mod".to_string(),
            version: "1.0.0".to_string(),
            summary: "Una mod di esempio.".to_string(),
            support_tags: vec!["✅ Stable".to_string()],
            artifacts: artifact_paths
                .iter()
                .map(|p| ArtifactRef {
                    path: (*p).to_string(),
                })
                .collect(),
        };
        fs::write(dir.join(SUBMISSION_FILE), descriptor.serialize().unwrap()).unwrap();
    }

    /// Req 6.3 — endpoint non configurato ⇒ dry-run a zero rete, successo.
    #[test]
    fn dry_run_without_endpoint_makes_no_network_calls() {
        let guard = TempGuard::new("dryrun");
        write_project(guard.path(), &["mymod.pulse"]);
        let client = FakeIndexClient::new(FakeBehavior::Success);

        let result = run_with_endpoint(guard.path(), &client, None);

        assert!(result.is_ok(), "dry-run deve avere successo: {result:?}");
        assert_eq!(*client.reachable_calls.borrow(), 0, "nessuna verifica di rete");
        assert!(client.uploaded.borrow().is_empty(), "nessun upload");
    }

    /// Req 6.6 — Submission_Descriptor assente ⇒ errore, nessuna richiesta di rete.
    #[test]
    fn missing_descriptor_errors_without_network() {
        let guard = TempGuard::new("nodescriptor");
        // Nessun submission.toml scritto.
        let client = FakeIndexClient::new(FakeBehavior::Success);

        let result =
            run_with_endpoint(guard.path(), &client, Some("https://index.example".to_string()));

        assert!(result.is_err(), "descrittore assente deve fallire");
        assert_eq!(*client.reachable_calls.borrow(), 0, "nessuna richiesta di rete");
        assert!(client.uploaded.borrow().is_empty());
    }

    /// Req 6.6 — un Upload_Artifact referenziato ma assente ⇒ errore, no rete,
    /// anche con endpoint configurato.
    #[test]
    fn missing_artifact_errors_without_network() {
        let guard = TempGuard::new("noartifact");
        // Descrittore che referenzia un binario mai creato sul filesystem.
        let descriptor = SubmissionDescriptor {
            schema_version: 1,
            mod_id: "com.example.mymod".to_string(),
            name: "My Mod".to_string(),
            version: "1.0.0".to_string(),
            summary: "x".to_string(),
            support_tags: Vec::new(),
            artifacts: vec![ArtifactRef {
                path: "missing.pulse".to_string(),
            }],
        };
        fs::write(
            guard.path().join(SUBMISSION_FILE),
            descriptor.serialize().unwrap(),
        )
        .unwrap();
        let client = FakeIndexClient::new(FakeBehavior::Success);

        let result =
            run_with_endpoint(guard.path(), &client, Some("https://index.example".to_string()));

        assert!(result.is_err(), "artefatto mancante deve fallire");
        assert_eq!(*client.reachable_calls.borrow(), 0, "nessuna richiesta di rete");
        assert!(client.uploaded.borrow().is_empty());
    }

    /// Req 6.1, 6.2 — endpoint configurato e raggiungibile ⇒ carica l'insieme
    /// completo (binari + metadati) e riporta l'id di risorsa.
    #[test]
    fn configured_endpoint_uploads_full_set_and_reports_id() {
        let guard = TempGuard::new("success");
        write_project(guard.path(), &["mymod.pulse", "extra.bin"]);
        let client = FakeIndexClient::new(FakeBehavior::Success);

        let result =
            run_with_endpoint(guard.path(), &client, Some("https://index.example".to_string()));

        assert!(result.is_ok(), "upload deve avere successo: {result:?}");
        assert_eq!(*client.reachable_calls.borrow(), 1, "reachable invocato una volta");

        // Insieme completo: 2 binari + 1 metadato (submission.toml).
        let uploaded = client.uploaded.borrow();
        let names: Vec<&str> = uploaded.iter().map(|a| a.name.as_str()).collect();
        assert_eq!(uploaded.len(), 3, "artefatti caricati: {names:?}");
        assert!(names.contains(&"mymod.pulse"));
        assert!(names.contains(&"extra.bin"));
        assert!(names.contains(&SUBMISSION_FILE));
    }

    /// Req 6.4 — endpoint non raggiungibile ⇒ errore, upload mai invocato.
    #[test]
    fn unreachable_endpoint_errors_and_skips_upload() {
        let guard = TempGuard::new("unreachable");
        write_project(guard.path(), &["mymod.pulse"]);
        let client = FakeIndexClient::new(FakeBehavior::Unreachable);

        let result =
            run_with_endpoint(guard.path(), &client, Some("https://index.example".to_string()));

        assert!(result.is_err(), "endpoint non raggiungibile deve fallire");
        assert_eq!(*client.reachable_calls.borrow(), 1, "reachable tentato");
        assert!(client.uploaded.borrow().is_empty(), "upload non invocato");
    }

    /// Req 6.5 — fallimento dell'upload dopo l'inizio ⇒ errore; il client non
    /// riporta successo su upload incompleto.
    #[test]
    fn upload_failure_errors() {
        let guard = TempGuard::new("uploadfail");
        write_project(guard.path(), &["mymod.pulse"]);
        let client = FakeIndexClient::new(FakeBehavior::UploadFails);

        let result =
            run_with_endpoint(guard.path(), &client, Some("https://index.example".to_string()));

        assert!(result.is_err(), "fallimento dell'upload deve propagare l'errore");
        assert_eq!(*client.reachable_calls.borrow(), 1);
    }

    /// Req 6.1, 6.3 — l'insieme raccolto in dry-run coincide con quello caricato
    /// quando l'endpoint è configurato (stessa raccolta, binari + metadati).
    #[test]
    fn collect_artifacts_covers_binaries_plus_metadata() {
        let guard = TempGuard::new("collect");
        write_project(guard.path(), &["a.pulse", "b.pulse"]);
        let text = fs::read_to_string(guard.path().join(SUBMISSION_FILE)).unwrap();
        let descriptor = SubmissionDescriptor::parse(&text).unwrap();

        let artifacts = collect_artifacts(guard.path(), &descriptor, &text).unwrap();

        let names: Vec<&str> = artifacts.iter().map(|a| a.name.as_str()).collect();
        assert_eq!(names, vec!["a.pulse", "b.pulse", SUBMISSION_FILE]);
        // I byte dei binari corrispondono al contenuto scritto.
        assert_eq!(artifacts[0].bytes, b"bytes-of-a.pulse");
        assert_eq!(artifacts[2].bytes, text.as_bytes());
    }

    // --- Helper puri di DefaultIndexClient (host-testabili, zero rete). -----

    /// Req 6.1 — il corpo multipart impacchetta ogni artefatto come parte `file`
    /// con il proprio nome logico e i byte esatti, chiuso dal boundary finale.
    #[test]
    fn multipart_body_packs_every_artifact() {
        let artifacts = vec![
            UploadArtifact {
                name: "mymod.pulse".to_string(),
                bytes: b"BINARY".to_vec(),
            },
            UploadArtifact {
                name: SUBMISSION_FILE.to_string(),
                bytes: b"meta".to_vec(),
            },
        ];

        let body = build_multipart_body(&artifacts);
        let rendered = String::from_utf8_lossy(&body);

        // Ogni artefatto ha una parte con il proprio filename e i propri byte.
        assert!(rendered.contains("filename=\"mymod.pulse\""));
        assert!(rendered.contains("BINARY"));
        assert!(rendered.contains(&format!("filename=\"{SUBMISSION_FILE}\"")));
        assert!(rendered.contains("meta"));
        // Chiusura con il boundary finale.
        assert!(rendered.ends_with(&format!("--{MULTIPART_BOUNDARY}--\r\n")));
    }

    /// Req 6.2 — l'id di risorsa è estratto dal campo `id` di un corpo JSON,
    /// sia come stringa sia come numero.
    #[test]
    fn extract_resource_id_reads_json_id() {
        assert_eq!(
            extract_resource_id(r#"{"id":"res-42","status":"ok"}"#).unwrap(),
            "res-42"
        );
        assert_eq!(extract_resource_id(r#"{"id":1234}"#).unwrap(), "1234");
    }

    /// Req 6.2 — corpo non-JSON ⇒ id = corpo grezzo ripulito; corpo vuoto ⇒
    /// errore (nessun successo senza id di risorsa).
    #[test]
    fn extract_resource_id_falls_back_and_rejects_empty() {
        assert_eq!(extract_resource_id("  res-plain\n").unwrap(), "res-plain");
        assert!(extract_resource_id("   \n").is_err());
    }
}
