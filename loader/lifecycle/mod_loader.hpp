// loader/lifecycle/mod_loader.hpp — Mod_Loader (Layer 4, orchestratore delle
// mod esterne). Questo file introduce la **discovery** della Mods_Directory
// (task 3.1, Requisiti 1.1–1.5) e i tipi `DiscoveredMod`/`DiscoveredModSet`.
//
// Stato incrementale (vedi tasks.md, Fase B): qui è implementata SOLO la
// scoperta dei candidati Mod_Package nella Mods_Directory. La dedup per
// Mod_Id (task 3.2, Req 1.6/1.7), la validazione, la risoluzione delle
// dipendenze e la pipeline completa (`ModLoader`, task 7.x) estenderanno questo
// stesso file riusando i tipi qui definiti. La discovery è quindi progettata
// come un pezzo **auto-contenuto e componibile** (funzioni libere + seam
// iniettabili), così le attività successive vi si innestano senza riscritture.
//
// Due seam iniettabili rendono la discovery host-testabile su un filesystem
// virtuale (coerentemente con il design, "discovery su filesystem virtuale"):
//   * `DirectoryLister`  — enumera le SOLE voci di primo livello (Req 1.1) e
//     distingue directory assente/vuota (Req 1.3) da directory non leggibile
//     (Req 1.4);
//   * `PackageOpener`    — tenta di aprire una voce come container `.pulse`
//     (Req 1.2): una voce è un candidato sse l'apertura riesce.
// Un `DirectoryLister` reale basato su `std::filesystem` è fornito da
// `default_directory_lister`. La lettura ZIP reale di un `.pulse` su disco non
// è ancora cablata (vedi Fase E / lettore ZIP) e viene iniettata come opener
// dai chiamanti/test.
//
// Logica originale Pulse. Stack: C++20/23.
#ifndef PULSE_LOADER_LIFECYCLE_MOD_LOADER_HPP
#define PULSE_LOADER_LIFECYCLE_MOD_LOADER_HPP

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/centralized_loader.hpp"     // pulse::loader::RuntimeInitFn
#include "core/loader_core.hpp"            // pulse::loader::DiagnosticSink
#include "core/runtime_context.hpp"        // pulse::loader::RuntimeContext
#include "hooking/hook_backend.hpp"        // pulse::hooking::IHookBackend
#include "hooking/rollback_store.hpp"      // pulse::hooking::RollbackStore
#include "lifecycle/compatibility.hpp"     // check_compatibility, CompatResult
#include "lifecycle/dependency_resolver.hpp"  // DependencyResolver, LoadPlan, Exclusion
#include "lifecycle/hook_ownership.hpp"    // HookOwnershipLedger, OwnedHook
#include "lifecycle/manifest.hpp"          // pulse::manifest::Manifest, ModId
#include "lifecycle/mod_manager.hpp"       // ModManager, EntryPointFn, TerminatorFn
#include "lifecycle/mod_manifest_validator.hpp"  // ModManifestValidator, ValidationResult
#include "lifecycle/module_loader.hpp"     // IModuleLoader, ModuleHandle, Bytes
#include "package/pulse_package.hpp"       // PulsePackage, OpenResult

// Forward declaration (Hook_Chaining): l'orchestratore di catena di Layer 3.
// Iniettato OPZIONALMENTE nel cablaggio così l'install/rimozione degli hook
// delle mod esterne passa per l'unica Underlying_Installation per Target_Address
// (coesistenza demo + mod esterne, Req 8). L'header completo è incluso solo nel
// .cpp per non appesantire questo header.
namespace pulse::hooking {
class HookChainRegistry;
}  // namespace pulse::hooking

