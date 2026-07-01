//! `pulse doctor` — diagnostica estensibile del `Dev_Environment` (Req 1).
//!
//! Il modulo espone il modello dell'`Environment_Report` e la politica del
//! codice di uscita (`report`), la definizione del trait [`Check`] con il
//! registry ordinato dei controlli built-in e l'orchestratore [`run_all`].
//!
//! ## Estensibilità
//!
//! Un controllo diagnostico è un qualunque tipo che implementa [`Check`]. Il
//! [`default_registry`] restituisce l'elenco **ordinato** dei controlli
//! built-in; nuovi controlli si aggiungono al registry senza toccare
//! l'orchestrazione, così l'esecuzione (`run_all`) resta stabile.
//!
//! ## Un verdetto per ogni controllo, senza interruzioni (Req 1.1, 1.4)
//!
//! [`run_all`] esegue **ogni** controllo del registry nell'ordine dato e
//! produce **esattamente una** voce (`CheckReportItem`) per controllo. Un esito
//! `Problema` **non** interrompe l'esecuzione: i controlli rimanenti vengono
//! comunque eseguiti (nessuna interruzione alla prima anomalia).
//!
//! ## Timeout di 30 s per controllo (Req 1.4)
//!
//! Ogni `Check` incapsula un **timeout di 30 s** ([`CHECK_TIMEOUT`]) sulle
//! proprie operazioni potenzialmente bloccanti (rilevazione di un componente
//! del `Dev_Environment`). L'helper [`run_with_timeout`] fornisce questo
//! confine in modo riusabile: esegue il lavoro del controllo e, se non termina
//! entro il timeout, registra un esito `Problema` con la relativa azione
//! correttiva e lascia proseguire l'orchestrazione.
//!
//! La logica è progettata per essere **pura e host-testabile** dove possibile:
//! `report::exit_code` è una funzione pura del report (Req 1.5, 1.6, 1.7) e
//! `run_all` è deterministico rispetto agli esiti dei controlli.

pub mod checks;
pub mod report;

use std::sync::mpsc;
use std::thread;
use std::time::Duration;

pub use report::{CheckOutcome, CheckReportItem, EnvironmentReport};

/// Timeout standard entro cui ogni controllo del `Dev_Environment` deve
/// concludere la propria rilevazione (Req 1.4). Allo scadere, il controllo
/// registra `Problema` con un'azione correttiva e l'esecuzione prosegue.
pub const CHECK_TIMEOUT: Duration = Duration::from_secs(30);

/// Un controllo diagnostico del `Dev_Environment`.
///
/// Ogni controllo espone un identificatore univoco ([`id`](Check::id)), una
/// descrizione del componente verificato ([`describe`](Check::describe)) e la
/// logica di rilevazione ([`run`](Check::run)) che produce **una** voce
/// dell'`Environment_Report`. L'implementazione di `run` incapsula il proprio
/// **timeout di 30 s** (tipicamente tramite [`run_with_timeout`]): allo scadere
/// restituisce un `CheckReportItem` con esito `Problema` e azione correttiva
/// (Req 1.4).
pub trait Check {
    /// Identificatore univoco e stabile del controllo (es. `gd-version`).
    fn id(&self) -> &str;

    /// Descrizione del componente del `Dev_Environment` verificato.
    fn describe(&self) -> &str;

    /// Esegue il controllo e produce la voce corrispondente
    /// dell'`Environment_Report`. Non deve mai divergere: allo scadere del
    /// timeout deve restituire una voce `Problema` con azione correttiva.
    fn run(&self) -> CheckReportItem;
}

/// Esegue il lavoro di un controllo con un **timeout** esplicito (Req 1.4).
///
/// Il lavoro `work` — potenzialmente bloccante (I/O, avvio di sottoprocessi,
/// ricerca di un componente) — viene eseguito su un thread dedicato. Se
/// completa entro `timeout`, la sua voce è restituita così com'è; altrimenti
/// viene prodotto un `CheckReportItem` con esito `Problema` che riporta la
/// mancata rilevazione entro il tempo massimo e la relativa `corrective_action`
/// (Req 1.4). In caso di timeout il thread di lavoro viene abbandonato
/// (detached) senza bloccare l'orchestrazione.
///
/// I parametri `id`, `description` e `corrective_action` descrivono la voce di
/// fallback usata allo scadere del timeout.
pub fn run_with_timeout<F>(
    id: impl Into<String>,
    description: impl Into<String>,
    corrective_action: impl Into<String>,
    timeout: Duration,
    work: F,
) -> CheckReportItem
where
    F: FnOnce() -> CheckReportItem + Send + 'static,
{
    let (tx, rx) = mpsc::channel();
    // Il thread è deliberatamente detached: se il lavoro non termina entro il
    // timeout non possiamo forzarne l'interruzione, quindi lo abbandoniamo e
    // proseguiamo con l'esito `Problema` (Req 1.4).
    thread::spawn(move || {
        let item = work();
        // L'invio può fallire se il ricevente ha già rinunciato per timeout:
        // in tal caso il risultato tardivo viene semplicemente scartato.
        let _ = tx.send(item);
    });

    match rx.recv_timeout(timeout) {
        Ok(item) => item,
        Err(_) => CheckReportItem::problema(id, description, corrective_action),
    }
}

