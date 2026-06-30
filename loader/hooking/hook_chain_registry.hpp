// loader/hooking/hook_chain_registry.hpp — orchestratore di catena del
// Hook_Chaining (Layer 3 — Hooking Engine, Requisiti 1, 4, 5, 7, 11).
//
// La HookChainRegistry è l'**unico orchestratore nuovo** introdotto dal
// Hook_Chaining: occupa il Layer 3 accanto a `HookChain`/`HookGate`/
// `RollbackStore` e diventa l'unico punto che possiede, per ogni
// Target_Address, *una* `HookChain` + *una* Underlying_Installation + *un*
// Head_Thunk. Sostituisce il punto di cablaggio in cui `ModLoader`/
// `runtime_entry` chiamavano `HookGate::install` una volta per registrazione:
// ora il **primo** anello per indirizzo crea l'installazione reale e
// l'Head_Thunk (transizione 0→1), mentre gli anelli successivi si limitano a
// **ri-cablare gli slot** dei vicini (relinking) e ad attribuirsi via
// `HookOwnershipLedger`. Il chaining si ottiene quindi **senza** una seconda
// `DobbyHook` (nessun `codice -1`), senza riscrivere il prologo né mutare il
// Rollback_Store.
//
// Questo header definisce lo **scheletro** (task 3.1): i tipi di supporto
// (`LinkSpec`, `ChainOpOutcome`, `ChainOpResult`), la struttura interna
// `ChainSlot` e le dichiarazioni dei metodi pubblici/privati della Registry.
// Le implementazioni complete arrivano nei task successivi:
//   * insertLink 0→1 (install via Hook_Gate)         — task 3.2;
//   * insertLink n→n+1 (relinking dei vicini)         — task 3.3;
//   * invarianti di catena                            — task 3.4;
//   * arnia di dispatch simulata                      — task 3.5;
//   * rimozione/attribuzione/teardown/re-enable       — Fase C (task 5.x);
//   * wiring Gate/registro/diagnostica                — Fase D (task 7.x).
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).
#ifndef PULSE_LOADER_HOOKING_HOOK_CHAIN_REGISTRY_HPP
#define PULSE_LOADER_HOOKING_HOOK_CHAIN_REGISTRY_HPP

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "bindings/bindings.hpp"
#include "hooking/head_thunk.hpp"
#include "hooking/hook_backend.hpp"
#include "hooking/hook_chain.hpp"
#include "hooking/rollback_store.hpp"
#include "lifecycle/hook_ownership.hpp"

