// loader/hooking/hook_chain.hpp — catena di hook ordinata (Layer 3 — Hooking
// Engine, Requisiti 3.1, 3.2, 3.3).
//
// Questo header definisce `HookNode` (un singolo hook installato da una mod su
// una funzione bersaglio) e `HookChain`, la struttura dati che mantiene gli
// hook concatenati sulla stessa funzione in un ordine deterministico e
// ripetibile (Requisito 3.3).
//
// Ordinamento della catena (Requisito 3.2/3.3):
//   * chiave primaria: `priority` DECRESCENTE — il gestore con Hook_Priority
//     più alto viene eseguito per primo;
//   * tie-break: `loadOrder` CRESCENTE — a parità di priorità l'ordine segue
//     l'ordine di caricamento risolto dal Dependency Resolver, garantendo la
//     stessa sequenza tra esecuzioni ripetute con lo stesso insieme di mod.
//
// La struttura dati e l'ordinamento (task 6.1) sono implementati da `add`
// (inserimento ordinato), `remove` (rimozione selettiva per owner) e
// `orderedNodes` (vista ordinata). L'esecuzione della catena (task 6.3) è
// realizzata da `HookChain::dispatch` insieme a `HookContext::callNext()`:
//   * ogni gestore riceve un `HookContext&` e invoca `callNext()` per passare
//     il controllo al gestore successivo; l'ultimo `callNext()` invoca il
//     trampolino della funzione originale (Req 3.1);
//   * se un gestore ritorna SENZA invocare `callNext()`, la catena si
//     interrompe: i gestori rimanenti e l'originale non vengono eseguiti e si
//     registra un evento di interruzione con mod + funzione (Req 3.4);
//   * se un gestore termina in modo anomalo (eccezione), l'errore è isolato,
//     si registra un evento di errore con mod + funzione e la catena PROSEGUE
//     invocando il gestore successivo (Req 3.5).
//
// Logica originale Pulse (Requisito 27), indipendente dal backend di hooking.
// Stack: C++20/23 (Requisito 26.1).
#ifndef PULSE_LOADER_HOOKING_HOOK_CHAIN_HPP
#define PULSE_LOADER_HOOKING_HOOK_CHAIN_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulse::hooking {

// ---------------------------------------------------------------------------
// ModId — identificatore della mod proprietaria di un hook.
//
// Modellato come stringa (l'identità della mod dichiarata nel Manifest,
// Requisito 16.1). Definito qui in forma minimale per la catena di hook; le
// attività successive potranno promuoverlo a un tipo condiviso.
// ---------------------------------------------------------------------------
using ModId = std::string;

// ---------------------------------------------------------------------------
// Dominio di Hook_Priority (Requisito 3.2): intero in [0, 1000], default 500.
// ---------------------------------------------------------------------------
inline constexpr int kHookPriorityMin = 0;
inline constexpr int kHookPriorityMax = 1000;
inline constexpr int kHookPriorityDefault = 500;

// Riconduce un valore di priorità grezzo nel dominio valido [0, 1000]
// (Requisito 3.2). Valori fuori intervallo vengono saturati ai limiti, così la
// catena mantiene sempre priorità nel dominio dichiarato.
[[nodiscard]] constexpr int clampHookPriority(int priority) noexcept {
    return std::clamp(priority, kHookPriorityMin, kHookPriorityMax);
}

// HookContext è definito più in basso (dopo HookChain): un gestore lo riceve
// per riferimento e ne invoca `callNext()` per passare al gestore successivo.
class HookContext;

// ---------------------------------------------------------------------------
// HandlerFn — gestore di un hook (task 6.3).
//
// Il gestore riceve un `HookContext&` tramite cui invoca `callNext()` per
// cedere il controllo al gestore successivo della catena (o, se è l'ultimo,
// alla funzione originale). Un gestore che NON chiama `callNext()` interrompe
// la catena (Req 3.4).
//
// Il membro `handler` di `HookNode` è defaultato (std::function vuota): i nodi
// restano costruibili e ordinabili senza un gestore, così i test di
// ordinamento (task 6.1/6.2) non devono fornirne uno. In `dispatch` un nodo
// senza gestore è trattato come trasparente e cede subito al successivo.
// ---------------------------------------------------------------------------
using HandlerFn = std::function<void(HookContext&)>;

