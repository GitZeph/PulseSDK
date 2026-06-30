// loader/hooking/chain_dispatch_harness.hpp — arnia di dispatch simulata del
// Hook_Chaining (veicolo di test host, Requisiti 3.1, 3.2, 3.3, 3.4, 3.5, 3.6,
// 3.8, 3.9, 4.4).
//
// Questo header NON fa parte del percorso reale a runtime: è il **veicolo di
// test host** (task 3.5) che emula, in CI e senza un backend reale, il percorso
// di dispatch costruito dalla HookChainRegistry ri-cablando i Trampoline_Slot
// (task 3.1-3.4). Emula fedelmente il percorso reale descritto nel design
// ("Principio del percorso reale" e il diagramma di dispatch a 2 anelli):
//
//   invocazione → `currentHead` (detour della testa)
//               → il detour chiama `callOriginal`, che legge `*slot`
//               → detour del successore
//               → … (un anello alla volta, senza salti)
//               → ultimo anello → `*slot` == Real_Trampoline
//               → Real_Original (simulato).
//
// L'arnia segue il **cablaggio effettivo dei puntatori** prodotto dalla
// Registry: legge `currentHead` come puntatore alla testa, e per ogni anello
// `callOriginal` legge `*slot` (lo slot `pulse_original` che la Registry ha
// scritto) per trovare il puntatore successivo. Quando `*slot` coincide con il
// Real_Trampoline (introspezione `HookChainRegistry::realTrampoline`) cede al
// Real_Original simulato. In questo modo l'arnia **verifica il relinking degli
// slot prodotto dalla Registry** ed è distinta da `HookChain::dispatch`, che è
// il modello logico di ordinamento (non il percorso reale).
//
// Garanzie emulate (Requisiti 3.x, 4.4):
//   * la testa è eseguita per prima (Req 3.1);
//   * `callOriginal` cede al successivo inoltrando i medesimi parametri e
//     propaga a monte il medesimo valore di ritorno prodotto a valle (Req 3.2,
//     3.3);
//   * l'ultimo `callOriginal` raggiunge il Real_Original via trampolino (Req 3.4);
//   * ciascun anello raggiunto è eseguito al più una volta, senza salti (Req
//     3.5, 3.6, 4.4) — l'arnia segue la lista concatenata degli slot;
//   * se un anello ritorna SENZA invocare `callOriginal`, gli anelli successivi
//     e il Real_Original non sono eseguiti, il chiamante riceve il valore
//     dell'anello interruttore e si registra un evento con Mod_Id e
//     Target_Address (Req 3.8, 3.9).
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).
// Header-only host-helper: nessuna dipendenza dal backend reale.
#ifndef PULSE_LOADER_HOOKING_CHAIN_DISPATCH_HARNESS_HPP
#define PULSE_LOADER_HOOKING_CHAIN_DISPATCH_HARNESS_HPP

#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "hooking/hook_chain.hpp"  // ModId, HookEventSink
#include "hooking/head_thunk.hpp"  // load_slot (acquire) — vista coerente (Req 9.1)

namespace pulse::hooking::test {

// ---------------------------------------------------------------------------
// HarnessLink — un anello (detour) di una mod modellato come oggetto callable.
//
// Il suo indirizzo (`detour()`, cast a `void*`) è ciò che la Registry scrive in
// `currentHead` e negli slot dei vicini; `nextSlot` è il Trampoline_Slot
// (`pulse_original`) che la Registry ri-cabla (`*slot = next.detour` oppure il
// Real_Trampoline per la coda). L'arnia recupera l'anello dal `void*` e ne
// invoca la `behavior`.
//
// Firma di prova: un singolo parametro `int` inoltrato lungo la catena e un
// valore di ritorno `int` propagato a monte — sufficiente a verificare l'inoltro
// di parametri (Req 3.3) e la propagazione del ritorno (Req 3.3) senza
// vincolarsi a un'ABI reale.
// ---------------------------------------------------------------------------
struct HarnessLink {
    // callOriginal: invocata dalla `behavior` per cedere il controllo all'anello
    // successivo (o, per la coda, al Real_Original). Inoltra `arg` e restituisce
    // il valore prodotto a valle (Req 3.2, 3.3).
    using CallOriginal = std::function<int(int arg)>;

