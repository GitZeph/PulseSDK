// sdk/events/event_bus.hpp — sistema di eventi dello SDK Pulse (Layer 5, Requisito 7).
//
// Implementazione header-only dell'`EventBus`. È collocata sotto sdk/events/ ed
// esposta pubblicamente tramite l'header <pulse/events.hpp>. Le API sono
// template (`on<E>`, `emit<E>`, `declareEventType<E>`), quindi l'intera logica
// vive negli header (nessun sorgente .cpp da compilare).
//
// Comportamenti richiesti dal Requisito 7:
//   * 7.1  Una mod può registrare uno o più gestori per un tipo di evento
//          PRECEDENTEMENTE DICHIARATO.
//   * 7.2  La registrazione per un tipo NON dichiarato è rifiutata senza
//          registrare il gestore e restituisce un errore "tipo non dichiarato".
//   * 7.3  `emit` invoca i gestori registrati per quel tipo NELL'ORDINE DI
//          REGISTRAZIONE, dal primo all'ultimo.
//   * 7.4  Se un gestore lancia un'eccezione, l'eccezione è ISOLATA: la
//          pubblicazione prosegue invocando i gestori successivi e viene
//          registrata un'indicazione dell'errore.
//   * 7.5  `unregisterMod` deregistra tutti i gestori di una mod (su disable).
//   * 7.6  Se un gestore indica `Propagation::Consumed`, la propagazione ai
//          gestori successivi si interrompe.
//
// Il "tipo di evento dichiarato" è modellato esplicitamente da un registro di
// `std::type_index` (vedi `declareEventType<E>()` / `isEventTypeDeclared<E>()`),
// così la registrazione per un tipo non dichiarato è osservabilmente rifiutata
// (Req 7.2).
//
// Stack: C++20/23 (Requisito 26.1). Header-only, nessuna dipendenza esterna.
#ifndef PULSE_EVENTS_EVENT_BUS_HPP
#define PULSE_EVENTS_EVENT_BUS_HPP

#include <algorithm>
#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pulse::events {

// ---------------------------------------------------------------------------
// ModId — identità della mod proprietaria di un gestore. Modellato come
// stringa, coerente con `pulse::hooking::ModId` e `pulse::lifecycle::ModId`.
// ---------------------------------------------------------------------------
using ModId = std::string;

// Identificatore univoco di un gestore registrato (per diagnostica e per
// eventuale deregistrazione puntuale).
using HandlerId = std::uint64_t;

// ---------------------------------------------------------------------------
// Propagation — esito di un gestore (Req 7.6).
//   Continue  prosegue la propagazione ai gestori successivi;
//   Consumed  consuma l'evento: i gestori successivi NON vengono invocati.
// ---------------------------------------------------------------------------
enum class Propagation { Continue, Consumed };

// ---------------------------------------------------------------------------
// Esito della registrazione di un gestore (`on`).
//   `registered` true se il gestore è stato registrato (tipo dichiarato);
//   `id`         identificatore del gestore registrato (valido se registered);
//   `eventTypeDeclared` false se la registrazione è stata RIFIUTATA perché il
//                       tipo di evento non era dichiarato (Req 7.2).
// ---------------------------------------------------------------------------
struct OnResult {
    bool registered = false;
    HandlerId id = 0;
    bool eventTypeDeclared = true;

    // Conversione di comodità: true sse il gestore è stato registrato.
    explicit operator bool() const noexcept { return registered; }
};

// ---------------------------------------------------------------------------
// Indicazione di un'eccezione isolata durante `emit` (Req 7.4). Raccolta a fini
// diagnostici; non interrompe la pubblicazione dell'evento.
// ---------------------------------------------------------------------------
struct HandlerError {
    ModId owner;            // mod proprietaria del gestore che ha lanciato
    HandlerId handler = 0;  // gestore coinvolto
    std::string typeName;   // nome del tipo di evento in pubblicazione
    std::string what;       // descrizione dell'eccezione (se std::exception)
};

// ---------------------------------------------------------------------------
// EventBus — bus di eventi type-safe (Requisito 7).
// ---------------------------------------------------------------------------
class EventBus {
public:
    EventBus() = default;

    // -----------------------------------------------------------------------
    // declareEventType<E> — dichiara il tipo di evento E (Req 7.1).
    // Solo i tipi dichiarati possono ricevere registrazioni di gestori.
    // Idempotente: dichiarare più volte lo stesso tipo è un no-op.
    // -----------------------------------------------------------------------
    template <class E>
    void declareEventType() {
        declared_.insert(std::type_index(typeid(E)));
    }

    // True se il tipo di evento E è stato dichiarato.
    template <class E>
    [[nodiscard]] bool isEventTypeDeclared() const {
        return declared_.find(std::type_index(typeid(E))) != declared_.end();
    }