namespace pulse::hooking {

// Alias locale verso il modello dati del Bindings System (Layer 2), così i
// riferimenti `bindings::...` risolvono anche dentro `pulse::hooking`.
namespace bindings = ::pulse::loader::bindings;

// ---------------------------------------------------------------------------
// LinkSpec — descrittore di un Hook_Link da ammettere in catena.
//
// Estende le informazioni di ordinamento (priority, loadOrder) con il detour
// grezzo e lo slot del trampolino `pulse_original` della registrazione SDK,
// così la Registry può ri-cablare gli slot senza conoscere la firma del
// bersaglio (Req 2.1, 2.2, 6.1, 11.1).
// ---------------------------------------------------------------------------
struct LinkSpec {
    ModId        owner;                            // Mod_Id proprietario (attribuzione, Req 6)
    std::string  symbol;                           // simbolo bersaglio (diagnostica, Req 11)
    int          priority{kHookPriorityDefault};   // 0..1000 (clamp, Req 2.3-2.5)
    uint64_t     loadOrder{0};                     // tie-break deterministico (Req 2.1, 2.2)
    void*        detour{nullptr};                  // detour grezzo (HookRegistration.detour)
    void**       slot{nullptr};                    // Trampoline_Slot pulse_original (HookRegistration.trampoline)
};

// ---------------------------------------------------------------------------
// ChainOpOutcome — esito di una operazione di catena (diagnostica/test, senza
// eccezioni).
// ---------------------------------------------------------------------------
enum class ChainOpOutcome {
    InsertedHead,        // anello inserito come nuovo Chain_Head
    InsertedMiddle,      // anello inserito tra predecessore e successore
    InsertedTail,        // anello inserito in coda
    CreatedInstall,      // 0→1: creata l'unica Underlying_Installation via gate
    RemovedKeepInstall,  // rimosso un anello, install mantenuta (Req 5.5)
    RemovedLastInstall,  // 1→0: rimossa l'install + restore byte-esatto (Req 5.3, 5.4)
    Rejected,            // gate negato / install fallita: nessun effetto (Req 1.8)
};

// ---------------------------------------------------------------------------
// ChainOpResult — risultato di una operazione di catena.
// ---------------------------------------------------------------------------
struct ChainOpResult {
    ChainOpOutcome     outcome{ChainOpOutcome::Rejected};
    HookError          error{};        // valorizzato su Rejected/fallimento
    std::vector<ModId> chainOrder;     // Chain_Head → coda dopo l'operazione (Req 11.4)
    std::uintptr_t     target{0};      // Target_Address dell'operazione (diagnostica teardown, Req 7.6)
};

// ---------------------------------------------------------------------------
// ChainOrderView — vista diagnostica osservabile del Chain_Order di un target
// (Req 11.4).
//
// Sequenza di Mod_Id dal Chain_Head (`[0]`) alla coda (`[n-1]`), derivata da
// `chain.orderedNodes()` mappando `node.owner`. È un alias di `std::vector<ModId>`
// (nessun overhead, nessuna rottura ABI): rende esplicito nel tipo di ritorno di
// `chainOrder()` che la sequenza restituita è il Chain_Order osservabile dalla
// testa alla coda, coerentemente con il design.
using ChainOrderView = std::vector<ModId>;   // [head, ..., tail]

// ---------------------------------------------------------------------------
// HookChainRegistry — orchestratore di catena per-Target_Address.
// ---------------------------------------------------------------------------
class HookChainRegistry {
public:
    HookChainRegistry(IHookBackend& backend,
                      RollbackStore& rollback,
                      pulse::lifecycle::HookOwnershipLedger& ledger,
                      HookEventSink log = nullptr);

    // Ammette/inserisce un anello sul Target_Address. Sul PRIMO anello
    // (transizione 0→1) ammette via Hook_Gate (binding risolto + available()),
    // persiste i byte originali nel Rollback_Store PRIMA dell'install, esegue
    // l'unica DobbyHook verso l'Head_Thunk stabile e cabla currentHead +
    // l'ultimo slot al trampolino reale (Req 1.6). Sugli anelli successivi NON
    // tocca il backend: ri-cabla i soli slot dei vicini (Req 1.2, 4.2) e, se la
    // testa cambia, aggiorna currentHead (Req 4.3). Attribuisce l'anello al
    // Mod_Id nel ledger (Req 6.1) ed emette diagnostica (Req 11.1, 11.3).
    //
    // SCHELETRO (task 3.1): implementazione in task 3.2 (0→1) e 3.3 (n→n+1).
    ChainOpResult insertLink(std::uintptr_t target,
                             const bindings::FunctionBinding& binding,
                             const LinkSpec& link);

    // Rimuove tutti gli anelli del Mod_Id da tutte le catene (rimozione
    // selettiva, Req 6.2/6.4). Per ciascuna catena: se restano anelli, ri-cabla
    // i vicini e mantiene l'install (Req 5.1, 5.5); se era l'ultimo anello,
    // rimuove l'unica install dal backend e ripristina i byte originali
    // byte-esatti via Rollback_Store (Req 5.3, 5.4). Rilascia l'attribuzione
    // del solo Mod_Id (Req 6.4). Emette diagnostica (Req 11.2, 11.3).
    //
    // SCHELETRO (task 3.1): implementazione in Fase C (task 5.x).
    std::vector<ChainOpResult> removeOwner(const ModId& owner);

