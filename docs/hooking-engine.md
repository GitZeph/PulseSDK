# Motore di hooking per piattaforma

> **Scopo del documento.** Questo file soddisfa il **Requisito 2.6**: per ciascuna
> piattaforma supportata riporta (a) il motore di hooking adottato, (b) i criteri
> tecnici alla base della scelta e (c) le combinazioni `(GD_Version, piattaforma)`
> supportate. La fonte di verità del design è la sezione *"Selezione del motore di
> hooking per piattaforma"* di `.kiro/specs/pulse-sdk/design.md`; questo documento
> ne riflette le decisioni così come sono implementate in `loader/hooking/`.

## 1. Architettura: un'unica astrazione, più backend

Pulse non dipende da uno specifico motore di hooking. Definisce l'interfaccia
platform-agnostic **`IHookBackend`** (`loader/hooking/hook_backend.hpp`) e seleziona
il backend concreto **a compile-time** in base al target. L'interfaccia espone
**solo tre primitive**:

| Primitiva | Firma | Scopo |
|-----------|-------|-------|
| `install` | `install(std::uintptr_t target, void* detour) -> Result<Trampoline>` | Installa un detour sulla funzione bersaglio e restituisce un trampolino verso l'originale (Req 2.2). |
| `remove`  | `remove(std::uintptr_t target) -> Result<void>` | Rimuove l'hook e ripristina il codice originale della funzione (Req 2.4). |
| `readOriginal` | `readOriginal(std::uintptr_t target, std::size_t len) -> Result<ByteSpan>` | Legge i byte originali del prologo **prima** di qualsiasi modifica, per il rollback persistente (Req 18.1). |

Ogni backend espone inoltre `name()` (per logging/diagnostica, es. `pulse-minhook`)
e `available()` (true solo se il backend è operativo sulla piattaforma/architettura
corrente).

**Punto chiave architetturale.** Tutta la logica di **catena di hook**, **ordinamento
per priorità**, **risoluzione dei conflitti** e **rollback** vive interamente nel
codice originale Pulse (vedi `hook_chain.*`, `rollback_store.*`, `crash_sentinel.*`) ed
è **indipendente dal backend** (Requisito 27). I backend si limitano a installare/
rimuovere il singolo detour e a leggere i byte originali; non conoscono priorità,
catene né conflitti. Questo isola Pulse dalle scelte di terze parti e consente di
sostituire un backend senza toccare la semantica osservabile dell'`Hooking_Engine`.

La politica di **retry (massimo 3 tentativi)** e il **rollback delle modifiche parziali**
in caso di fallimento (Req 2.5) sono applicati a monte, nell'engine, non nei backend.

Compilabilità cross-platform: ogni backend concreto resta **definito e compilabile**
su qualunque host. Quando il backend non è abilitato per il target corrente
(macro CMake non definita), le primitive ritornano `HookErrorCode::Unsupported` e
`available()` ritorna `false`, così l'interfaccia e i path di altre piattaforme
continuano a compilare. Le factory (`make_minhook_backend()`, `make_dobby_backend()`,
`make_shadowhook_backend()`) restituiscono **sempre** un'istanza valida, mai `nullptr`.

## 2. Backend adottato per piattaforma e criteri di scelta

| Piattaforma | Architettura | Backend adottato | Identificatore | Criteri tecnici della scelta |
|-------------|--------------|------------------|----------------|------------------------------|
| **Windows** | x86-64 | **MinHook** (fork interno `pulse-minhook`) + **Zydis** | `windows-x64` | Maturo e con trampolino affidabile su x64; licenza BSD compatibile; footprint minimo. Si integra **Zydis** come disassembler a lunghezza per relocare correttamente i prologhi non banali (istruzioni RIP-relative, salti, prologhi a lunghezza variabile). |
| **macOS**   | x86-64 / arm64 | **Dobby** | `macos-x64` / `macos-arm64` | Unico backend che copre **uniformemente** sia x86-64 sia arm64 (Apple Silicon) con un'unica API; gestione **PAC** (Pointer Authentication) su arm64e. Riduce il codice specifico per architettura sui target Apple. |
| **Android** | arm64-v8a | **ShadowHook** (ByteDance) | `android-arm64` | Specializzato sull'inline hook arm64/Thumb/arm32; gestione robusta della **cache d'istruzioni** (I-cache flush dopo la patch) e dei **thread**; attivamente mantenuto. Astrae automaticamente il set d'istruzioni del bersaglio. |
| **Android** | armeabi-v7a (32-bit) | **ShadowHook** (modalità Thumb/arm32) | `android-armv7` | Stesso backend: copre il **32-bit** richiesto dal Requisito 1.1. ShadowHook rileva automaticamente ARM vs Thumb e reloca il prologo nel set corretto, evitando la gestione manuale del thumb-bit e di `__builtin___clear_cache`. |
| **iOS**     | arm64 / arm64e | **Dobby** | `ios-arm64` | Supporto **arm64e/PAC**, collaudato in contesti jailbreak; coerente con macOS arm64, così da condividere il backend e ridurre il codice specifico per piattaforma. |