    // -----------------------------------------------------------------------
    // on<E> — registra un gestore per il tipo di evento E (Req 7.1, 7.2).
    //
    // Se E non è stato dichiarato, la registrazione è RIFIUTATA: nessun gestore
    // viene aggiunto e il risultato indica `eventTypeDeclared == false` (Req 7.2).
    // Più gestori per lo stesso tipo sono ammessi e invocati in ordine di
    // registrazione (Req 7.1, 7.3).
    // -----------------------------------------------------------------------
    template <class E>
    OnResult on(ModId owner, std::function<Propagation(const E&)> handler) {
        const std::type_index key(typeid(E));
        if (declared_.find(key) == declared_.end()) {
            // Tipo non dichiarato: rifiuta senza registrare (Req 7.2).
            return OnResult{/*registered=*/false, /*id=*/0,
                            /*eventTypeDeclared=*/false};
        }

        const HandlerId id = ++lastHandlerId_;
        // Type-erasure: il gestore tipizzato è incapsulato in una invocazione
        // che riceve il puntatore all'evento concreto. `emit<E>` garantisce che
        // il puntatore punti effettivamente a un `E`.
        auto invoke = [handler = std::move(handler)](const void* event) {
            return handler(*static_cast<const E*>(event));
        };
        handlers_[key].push_back(
            HandlerEntry{id, std::move(owner), std::move(invoke)});
        return OnResult{/*registered=*/true, id, /*eventTypeDeclared=*/true};
    }

    // -----------------------------------------------------------------------
    // emit<E> — pubblica un evento ai gestori registrati per il tipo E.
    //
    //   * invoca i gestori NELL'ORDINE DI REGISTRAZIONE (Req 7.3);
    //   * isola le eccezioni di un gestore e prosegue con i successivi,
    //     registrando un'indicazione dell'errore (Req 7.4);
    //   * si arresta se un gestore restituisce `Propagation::Consumed` (Req 7.6).
    //
    // Restituisce il numero di gestori effettivamente invocati (diagnostica).
    // -----------------------------------------------------------------------
    template <class E>
    std::size_t emit(const E& event) {
        const std::type_index key(typeid(E));
        auto it = handlers_.find(key);
        if (it == handlers_.end()) {
            return 0;
        }

        std::size_t invoked = 0;
        // Itera per indice su una copia degli indici impliciti: i gestori non
        // sono modificati durante un singolo emit; la registrazione è snapshot
        // dell'ordine corrente. Si itera sul vettore vivo, l'ordine è stabile.
        const auto& entries = it->second;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const HandlerEntry& entry = entries[i];
            ++invoked;
            Propagation result = Propagation::Continue;
            try {
                result = entry.invoke(&event);
            } catch (const std::exception& ex) {
                // Eccezione isolata: registra e prosegui (Req 7.4).
                recordError(entry, typeid(E).name(), ex.what());
                continue;
            } catch (...) {
                recordError(entry, typeid(E).name(), "eccezione sconosciuta");
                continue;
            }
            if (result == Propagation::Consumed) {
                // Propagazione interrotta (Req 7.6).
                break;
            }
        }
        return invoked;
    }

    // -----------------------------------------------------------------------
    // unregisterMod — deregistra tutti i gestori di una mod (Req 7.5).
    // Usata quando una mod che ha registrato gestori viene disabilitata.
    // Restituisce il numero di gestori rimossi.
    // -----------------------------------------------------------------------
    std::size_t unregisterMod(const ModId& owner) {
        std::size_t removed = 0;
        for (auto& [key, entries] : handlers_) {
            const std::size_t before = entries.size();
            entries.erase(
                std::remove_if(entries.begin(), entries.end(),
                               [&owner](const HandlerEntry& e) {
                                   return e.owner == owner;
                               }),
                entries.end());
            removed += before - entries.size();
        }
        return removed;
    }

    // -----------------------------------------------------------------------
    // Introspezione / diagnostica.
    // -----------------------------------------------------------------------

    // Numero di gestori registrati per il tipo di evento E.
    template <class E>
    [[nodiscard]] std::size_t handlerCount() const {
        auto it = handlers_.find(std::type_index(typeid(E)));
        return it == handlers_.end() ? 0 : it->second.size();
    }

    // Numero di gestori posseduti da una mod su tutti i tipi di evento.
    [[nodiscard]] std::size_t handlersForMod(const ModId& owner) const {
        std::size_t n = 0;
        for (const auto& [key, entries] : handlers_) {
            for (const auto& e : entries) {
                if (e.owner == owner) {
                    ++n;
                }
            }
        }
        return n;
    }

    // Indicazioni delle eccezioni isolate durante le pubblicazioni (Req 7.4).
    [[nodiscard]] const std::vector<HandlerError>& errors() const noexcept {
        return errors_;
    }

    // Svuota il registro diagnostico delle eccezioni isolate.
    void clearErrors() noexcept { errors_.clear(); }

private:
    struct HandlerEntry {
        HandlerId id = 0;
        ModId owner;
        std::function<Propagation(const void*)> invoke;
    };

    void recordError(const HandlerEntry& entry, std::string_view typeName,
                     std::string_view what) {
        errors_.push_back(HandlerError{entry.owner, entry.id,
                                       std::string(typeName),
                                       std::string(what)});
    }

    std::unordered_set<std::type_index> declared_;
    std::unordered_map<std::type_index, std::vector<HandlerEntry>> handlers_;
    std::vector<HandlerError> errors_;
    HandlerId lastHandlerId_ = 0;
};

}  // namespace pulse::events

#endif  // PULSE_EVENTS_EVENT_BUS_HPP
