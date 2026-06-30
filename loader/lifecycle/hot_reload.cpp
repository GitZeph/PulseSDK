// loader/lifecycle/hot_reload.cpp — implementazione dell'hot-reload dev
// (Requisiti 15.1, 15.2, 15.3, 15.4, 15.5; design IMP-11).
//
// Vedi hot_reload.hpp per il contratto. La logica riusa `HookEngine` per la
// rimozione selettiva (Req 2.4) e l'installazione atomica con retry/rollback
// (Req 2.5), e modella il budget di 5 s con un clock iniettabile (Req 15.1).

#include "lifecycle/hot_reload.hpp"

#include <utility>

namespace pulse::lifecycle {

HotReload::HotReload(hooking::HookEngine& engine, HotReloadClock clock, HotReloadLogSink log)
    : engine_(engine), clock_(std::move(clock)), log_(std::move(log)) {
    // Default: orologio monotòno reale. Iniettabile nei test per pilotare il
    // budget (Req 15.1) senza dipendere dal tempo di parete.
    if (!clock_) {
        clock_ = [] { return std::chrono::steady_clock::now(); };
    }
}

HotReloadResult HotReload::installInitial(const ModId& mod,
                                          std::vector<hooking::HookRequest> hooks) {
    HotReloadResult result;

    // Installazione atomica: o tutti gli hook o nessuno (Req 2.5).
    hooking::InstallOutcome outcome = engine_.installAll(hooks);
    if (!outcome.installed) {
        result.status = HotReloadStatus::Failed;
        result.message = "installazione iniziale della mod '" + mod + "' fallita";
        if (outcome.error) {
            result.message += ": funzione '" + outcome.error->function +
                              "', causa: " + outcome.error->cause.message;
        }
        logEvent(result.message);
        return result;
    }

    // Registra gli hook come versione corrente (per il ripristino su un futuro
    // reload fallito, Req 15.4).
    result.newHooksInstalled = hooks.size();
    installed_[mod] = std::move(hooks);

    result.status = HotReloadStatus::Reloaded;
    result.message = "installazione iniziale della mod '" + mod + "' completata (" +
                     std::to_string(result.newHooksInstalled) + " hook)";
    logEvent(result.message);
    return result;
}

HotReloadResult HotReload::reload(const ModId& mod,
                                  std::vector<hooking::HookRequest> newHooks,
                                  bool devMode,
                                  std::function<void()> onPreviousRemoved) {
    HotReloadResult result;
    const auto start = now();

    // ---- (Req 15.5) Gating sulla modalità sviluppo. -----------------------
    // Fuori dalla modalità sviluppo la richiesta è rifiutata e la mod corrente
    // resta INVARIATA (nessun hook rimosso o installato).
    if (!devMode) {
        result.status = HotReloadStatus::RejectedNotDevMode;
        result.message =
            "hot-reload della mod '" + mod +
            "' rifiutato: il ricaricamento è disponibile solo in modalità sviluppo";
        result.elapsed = now() - start;
        result.withinBudget = result.elapsed <= budget_;
        logEvent(result.message);
        return result;
    }

    // Snapshot dello stato precedente al reload, necessario al ripristino su
    // fallimento (Req 15.4). Copia: il registro viene aggiornato solo a esito noto.
    std::vector<hooking::HookRequest> previous;
    if (const auto it = installed_.find(mod); it != installed_.end()) {
        previous = it->second;
    }

    // ---- (Req 15.2) Rimozione degli hook della versione precedente PRIMA ---
    // dell'installazione di quelli nuovi, con conferma.
    const hooking::RemoveOutcome removeOutcome = engine_.remove(mod);
    result.previousHooksRemoved = removeOutcome.hooksRemoved;
    installed_.erase(mod);  // la versione precedente non è più la corrente
    logEvent("hot-reload mod '" + mod + "': rimossi " +
             std::to_string(removeOutcome.hooksRemoved) +
             " hook della versione precedente");

    // Seam osservabile: tra rimozione e installazione lo stato del backend
    // riflette l'assenza degli hook della versione precedente (Req 15.2).
    if (onPreviousRemoved) {
        onPreviousRemoved();
    }

    // ---- (Req 15.3) Installazione atomica degli hook della nuova versione. -
    const hooking::InstallOutcome installOutcome = engine_.installAll(newHooks);

    if (!installOutcome.installed) {
        // ---- (Req 15.4) Installazione fallita: ripristina lo stato precedente.
        // L'engine ha già annullato le installazioni parziali della nuova
        // versione (rollback atomico del batch, Req 2.5); reinstalliamo gli
        // hook della versione precedente per riportare la mod allo stato
        // pre-reload. Il processo di GD resta attivo.
        std::string cause = "causa sconosciuta";
        if (installOutcome.error) {
            cause = "funzione '" + installOutcome.error->function +
                    "', causa: " + installOutcome.error->cause.message;
        }

        const hooking::InstallOutcome restore = engine_.installAll(previous);
        if (restore.installed) {
            installed_[mod] = previous;
            result.previousStateRestored = true;
        } else {
            // Anche il ripristino è fallito: la mod resta senza hook installati;
            // si segnala comunque al developer mantenendo GD attivo.
            result.previousStateRestored = false;
        }

        result.status = HotReloadStatus::Failed;
        result.message = "hot-reload della mod '" + mod +
                         "' fallito (" + cause + "); stato precedente " +
                         (result.previousStateRestored ? "ripristinato"
                                                        : "NON ripristinato") +
                         ", processo di Geometry Dash mantenuto attivo";
        result.elapsed = now() - start;
        result.withinBudget = result.elapsed <= budget_;
        logEvent(result.message);
        return result;
    }

    // Installazione riuscita: la nuova versione è ora la corrente.
    result.newHooksInstalled = newHooks.size();

    // ---- (Req 15.1) Verifica del budget di completamento (≤ 5 s). ----------
    result.elapsed = now() - start;
    result.withinBudget = result.elapsed <= budget_;

    if (!result.withinBudget) {
        // Budget superato: il reload non può considerarsi completato entro 5 s
        // (Req 15.1). Trattato come fallimento (Req 15.4): si rimuovono gli
        // hook della nuova versione e si ripristina lo stato precedente, con GD
        // mantenuto attivo.
        engine_.remove(mod);
        const hooking::InstallOutcome restore = engine_.installAll(previous);
        if (restore.installed) {
            installed_[mod] = previous;
            result.previousStateRestored = true;
        } else {
            result.previousStateRestored = false;
        }

        result.status = HotReloadStatus::Failed;
        result.newHooksInstalled = 0;
        result.message = "hot-reload della mod '" + mod +
                         "' fallito: superato il budget di " +
                         std::to_string(budget_.count()) +
                         " ms; stato precedente " +
                         (result.previousStateRestored ? "ripristinato"
                                                        : "NON ripristinato") +
                         ", processo di Geometry Dash mantenuto attivo";
        logEvent(result.message);
        return result;
    }

    // ---- (Req 15.3) Conferma dell'avvenuta installazione al loader. --------
    installed_[mod] = std::move(newHooks);
    result.status = HotReloadStatus::Reloaded;
    result.message = "hot-reload della mod '" + mod + "' completato: rimossi " +
                     std::to_string(result.previousHooksRemoved) +
                     " hook della versione precedente, installati " +
                     std::to_string(result.newHooksInstalled) +
                     " hook della nuova versione";
    logEvent(result.message);
    return result;
}

std::vector<hooking::HookRequest> HotReload::currentHooks(const ModId& mod) const {
    const auto it = installed_.find(mod);
    if (it == installed_.end()) {
        return {};
    }
    return it->second;
}

}  // namespace pulse::lifecycle
