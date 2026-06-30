# Motivazione delle scelte di tooling

Questo documento soddisfa il **Requisito 26.6**: *dove una componente di tooling beneficia di un linguaggio diverso dal C++, la scelta del linguaggio alternativo e la relativa motivazione devono essere documentate nel repository.*

Loader e SDK sono in **C++20/23** (Requisito 26.1) perché devono interoperare a livello ABI con il binario di Geometry Dash (compilato in C++/Cocos2d-x) e perché i `concepts`/`requires` abilitano la verifica della firma degli hook a compile-time. La **Pulse CLI** (cartella `cli/`, Requisito 14 e 26.6) è invece scritta in **Rust**. Di seguito la motivazione.

## Perché Rust per la Pulse CLI

### 1. Binari statici e distribuzione cross-platform

La CLI è uno strumento standalone che il Developer installa sulla propria macchina di sviluppo. Rust produce **binari statici** auto-contenuti, **senza dipendenze runtime** da installare (niente runtime gestiti, niente DLL/`.so` da distribuire a corredo). Questo rende la distribuzione su Windows, macOS e Linux semplice e affidabile: un singolo eseguibile `pulse` per ciascun target, facile da scaricare ed eseguire.

### 2. Parsing degli argomenti robusto e gestione errori

La CLI espone sottocomandi (`pulse new`, `pulse build`, `pulse publish`, hot-reload). Rust offre un ecosistema maturo per questo dominio:

- **`clap`** (con la feature `derive`) per un parsing degli argomenti dichiarativo, type-safe, con help e validazione generati automaticamente.
- **`anyhow`** per la propagazione ergonomica degli errori nei flussi applicativi.
- **`thiserror`** per definire tipi di errore strutturati nelle librerie interne.

Questo permette messaggi di errore chiari e diagnostici (es. conflitto di directory in scaffolding, campi Manifest non validi in build/publish), come richiesto dal Requisito 14.

### 3. Parsing e serializzazione TOML robusti

Il Manifest delle mod (`pulse.toml`) richiede un parser/serializer con proprietà di **round-trip** garantita (Requisito 16.5): `parse ∘ serialize ∘ parse == parse`. Rust copre questa esigenza con:

- **`serde`** (con `derive`) per la (de)serializzazione type-safe su struct tipizzate.
- **`toml`** per la lettura e scrittura del formato TOML.

La combinazione `serde` + `toml` rende naturale modellare lo schema del Manifest come tipi Rust e ottenere round-trip affidabile senza parsing manuale.

### 4. Nessun vincolo ABI

La CLI **non** è linkata nel processo di Geometry Dash: è un processo separato che gira sulla macchina di sviluppo. Di conseguenza **non esiste alcun vincolo di interoperabilità ABI** con il binario di GD o con loader/SDK. Questo è ciò che distingue la CLI da loader e SDK: questi ultimi *devono* essere C++ proprio per l'interop ABI con GD, mentre la CLI è libera di adottare il linguaggio più adatto al suo dominio (tooling da riga di comando) senza alcuna penalità di integrazione.

## Dipendenze effettive

Coerentemente con quanto sopra, `cli/Cargo.toml` dichiara (con versioni pinnate per build riproducibili):

| Crate | Ruolo |
|-------|-------|
| `clap` (feature `derive`) | parsing argomenti e sottocomandi |
| `serde` (feature `derive`) | (de)serializzazione type-safe |
| `toml` | parsing/serializzazione del Manifest `pulse.toml` |
| `anyhow` | gestione/propagazione errori applicativi |
| `thiserror` | tipi di errore strutturati |

## Sintesi

| Componente | Linguaggio | Motivo principale |
|------------|------------|-------------------|
| `loader/`, `sdk/` | C++20/23 | interop ABI con GD + verifica firma hook a compile-time |
| `cli/` | Rust | binari statici cross-platform, parsing/errori robusti, TOML affidabile, nessun vincolo ABI |