### Note tecniche per backend

- **MinHook + Zydis (Windows x64)** — `loader/hooking/minhook_backend.{hpp,cpp}`.
  Abilitato dalla macro CMake `PULSE_HOOK_BACKEND_MINHOOK`. Zydis copre i prologhi
  che MinHook da solo non saprebbe relocare; in caso di prologo non relocabile la
  primitiva ritorna `HookErrorCode::UnsupportedPrologue`.
- **Dobby (macOS/iOS)** — `loader/hooking/dobby_backend.{hpp,cpp}`. Abilitato dalla
  macro `PULSE_HOOK_BACKEND_DOBBY` (opzione `PULSE_ENABLE_DOBBY` ON + Dobby
  disponibile via FetchContent). Mappa `install`/`remove` su `DobbyHook`/`DobbyDestroy`
  e gestisce PAC su arm64e.
- **ShadowHook (Android)** — `loader/hooking/shadowhook_backend.{hpp,cpp}`. Abilitato
  dalla macro `PULSE_HOOK_BACKEND_SHADOWHOOK`. Identifica ogni hook tramite uno
  "stub" opaco restituito da `shadowhook_hook_sym_addr`, mappato dall'indirizzo
  bersaglio per implementare `remove(target)` via `shadowhook_unhook`.

## 3. Combinazioni `(GD_Version, piattaforma)` supportate

L'`Hooking_Engine` installa hook solo dove esistono **bindings risolti a corrispondenza
esatta** per la coppia `(GD_Version, piattaforma)` (Requisiti 20.2/20.4): su una coppia
non risolta non viene installato alcun hook. Le versioni di riferimento del progetto
sono **GD 2.2074** e **GD 2.2081** (allineate al metro di confronto Geode 4.x, Req 24.4).

La capacità di hooking di ciascun backend è indipendente dalla `GD_Version`: ciò che
abilita una specifica coppia è la **disponibilità dei bindings** per quella coppia, non
il backend. La tabella seguente riporta lo stato corrente.

| GD_Version | Piattaforma (`platformId`) | Backend | Bindings disponibili | Stato |
|------------|----------------------------|---------|----------------------|-------|
| 2.2074 | `windows-x64` | MinHook + Zydis | Sì — set embedded MVP (`MenuLayer::init`) | **Supportata (MVP)** |
| 2.2081 | `windows-x64` | MinHook + Zydis | Pianificata | Pianificata |
| 2.2074 / 2.2081 | `macos-x64` | Dobby | Pianificata | Backend pronto, bindings pianificati |
| 2.2074 / 2.2081 | `macos-arm64` | Dobby | Pianificata | Backend pronto, bindings pianificati |
| 2.2074 / 2.2081 | `android-arm64` | ShadowHook | Pianificata | Backend pronto, bindings pianificati |
| 2.2074 / 2.2081 | `android-armv7` | ShadowHook (Thumb/arm32) | Pianificata | Backend pronto, bindings pianificati |
| 2.2074 / 2.2081 | `ios-arm64` | Dobby | Pianificata | Backend pronto, bindings pianificati |

**Stato attuale (MVP).** L'unica combinazione con bindings embedded e quindi
immediatamente hookabile end-to-end è **`(2.2074, windows-x64)`**, con l'offset e la
firma di `MenuLayer::init` (vedi `loader/bindings/embedded_bindings_provider.cpp` e
design → MVP). Per le altre piattaforme il backend di hooking è **già implementato e
selezionato a compile-time**; l'abilitazione di una coppia richiede unicamente
l'aggiunta del relativo `BindingSet` per quella `(GD_Version, piattaforma)`, senza
modifiche alla logica dell'engine.

L'identificatore di piattaforma segue il formato `<os>-<arch>` esposto da
`pulse::loader::platform_id` (`loader/core/runtime_context.cpp`): `windows-x64`,
`macos-x64`, `macos-arm64`, `android-arm64`, `android-armv7`, `ios-arm64`.

## 4. Riferimenti

- Design: `.kiro/specs/pulse-sdk/design.md` — sezione *"Selezione del motore di hooking per piattaforma"*.
- Requisiti: `.kiro/specs/pulse-sdk/requirements.md` — Requisito 2 (in particolare 2.6), Requisito 18 (rollback), Requisito 20 (bindings), Requisito 27 (logica indipendente dal backend).
- Codice: `loader/hooking/hook_backend.hpp` (interfaccia `IHookBackend`), `minhook_backend.*`, `dobby_backend.*`, `shadowhook_backend.*`, `hook_chain.*`, `rollback_store.*`, `crash_sentinel.*`.
- Bindings: `loader/bindings/embedded_bindings_provider.*` (set MVP `(2.2074, windows-x64)`).
