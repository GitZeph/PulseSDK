//! `Environment_Report` di `pulse doctor` — esiti, voci, rendering testuale e
//! politica del codice di uscita (Req 1.2, 1.5, 1.6, 1.7).
//!
//! Un `Environment_Report` è l'aggregato dei controlli eseguiti da `pulse
//! doctor`: una voce (`CheckReportItem`) per ogni controllo, con un
//! identificatore univoco, la descrizione del componente verificato, un esito
//! (`CheckOutcome`) e — quando l'esito è `Warning`/`Problema` — un'azione
//! correttiva concreta (Req 1.2, 1.3).
//!
//! La **politica del codice di uscita** (`exit_code`) è una **funzione pura**
//! del report: il codice è diverso da zero **se e solo se** almeno una voce ha
//! esito `Problema`; altrimenti è zero, anche in presenza di soli `Warning`
//! (Req 1.5, 1.6, 1.7).

use std::fmt;

/// Esito di un singolo controllo del `Dev_Environment` (Req 1.2).
///
/// L'insieme degli esiti è chiuso: `{ ok, warning, problema }`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CheckOutcome {
    /// Il componente verificato è a posto.
    Ok,
    /// Il componente è utilizzabile ma presenta un'anomalia non bloccante.
    Warning,
    /// Il componente è assente o non funzionante: blocca l'ambiente.
    Problema,
}

impl CheckOutcome {
    /// Etichetta testuale stabile dell'esito, usata nel rendering del report.
    pub fn label(self) -> &'static str {
        match self {
            CheckOutcome::Ok => "ok",
            CheckOutcome::Warning => "warning",
            CheckOutcome::Problema => "problema",
        }
    }
}

impl fmt::Display for CheckOutcome {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.label())
    }
}

/// Una voce dell'`Environment_Report`: il risultato di un singolo controllo
/// (Req 1.1, 1.2, 1.3).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckReportItem {
    /// Identificatore univoco del controllo (es. `gd-version`).
    pub id: String,
    /// Descrizione del componente verificato.
    pub description: String,
    /// Esito del controllo.
    pub outcome: CheckOutcome,
    /// Azione correttiva concreta ed eseguibile dall'utente; presente quando
    /// l'esito è `Warning` o `Problema` (Req 1.3).
    pub corrective_action: Option<String>,
}

impl CheckReportItem {
    /// Costruisce una voce con esito `Ok` (nessuna azione correttiva).
    pub fn ok(id: impl Into<String>, description: impl Into<String>) -> Self {
        Self {
            id: id.into(),
            description: description.into(),
            outcome: CheckOutcome::Ok,
            corrective_action: None,
        }
    }

    /// Costruisce una voce con esito `Warning` e la relativa azione correttiva.
    pub fn warning(
        id: impl Into<String>,
        description: impl Into<String>,
        corrective_action: impl Into<String>,
    ) -> Self {
        Self {
            id: id.into(),
            description: description.into(),
            outcome: CheckOutcome::Warning,
            corrective_action: Some(corrective_action.into()),
        }
    }

    /// Costruisce una voce con esito `Problema` e la relativa azione correttiva.
    pub fn problema(
        id: impl Into<String>,
        description: impl Into<String>,
        corrective_action: impl Into<String>,
    ) -> Self {
        Self {
            id: id.into(),
            description: description.into(),
            outcome: CheckOutcome::Problema,
            corrective_action: Some(corrective_action.into()),
        }
    }
}

/// L'`Environment_Report`: una voce per ogni controllo eseguito, nell'ordine di
/// esecuzione (Req 1.1).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct EnvironmentReport {
    pub items: Vec<CheckReportItem>,
}

impl EnvironmentReport {
    /// Crea un report a partire dalle voci dei controlli eseguiti.
    pub fn new(items: Vec<CheckReportItem>) -> Self {
        Self { items }
    }

    /// `true` se **tutti** i controlli hanno esito `Ok`, ovvero il
    /// `Dev_Environment` è pronto (Req 1.5). Un report vuoto è considerato
    /// pronto (nessun controllo fallito).
    pub fn ready(&self) -> bool {
        self.items
            .iter()
            .all(|item| item.outcome == CheckOutcome::Ok)
    }

    /// `true` se almeno una voce ha l'esito indicato.
    pub fn any(&self, outcome: CheckOutcome) -> bool {
        self.items.iter().any(|item| item.outcome == outcome)
    }