namespace pulse::lifecycle {

// ModId del Mod_Loader: coerente con `pulse::manifest::ModId`,
// `pulse::lifecycle::ModId` e `pulse::hooking::ModId` (stringa).
using ModId = pulse::manifest::ModId;

// ---------------------------------------------------------------------------
// DiscoveredMod / DiscoveredModSet (Req 1.1, 1.6).
//
// Un candidato Mod_Package scoperto nella Mods_Directory: il nome della voce
// (usato per il determinismo dell'ordinamento e, in task 3.2, per il tie-break
// della dedup), il Mod_Id dichiarato nel suo manifest e il container `.pulse`
// già aperto con successo (l'apertura è la condizione di riconoscibilità,
// Req 1.2).
// ---------------------------------------------------------------------------
struct DiscoveredMod {
    std::string entryName;                  // nome di voce nella Mods_Directory
    ModId modId;                            // id dichiarato nel manifest
    pulse::package::PulsePackage package;   // container aperto con successo
};

// Invariante (completata in task 3.2): ordinato per `entryName` lessicografico
// (determinismo, Req 1.6) e — dopo la dedup — ogni Mod_Id al più una volta.
using DiscoveredModSet = std::vector<DiscoveredMod>;

// ---------------------------------------------------------------------------
// DirectoryReadStatus — esito della lettura della Mods_Directory.
// Distingue i tre casi richiesti da Req 1.3/1.4.
// ---------------------------------------------------------------------------
enum class DirectoryReadStatus {
    Ok,           // directory leggibile (può comunque non avere voci)
    Absent,       // directory inesistente (Req 1.3 → set vuoto, esito registrato)
    NotReadable,  // directory presente ma non leggibile (Req 1.4 → diagnostica)
};

// Risultato dell'enumerazione di primo livello della Mods_Directory.
struct DirectoryListing {
    DirectoryReadStatus status{DirectoryReadStatus::Ok};
    std::vector<std::string> entryNames;  // SOLO voci di primo livello (Req 1.1)
    std::string error;                    // valorizzato sse status==NotReadable
};

// Seam: enumera le SOLE voci di primo livello della Mods_Directory (nessuna
// discesa ricorsiva, Req 1.1). Iniettabile per i test su filesystem virtuale.
using DirectoryLister =
    std::function<DirectoryListing(const std::filesystem::path&)>;

// Seam: tenta di aprire una voce di primo livello come container `.pulse`
// (Req 1.2). `entryPath` è il percorso completo della voce, `entryName` il suo
// nome nella Mods_Directory. Ritorna l'`OpenResult` di `PulsePackage::open`.
using PackageOpener = std::function<pulse::package::OpenResult(
    const std::filesystem::path& entryPath, std::string_view entryName)>;

// ---------------------------------------------------------------------------
// default_directory_lister — `DirectoryLister` reale basato su std::filesystem.
//
// Enumera le sole voci di primo livello (no ricorsione). Non lancia: usa
// `std::error_code`. Directory inesistente → `Absent`; percorso non-directory o
// iterazione fallita → `NotReadable` con la causa; altrimenti `Ok` con i nomi
// di voce (file e sottocartelle di primo livello).
// ---------------------------------------------------------------------------
[[nodiscard]] DirectoryListing default_directory_lister(
    const std::filesystem::path& modsDir);

// ---------------------------------------------------------------------------
// discover_mods — scoperta dei candidati Mod_Package (Req 1.1–1.5).
//
// Comportamento:
//   * enumera le SOLE voci di primo livello via `lister` (Req 1.1);
//   * Mods_Directory assente/vuota → `DiscoveredModSet` vuoto + esito
//     registrato sul `sink` (Req 1.3);
//   * Mods_Directory non leggibile → set vuoto + diagnostica con directory e
//     causa (Req 1.4);
//   * per ogni voce: se non termina in `.pulse` oppure l'apertura via `opener`
//     fallisce, la voce è ignorata con una diagnostica che la identifica e ne
//     riporta la causa, e la scoperta prosegue con le restanti (Req 1.2, 1.5);
//   * i candidati sono ordinati per `entryName` lessicografico (determinismo,
//     Req 1.6). La dedup per Mod_Id è il task 3.2.
//
// Non lancia eccezioni a fronte di un `lister`/`opener` che rispettano il
// contratto dei seam. `sink` può essere nullo (le diagnostiche sono scartate).
[[nodiscard]] DiscoveredModSet discover_mods(
    const std::filesystem::path& modsDir, const DirectoryLister& lister,
    const PackageOpener& opener, const pulse::loader::DiagnosticSink& sink);

// Overload di convenienza: usa `default_directory_lister` come enumeratore
// reale e l'`opener` iniettato per l'apertura dei container `.pulse`.
[[nodiscard]] DiscoveredModSet discover_mods(
    const std::filesystem::path& modsDir, const PackageOpener& opener,
    const pulse::loader::DiagnosticSink& sink);

// ---------------------------------------------------------------------------
// dedup_by_mod_id — dedup deterministica per Mod_Id (task 3.2, Req 1.6, 1.7).
//
// Garantisce che ogni Mod_Id compaia **al più una volta** nel
// `DiscoveredModSet` (Req 1.6). In caso di collisione (due o più Mod_Package
// scoperti che dichiarano lo stesso Mod_Id) mantiene il Mod_Package il cui
// `entryName` precede in **ordine lessicografico** ed esclude gli altri,
// registrando per ciascuna esclusione una diagnostica che nomina il Mod_Id
// duplicato e le voci coinvolte (la voce esclusa e la voce mantenuta) (Req 1.7).
//
// Funzione **componibile** e auto-contenuta: il chiamante tipico la applica
// all'output di `discover_mods` (`dedup_by_mod_id(discover_mods(...), sink)`),
// ma il comportamento è indipendente dall'ordine d'ingresso — l'insieme è
// riordinato per `entryName` lessicografico, così il vincitore di ogni
// collisione è deterministicamente il minimo lessicografico e l'output resta
// ordinato per `entryName` (coerente con il determinismo della discovery, 3.1).
//
// Prende il set per valore (consuma/sposta i `PulsePackage`); `sink` può essere
// nullo (le diagnostiche sono scartate). Non lancia eccezioni.
[[nodiscard]] DiscoveredModSet dedup_by_mod_id(
    DiscoveredModSet candidates, const pulse::loader::DiagnosticSink& sink);

// ---------------------------------------------------------------------------
// Modelli diagnostici del Mod_Loader (Data Models del design — partizione
// degli esiti e insieme CHIUSO delle cause, Req 10.1–10.4).
//
// Questi tipi sono introdotti qui dal task 7.1 perché servono già alla
// traduzione delle mod escluse dal `DependencyResolver` in diagnostica
// attribuita (Req 4.3–4.6). Il task 7.19 (DiagnosticLedger) costruirà su questi
// stessi tipi la garanzia di partizione completa {caricata|esclusa|isolata} e
// gli eventi di install/remove. L'insieme delle cause è quello CHIUSO definito
// dal design (Req 10.2): nessun valore aggiuntivo.
// ---------------------------------------------------------------------------

// Esito osservabile di una mod individuata: partizione a tre vie (Req 10.4).
enum class ModOutcome { Loaded, Excluded, Isolated };

// Insieme CHIUSO delle categorie di causa diagnostica (Req 10.2).
enum class CauseCategory {
    InvalidPackage,                 // pacchetto non valido
    VersionOrPlatformIncompatible,  // incompatibilità di versione o piattaforma
    DependencyUnsatisfied,          // dipendenza non soddisfatta (mancante/incompatibile/esclusa)
    DependencyCycle,                // ciclo di dipendenze
    ModuleNotLoadable,              // modulo non caricabile
    EntryPointFailed,               // entry point fallito
    SymbolUnresolved,               // simbolo non risolto
};

[[nodiscard]] std::string_view to_string(ModOutcome outcome) noexcept;
[[nodiscard]] std::string_view to_string(CauseCategory cause) noexcept;

// Voce diagnostica attribuita: identifica il Mod_Id, l'esito (caricata vs
// esclusa vs isolata) ed esattamente una causa dell'insieme chiuso (assente solo
// per `Loaded`), più un messaggio leggibile (Req 10.2).
struct DiagnosticEntry {
    ModId modId;
    ModOutcome outcome{ModOutcome::Excluded};
    std::optional<CauseCategory> cause;
    std::string message;

    friend bool operator==(const DiagnosticEntry&, const DiagnosticEntry&) = default;
};

// ---------------------------------------------------------------------------
// HookOp / HookEvent — evento osservabile di install/remove di un hook
// (Req 10.3).
//
// Ogni installazione o rimozione di un hook di una mod emette un evento che
// identifica il Mod_Id proprietario, il simbolo bersaglio e il tipo di
// operazione (installazione vs rimozione). Il `DiagnosticLedger` ne conserva la
// sequenza in ordine di emissione.
// ---------------------------------------------------------------------------
enum class HookOp { Install, Remove };

[[nodiscard]] std::string_view to_string(HookOp op) noexcept;

struct HookEvent {
    ModId modId;        // Mod_Id proprietario dell'hook
    std::string symbol; // simbolo bersaglio
    HookOp op{HookOp::Install};  // installazione vs rimozione