// OriginalFn — invocazione della funzione originale (trampolino) eseguita
// dall'ultimo `callNext()` della catena (Req 3.1). Modellata qui come callable
// opaco senza argomenti: la versione tipizzata per-firma vive nello SDK.
using OriginalFn = std::function<void()>;

// HookEventSink — sink degli eventi diagnostici della catena (interruzione,
// Req 3.4; errore isolato, Req 3.5). Callback leggero coerente con il
// DiagnosticSink del Loader Core; se nullo, gli eventi sono scartati.
using HookEventSink = std::function<void(std::string_view)>;

// ---------------------------------------------------------------------------
// HookNode — un singolo hook installato da una mod sulla funzione bersaglio.
// ---------------------------------------------------------------------------
struct HookNode {
    ModId    owner;                          // mod proprietaria (per rimozione selettiva, Req 2.4)
    int      priority = kHookPriorityDefault; // 0..1000, default 500 (Req 3.2)
    uint64_t loadOrder = 0;                  // tie-break deterministico (Req 3.3)
    HandlerFn handler;                       // gestore (eseguito da dispatch, riceve HookContext&)
    // --- Campi di relinking del Trampoline_Slot (Hook_Chaining, Req 2.1, 2.2,
    // 4.2, 5.1) ---
    //
    // Usati dalla HookChainRegistry per il cablaggio reale degli anelli: la
    // Registry scrive `*slot = detour del successivo` (o il trampolino reale per
    // la coda) e legge `detour` come bersaglio dell'anello. Entrambi sono
    // opzionali (default nullptr): i test di solo ordinamento e la `dispatch`
    // di simulazione restano validi senza fornirli, e `precedes`/`add`/`remove`/
    // `orderedNodes` continuano a ordinare per `priority` DESC, `loadOrder` ASC
    // ignorando `detour`/`slot`.
    void*    detour = nullptr;               // detour grezzo della registrazione SDK (HookRegistration.detour)
    void**   slot   = nullptr;               // Trampoline_Slot pulse_original (HookRegistration.trampoline)
};

// ---------------------------------------------------------------------------
// HookChain — catena ordinata di hook sulla stessa funzione (Req 3.1/3.2/3.3).
//
// Invariante mantenuta da `add`/`remove`: `nodes_` è sempre ordinato per
// priority DESC, poi loadOrder ASC. `orderedNodes()` restituisce direttamente
// questa vista, senza riordinare.
// ---------------------------------------------------------------------------
class HookChain {
public:
    // Ritorna true se `a` deve precedere `b` nella catena: priorità più alta
    // prima; a parità, loadOrder più basso prima (Req 3.2, 3.3).
    [[nodiscard]] static bool precedes(const HookNode& a, const HookNode& b) noexcept {
        if (a.priority != b.priority) {
            return a.priority > b.priority;  // priority DESC
        }
        return a.loadOrder < b.loadOrder;    // loadOrder ASC
    }

    // Inserisce un hook mantenendo l'ordinamento (Req 3.2, 3.3). La priorità
    // viene ricondotta al dominio valido [0, 1000] (Req 3.2). L'inserimento è
    // ordinato: il nodo è posizionato dopo tutti gli hook che lo precedono
    // secondo `precedes`, preservando un ordine stabile e deterministico tra
    // nodi equivalenti (stessa priority e stesso loadOrder) per ordine di
    // inserimento.
    void add(HookNode node);

    // Rimuove tutti gli hook posseduti da `owner`, preservando gli altri e il
    // loro ordine relativo (rimozione selettiva, Req 2.4). Ritorna il numero
    // di hook rimossi (0 se la mod non aveva hook in questa catena).
    std::size_t remove(const ModId& owner);