    // Smonta TUTTE le catene rimuovendo gli anelli delle mod Enabled nell'ordine
    // fornito (il chiamante passa l'inverso di LoadPlan.order, Req 7.1). Isola i
    // fallimenti per Mod_Id+Target_Address e prosegue (Req 7.6). Al termine non
    // resta alcuna install né alcun anello (Req 7.4).
    //
    // SCHELETRO (task 3.1): implementazione in Fase C (task 5.5).
    void teardown(const std::vector<ModId>& reverseOrder);

    // Vista osservabile del Chain_Order corrente di un target come sequenza di
    // Mod_Id, dal Chain_Head alla coda (Req 11.4). Restituisce una ChainOrderView
    // (alias di std::vector<ModId>): vuota se il target non possiede una catena.
    [[nodiscard]] ChainOrderView chainOrder(std::uintptr_t target) const;

    // Introspezione/test.
    [[nodiscard]] std::size_t installCount() const noexcept;   // # Underlying_Installation attive
    [[nodiscard]] std::size_t chainSize(std::uintptr_t target) const;
    [[nodiscard]] bool hasInstall(std::uintptr_t target) const;

    // Testa corrente (`currentHead`) di un target: il detour del Chain_Head verso
    // cui l'Head_Thunk salta (Req 3.1, 4.3, 5.2). nullptr se il target non ha
    // catena. Introspezione per diagnostica/test del relinking della testa.
    [[nodiscard]] void* currentHead(std::uintptr_t target) const;

    // Real_Trampoline di un target: l'indirizzo restituito dall'unica DobbyHook
    // verso cui lo slot dell'ultimo anello (coda) inoltra per raggiungere il
    // Real_Original (Req 3.4). nullptr se il target non ha una
    // Underlying_Installation attiva. Introspezione per l'arnia di dispatch
    // host (task 3.5), che riconosce la coda confrontando `*slot` con questo
    // valore per cedere al Real_Original simulato.
    [[nodiscard]] void* realTrampoline(std::uintptr_t target) const;

private:
    // -----------------------------------------------------------------------
    // ChainSlot — stato interno per un singolo Target_Address (Req 1, 4, 5).
    //
    // Invarianti mantenuti dalla Registry (vedi design "Data Models"):
    //   installed == (chain non vuota)                                 (Req 1.3, 1.4, 5.5)
    //   installed ⇒ head.currentHead == orderedNodes().front().detour  (Req 3.1, 4.3, 5.2)
    //   per ogni i<n-1: *orderedNodes()[i].slot == [i+1].detour         (Req 4.2, 5.1)
    //   *orderedNodes().back().slot == realTrampoline                   (Req 3.4)
    //   # DobbyHook su address == (installed ? 1 : 0)                   (Req 1.3, 1.7)
    // -----------------------------------------------------------------------
    struct ChainSlot {
        HookChain      chain;                  // riuso: ordinamento priority DESC, loadOrder ASC
        HeadCell       head;                   // currentHead atomico (letto dall'Head_Thunk)
        void*          headThunk{nullptr};     // bersaglio dell'unica DobbyHook
        Trampoline     realTrampoline{};       // restituito dalla DobbyHook (coda → Real_Original)
        bool           installed{false};       // true sse esiste l'Underlying_Installation
        std::uintptr_t address{0};             // Target_Address (chiave)
        // I byte originali sono nel RollbackStore (owner = Mod_Id del primo anello).
    };