    friend bool operator==(const HookEvent&, const HookEvent&) = default;
};

// ---------------------------------------------------------------------------
// DiagnosticLedger — registro osservabile del ciclo di vita (task 7.19,
// Req 10.1–10.4). Costruito SOPRA i tipi `ModOutcome`/`CauseCategory`/
// `DiagnosticEntry` (task 7.1).
//
// Garantisce la PARTIZIONE COMPLETA degli esiti (Req 10.4): ogni mod individuata
// in discovery finisce in ESATTAMENTE UNO tra {Loaded, Excluded, Isolated}. Il
// ledger conserva al più una `DiagnosticEntry` per Mod_Id: un secondo tentativo
// di registrare un esito per un Mod_Id già registrato è RIFIUTATO (ritorna
// `false`) lasciando invariata la voce esistente, così:
//   * un Mod_Id non può comparire in due esiti diversi (a due a due disgiunti,
//     Req 10.4);
//   * ogni Mod_Id caricato compare UNA SOLA volta, senza duplicati (Req 10.1).
// Ogni esito `Excluded`/`Isolated` porta ESATTAMENTE una `CauseCategory`
// dell'insieme CHIUSO (Req 10.2); `Loaded` non ha causa (Req 10.1).
//
// Inoltre registra gli eventi di install/remove degli hook (Req 10.3): ogni
// evento identifica Mod_Id, simbolo bersaglio e tipo di operazione, conservati
// in ordine di emissione.
//
// Il ledger NON installa/rimuove hook né scopre mod: è un registro PURO,
// alimentato dall'orchestratore (Mod_Loader/`ModManagerWiring`). Single-thread,
// coerentemente con il resto del sottosistema.
// ---------------------------------------------------------------------------
class DiagnosticLedger {
public:
    // Registra l'esito di una mod individuata. Ritorna `true` se la voce è stata
    // registrata; `false` se il Mod_Id aveva GIÀ un esito (la voce esistente è
    // mantenuta — l'invariante di partizione è preservato).
    bool recordLoaded(const ModId& modId, std::string message = {});
    bool recordExcluded(const ModId& modId, CauseCategory cause,
                        std::string message = {});
    bool recordIsolated(const ModId& modId, CauseCategory cause,
                        std::string message = {});

    // Registra una `DiagnosticEntry` già costruita (es. dall'output di
    // `resolve_load_plan`). Stessa semantica fail-soft delle `record*`.
    bool record(DiagnosticEntry entry);

    // Eventi di install/remove di un hook (Req 10.3).
    void recordHookInstalled(const ModId& modId, std::string symbol);
    void recordHookRemoved(const ModId& modId, std::string symbol);

    // --- Query degli esiti -------------------------------------------------

    [[nodiscard]] bool contains(const ModId& modId) const;
    [[nodiscard]] std::optional<ModOutcome> outcomeOf(const ModId& modId) const;

    [[nodiscard]] std::vector<ModId> loaded() const;          // Req 10.1
    [[nodiscard]] std::vector<DiagnosticEntry> excluded() const;
    [[nodiscard]] std::vector<DiagnosticEntry> isolated() const;

    // Tutte le voci registrate, in ordine di registrazione.
    [[nodiscard]] const std::vector<DiagnosticEntry>& entries() const {
        return entries_;
    }

    // Tutti gli eventi hook, in ordine di emissione (Req 10.3).
    [[nodiscard]] const std::vector<HookEvent>& hookEvents() const {
        return hookEvents_;
    }

    // --- Verifica della partizione (Req 10.4) ------------------------------

    // True se l'insieme dei Mod_Id registrati coincide ESATTAMENTE con
    // `discovered`: ogni mod individuata compare in esattamente uno degli esiti
    // (unione = individuate) e nessun esito riguarda una mod non individuata.
    // Per costruzione ogni Mod_Id è già in al più un esito (a due a due
    // disgiunti), quindi questo controllo verifica la COMPLETEZZA.
    [[nodiscard]] bool partitionComplete(
        const std::vector<ModId>& discovered) const;

    // Mod individuate prive di un esito registrato (vuoto sse la partizione è
    // completa). Utile alla diagnostica della completezza.
    [[nodiscard]] std::vector<ModId> missingFrom(
        const std::vector<ModId>& discovered) const;

private:
    // Inserisce un esito sse il Mod_Id non è ancora registrato (ritorna `false`
    // altrimenti, preservando l'invariante di partizione).
    bool insertOutcome(DiagnosticEntry entry);