    // Vista ordinata della catena: priority DESC, poi loadOrder ASC
    // (Req 3.2, 3.3). Deterministica e ripetibile.
    [[nodiscard]] const std::vector<HookNode>& orderedNodes() const noexcept {
        return nodes_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
    [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }

    // True se `owner` possiede almeno un hook nella catena.
    [[nodiscard]] bool contains(const ModId& owner) const noexcept;

    void clear() noexcept { nodes_.clear(); }

    // Esegue la catena di hook sulla funzione `functionName` (task 6.3).
    //
    // Invoca i gestori nell'ordine deterministico di `orderedNodes()`: il primo
    // gestore è eseguito subito; ciascun gestore prosegue invocando
    // `HookContext::callNext()`. L'ultimo `callNext()` (oltre l'ultimo gestore)
    // invoca `original` — il trampolino della funzione originale (Req 3.1). Una
    // catena vuota invoca direttamente `original`.
    //
    // Comportamenti (Req 3.4/3.5):
    //   * un gestore che ritorna SENZA chiamare `callNext()` interrompe la
    //     catena (gestori rimanenti e originale NON eseguiti) e registra su
    //     `log` un evento di interruzione con mod + funzione;
    //   * un gestore che lancia un'eccezione è isolato (l'eccezione non si
    //     propaga al gioco né agli altri gestori), si registra su `log` un
    //     evento di errore con mod + funzione e la catena PROSEGUE invocando il
    //     gestore successivo.
    //
    // `log` può essere nullo (eventi scartati). `dispatch` non modifica la
    // catena ed è rieseguibile (const).
    void dispatch(std::string_view functionName,
                  const OriginalFn& original,
                  const HookEventSink& log = nullptr) const;

private:
    std::vector<HookNode> nodes_{};  // sempre ordinato per `precedes`
};

// ---------------------------------------------------------------------------
// HookContext — contesto di esecuzione di una catena, passato a ogni gestore
// (task 6.3, Req 3.1/3.4/3.5).
//
// Un gestore invoca `callNext()` per cedere il controllo al gestore successivo
// della catena; l'ultimo `callNext()` invoca il trampolino originale (Req 3.1).
// Se un gestore ritorna senza chiamare `callNext()`, la catena si interrompe
// (Req 3.4). Le eccezioni di un gestore sono isolate e la catena prosegue
// (Req 3.5).
//
// Il contesto è creato e guidato da `HookChain::dispatch`; non è copiabile e
// non è thread-safe (una catena viene eseguita su un singolo thread per volta).
// ---------------------------------------------------------------------------
class HookContext {
public:
    HookContext(const HookContext&) = delete;
    HookContext& operator=(const HookContext&) = delete;

    // Cede il controllo al gestore successivo della catena, oppure — se il
    // gestore corrente è l'ultimo — alla funzione originale (Req 3.1).
    //
    // Idempotente entro lo stesso gestore: una seconda invocazione dallo stesso
    // gestore è ignorata, così la coda della catena non viene eseguita due volte.
    // Invocata fuori da un gestore (nessun gestore attivo) è un no-op.
    void callNext();

    // Nome della funzione bersaglio in esecuzione (per diagnostica).
    [[nodiscard]] std::string_view functionName() const noexcept {
        return functionName_;
    }

    // True se il gestore attualmente in esecuzione ha già invocato `callNext()`.
    [[nodiscard]] bool nextWasCalled() const noexcept {
        return current_ != nullptr && current_->nextCalled;
    }

private:
    friend class HookChain;

    HookContext(const std::vector<HookNode>& nodes, const OriginalFn& original,
                std::string_view functionName, const HookEventSink& log) noexcept
        : nodes_(nodes),
          original_(original),
          functionName_(functionName),
          log_(log) {}

    // Stato di un gestore in esecuzione: la sua posizione nella catena e se ha
    // già ceduto al successivo. Vive sullo stack del frame `invokeLink`, così
    // l'annidamento (callNext che invoca il link successivo) è gestito tramite
    // salvataggio/ripristino di `current_`.
    struct LinkState {
        std::size_t pos;
        bool nextCalled;
    };

    // Avvia l'esecuzione dal primo link della catena.
    void run() { invokeLink(0); }

    // Esegue il link in posizione `pos`: un gestore se `pos < nodes.size()`,
    // altrimenti la funzione originale (fine catena).
    void invokeLink(std::size_t pos);

    // Inoltra un evento diagnostico al sink, se presente.
    void logEvent(const std::string& message) const {
        if (log_) {
            log_(message);
        }
    }

    const std::vector<HookNode>& nodes_;
    const OriginalFn& original_;
    std::string_view functionName_;
    const HookEventSink& log_;
    LinkState* current_{nullptr};
};

}  // namespace pulse::hooking

#endif  // PULSE_LOADER_HOOKING_HOOK_CHAIN_HPP