    // Operazioni interne (implementate nei task 3.2-3.5 / Fase C). Dichiarate
    // qui per fissare lo scheletro della Registry.
    //
    //   installFirst()   — transizione 0→1: ammissione via gate + DobbyHook
    //                       verso l'Head_Thunk + persistenza byte (task 3.2);
    //   relinkNeighbors() — n→n+1 / rimozione non-ultima: ri-cabla i soli slot
    //                       dei vicini (task 3.3 / 5.1);
    //   removeLast()      — transizione 1→0: backend.remove + restore byte-esatto
    //                       (Fase C, task 5.2);
    //   rewireHead()      — aggiorna currentHead al cambio di Chain_Head
    //                       (task 3.3 / 5.1).

    // Transizione 0→1 (task 3.2): crea l'unica Underlying_Installation per il
    // Target_Address. Ammette il PRIMO anello via `HookGate` (binding risolto +
    // `available()`), persiste gli `originalBytes` del prologo nel RollbackStore
    // con `owner = Mod_Id` PRIMA dell'install, esegue l'unica DobbyHook verso
    // l'Head_Thunk stabile, ottiene il Real_Trampoline e cabla
    // `currentHead = L0.detour` + `*L0.slot = Real_Trampoline`. Se il gate nega
    // o l'install fallisce: nessuna catena, nessuna install parziale, diagnostica
    // con Mod_Id + Target_Address (Req 1.3, 1.6, 1.8, 5.4).
    ChainOpResult installFirst(std::uintptr_t target,
                               const bindings::FunctionBinding& binding,
                               const LinkSpec& link);

    // Transizione n→n+1 (task 3.3): inserisce un anello su un Target_Address che
    // possiede già una Underlying_Installation attiva. `HookChain::add` posiziona
    // il nodo secondo il Chain_Order (priority DESC, loadOrder ASC); la Registry
    // ri-cabla i SOLI Trampoline_Slot dei vicini — `*pred.slot = nuovo.detour`,
    // `*nuovo.slot = succ.detour` (o `Real_Trampoline` se il nuovo è la coda) — e,
    // se il nuovo anello è la testa, aggiorna `currentHead` con una singola
    // scrittura atomica. NESSUNA chiamata al backend (nessun errore "indirizzo già
    // hookato"), prologo e Rollback_Store invariati (Req 1.2, 1.7, 4.1, 4.2, 4.3).
    ChainOpResult insertSubsequent(ChainSlot& slot,
                                   std::uintptr_t target,
                                   const bindings::FunctionBinding& binding,
                                   const LinkSpec& link);

    // Rimozione di un anello non-ultimo con relinking dei vicini, SENZA
    // disinstallazione (task 5.1, Req 5.1, 5.2, 5.6). Rimuove dalla `slot.chain`
    // tutti gli anelli del `owner` via `HookChain::remove` (che preserva l'ordine
    // relativo dei restanti) e ri-cabla la catena rimasta:
    //   * `currentHead = nuovo Chain_Head.detour` se la testa è cambiata (Req 5.2);
    //   * per ogni anello non-coda `*slot = detour del successore`, e per la coda
    //     `*slot = Real_Trampoline` (Req 5.1).
    // Per la rimozione di un singolo anello questo equivale a toccare i soli
    // puntatori dei vicini — `*pred.slot = succ.detour` (o `Real_Trampoline` se
    // il predecessore diventa coda) — perché gli slot degli altri anelli puntano
    // già al successore corretto e la riscrittura è idempotente. NESSUNA chiamata
    // al backend, NESSUN ripristino dei byte originali: la Underlying_Installation
    // resta attiva (Req 5.5). PRECONDIZIONE: invocata solo quando, dopo la
    // rimozione, resta almeno un anello (keep-install); la transizione 1→0
    // (ultimo anello) è gestita altrove (task 5.2).
    void relinkAfterRemoval(ChainSlot& slot, const ModId& owner);