    std::vector<DiagnosticEntry> entries_;          // ordine di registrazione
    std::unordered_map<ModId, std::size_t> byMod_;  // Mod_Id → indice in entries_
    std::vector<HookEvent> hookEvents_;             // Req 10.3
};

// ---------------------------------------------------------------------------
// Stadio di risoluzione delle dipendenze del Mod_Loader (task 7.1, Req 4.1–4.7).
//
// `ResolvedLoadPlan` è l'esito della proiezione dei Mod_Manifest compatibili in
// `ResolverManifest` (via `Manifest::toResolverManifest()`) seguita
// dall'invocazione del `DependencyResolver`:
//   * `order`    — i Mod_Id da caricare, ESCLUSIVAMENTE quelli presenti in
//                  `LoadPlan.order` e nell'ORDINE in cui vi compaiono (Req 4.2);
//                  gli stadi successivi (task 7.4) caricano solo questi, in
//                  quest'ordine.
//   * `excluded` — una `DiagnosticEntry` per ogni mod esclusa dal resolver
//                  (dipendenza mancante/incompatibile, ciclo, esclusione
//                  transitiva) con la causa dell'insieme chiuso e il messaggio
//                  derivato dalla causa riportata dal resolver (Req 4.3–4.6).
// ---------------------------------------------------------------------------
struct ResolvedLoadPlan {
    std::vector<ModId> order;                 // carica solo questi, in ordine (Req 4.2)
    std::vector<DiagnosticEntry> excluded;    // mod escluse + causa (Req 4.3–4.6)
};

// Mappa il motivo di esclusione del `DependencyResolver` sull'insieme chiuso di
// `CauseCategory` (Req 10.2): cicli → `DependencyCycle`; dipendenza
// mancante/incompatibile/esclusa transitivamente → `DependencyUnsatisfied`.
[[nodiscard]] CauseCategory cause_category_for(
    const ExclusionReason& reason) noexcept;

// Costruisce il messaggio diagnostico leggibile per una mod esclusa dal
// resolver, riportando la causa del resolver (Req 4.3/4.4/4.5).
[[nodiscard]] std::string exclusion_message(const ModId& mod,
                                            const ExclusionReason& reason);

// ---------------------------------------------------------------------------
// resolve_load_plan — proietta i Mod_Manifest compatibili in `ResolverManifest`,
// invoca il `DependencyResolver` e traduce l'esito (task 7.1, Req 4.1–4.7).
//
// Comportamento:
//   * ogni `compatible[i]` è proiettato via `toResolverManifest()` e passato al
//     `DependencyResolver` (Req 4.1);
//   * `ResolvedLoadPlan.order` riproduce esattamente `LoadPlan.order` (carica
//     solo queste mod, nell'ordine dato — Req 4.2); ogni mod vi compare dopo
//     tutte quelle da cui dipende (garanzia del resolver);
//   * ogni `LoadPlan.excluded` diventa una `DiagnosticEntry{outcome=Excluded}`
//     con la `CauseCategory` corretta e un messaggio con la causa del resolver,
//     emessa anche sul `sink` (Req 4.3/4.4/4.5/4.6);
//   * `order` vuoto → `order` vuoto + esito registrato sul `sink` (zero mod, GD
//     prosegue — Req 4.7).
//
// Non lancia eccezioni. `sink` può essere nullo (le diagnostiche restano nel
// `ResolvedLoadPlan` ma non vengono inoltrate).
[[nodiscard]] ResolvedLoadPlan resolve_load_plan(
    const std::vector<pulse::manifest::Manifest>& compatible,
    const pulse::loader::DiagnosticSink& sink);

// ---------------------------------------------------------------------------
// Fail-open su Module_Loader non disponibile (task 7.23, Req 11.5; coerente con
// il fail-open su backend non disponibile di Req 9.1–9.3).
//
// Quando il Module_Loader della piattaforma corrente
// (`make_platform_module_loader()`) riporta `available() == false` — perché la
// piattaforma non è il primo deliverable (Windows/Android/iOS) o perché il
// backend reale non è abilitato — il Mod_Loader NON deve tentare alcun
// caricamento: zero mod, zero install (byte di eseguibile/asset invariati,
// Req 9.3) e una diagnostica che NOMINA la piattaforma del Runtime_Context e il
// Module_Loader (Req 11.5). GD prosegue fino al menu.
//
// `ModuleLoaderAvailability` è l'esito del controllo: `available` riflette
// `IModuleLoader::available()`; quando è `false`, `diagnostic` contiene il
// messaggio fail-open (con piattaforma e nome del loader) già pronto per il
// `sink`. Il PIPELINE ENTRY (task 7.22, `CentralizedLoader`/runtime_entry) usa
// `check_module_loader_availability` PRIMA di costruire/eseguire il cablaggio,
// così può loggare la causa e degradare a "GD senza mod" senza nemmeno entrare
// nella discovery; `ModManagerWiring::runNoThrow` applica comunque lo stesso
// guard in modo difensivo (zero mod, zero install) se invocato con un
// Module_Loader non disponibile.
// ---------------------------------------------------------------------------
struct ModuleLoaderAvailability {
    bool available{false};
    std::string diagnostic;  // valorizzato sse !available (nomina piattaforma+loader)

    [[nodiscard]] explicit operator bool() const noexcept { return available; }
};

// Controlla la disponibilità del Module_Loader rispetto al Runtime_Context
// (Req 11.5). Se `moduleLoader.available()` è `false`, costruisce un messaggio
// diagnostico che nomina la piattaforma (`runtimeContext.platformId`) e il
// Module_Loader (`moduleLoader.name()`); altrimenti `diagnostic` è vuoto. Non
// lancia, non tocca alcun byte: è una pura query usata dal pipeline entry per
// degradare a "GD senza mod" up front.
[[nodiscard]] ModuleLoaderAvailability check_module_loader_availability(
    const IModuleLoader& moduleLoader,
    const pulse::loader::RuntimeContext& runtimeContext);

// ---------------------------------------------------------------------------
// Cablaggio del ModManager: install attribuito via finestre di epoca
// (task 7.4, Requisiti 5.4, 5.5, 5.6, 5.7, 9.1, 9.4).
//
// `ModManagerWiring` traduce ogni Mod_Id di `LoadPlan.order` in una coppia
// `EntryPointFn`/`TerminatorFn` registrata nel `ModManager`. La macchina a
// stati del manager garantisce che l'`EntryPointFn` sia invocato ESATTAMENTE
// una volta alla transizione verso `Enabled` (Req 5.4); l'`EntryPointFn`:
//   (a) apre l'epoca (`HookOwnershipLedger::openEpoch`) PRIMA del caricamento,
//       così le registrazioni `PULSE_HOOK` del Mod_Module (eseguite al `dlopen`,
//       static init) cadono nella finestra `[start, count())` (design §6);
//   (b) risolve l'entry point dichiarato via `IModuleLoader` e lo invoca UNA
//       volta (Req 5.4); un entry point non risolvibile → nessuna invocazione +
//       fallimento isolato con diagnostica che nomina Mod_Id + simbolo (Req 5.3);
//   (c) esegue `pulse::hooks::resolve_all` sui bindings del Runtime_Context
//       (Req 5.5);
//   (d) per ogni voce RISOLTA nella finestra del mod installa il detour via
//       `HookGate` — persistendo i byte originali nel `RollbackStore` con
//       `owner = Mod_Id` PRIMA dell'install (Req 8.3, 9.5) — cabla il
//       trampolino via `pulse::hooks::bind_trampoline` (Req 5.6) e attribuisce
//       l'hook al Mod_Id via `HookOwnershipLedger::attribute` (Req 5.8);
//   (e) NESSUN install su indirizzi non risolti: ogni voce non risolta della
//       finestra produce solo una diagnostica con Mod_Id + simbolo (Req 5.7,
//       9.1, 9.4).
// Il `TerminatorFn` rimuove gli hook del SOLO Mod_Id (via il backend, ripristino
// byte-esatto) e ne rilascia l'attribuzione nel ledger.
//
// I collaboratori reali (Module_Loader, backend di hooking, RollbackStore,
// ledger) sono iniettati per riferimento; il resolver dei simboli e
// l'invocatore dell'entry point sono seam iniettabili che rendono il cablaggio
// host-testabile (FakeModuleLoader, FakeBackend, resolver fittizio) senza un
// binario reale di GD. Il pieno isolamento fail-open (7.7), le transizioni
// enable/disable/re-enable (7.11), il rollback su install parziale (7.14), il
// teardown ordinato (7.17) e il cablaggio in `runtime_entry` (7.22) sono
// costruiti SOPRA questo cablaggio nelle rispettive attività.
// ---------------------------------------------------------------------------

// Lunghezza del prologo originale persistito nel RollbackStore prima di ogni
// install (coerente con il prologo modellato dal FakeBackend host-testabile).
inline constexpr std::size_t kRollbackPrologueBytes = 16;

// Specifica di cablaggio di una singola mod: Mod_Id, byte del Mod_Module
// (`code/module.pulsebin`, già verificati) e simbolo dell'entry point dichiarato
// nel Mod_Manifest da invocare all'enable.
struct ModWiringSpec {
    ModId modId;
    Bytes moduleImage;
    std::string entrySymbol;
};

class ModManagerWiring {
public:
    // Resolver dei simboli verso gli indirizzi del Runtime_Context (bindings):
    // restituisce l'indirizzo risolto della funzione bersaglio o `nullptr` se il
    // simbolo non è risolvibile. Passato a `pulse::hooks::resolve_all` (Req 5.5).
    using SymbolResolver = std::function<void*(std::string_view symbol)>;

