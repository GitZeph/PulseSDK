# Vantaggi competitivi di Pulse rispetto a Geode

Questo documento soddisfa il **Requisito 24**: documentare in modo esplicito e
verificabile i vantaggi competitivi di Pulse rispetto a Geode, così da indirizzare le
decisioni di progettazione e comunicare il valore del prodotto.

In particolare il documento copre:

- **Req 24.1** — almeno dieci miglioramenti rispetto a Geode, ciascuno con tutti gli
  elementi obbligatori: un **identificatore univoco**, la descrizione del **limite
  attuale di Geode**, l'**approccio proposto da Pulse** e almeno un **criterio
  osservabile** che ne dimostri il vantaggio.
- **Req 24.2 / Req 24.3** — una **tabella di confronto** delle funzionalità tra Pulse e
  Geode con almeno dieci funzionalità, dove ogni riga riporta il nome della
  funzionalità, lo stato di supporto in Geode e lo stato di supporto in Pulse.
- **Req 24.4** — la versione di Geode usata come riferimento per il confronto.

## Versione di Geode di riferimento

> **Versione di Geode di riferimento per il confronto: Geode 4.x su GD 2.2074/2.2081.**

Questa è la baseline concettuale e il metro di confronto adottati dal progetto
(Requisito 24.4). I limiti di Geode descritti più sotto sono riferiti a questa linea di
versione; le versioni di Geometry Dash di riferimento allineate sono **GD 2.2074** e
**GD 2.2081** (vedi `docs/hooking-engine.md`).

## Enforcement automatico della completezza (Req 24.2/24.3)

La tabella dei miglioramenti e la tabella di confronto sono espresse come **dati
machine-readable** delimitati da marcatori. Un **checker automatico**
(`pulse_pulse_vs_geode_checker_test`, registrato come test CTest) analizza questo
documento e **fallisce** se:

- la tabella dei miglioramenti contiene **meno di dieci righe**;
- una riga dei miglioramenti è priva di uno qualsiasi degli elementi obbligatori
  (identificatore univoco, limite di Geode, approccio di Pulse o criterio osservabile),
  indicando nel messaggio di errore **quali elementi mancano** (Req 24.2);
- la tabella di confronto contiene **meno di dieci funzionalità**;
- una riga di confronto è priva del nome funzionalità, dello stato in Geode o dello
  stato in Pulse;
- la **versione di Geode di riferimento** è assente dal documento (Req 24.4).

Non rimuovere i marcatori `PULSE-IMPROVEMENTS-START` / `PULSE-IMPROVEMENTS-END` e
`PULSE-COMPARISON-START` / `PULSE-COMPARISON-END`: sono usati dal checker.

## Miglioramenti rispetto a Geode (Req 24.1)

Ogni miglioramento ha quattro colonne obbligatorie: **ID** (identificatore univoco),
**Limite di Geode** (il limite attuale di Geode), **Approccio Pulse** (l'approccio
proposto da Pulse) e **Criterio osservabile** (almeno un criterio osservabile che
dimostra il vantaggio).

<!-- PULSE-IMPROVEMENTS-START -->