    // Variante non-locking di `removeOwner` (task 5.5). Contiene tutta la logica
    // di rimozione selettiva per Mod_Id (relinking dei vicini / transizione 1→0
    // + rilascio dal ledger) SENZA acquisire il `mutex_`. È invocata sia da
    // `removeOwner` (che acquisisce il lock una volta) sia da `teardown` (che
    // detiene già il lock per l'intera sequenza): evita il doppio lock dello
    // stesso `std::mutex` non ricorsivo (deadlock). PRECONDIZIONE: il chiamante
    // detiene `mutex_`.
    std::vector<ChainOpResult> removeOwnerLocked(const ModId& owner);

    // Transizione 1→0 (task 5.2, Req 5.3, 5.4): rimozione dell'ULTIMO anello
    // rimasto su un Target_Address. Rimuove gli anelli del `owner` dalla catena
    // (che diventa vuota), rimuove l'unica Underlying_Installation dal backend
    // (`backend_.remove(target)` — l'unica DobbyHook, Req 5.3) e ripristina i
    // byte originali del prologo in modo byte-esatto tramite il Rollback_Store
    // (`RollbackStore::restore` del record persistito alla transizione 0→1,
    // Req 5.4). Il write-back dei byte originali è instradato attraverso il
    // backend. Su fallimento della rimozione isola la causa e la riporta
    // (ChainOpOutcome::Rejected con l'errore), senza dismettere il ChainSlot.
    // Su successo segna lo slot come non installato (la dismissione effettiva
    // dalla mappa `chains_` è eseguita dal chiamante `removeOwner`). NON tocca
    // il HookOwnershipLedger (rilascio selettivo cablato in task 5.3).
    // PRECONDIZIONE: invocata solo quando, dopo la rimozione, NON resta alcun
    // anello (transizione 1→0); il caso keep-install è gestito da
    // `relinkAfterRemoval` (task 5.1). Emette diagnostica della rimozione anello
    // + rimozione install (Req 11.2, 11.3).
    ChainOpResult removeLast(ChainSlot& slot, std::uintptr_t target,
                             const ModId& owner);

    // Verifica strutturale degli invarianti di catena di un ChainSlot (task
    // 3.4). Codifica, come predicato controllabile, gli invarianti documentati
    // sopra su ChainSlot e usato come guardia (sotto `assert`) al termine di
    // ogni operazione mutativa così che una regressione futura li violi
    // rumorosamente nelle build di debug/test:
    //   installed == (chain non vuota)                                 (Req 1.3, 1.4, 5.5)
    //   installed ⇒ currentHead == orderedNodes().front().detour       (Req 3.1, 4.3, 5.2)
    //   per ogni i<n-1: *orderedNodes()[i].slot == [i+1].detour          (Req 4.2, 5.1)
    //   *orderedNodes().back().slot == realTrampoline                   (Req 3.4)
    // Gli slot nulli (anelli host di solo ordinamento) sono ignorati nel
    // controllo del relinking. Non muta alcuno stato (const, noexcept).
    [[nodiscard]] bool slotInvariantsHold(const ChainSlot& slot) const noexcept;

    // Conta le Underlying_Installation attive SENZA acquisire il `mutex_`
    // (variante non-locking di `installCount`). Usata da `teardown`, che detiene
    // già il lock, per la diagnostica finale senza incorrere in un doppio lock.
    [[nodiscard]] std::size_t installCountLocked() const noexcept;

    // Emette un evento diagnostico sul sink, se presente (Req 11.x).
    void emit(const std::string& message) const {
        if (log_) {
            log_(message);
        }
    }

    IHookBackend&                            backend_;
    RollbackStore&                           rollback_;
    pulse::lifecycle::HookOwnershipLedger&   ledger_;
    HookEventSink                            log_;
    std::unordered_map<std::uintptr_t, ChainSlot> chains_;
    mutable std::mutex                       mutex_;  // serializza la mutazione strutturale (Req 9.3)
};

}  // namespace pulse::hooking

#endif  // PULSE_LOADER_HOOKING_HOOK_CHAIN_REGISTRY_HPP