    // behavior: la logica del detour. Riceve l'argomento entrante e la
    // continuazione `callOriginal`; restituisce il valore da propagare a monte.
    // Una behavior che ritorna SENZA invocare `callOriginal` interrompe la
    // catena (Req 3.8, 3.9). Una behavior vuota è trasparente: cede subito al
    // successivo inoltrando l'argomento.
    using Behavior = std::function<int(int arg, const CallOriginal& callOriginal)>;

    ModId    owner;                // Mod_Id proprietario (diagnostica/short-circuit, Req 3.8)
    Behavior behavior;             // logica del detour (eseguita dall'arnia)
    void*    nextSlot = nullptr;   // *slot scritto dalla Registry: detour del successore o Real_Trampoline

    // Puntatore detour grezzo che la Registry memorizza in currentHead/slot.
    [[nodiscard]] void* detour() noexcept { return static_cast<void*>(this); }
    // Trampoline_Slot (`void**`) da passare come `LinkSpec.slot`.
    [[nodiscard]] void** slot() noexcept { return &nextSlot; }
};

// ---------------------------------------------------------------------------
// DispatchTrace — esito di una invocazione emulata della catena.
// ---------------------------------------------------------------------------
struct DispatchTrace {
    int                value = 0;             // valore restituito al chiamante della GD (Req 3.3, 3.9)
    std::vector<ModId> executed;              // anelli eseguiti, dalla testa in avanti (Req 3.1, 4.4)
    bool               reachedOriginal = false;  // true se il Real_Original è stato invocato (Req 3.4)
    bool               shortCircuited = false;    // true se un anello non ha invocato callOriginal (Req 3.8)
    ModId              shortCircuitOwner;     // Mod_Id dell'anello interruttore (Req 3.8)
};

// ---------------------------------------------------------------------------
// ChainDispatchHarness — driver di dispatch che segue il cablaggio della
// Registry.
//
// Costruito con il Target_Address, il valore del Real_Trampoline (da
// `HookChainRegistry::realTrampoline(target)`), il Real_Original simulato e un
// sink diagnostico opzionale. `dispatch(currentHead, arg)` emula una invocazione
// di GD che entra dal `currentHead` (anche questo da
// `HookChainRegistry::currentHead(target)`).
// ---------------------------------------------------------------------------
class ChainDispatchHarness {
public:
    ChainDispatchHarness(std::uintptr_t target,
                         void* realTrampoline,
                         std::function<int(int)> realOriginal,
                         HookEventSink log = nullptr)
        : target_(target),
          realTrampoline_(realTrampoline),
          realOriginal_(std::move(realOriginal)),
          log_(std::move(log)) {}

    // Emula una invocazione del Target_Address che entra dalla testa corrente.
    // Restituisce la traccia completa: valore finale, ordine di esecuzione,
    // raggiungimento del Real_Original ed eventuale short-circuit.
    //
    // Modello *live*: gli slot sono riletti (con load acquire) a ogni
    // `callOriginal`, seguendo il cablaggio corrente dei puntatori. È il veicolo
    // usato dai test di traversamento/short-circuit (Fase B), dove non vi sono
    // mutazioni in volo.
    DispatchTrace dispatch(void* currentHead, int arg) {
        DispatchTrace trace;
        trace.value = invokePointer(currentHead, arg, trace);
        return trace;
    }