/// Registry **ordinato** dei controlli diagnostici built-in (Req 1.1).
///
/// L'ordine delle voci determina l'ordine di esecuzione e di presentazione
/// nell'`Environment_Report`. I tre controlli built-in — [`GdVersionCheck`],
/// [`CppToolchainCheck`] e [`ToolchainConfigCheck`] (modulo [`checks`]) — sono
/// restituiti in quest'ordine; ciascuno incapsula il proprio **timeout di 30 s**
/// ([`CHECK_TIMEOUT`]) tramite [`run_with_timeout`] nella propria
/// implementazione di [`Check::run`] (Req 1.4).
///
/// [`GdVersionCheck`]: checks::GdVersionCheck
/// [`CppToolchainCheck`]: checks::CppToolchainCheck
/// [`ToolchainConfigCheck`]: checks::ToolchainConfigCheck
pub fn default_registry() -> Vec<Box<dyn Check>> {
    vec![
        Box::new(checks::GdVersionCheck),
        Box::new(checks::CppToolchainCheck),
        Box::new(checks::ToolchainConfigCheck),
    ]
}

/// Esegue **tutti** i controlli del registry e aggrega l'`Environment_Report`
/// (Req 1.1, 1.4).
///
/// Produce **esattamente una** voce per controllo, nell'ordine del registry.
/// Un esito `Problema` **non** interrompe l'esecuzione: ogni controllo
/// successivo viene comunque eseguito (nessuna interruzione alla prima
/// anomalia).
pub fn run_all(registry: &[Box<dyn Check>]) -> EnvironmentReport {
    let items = registry.iter().map(|check| check.run()).collect();
    EnvironmentReport::new(items)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Controllo di test con esito predeterminato, per verificare
    /// l'orchestrazione senza effetti collaterali.
    struct FakeCheck {
        id: String,
        description: String,
        outcome: CheckOutcome,
    }

    impl FakeCheck {
        fn new(id: &str, outcome: CheckOutcome) -> Self {
            Self {
                id: id.to_string(),
                description: format!("descrizione di {id}"),
                outcome,
            }
        }
    }

    impl Check for FakeCheck {
        fn id(&self) -> &str {
            &self.id
        }

        fn describe(&self) -> &str {
            &self.description
        }

        fn run(&self) -> CheckReportItem {
            match self.outcome {
                CheckOutcome::Ok => CheckReportItem::ok(self.id(), self.describe()),
                CheckOutcome::Warning => {
                    CheckReportItem::warning(self.id(), self.describe(), "azione di avviso")
                }
                CheckOutcome::Problema => {
                    CheckReportItem::problema(self.id(), self.describe(), "azione correttiva")
                }
            }
        }
    }

    #[test]
    fn run_all_una_voce_per_controllo_nello_stesso_ordine() {
        let registry: Vec<Box<dyn Check>> = vec![
            Box::new(FakeCheck::new("uno", CheckOutcome::Ok)),
            Box::new(FakeCheck::new("due", CheckOutcome::Warning)),
            Box::new(FakeCheck::new("tre", CheckOutcome::Problema)),
        ];
        let report = run_all(&registry);
        // Esattamente una voce per controllo (Req 1.1).
        assert_eq!(report.items.len(), registry.len());
        // Stesso ordine del registry.
        let ids: Vec<&str> = report.items.iter().map(|i| i.id.as_str()).collect();
        assert_eq!(ids, vec!["uno", "due", "tre"]);
    }

    #[test]
    fn run_all_prosegue_oltre_un_problema() {
        // Un `Problema` non deve interrompere l'esecuzione dei controlli
        // successivi (Req 1.4): il controllo dopo il problema è comunque
        // presente nel report.
        let registry: Vec<Box<dyn Check>> = vec![
            Box::new(FakeCheck::new("prima", CheckOutcome::Problema)),
            Box::new(FakeCheck::new("dopo", CheckOutcome::Ok)),
        ];
        let report = run_all(&registry);
        assert_eq!(report.items.len(), 2);
        assert_eq!(report.items[0].outcome, CheckOutcome::Problema);
        assert_eq!(report.items[1].id, "dopo");
        assert_eq!(report.items[1].outcome, CheckOutcome::Ok);
    }

    #[test]
    fn run_all_registry_vuoto_produce_report_vuoto() {
        let registry: Vec<Box<dyn Check>> = Vec::new();
        let report = run_all(&registry);
        assert!(report.items.is_empty());
        assert!(report.ready());
    }

    #[test]
    fn default_registry_e_ordinato_e_coerente_con_run_all() {
        // Il registry di default è attualmente vuoto (i controlli built-in
        // arrivano in un'attività successiva); l'orchestrazione deve comunque
        // funzionare e produrre una voce per controllo.
        let registry = default_registry();
        let report = run_all(&registry);
        assert_eq!(report.items.len(), registry.len());
    }

    #[test]
    fn run_with_timeout_restituisce_il_risultato_del_lavoro_veloce() {
        let item = run_with_timeout(
            "veloce",
            "controllo veloce",
            "non dovrebbe servire",
            Duration::from_secs(5),
            || CheckReportItem::ok("veloce", "controllo veloce"),
        );
        assert_eq!(item.id, "veloce");
        assert_eq!(item.outcome, CheckOutcome::Ok);
    }

    #[test]
    fn run_with_timeout_registra_problema_allo_scadere() {
        // Timeout breve con lavoro più lento ⇒ voce `Problema` con azione
        // correttiva (Req 1.4). Non attende i 30 s reali: il timeout è un
        // parametro esplicito, così il comportamento è host-testabile.
        let item = run_with_timeout(
            "lento",
            "controllo lento",
            "riprova o verifica manualmente il componente",
            Duration::from_millis(50),
            || {
                thread::sleep(Duration::from_millis(500));
                CheckReportItem::ok("lento", "controllo lento")
            },
        );
        assert_eq!(item.id, "lento");
        assert_eq!(item.outcome, CheckOutcome::Problema);
        assert!(item.corrective_action.is_some());
    }
}