    // Invocatore dell'entry point del Mod_Module: dato il Mod_Id e l'indirizzo
    // risolto dell'entry point, lo invoca UNA volta e ne riporta l'esito
    // (Req 5.4). Iniettabile per i test host (il default invoca l'indirizzo come
    // `void(*)()`, comportamento reale verificato in Fase E).
    using EntryPointInvoker =
        std::function<EntryPointOutcome(const ModId& modId, void* entryPoint)>;

    ModManagerWiring(IModuleLoader& moduleLoader,
                     pulse::hooking::IHookBackend& backend,
                     pulse::hooking::RollbackStore& rollback,
                     HookOwnershipLedger& ledger,
                     pulse::loader::RuntimeContext runtimeContext,
                     SymbolResolver resolver, EntryPointInvoker invoker = {},
                     pulse::loader::DiagnosticSink sink = {});

    // Registra nel `ModManager` l'`EntryPointFn` e il `TerminatorFn` del Mod_Id
    // descritto da `spec`. La mod è lasciata nello stato iniziale `Installed`:
    // l'abilitazione (e quindi l'install attribuito) avviene quando il chiamante
    // transita la mod verso `Enabled` (tipicamente via `ModManager::enableAll`).
    void registerMod(ModManager& manager, ModWiringSpec spec);

    // -----------------------------------------------------------------------
    // Barriera no-throw per-mod (task 7.7, Requisiti 6.1, 6.2, 6.3, 6.4, 6.5,
    // 6.6). Questo è il punto di ingresso che l'orchestratore
    // (`CentralizedLoader`/`pulse_loader_runtime_entry`, task 7.22) invoca per
    // abilitare le mod di `LoadPlan.order` confinando OGNI fallimento alla
    // singola mod.
    //
    // Contratto (Req 6):
    //   * abilita le mod nell'ordine `order`, isolando ogni fallimento alla sola
    //     mod (Req 6.1): modulo non caricabile → 0 hook del mod + diagnostica;
    //   * entry point in errore o che lancia un'eccezione → mod portata a
    //     `Disabled` con rollback BYTE-ESATTO degli eventuali hook già
    //     installati di quella mod via `RollbackStore`/backend (Req 6.2);
    //   * NON propaga MAI alcuna eccezione (la funzione è `noexcept`): qualunque
    //     eccezione di qualunque stadio è catturata e convertita in diagnostica
    //     con il Mod_Id quando disponibile (Req 6.3, 6.4);
    //   * carica e abilita TUTTE le mod restanti valide indipendentemente dal
    //     numero di mod fallite (Req 6.6);
    //   * gli hook delle mod abilitate con successo restano invariati: un
    //     fallimento isolato non aggiunge né rimuove hook altrui (Req 6.5).
    //
    // Guard di disponibilità del Module_Loader (task 7.23, Req 11.5): se il
    // Module_Loader iniettato riporta `available() == false`, NESSUNA mod è
    // caricata o abilitata (ritorna un `EnableAllResult` vuoto, zero install,
    // byte di eseguibile/asset invariati — Req 9.3) e viene registrata una
    // diagnostica che nomina la piattaforma del Runtime_Context e il loader.
    //
    // Restituisce l'`EnableAllResult` (mod abilitate + mod fallite, in ordine),
    // così l'orchestratore può riportare l'esito senza ispezionare il manager.
    EnableAllResult runNoThrow(ModManager& manager,
                               const std::vector<ModId>& order) noexcept;

    // -----------------------------------------------------------------------
    // Transizioni di ciclo di vita enable/disable/re-enable (task 7.11,
    // Requisiti 7.1, 7.2, 7.3, 7.4, 7.5, 7.7). Questi metodi cablano le
    // transizioni della state machine del `ModManager` con gli effetti sugli
    // hook attribuiti al Mod_Id, confinando ogni eccezione (barriera no-throw
    // dell'`EntryPointFn` per l'enable, rollback no-throw per il disable).
    //
    // `enable` — abilita (primo enable) o RI-abilita (re-enable) un Mod_Id:
    //   * primo enable (modulo non ancora caricato): apre l'epoca, esegue il
    //     `dlopen` (via Module_Loader), invoca l'entry point UNA volta (Req 7.1),
    //     esegue `resolve_all`, registra la finestra di epoca e installa i soli
    //     hook risolti → Enabled;
    //   * re-enable (modulo già caricato, Disabled): NESSUN nuovo `dlopen` (il
    //     Mod_Module resta caricato), re-invoca l'entry point, ri-esegue
    //     `resolve_all` e reinstalla via gate i SOLI hook con binding risolto
    //     RIUSANDO gli indici della finestra memorizzata → Enabled (Req 7.5);
    //   * entry point in errore/eccezione → Disabled con rollback byte-esatto
    //     degli hook eventualmente installati nel tentativo (Req 7.6, gestito
    //     dalla barriera/dal ModManager);
    //   * transizione non ammessa dalla state machine → rifiutata mantenendo
    //     stato e hook + diagnostica (Req 7.4).
    // È equivalente a `manager.enable(modId)` (l'`EntryPointFn` registrato è la
    // barriera no-throw `onEnable`), esposta qui per simmetria con `disable`.
    TransitionResult enable(ModManager& manager, const ModId& modId);

    // `disable` — disabilita un Mod_Id Enabled:
    //   * transizione ammessa Enabled→Disabled applicata: rimuove TUTTI gli hook
    //     del Mod_Id byte-esatto via `RollbackStore`/backend → Disabled, con
    //     ZERO hook del mod mantenuti mentre è Disabled (Req 7.2, 7.3); il
    //     Mod_Module NON viene scaricato e il Mod_Package resta nella
    //     Mods_Directory (Req 7.7) — il re-enable potrà riusarlo senza `dlopen`;
    //   * transizione non ammessa → rifiutata mantenendo stato e hook invariati
    //     + diagnostica del ModManager (Req 7.4): nessun hook viene rimosso.
    TransitionResult disable(ModManager& manager, const ModId& modId);