| ID | Limite di Geode | Approccio Pulse | Criterio osservabile |
|----|-----------------|-----------------|----------------------|
| IMP-01 | Macro `$modify(MyClass, TargetClass)` che genera una classe derivata; gli errori di firma emergono spesso a link-time o a runtime. | `PULSE_HOOK(Symbol)` con concept C++20 che confronta la firma dichiarata con quella del binding e preserva `callOriginal`. | Una firma incompatibile produce un errore **a compile-time** con messaggio diagnostico (Req 5.2); esiste un test di non-compilazione automatizzato. |
| IMP-02 | Priorità presente ma il comportamento a parità di priorità non è garantito deterministico tra run diversi. | Catena ordinata per `Hook_Priority` decrescente con tie-break sull'ordine di caricamento risolto. | Stessa sequenza di esecuzione su run ripetuti con lo stesso set di mod (Req 3.3); verificato da property test di determinismo. |
| IMP-03 | Disabilitazione manuale via "safe mode"; il ripristino dei byte originali non è garantito persistente. | Byte originali persistiti su disco **prima** dell'applicazione dell'hook + auto-disable della mod su crash entro 60 s. | Dopo un crash, al riavvio la mod è disabilitata e i byte sono ripristinati (Req 18.1, 18.2, 18.4). |
| IMP-04 | `m_fields` tramite classe `Fields` annidata con chiavi implicite per tipo. | `PulseField<T>` con chiave esplicita, default tipizzato, isolamento per-istanza e cleanup automatico al distruttore. | Un conflitto di tipo sulla stessa chiave produce un errore a compile-time (Req 6.6); l'isolamento per-istanza è verificato da property test. |
| IMP-05 | `mod.json` (JSON piatto) senza round-trip di serializzazione garantito. | `pulse.toml` con schema versionato e parser/serializer con round-trip provato. | `parse∘serialize∘parse == parse` (Req 16.5); verificato da property test di round-trip. |
| IMP-06 | Permessi limitati e modello di consenso non granulare. | Permessi dichiarativi obbligatori + consenso esplicito dell'User + enforcement runtime nel Sandbox. | Un'operazione senza permesso è negata prima di qualunque effetto osservabile (Req 17.4); verificato da test di negazione. |
| IMP-07 | Solo C++ nativo, nessun runtime di scripting integrato. | Runtime di scripting Lua/JS con le stesse capability gated dal modello di permessi. | Una mod di scripting esegue hook/eventi/UI entro i permessi dichiarati (Req 19.3). |
| IMP-08 | Broma con aggiornamenti manuali degli offset per ciascuna versione. | Indice `(GD_Version, piattaforma)` con fetch auto-aggiornante e gate sugli indirizzi non risolti. | Zero hook installati su indirizzi non risolti (Req 20.4); risoluzione a corrispondenza esatta senza fuzzy-match. |
| IMP-09 | Utility thread-pool di base senza contratto di latenza di avvio. | Task system con avvio ≤ 1 ms, continuazione sul main thread al frame successivo e cancellazione su disable. | L'avvio di un task non blocca il main thread per più di 1 ms (Req 11.1). |
| IMP-10 | Sistema node-ID e layout presenti ma senza budget temporale né gestione esplicita delle collisioni. | Node-ID stabili fino a 256 caratteri + layout con budget 16 ms e disposizione di fallback su conflitto. | Layout completato entro 16 ms (Req 8.4); collisione di node-ID gestita senza sovrascrittura (Req 8.7). |
| IMP-11 | Hot-reload limitato o assente per le mod native. | Hot-reload in modalità sviluppo ≤ 5 s con rimozione e reinstallazione atomica degli hook. | Reload completato senza chiudere Geometry Dash (Req 15.1). |
| IMP-12 | Firma del pacchetto presente ma senza verifica di integrità obbligatoria pre-esecuzione. | Firma digitale obbligatoria + verifica pre-install + verifica di integrità pre-exec. | Un pacchetto senza firma valida è rifiutato (Req 23.4); l'integrità è verificata prima dell'esecuzione (Req 28.6). |

<!-- PULSE-IMPROVEMENTS-END -->

## Tabella di confronto delle funzionalità (Req 24.2/24.3)

Ogni riga riporta il **nome della funzionalità**, lo **stato di supporto in Geode** e lo
**stato di supporto in Pulse**.

<!-- PULSE-COMPARISON-START -->

| Funzionalità | Stato in Geode | Stato in Pulse |
|--------------|----------------|----------------|
| Sintassi di hooking dichiarativa | Macro `$modify`, verifica firma debole | `PULSE_HOOK` type-safe con verifica firma a compile-time |
| Ordinamento deterministico di hook multipli | Priorità senza tie-break garantito | Ordine per priorità + tie-break su load order, deterministico |
| Recovery persistente da crash | Safe mode manuale | Rollback byte persistito + auto-disable su crash |
| Field injection type-safe | `m_fields` con chiavi implicite | `PulseField<T>` con chiave esplicita e isolamento per-istanza |
| Formato manifest | `mod.json` (JSON piatto) | `pulse.toml` con schema versionato e round-trip provato |
| Sandbox a permessi granulari | Permessi limitati | Permessi dichiarativi + consenso User + enforcement runtime |
| Scripting (Lua/JS) | Non supportato (solo C++) | Supportato con capability gated dai permessi |
| Bindings/offset auto-aggiornanti | Aggiornamenti manuali (Broma) | Indice `(GD_Version, piattaforma)` con gate su non risolti |
| Sistema asincrono/task | Thread-pool di base | Task system con avvio ≤ 1 ms e continuazione sul main thread |
| UI/layout con budget temporale | Layout senza budget garantito | Node-ID stabili + layout ≤ 16 ms con fallback |
| Hot-reload delle mod native | Limitato/assente | Hot-reload dev ≤ 5 s con swap atomico degli hook |
| Integrità del pacchetto | Firma presente | Firma obbligatoria + verifica integrità pre-exec |

<!-- PULSE-COMPARISON-END -->

## Note sulla verifica

- Il checker considera una riga dei miglioramenti **completa** quando tutte e quattro le
  colonne (ID, limite di Geode, approccio Pulse, criterio osservabile) sono non vuote e
  l'ID ha la forma `IMP-NN`.
- Il checker considera una riga di confronto **completa** quando il nome funzionalità,
  lo stato in Geode e lo stato in Pulse sono tutti non vuoti.
- Aggiungere un miglioramento significa aggiungere una riga alla tabella dei
  miglioramenti mantenendo tutte le colonne valorizzate; lo stesso vale per la tabella
  di confronto.

## Riferimenti

- Esempio di hooking affiancato Pulse vs Geode: [`docs/pulse-vs-geode-hook-example.md`](./pulse-vs-geode-hook-example.md) (voce `IMP-01`).
- Checklist di parità funzionale: [`docs/geode-parity.md`](./geode-parity.md).
- Requisiti correlati: Requisito 24 (24.1, 24.2, 24.3, 24.4).
