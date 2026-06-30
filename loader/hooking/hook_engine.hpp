// loader/hooking/hook_engine.hpp — coordinatore di installazione/rimozione
// degli hook con retry e rollback atomico (Layer 3 — Hooking Engine,
// Requisiti 2.4, 2.5).
//
// `HookEngine` mette insieme i tre mattoni già presenti dell'Hooking Engine:
//   * `IHookBackend`  — primitive install/remove/readOriginal del backend di
//                       piattaforma (o del FakeBackend nei test);
//   * `HookChain`     — catena ordinata di hook per ciascuna funzione bersaglio
//                       (ordinamento + rimozione selettiva per owner, Req 2.4);
// e aggiunge la POLITICA di installazione richiesta dal Requisito 2.5:
//
//   «IF l'installazione di un hook fallisce dopo un massimo di 3 tentativi,
//    THEN annullare tutte le modifiche parziali, lasciare invariato il codice
//    della funzione bersaglio e restituire alla Mod un esito di errore che
//    indica la funzione e la causa del fallimento.»
//
// Modello (logica originale Pulse, Requisito 27):
//   * per ogni funzione bersaglio l'engine installa UNA sola volta il detour
//     sul backend (il primo hook su quella funzione), poi concatena gli hook
//     successivi della stessa funzione nella `HookChain` senza ritoccare il
//     codice macchina;
//   * `install` tenta `backend.install` fino a `kMaxInstallAttempts` (3) volte
//     prima di rinunciare; al fallimento definitivo non resta alcuna modifica
//     parziale (nessun nodo in catena, nessun detour sul backend) e si
//     restituisce `InstallError{function, cause}`;
//   * `installAll` è la variante batch ATOMICA: o tutti gli hook richiesti
//     sono installati, oppure — al primo fallimento definitivo — vengono
//     annullati anche quelli già installati in questo batch (rollback), così
//     lo stato torna identico a prima della chiamata;
//   * `remove(owner)` realizza la rimozione selettiva (Req 2.4): toglie i soli
//     hook di quella mod dalle catene e, quando una catena resta vuota,
//     ripristina il codice originale della funzione via `backend.remove`,
//     lasciando intatti gli hook delle altre mod.
//
// È header-only e privo di dipendenze esterne oltre alle interfacce canoniche
// `hooking/hook_backend.hpp` e `hooking/hook_chain.hpp`. La logica di catena
// vive in `hook_chain.cpp` (compilata in `pulse::loader`).
//
// Stack: C++20/23 (Requisito 26.1). Non thread-safe: l'engine è guidato dal
// thread di caricamento/lifecycle.
#ifndef PULSE_LOADER_HOOKING_HOOK_ENGINE_HPP
#define PULSE_LOADER_HOOKING_HOOK_ENGINE_HPP

#include "hooking/hook_backend.hpp"
#include "hooking/hook_chain.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pulse::hooking {

// Numero massimo di tentativi di `backend.install` prima di rinunciare
// (Requisito 2.5).
inline constexpr int kMaxInstallAttempts = 3;

// ---------------------------------------------------------------------------
// InstallError — esito di errore restituito alla Mod: funzione bersaglio +
// causa del fallimento (Requisito 2.5).
// ---------------------------------------------------------------------------
struct InstallError {
    std::string function;  // nome simbolico della funzione bersaglio
    HookError cause;       // categoria + messaggio riportati dal backend
};

// ---------------------------------------------------------------------------
// InstallOutcome — esito di una richiesta di installazione (singola o batch).
// ---------------------------------------------------------------------------
struct InstallOutcome {
    bool installed = false;             // true sse l'installazione è riuscita
    std::size_t attempts = 0;           // tentativi di backend.install effettuati
    std::optional<InstallError> error;  // valorizzato sse !installed (Req 2.5)

    [[nodiscard]] bool ok() const noexcept { return installed; }
};

// ---------------------------------------------------------------------------
// HookRequest — richiesta di installazione di un singolo hook di una Mod su
// una funzione bersaglio.
// ---------------------------------------------------------------------------
struct HookRequest {
    ModId owner;                          // mod proprietaria (rimozione selettiva, Req 2.4)
    std::string functionName;             // nome simbolico (per l'esito di errore, Req 2.5)
    std::uintptr_t target = 0;            // indirizzo della funzione bersaglio
    void* detour = nullptr;               // detour/dispatcher della catena per questo target
    int priority = kHookPriorityDefault;  // 0..1000, default 500 (Req 3.2)
    std::uint64_t loadOrder = 0;          // tie-break deterministico (Req 3.3)
    HandlerFn handler{};                  // gestore (opzionale: vuoto = trasparente)
};

// ---------------------------------------------------------------------------
// RemoveOutcome — esito della rimozione selettiva degli hook di una Mod
// (Requisito 2.4; ripristino byte via backend, Req 18.4).
// ---------------------------------------------------------------------------
struct RemoveOutcome {
    std::size_t hooksRemoved = 0;       // nodi rimossi dalle catene
    std::size_t targetsRestored = 0;    // funzioni ripristinate (backend.remove riuscito)
    std::vector<InstallError> errors;   // fallimenti di ripristino del backend

    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
};

