// loader/hooking/hook_chain.cpp — implementazione della catena di hook
// ordinata (Layer 3 — Hooking Engine, Requisiti 3.1, 3.2, 3.3).
//
// Vedi hook_chain.hpp per il contratto. Questa unità implementa la struttura
// dati e l'ordinamento (task 6.1): add (inserimento ordinato), remove
// (rimozione selettiva per owner), contains; e l'esecuzione della catena
// (task 6.3): dispatch + HookContext::callNext()/invokeLink().

#include "hooking/hook_chain.hpp"

#include <algorithm>
#include <exception>
#include <string>

namespace pulse::hooking {

void HookChain::add(HookNode node) {
    // Riconduce la priorità al dominio valido [0, 1000] (Req 3.2).
    node.priority = clampHookPriority(node.priority);

    // Inserimento ordinato: trova la prima posizione il cui nodo NON precede il
    // nuovo nodo secondo `precedes`. `upper_bound` con il comparatore `precedes`
    // posiziona il nuovo nodo dopo tutti gli equivalenti già presenti, rendendo
    // l'inserimento stabile per ordine di inserimento a parità di chiave.
    const auto pos = std::upper_bound(
        nodes_.begin(), nodes_.end(), node,
        [](const HookNode& a, const HookNode& b) { return precedes(a, b); });

    nodes_.insert(pos, std::move(node));
}

std::size_t HookChain::remove(const ModId& owner) {
    // Rimozione selettiva: elimina i soli hook di `owner` preservando l'ordine
    // relativo degli altri (Req 2.4). `std::remove_if` mantiene la stabilità,
    // quindi l'invariante di ordinamento resta valida senza riordinare.
    const auto newEnd = std::remove_if(
        nodes_.begin(), nodes_.end(),
        [&owner](const HookNode& n) { return n.owner == owner; });

    const std::size_t removed =
        static_cast<std::size_t>(std::distance(newEnd, nodes_.end()));
    nodes_.erase(newEnd, nodes_.end());
    return removed;
}

bool HookChain::contains(const ModId& owner) const noexcept {
    return std::any_of(nodes_.begin(), nodes_.end(),
                       [&owner](const HookNode& n) { return n.owner == owner; });
}

// ---------------------------------------------------------------------------
// Esecuzione della catena (task 6.3, Req 3.1/3.4/3.5).
// ---------------------------------------------------------------------------

void HookChain::dispatch(std::string_view functionName,
                         const OriginalFn& original,
                         const HookEventSink& log) const {
    // Il contesto è guidato internamente: parte dal primo link e, di link in
    // link, esegue gestori e/o l'originale secondo le invocazioni di callNext().
    HookContext ctx(nodes_, original, functionName, log);
    ctx.run();
}

void HookContext::invokeLink(std::size_t pos) {
    // Oltre l'ultimo gestore: l'ultimo callNext() invoca la funzione originale
    // (Req 3.1). Una catena vuota arriva qui con pos == 0.
    if (pos >= nodes_.size()) {
        if (original_) {
            original_();
        }
        return;
    }

    const HookNode& node = nodes_[pos];

    // Nodo senza gestore: trasparente, cede subito al link successivo. (I nodi
    // dei test di solo ordinamento non hanno gestore; in dispatch non bloccano.)
    if (!node.handler) {
        invokeLink(pos + 1);
        return;
    }

    // Stato locale del gestore corrente; salva/ripristina `current_` per gestire
    // l'annidamento quando callNext() invoca ricorsivamente il link successivo.
    LinkState state{pos, false};
    LinkState* saved = current_;
    current_ = &state;

    bool threw = false;
    try {
        node.handler(*this);
    } catch (const std::exception& e) {
        // Req 3.5: isola l'eccezione, registra l'errore (mod + funzione).
        threw = true;
        logEvent("errore isolato nel gestore della mod '" + node.owner +
                 "' per la funzione '" + std::string(functionName_) +
                 "': " + e.what());
    } catch (...) {
        threw = true;
        logEvent("errore isolato nel gestore della mod '" + node.owner +
                 "' per la funzione '" + std::string(functionName_) +
                 "': eccezione sconosciuta");
    }

    current_ = saved;

    // Se il gestore ha già ceduto (callNext), la coda della catena è già stata
    // eseguita dentro la sua chiamata: nulla da fare qui.
    if (state.nextCalled) {
        return;
    }

    if (threw) {
        // Req 3.5: prosegue la catena invocando il gestore successivo (o, se
        // assente, l'originale).
        invokeLink(pos + 1);
    } else {
        // Req 3.4: il gestore è ritornato senza invocare callNext(). La catena
        // si interrompe (gestori rimanenti e originale NON eseguiti) e si
        // registra l'evento di interruzione con mod + funzione.
        logEvent("catena di hook interrotta: la mod '" + node.owner +
                 "' non ha invocato il gestore successivo per la funzione '" +
                 std::string(functionName_) + "'");
    }
}

void HookContext::callNext() {
    if (current_ == nullptr) {
        return;  // invocato fuori da un gestore: no-op
    }
    if (current_->nextCalled) {
        return;  // idempotente: seconda invocazione dallo stesso gestore ignorata
    }
    current_->nextCalled = true;
    invokeLink(current_->pos + 1);
}

}  // namespace pulse::hooking
