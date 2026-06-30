# Checklist di parità funzionale con Geode

Questo documento soddisfa il **Requisito 25.3**: *definire la parità funzionale con
Geode come l'elenco verificabile delle capacità di Geode che Pulse deve replicare,
associando a ciascuna capacità un criterio di accettazione osservabile che ne attesta
il raggiungimento.*

La parità è espressa come **checklist verificabile a macchina**: ogni capability di
Geode è una riga della tabella sottostante con tre colonne obbligatorie:

1. **Capability Geode** — la funzionalità di Geode che Pulse deve replicare.
2. **Criterio di accettazione osservabile** — un comportamento misurabile/osservabile
   che attesta il raggiungimento della parità per quella capability.
3. **Requisito Pulse** — l'identificatore del requisito Pulse (`Req N`) che definisce
   e copre quella capability.

Un **checker automatico** (`pulse_geode_parity_checker_test`, registrato come test
CTest) analizza la tabella e **fallisce** se anche una sola capability attesa è
assente o se una riga è priva del criterio osservabile o del requisito Pulse mappato.
In questo modo la completezza della mappatura è enforced dalla suite di test.

Versione di Geode di riferimento per il confronto: **Geode SDK v4.x** (linea `4.0`).

La tabella seguente è delimitata da marcatori machine-readable usati dal checker.
Non rimuovere i marcatori `PARITY-TABLE-START` / `PARITY-TABLE-END`.

<!-- PARITY-TABLE-START -->

| Capability Geode | Criterio di accettazione osservabile | Requisito Pulse |
|------------------|--------------------------------------|-----------------|
| hooking | Un hook installato reindirizza la funzione bersaglio al gestore e `callOriginal` preserva parametri e valore di ritorno della firma originale; più hook sulla stessa funzione si concatenano per priorità DESC poi loadOrder ASC in modo deterministico. | Req 2 |
| field injection | Lo stato iniettato via `PulseField` è isolato per istanza della classe del gioco, persiste finché l'istanza è viva ed è rilasciato alla distruzione dell'istanza senza alterare le altre istanze. | Req 6 |
| eventi | La registrazione di un gestore ha successo se e solo se il tipo evento è stato dichiarato; alla pubblicazione i gestori sono invocati nell'ordine di registrazione, le eccezioni sono isolate e un esito Consumed interrompe la propagazione. | Req 7 |
| UI/layout & node-ID | I nodi della UI sono indirizzabili tramite ID stabili e una lookup per ID restituisce lo stesso nodo attraverso ricostruzioni di layout equivalenti. | Req 8 |
| settings | Le impostazioni tipizzate di una Mod sono dichiarate con nomi univoci; un valore persistito non valido per il tipo dichiarato ricade sul default senza errore bloccante. | Req 9 |
| storage | Una lettura dopo scritture restituisce il valore scritto più di recente per quella chiave; una chiave mai scritta restituisce assente senza errore; una scrittura che supererebbe la capacità è rifiutata. | Req 10 |
| async/task | Le operazioni asincrone (task) eseguono fuori dal thread principale e consegnano il risultato o l'errore in modo osservabile senza bloccare il loop di gioco. | Req 11 |
| HTTP | Le richieste HTTP sono effettuate solo previa concessione del permesso dichiarato e restituiscono in modo osservabile esito di successo (stato/corpo) o errore. | Req 12 |
| mod manager | Le dipendenze sono risolte in ordine topologico deterministico; mod con dipendenza mancante/incompatibile o coinvolte in un ciclo sono escluse e segnalate senza impedire il caricamento delle restanti. | Req 4 |
| marketplace/signing | Un Pulse_Package è installato se e solo se la verifica della firma digitale ha successo; una firma assente o non valida comporta il rifiuto dell'installazione senza scrivere alcun file del pacchetto. | Req 23 |
| multi-platform | Il loader effettua il bootstrap e installa hook sulle piattaforme supportate (Windows, macOS, Android 32/64-bit, iOS) selezionando i binding esatti per coppia (GD_Version, piattaforma) senza fuzzy-match. | Req 1 |
| scripting | I linguaggi di scripting supportati possono dichiarare ed eseguire hook ed eventi attraverso le stesse API gated del SDK nativo, soggetti al medesimo modello di permessi. | Req 19 |

<!-- PARITY-TABLE-END -->

## Note sulla verifica

- Il checker considera **capability attese** l'insieme: `hooking`, `field injection`,
  `eventi`, `UI/layout & node-ID`, `settings`, `storage`, `async/task`, `HTTP`,
  `mod manager`, `marketplace/signing`, `multi-platform`, `scripting`.
- Una riga è considerata **completa** quando la colonna del criterio osservabile e la
  colonna del requisito Pulse sono entrambe non vuote e il requisito ha la forma
  `Req N` (con `N` numerico).
- Aggiungere una nuova capability significa aggiungere una riga alla tabella e, se è
  attesa per la parità, includerla nell'insieme atteso del checker.