    // Emula una invocazione che osserva una **vista coerente** del Chain_Order
    // (modello di concorrenza, Req 9.1, 9.2). La catena è "congelata" in uno
    // snapshot all'ingresso: la testa è letta **una volta** (`currentHead`,
    // letto a monte dal chiamante con semantica acquire) e gli slot sono
    // attraversati una volta per costruire la sequenza degli anelli presenti
    // all'inizio della dispatch. La successiva esecuzione inoltra lungo questo
    // snapshot, così una mutazione strutturale che avvenga **durante** la
    // dispatch (insert/remove dentro la behavior di un anello) NON altera la
    // dispatch in corso: ciascun anello presente all'inizio è eseguito al più
    // una volta e nessuno è saltato (Req 9.1), e la mutazione ha effetto solo da
    // una invocazione successiva (Req 9.2). Modella la garanzia descritta nel
    // design ("la dispatch corrente prosegue sulla vista che ha già caricato").
    DispatchTrace dispatchCoherent(void* currentHead, int arg) {
        // Snapshot all'ingresso: segui la catena esistente leggendo gli slot con
        // load acquire (simmetrico allo store release del relinking). Lo
        // snapshot è congelato PRIMA di eseguire qualunque behavior, quindi
        // immune a mutazioni successive degli slot/della testa.
        std::vector<HarnessLink*> snapshot;
        void* ptr = currentHead;
        while (ptr != nullptr && ptr != realTrampoline_) {
            auto* link = static_cast<HarnessLink*>(ptr);
            snapshot.push_back(link);
            ptr = load_slot(link->slot());
        }
        DispatchTrace trace;
        trace.value = invokeSnapshot(snapshot, 0, arg, trace);
        return trace;
    }

private:
    // Esegue la dispatch lungo lo snapshot congelato (vista coerente): l'anello
    // in posizione `index` cede al successivo dello snapshot (o al Real_Original
    // se è la coda dello snapshot), indipendentemente da mutazioni concorrenti
    // degli slot reali (Req 9.1, 9.2).
    int invokeSnapshot(const std::vector<HarnessLink*>& snapshot, std::size_t index,
                       int arg, DispatchTrace& trace) {
        if (index >= snapshot.size()) {
            // Coda dello snapshot raggiunta: cede al Real_Original via trampolino
            // (Req 3.4).
            trace.reachedOriginal = true;
            return realOriginal_ ? realOriginal_(arg) : arg;
        }

        HarnessLink* link = snapshot[index];
        trace.executed.push_back(link->owner);

        bool calledOriginal = false;
        HarnessLink::CallOriginal callOriginal = [&](int nextArg) -> int {
            calledOriginal = true;
            return invokeSnapshot(snapshot, index + 1, nextArg, trace);
        };

        const int ret = link->behavior ? link->behavior(arg, callOriginal)
                                        : callOriginal(arg);

        if (!calledOriginal) {
            trace.shortCircuited = true;
            trace.shortCircuitOwner = link->owner;
            emitShortCircuit(link->owner);
        }
        return ret;
    }

    // Segue il cablaggio effettivo dei puntatori: un `void*` è il Real_Trampoline
    // (→ Real_Original) oppure un `HarnessLink*` (detour di un anello).
    int invokePointer(void* ptr, int arg, DispatchTrace& trace) {
        if (ptr == realTrampoline_) {
            // La coda ha raggiunto il Real_Original attraverso il trampolino
            // reale (Req 3.4).
            trace.reachedOriginal = true;
            return realOriginal_ ? realOriginal_(arg) : arg;
        }

        auto* link = static_cast<HarnessLink*>(ptr);
        trace.executed.push_back(link->owner);

        // `callOriginal` legge `*slot` (link->nextSlot) per trovare il puntatore
        // successivo, esattamente come la `callOriginal` dello SDK inoltra
        // attraverso lo slot `pulse_original` (Req 3.2). Lo slot è letto con un
        // load atomico a semantica acquire, simmetrico allo store release del
        // relinking della Registry, così non si osserva mai un puntatore
        // lacerato (Req 9.1). L'anello è eseguito al più una volta: l'arnia
        // segue la lista concatenata senza ripercorrere (Req 3.5, 3.6, 4.4).
        bool calledOriginal = false;
        HarnessLink::CallOriginal callOriginal = [&](int nextArg) -> int {
            calledOriginal = true;
            return invokePointer(load_slot(link->slot()), nextArg, trace);
        };

        // Anello trasparente (nessuna behavior): cede subito al successivo
        // inoltrando l'argomento, propagando a monte il valore di ritorno.
        const int ret = link->behavior ? link->behavior(arg, callOriginal)
                                        : callOriginal(arg);

        if (!calledOriginal) {
            // Short-circuit (Req 3.8, 3.9): gli anelli successivi e il
            // Real_Original NON sono eseguiti; il chiamante riceve il valore
            // dell'anello interruttore; si registra un evento con Mod_Id e
            // Target_Address.
            trace.shortCircuited = true;
            trace.shortCircuitOwner = link->owner;
            emitShortCircuit(link->owner);
        }
        return ret;
    }

    void emitShortCircuit(const ModId& owner) const {
        if (!log_) {
            return;
        }
        std::ostringstream os;
        os << "hook-chaining: catena interrotta da mod '" << owner << "' @ 0x"
           << std::hex << target_ << " (anello senza callOriginal)";
        log_(os.str());
    }

    std::uintptr_t            target_;
    void*                     realTrampoline_;
    std::function<int(int)>   realOriginal_;
    HookEventSink             log_;
};

}  // namespace pulse::hooking::test

#endif  // PULSE_LOADER_HOOKING_CHAIN_DISPATCH_HARNESS_HPP