    // -----------------------------------------------------------------------
    // Teardown pulito alla chiusura del gioco (task 7.17, Requisiti 8.1, 8.2,
    // 8.3, 8.4, 8.5, 8.6, 8.7, 8.8). Costruito SOPRA `ModManager::shutdown`.
    //
    // Contratto (Req 8):
    //   * invoca il terminator di OGNI mod in stato Enabled ESATTAMENTE una
    //     volta nell'ordine ESATTAMENTE inverso rispetto a `order`
    //     (`LoadPlan.order`) — delega a `ModManager::shutdown(order)` che itera
    //     in reverse, salta le mod non Enabled e invoca ciascun terminator una
    //     sola volta (Req 8.1);
    //   * il terminator registrato (`onTerminate` → `rollbackModHooks`) rimuove
    //     TUTTI gli hook del SOLO Mod_Id via l'Hook_Engine ripristinando i byte
    //     originali byte-esatto via `RollbackStore`/backend (Req 8.2, 8.3, 8.4)
    //     e rilascia l'attribuzione nel ledger;
    //   * isola ogni fallimento di rimozione del singolo hook (terminator che
    //     "fallisce") e prosegue: dopo l'invocazione del terminator riesegue
    //     comunque `rollbackModHooks` (idempotente) così TUTTI gli hook del
    //     Mod_Id sono rimossi e l'attribuzione rilasciata anche in caso di
    //     fallimento parziale, senza interrompere il teardown delle restanti
    //     mod (Req 8.5);
    //   * a hook rimossi, scarica il Mod_Module via il `Module_Loader`
    //     (`unload`, Req 8.7) e porta la mod allo stato `Removed`
    //     (Enabled→Disabled→Removed, le sole transizioni ammesse, Req 8.8);
    //   * al termine ZERO hook attribuiti ad alcuna mod restano installati nel
    //     ledger (Req 8.6).
    //
    // È `noexcept`: nessuna eccezione lascia mai il teardown (la sequenza di
    // chiusura del processo non deve poter propagare). Restituisce gli Mod_Id
    // di cui è stato invocato il terminator, nell'ordine di invocazione
    // (reverse load order), così i chiamanti/test possono verificarne l'ordine.
    std::vector<ModId> teardown(ModManager& manager,
                                const std::vector<ModId>& order) noexcept;

    // --- Introspezione (diagnostica/test) ---------------------------------

    // Collega un `DiagnosticLedger` osservabile (task 7.19, Req 10.3): quando
    // impostato, ogni install/remove di hook nei percorsi del cablaggio
    // (`installWindow`/`rollbackModHooks`) emette un `HookEvent` con Mod_Id,
    // simbolo e tipo di operazione. Nullable: se non impostato, l'emissione
    // degli eventi è disattivata (le diagnostiche testuali sul `sink` restano
    // invariate). Il ledger è di proprietà del chiamante (riferimento debole).
    void setDiagnosticLedger(DiagnosticLedger* diagnostics) noexcept {
        diagnostics_ = diagnostics;
    }

    // Collega l'orchestratore di catena del Hook_Chaining (opzionale, task 7.5).
    // Quando impostato, `installWindow`/`rollbackModHooks` instradano l'install
    // e la rimozione degli hook degli mod ATTRAVERSO la `HookChainRegistry`:
    // più Hook_Link sullo stesso Target_Address coesistono su UN'UNICA
    // Underlying_Installation (il primo anello crea l'install reale + Head_Thunk,
    // i successivi solo ri-cablano gli slot dei vicini), così la demo interna e
    // le mod esterne che hookano la stessa funzione non confliggono (nessun
    // secondo `DobbyHook`, nessun `codice -1` — Req 8.1-8.4). La rimozione passa
    // per `removeOwner` (relink finché restano anelli; uninstall byte-esatto solo
    // sull'ultimo). Quando NON impostato (default, test host), il cablaggio usa
    // il percorso diretto `HookGate`/`RollbackStore` storico, invariato. Il
    // registry è di proprietà del chiamante (riferimento debole, lifetime di
    // processo nel Loader_Artifact).
    void setChainRegistry(pulse::hooking::HookChainRegistry* chainRegistry) noexcept {
        chainRegistry_ = chainRegistry;
    }

    // True se il Mod_Module del Mod_Id risulta caricato (handle valido).
    [[nodiscard]] bool moduleLoaded(const ModId& modId) const;

    // Disponibilità del Module_Loader iniettato (Req 11.5): inoltra
    // `IModuleLoader::available()`. Quando è `false`, `runNoThrow` non tenta
    // alcun caricamento (zero mod, zero install, byte invariati) e registra una
    // diagnostica che nomina la piattaforma del Runtime_Context. Esposto qui
    // così il pipeline entry può interrogarlo direttamente sul cablaggio.
    [[nodiscard]] bool moduleLoaderAvailable() const noexcept;

    // Numero di volte in cui l'entry point del Mod_Id è stato invocato (Req 5.4).
    [[nodiscard]] int entryPointInvocations(const ModId& modId) const;

    // Indici della finestra di epoca registrata per il Mod_Id (vuoto se la mod
    // non è mai stata abilitata con successo).
    [[nodiscard]] std::pair<std::size_t, std::size_t> epochWindow(
        const ModId& modId) const;

private:
    // Stato runtime per-mod, persistente tra enable/disable (riuso del modulo
    // caricato per il re-enable, task 7.11). In questo task viene popolato al
    // primo enable e usato dal terminator.
    struct ModRuntimeState {
        ModWiringSpec spec;
        ModuleHandle handle{};
        bool loaded{false};
        int entryInvocations{0};
        std::size_t windowStart{0};
        std::size_t windowEnd{0};
        // True dopo il PRIMO enable andato a buon fine (finestra di epoca
        // registrata e hook installati): distingue il primo enable dal
        // re-enable, così quest'ultimo riusa gli indici della finestra
        // memorizzata senza un nuovo `dlopen` (Req 7.5).
        bool windowRecorded{false};
    };

    // Azione di enable di un Mod_Id (corpo dell'EntryPointFn). Vedi descrizione
    // della classe per la sequenza (a)…(e). È una **barriera no-throw**: il
    // corpo effettivo (`onEnableImpl`) è racchiuso in un try/catch che converte
    // qualunque eccezione in `EntryPointOutcome::failure` con diagnostica
    // attribuita al Mod_Id e ripristina byte-esatto gli eventuali hook già
    // installati del mod (Req 6.2, 6.3, 6.4): nessuna eccezione lascia mai
    // l'`EntryPointFn`.
    EntryPointOutcome onEnable(const ModId& modId);