// ---------------------------------------------------------------------------
// HookEngine — coordinatore backend + catena + retry/rollback.
// ---------------------------------------------------------------------------
class HookEngine {
public:
    explicit HookEngine(IHookBackend& backend, HookEventSink log = nullptr)
        : backend_(backend), log_(std::move(log)) {}

    // Installa un singolo hook (Req 2.5).
    //
    // Se la funzione bersaglio non è ancora hookata sul backend, tenta
    // `backend.install` fino a `kMaxInstallAttempts` volte. Al fallimento
    // definitivo NON resta alcuna modifica parziale (atomicità del singolo
    // hook: nessun nodo in catena, codice della funzione invariato) e si
    // restituisce `InstallError{functionName, cause}`. Se la funzione è già
    // hookata, l'hook viene semplicemente concatenato (nessun tentativo sul
    // backend, nessun fallimento possibile).
    InstallOutcome install(const HookRequest& req) {
        std::vector<Step> journal;
        InstallOutcome out = installOne(req, journal);
        // In caso di fallimento, `installOne` non lascia modifiche parziali:
        // il journal è vuoto, quindi non c'è nulla da annullare. In caso di
        // successo, il journal documenta il singolo passo eseguito.
        return out;
    }

    // Installa atomicamente un batch di hook: o TUTTI o NESSUNO (Req 2.5).
    //
    // Installa le richieste in ordine. Al primo fallimento definitivo (dopo i
    // 3 tentativi su quella funzione) annulla tutte le installazioni già
    // effettuate in QUESTO batch (rollback) e restituisce l'esito di errore
    // che indica la funzione fallita e la causa, lasciando lo stato identico a
    // prima della chiamata.
    InstallOutcome installAll(const std::vector<HookRequest>& reqs) {
        std::vector<Step> journal;
        std::size_t totalAttempts = 0;

        for (const HookRequest& req : reqs) {
            InstallOutcome step = installOne(req, journal);
            totalAttempts += step.attempts;
            if (!step.installed) {
                // Rollback atomico delle modifiche parziali di questo batch.
                rollback(journal);
                InstallOutcome out;
                out.installed = false;
                out.attempts = totalAttempts;
                out.error = step.error;  // funzione + causa del fallimento
                logEvent("installazione batch annullata: rollback di " +
                         std::to_string(journal.size()) +
                         " hook dopo il fallimento su '" +
                         (step.error ? step.error->function : std::string{}) +
                         "'");
                return out;
            }
        }

        InstallOutcome out;
        out.installed = true;
        out.attempts = totalAttempts;
        return out;
    }

    // Rimozione selettiva degli hook di `owner` (Req 2.4).
    //
    // Toglie i soli hook di quella mod da tutte le catene, preservando gli hook
    // delle altre mod e il loro ordine. Quando una catena resta vuota, ripristina
    // il codice originale della funzione via `backend.remove` (Req 18.4). Se un
    // ripristino del backend fallisce, l'errore (funzione + causa) è raccolto in
    // `RemoveOutcome::errors` e la rimozione prosegue sulle altre funzioni.
    RemoveOutcome remove(const ModId& owner) {
        RemoveOutcome outcome;

        for (auto it = targets_.begin(); it != targets_.end();) {
            Target& t = it->second;
            const std::size_t removed = t.chain.remove(owner);
            outcome.hooksRemoved += removed;

            if (removed > 0 && t.chain.empty() && t.backendInstalled) {
                // Ultima catena svuotata su questa funzione: ripristina il
                // codice originale tramite il backend (Req 2.4, 18.4).
                auto r = backend_.remove(t.address);
                if (r.has_value()) {
                    t.backendInstalled = false;
                    ++outcome.targetsRestored;
                } else {
                    outcome.errors.push_back(InstallError{t.name, r.error()});
                }
            }

            // Pulizia: rimuovi l'entry quando non restano né hook né detour.
            if (t.chain.empty() && !t.backendInstalled) {
                it = targets_.erase(it);
            } else {
                ++it;
            }
        }

        return outcome;
    }

    // -----------------------------------------------------------------------
    // Introspezione per i test e la diagnostica.
    // -----------------------------------------------------------------------

    // Numero di funzioni attualmente hookate sul backend (detour attivo).
    [[nodiscard]] std::size_t installedTargets() const noexcept {
        std::size_t n = 0;
        for (const auto& [addr, t] : targets_) {
            if (t.backendInstalled) ++n;
        }
        return n;
    }

    // Numero totale di hook (nodi) presenti in tutte le catene.
    [[nodiscard]] std::size_t totalHooks() const noexcept {
        std::size_t n = 0;
        for (const auto& [addr, t] : targets_) {
            n += t.chain.size();
        }
        return n;
    }