    /// Rendering testuale completo del report: intestazione, una riga per ogni
    /// controllo con esito e — se presente — azione correttiva, e una riga di
    /// sintesi finale (Req 1.5, 1.6, 1.7).
    pub fn render(&self) -> String {
        let mut out = String::new();
        out.push_str("Pulse doctor — diagnostica dell'ambiente di sviluppo\n");
        out.push_str("====================================================\n");

        if self.items.is_empty() {
            out.push_str("Nessun controllo eseguito.\n");
        } else {
            for item in &self.items {
                out.push_str(&format!(
                    "[{outcome}] {id} — {description}\n",
                    outcome = item.outcome.label(),
                    id = item.id,
                    description = item.description,
                ));
                if let Some(action) = &item.corrective_action {
                    out.push_str(&format!("      ↳ azione: {action}\n"));
                }
            }
        }

        out.push_str("----------------------------------------------------\n");
        if self.ready() {
            out.push_str("Il tuo ambiente di sviluppo è pronto.\n");
        } else if self.any(CheckOutcome::Problema) {
            out.push_str(
                "Sono stati rilevati problemi che bloccano l'ambiente: \
                 risolvili con le azioni indicate.\n",
            );
        } else {
            out.push_str(
                "Ambiente utilizzabile con avvisi: valuta le azioni indicate.\n",
            );
        }
        out
    }
}

impl fmt::Display for EnvironmentReport {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.render())
    }
}

/// Politica del codice di uscita di `pulse doctor` — **funzione pura** del
/// report.
///
/// Restituisce un codice **diverso da zero se e solo se** almeno una voce ha
/// esito `Problema` (Req 1.6); altrimenti `0`, anche quando sono presenti solo
/// esiti `Warning` (Req 1.5, 1.7).
pub fn exit_code(report: &EnvironmentReport) -> i32 {
    if report.any(CheckOutcome::Problema) {
        1
    } else {
        0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ready_solo_con_tutti_ok() {
        let report = EnvironmentReport::new(vec![
            CheckReportItem::ok("gd-version", "Versione di GD installata"),
            CheckReportItem::ok("cpp-toolchain", "Toolchain C++"),
        ]);
        assert!(report.ready());
        assert_eq!(exit_code(&report), 0);
    }

    #[test]
    fn report_vuoto_e_pronto() {
        let report = EnvironmentReport::default();
        assert!(report.ready());
        assert_eq!(exit_code(&report), 0);
    }

    #[test]
    fn solo_warning_esce_con_zero() {
        // Req 1.7: nessun problema, almeno un warning ⇒ codice 0.
        let report = EnvironmentReport::new(vec![
            CheckReportItem::ok("gd-version", "Versione di GD installata"),
            CheckReportItem::warning(
                "cpp-toolchain",
                "Toolchain C++",
                "Aggiorna il compilatore alla versione consigliata.",
            ),
        ]);
        assert!(!report.ready());
        assert!(!report.any(CheckOutcome::Problema));
        assert_eq!(exit_code(&report), 0);
    }

    #[test]
    fn un_problema_esce_con_diverso_da_zero() {
        // Req 1.6: almeno un problema ⇒ codice ≠ 0.
        let report = EnvironmentReport::new(vec![
            CheckReportItem::ok("gd-version", "Versione di GD installata"),
            CheckReportItem::problema(
                "cpp-toolchain",
                "Toolchain C++",
                "Installa un compilatore C++ compatibile.",
            ),
        ]);
        assert!(!report.ready());
        assert_ne!(exit_code(&report), 0);
    }

    #[test]
    fn problema_prevale_sui_warning() {
        // Con warning + problema, la presenza del problema domina (Req 1.6).
        let report = EnvironmentReport::new(vec![
            CheckReportItem::warning("a", "A", "azione A"),
            CheckReportItem::problema("b", "B", "azione B"),
        ]);
        assert_ne!(exit_code(&report), 0);
    }

    #[test]
    fn render_include_ogni_voce_ed_esito() {
        let report = EnvironmentReport::new(vec![
            CheckReportItem::ok("gd-version", "Versione di GD installata"),
            CheckReportItem::problema(
                "cpp-toolchain",
                "Toolchain C++",
                "Installa un compilatore C++ compatibile.",
            ),
        ]);
        let text = report.render();
        assert!(text.contains("gd-version"));
        assert!(text.contains("Versione di GD installata"));
        assert!(text.contains("cpp-toolchain"));
        assert!(text.contains("[ok]"));
        assert!(text.contains("[problema]"));
        assert!(text.contains("Installa un compilatore C++ compatibile."));
    }

    #[test]
    fn render_report_pronto_segnala_ambiente_pronto() {
        let report = EnvironmentReport::new(vec![CheckReportItem::ok(
            "gd-version",
            "Versione di GD installata",
        )]);
        assert!(report.render().contains("pronto"));
    }
}