    // Corpo effettivo dell'enable (può lanciare): caricamento del Mod_Module,
    // risoluzione+invocazione dell'entry point, `resolve_all`, install della
    // finestra. Le eccezioni sono confinate dalla barriera `onEnable`.
    EntryPointOutcome onEnableImpl(const ModId& modId);

    // Rollback BYTE-ESATTO di TUTTI gli hook attualmente attribuiti al Mod_Id
    // (Req 6.2, 7.2): rimuove ciascun hook del mod via il backend (ripristino
    // dei byte originali persistiti nel `RollbackStore` prima dell'install,
    // Req 8.3/9.5) e rilascia l'attribuzione nel ledger. Isola e prosegue se la
    // rimozione di un singolo hook fallisce. Non lancia.
    void rollbackModHooks(const ModId& modId) noexcept;

    // Azione del terminator di un Mod_Id: rimuove i suoi soli hook (byte-esatto)
    // e rilascia l'attribuzione nel ledger.
    void onTerminate(const ModId& modId);

    // Installa, per la finestra `[start, end)` del Mod_Id, i soli hook con
    // binding risolto (Req 5.6/5.7); persiste i byte originali con owner=Mod_Id
    // prima di ogni install (Req 8.3/9.5) e attribuisce l'hook (Req 5.8).
    //
    // Install TRANSAZIONALE (task 7.14, Req 9.5): se l'`HookGate::install` di un
    // binding RISOLTO fallisce dopo che almeno un hook della stessa mod è già
    // stato installato in questa finestra, l'installazione è ABORTITA — non si
    // tentano gli hook successivi — e la funzione restituisce `false`. Il
    // chiamante (`onEnableImpl`) reagisce rimuovendo byte-esatto via
    // `RollbackStore`/backend gli hook già installati di quella mod
    // (`rollbackModHooks`), confinando il fallimento al solo Mod_Id e portando
    // la mod a Disabled con zero hook; le altre mod proseguono (Req 9.5, 6.6).
    // Gli indirizzi NON risolti restano un semplice skip con diagnostica
    // (Req 5.7/9.4) e NON costituiscono un fallimento transazionale; un backend
    // non disponibile (`available()==false`) è il fail-open di Req 9.1/9.3 (0
    // hook installati, nessuna transazione da annullare). Restituisce `true` se
    // nessun install di un binding risolto è fallito.
    [[nodiscard]] bool installWindow(const ModId& modId, std::size_t start,
                                     std::size_t end);

    void emit(const std::string& message) const;

    IModuleLoader& moduleLoader_;
    pulse::hooking::IHookBackend& backend_;
    pulse::hooking::RollbackStore& rollback_;
    HookOwnershipLedger& ledger_;
    pulse::loader::RuntimeContext runtimeContext_;
    SymbolResolver resolver_;
    EntryPointInvoker invoker_;
    pulse::loader::DiagnosticSink sink_;
    std::unordered_map<ModId, ModRuntimeState> state_;
    // DiagnosticLedger osservabile per gli eventi hook (Req 10.3), opzionale.
    DiagnosticLedger* diagnostics_{nullptr};
    // Orchestratore di catena del Hook_Chaining (Req 8), opzionale: quando
    // impostato, install/rimozione passano per la catena (coesistenza su
    // un'unica Underlying_Installation per Target_Address). Riferimento debole.
    pulse::hooking::HookChainRegistry* chainRegistry_{nullptr};
};

// ---------------------------------------------------------------------------
// ModLoader — orchestratore di Layer 4 (task 7.22, Requisiti 1.1, 6.3, 9.6).
//
// È il nuovo componente che CABLA l'intera pipeline host-testabile delle mod
// esterne (discovery → dedup → validazione → compatibilità → risoluzione delle
// dipendenze → cablaggio del `ModManager` via `ModManagerWiring`) e la espone al
// `CentralizedLoader` come step di inizializzazione del runtime
// (`RuntimeInitFn`). NON rimpiazza il `CentralizedLoader`: gli fornisce l'init
// step, ereditandone gratis il watchdog/timeout (10 s) e la degradazione "GD
// senza mod" su iniezione fallita o timeout (design §1).
//
// Demo interna (osservabilità `pulse-gd-integration` invariata): all'avvio della
// pipeline il ledger esegue il seeding della demo (`seedBuiltinDemo`), così le
// registrazioni `PULSE_HOOK` preesistenti del Loader_Artifact (la demo
// `menulayer_init_hook`, registrata allo static-init) sono attribuite al Mod_Id
// riservato `kBuiltinDemoModId` PRIMA di aprire qualunque epoca di mod esterna
// (Req 9.6). La demo NON viene rimossa: resta un "mod" di prima classe del
// ledger.
//
// Fail-open assoluto (Req 6.3, 6.4): né `run` né l'init step prodotto da
// `asInitStep` propagano MAI un'eccezione; ogni fallimento di qualunque stadio è
// confinato e registrato in diagnostica. L'init step riporta sempre il numero di
// hook installati al termine (Req 9.6) e lascia GD proseguire.
//
// Seam iniettabili (host-testabilità): il `PackageOpener` (apertura dei
// container `.pulse` della discovery) e, opzionalmente, il `DirectoryLister`, il
// `SymbolResolver` (risoluzione dei simboli bersaglio del Runtime_Context) e
// l'`EntryPointInvoker` (invocazione dell'entry point del Mod_Module) sono
// iniettabili così l'intera pipeline gira in CI con `FakeModuleLoader`/
// `FakeBackend`/filesystem virtuale, senza un binario reale di GD. Nel
// Loader_Artifact reale (`runtime_entry.cpp`) il resolver è cablato sui bindings
// `.pbind` verificati e l'invocatore usa il default (invoca l'indirizzo come
// `void(*)()`, comportamento verificato in Fase E).
// ---------------------------------------------------------------------------

// Esito complessivo di una esecuzione del Mod_Loader (per diagnostica/test).
struct ModLoadOutcome {
    std::vector<ModId> loaded;               // Mod_Id caricati (Req 10.1, una sola volta)
    std::vector<DiagnosticEntry> excluded;   // mod escluse + causa (Req 10.2)
    std::vector<DiagnosticEntry> isolated;   // mod isolate (fail-open) + causa (Req 10.2)
    std::size_t installedHooks{0};           // hook attivi al termine (Req 9.6)
};

class ModLoader {
public:
    // Costruisce con il `RuntimeContext` rilevato dal Mach-O e i collaboratori
    // RIUSATI iniettati per riferimento (devono sopravvivere al ModLoader).
    ModLoader(const pulse::loader::RuntimeContext& ctx,
              IModuleLoader& moduleLoader,
              pulse::hooking::IHookBackend& backend,
              pulse::hooking::RollbackStore& rollback,
              pulse::loader::DiagnosticSink sink);