    // True se la funzione a `target` ha un detour installato sul backend.
    [[nodiscard]] bool isTargetInstalled(std::uintptr_t target) const noexcept {
        const auto it = targets_.find(target);
        return it != targets_.end() && it->second.backendInstalled;
    }

    // Numero di hook posseduti da `owner` su tutte le funzioni.
    [[nodiscard]] std::size_t hooksForOwner(const ModId& owner) const {
        std::size_t n = 0;
        for (const auto& [addr, t] : targets_) {
            for (const HookNode& node : t.chain.orderedNodes()) {
                if (node.owner == owner) ++n;
            }
        }
        return n;
    }

    // Catena della funzione a `target`, o nullptr se non esiste.
    [[nodiscard]] const HookChain* chainFor(std::uintptr_t target) const {
        const auto it = targets_.find(target);
        return it != targets_.end() ? &it->second.chain : nullptr;
    }

private:
    // Stato di una singola funzione bersaglio.
    struct Target {
        std::uintptr_t address = 0;
        std::string name;
        HookChain chain{};
        bool backendInstalled = false;
        Trampoline trampoline{};
        void* detour = nullptr;
    };

    // Passo registrato durante un (batch di) install, usato per il rollback.
    struct Step {
        std::uintptr_t target = 0;
        ModId owner;
        bool freshBackend = false;  // questo passo ha installato il detour sul backend
    };

    // Installa un singolo hook registrando il passo nel journal in caso di
    // successo. In caso di fallimento definitivo NON modifica lo stato (nessun
    // passo aggiunto al journal): atomicità del singolo hook (Req 2.5).
    InstallOutcome installOne(const HookRequest& req, std::vector<Step>& journal) {
        Target& t = targets_[req.target];
        const bool newlyTracked = (t.address == 0 && !t.backendInstalled && t.chain.empty());
        t.address = req.target;
        if (t.name.empty()) {
            t.name = req.functionName;
        }

        const bool freshTarget = !t.backendInstalled;
        InstallOutcome out;

        if (freshTarget) {
            // Primo hook su questa funzione: installa il detour sul backend con
            // retry fino a kMaxInstallAttempts (Req 2.5).
            Result<Trampoline> r = Result<Trampoline>::err(
                HookErrorCode::None, "nessun tentativo eseguito");
            for (out.attempts = 0; out.attempts < static_cast<std::size_t>(kMaxInstallAttempts);) {
                ++out.attempts;
                r = backend_.install(req.target, req.detour);
                if (r.has_value()) {
                    break;
                }
            }

            if (!r.has_value()) {
                // Fallimento definitivo: nessuna modifica parziale. Il backend
                // non lascia stato su install fallito; non abbiamo aggiunto nodi
                // alla catena. Rimuovi l'entry se appena creata e ancora vuota,
                // così la funzione bersaglio resta invariata (Req 2.5).
                if (newlyTracked && t.chain.empty() && !t.backendInstalled) {
                    targets_.erase(req.target);
                }
                out.installed = false;
                out.error = InstallError{req.functionName, r.error()};
                logEvent("installazione fallita dopo " +
                         std::to_string(out.attempts) +
                         " tentativi sulla funzione '" + req.functionName +
                         "': " + r.error().message);
                return out;
            }

            t.trampoline = std::move(r).value();
            t.backendInstalled = true;
            t.detour = req.detour;
        }

        // Backend pronto (o già installato): concatena l'hook (Req 3.1/3.2).
        t.chain.add(HookNode{req.owner, req.priority, req.loadOrder, req.handler});

        journal.push_back(Step{req.target, req.owner, freshTarget});
        out.installed = true;
        return out;
    }

    // Annulla i passi del journal in ordine inverso: rimuove i nodi aggiunti e,
    // per i target il cui detour è stato installato in questo batch, esegue
    // backend.remove ripristinando il codice originale (Req 2.5 — atomicità).
    void rollback(const std::vector<Step>& journal) {
        for (auto it = journal.rbegin(); it != journal.rend(); ++it) {
            const Step& step = *it;
            const auto found = targets_.find(step.target);
            if (found == targets_.end()) {
                continue;
            }
            Target& t = found->second;

            // Rimuove il nodo aggiunto da questo passo (per owner sulla funzione).
            t.chain.remove(step.owner);

            // Se questo passo aveva installato il detour, ripristina il backend.
            if (step.freshBackend && t.backendInstalled) {
                auto r = backend_.remove(step.target);
                if (r.has_value()) {
                    t.backendInstalled = false;
                }
            }

            if (t.chain.empty() && !t.backendInstalled) {
                targets_.erase(found);
            }
        }
    }

    void logEvent(const std::string& message) const {
        if (log_) {
            log_(message);
        }
    }

    IHookBackend& backend_;
    HookEventSink log_;
    // Ordinato per indirizzo: introspezione deterministica nei test.
    std::map<std::uintptr_t, Target> targets_{};
};

}  // namespace pulse::hooking

#endif  // PULSE_LOADER_HOOKING_HOOK_ENGINE_HPP