    // --- Seam iniettabili (impostare PRIMA di run/asInitStep) --------------

    // Apertura dei container `.pulse` della discovery (Req 1.2). Senza opener
    // nessuna mod è riconoscibile: la pipeline scopre zero mod (fail-open).
    void setPackageOpener(PackageOpener opener) { opener_ = std::move(opener); }

    // Enumeratore di primo livello della Mods_Directory (Req 1.1). Default:
    // `default_directory_lister` (std::filesystem). Iniettabile per i test su
    // filesystem virtuale.
    void setDirectoryLister(DirectoryLister lister) {
        lister_ = std::move(lister);
    }

    // Resolver dei simboli bersaglio verso gli indirizzi del Runtime_Context
    // (Req 5.5). Default: nessun simbolo risolto (zero install — coerente con il
    // fail-open "GD senza mod"). Nel runtime reale è cablato sui bindings.
    void setSymbolResolver(ModManagerWiring::SymbolResolver resolver) {
        resolver_ = std::move(resolver);
    }

    // Invocatore dell'entry point del Mod_Module (Req 5.4). Default: invoca
    // l'indirizzo come `void(*)()`. Iniettabile per i test host.
    void setEntryPointInvoker(ModManagerWiring::EntryPointInvoker invoker) {
        invoker_ = std::move(invoker);
    }

    // Orchestratore di catena del Hook_Chaining (opzionale, task 7.5). Quando
    // impostato, il cablaggio (`ModManagerWiring`) instrada install/rimozione
    // degli hook delle mod ATTRAVERSO la catena, così demo interna e mod esterne
    // che hookano la stessa funzione coesistono sull'unica Underlying_Installation
    // (Req 8). Va impostato PRIMA di `run`/`asInitStep`: il puntatore è inoltrato
    // al `ModManagerWiring` quando viene costruito. Riferimento debole (lifetime
    // di processo nel Loader_Artifact).
    void setChainRegistry(pulse::hooking::HookChainRegistry* chainRegistry) {
        chainRegistry_ = chainRegistry;
        if (wiring_) wiring_->setChainRegistry(chainRegistry_);
    }

    // Pipeline completa host-testabile (Req 1.1, 4, 5, 6, 9, 10): discovery →
    // dedup → validazione → compatibilità → risoluzione dipendenze → cablaggio
    // ed enable via `ModManagerWiring::runNoThrow`, dopo aver attribuito le
    // registrazioni della demo al Mod_Id riservato (`seedBuiltinDemo`). NON
    // propaga eccezioni (Req 6.3, 6.4): ogni fallimento è confinato e
    // registrato. Restituisce l'esito complessivo (caricate/escluse/isolate +
    // hook installati). Idempotente sul seeding della demo.
    ModLoadOutcome run(const std::filesystem::path& modsDir);

    // Adattatore verso il `CentralizedLoader`: incapsula `run()` in un
    // `RuntimeInitFn` che riporta `installedHooks` e NON lancia mai (Req 6.3,
    // 6.4, 9.6). L'esito è sempre "mod caricate" (fail-open: anche con zero mod
    // GD prosegue, ereditando il watchdog/timeout del CentralizedLoader).
    [[nodiscard]] pulse::loader::RuntimeInitFn asInitStep(
        const std::filesystem::path& modsDir);

    // Teardown ordinato alla chiusura del gioco (Req 8): shutdown in ordine
    // inverso rispetto al `LoadPlan.order` dell'ultima `run`, rimozione hook per
    // owner byte-esatta, unload, Removed. `noexcept` (delega a
    // `ModManagerWiring::teardown`).
    void teardown() noexcept;

    // --- Introspezione (diagnostica/test) ---------------------------------

    // Numero di hook attualmente attribuiti come installati (Req 9.6): a regime
    // è l'unione degli hook delle mod Enabled (compresa la demo, se attribuita).
    [[nodiscard]] std::size_t installedHooks() const {
        return ledger_.allInstalled().size();
    }

    // Ledger di proprietà degli hook (per il seeding/attribuzione della demo dal
    // Loader_Artifact e per l'ispezione nei test).
    [[nodiscard]] HookOwnershipLedger& ledger() noexcept { return ledger_; }
    [[nodiscard]] const HookOwnershipLedger& ledger() const noexcept {
        return ledger_;
    }

    // Registro diagnostico osservabile (partizione esiti + eventi hook).
    [[nodiscard]] DiagnosticLedger& diagnostics() noexcept { return diagnostics_; }
    [[nodiscard]] const DiagnosticLedger& diagnostics() const noexcept {
        return diagnostics_;
    }

    // State machine del ciclo di vita delle mod cablata dalla pipeline.
    [[nodiscard]] ModManager& manager() noexcept { return manager_; }

    // Ordine di caricamento risolto dall'ultima `run` (per il teardown/test).
    [[nodiscard]] const std::vector<ModId>& loadOrder() const noexcept {
        return loadOrder_;
    }

private:
    void emit(const std::string& message) const;

    pulse::loader::RuntimeContext ctx_;
    IModuleLoader& moduleLoader_;
    pulse::hooking::IHookBackend& backend_;
    pulse::hooking::RollbackStore& rollback_;
    pulse::loader::DiagnosticSink sink_;

    // Seam iniettabili (vedi setter).
    PackageOpener opener_;
    DirectoryLister lister_;
    ModManagerWiring::SymbolResolver resolver_;
    ModManagerWiring::EntryPointInvoker invoker_;
    // Orchestratore di catena del Hook_Chaining (Req 8), opzionale: inoltrato al
    // `wiring_` quando costruito. Riferimento debole.
    pulse::hooking::HookChainRegistry* chainRegistry_{nullptr};

    // Collaboratori di proprietà del ModLoader, persistenti tra run e teardown.
    HookOwnershipLedger ledger_;
    DiagnosticLedger diagnostics_;
    ModManager manager_;
    std::unique_ptr<ModManagerWiring> wiring_;
    std::vector<ModId> loadOrder_;
    bool demoSeeded_{false};
};

}  // namespace pulse::lifecycle

#endif  // PULSE_LOADER_LIFECYCLE_MOD_LOADER_HPP
