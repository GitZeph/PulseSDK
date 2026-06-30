// loader/lifecycle/mod_manager.cpp — implementazione della state machine del
// `ModManager` (Requisiti 4.4, 4.5, 4.6, 4.8).
//
// Compilato in `pulse::loader` tramite il glob `lifecycle/*.cpp`.
#include "lifecycle/mod_manager.hpp"

#include <array>
#include <exception>
#include <string>
#include <utility>

namespace pulse::lifecycle {

std::string_view to_string(ModState state) noexcept {
    switch (state) {
        case ModState::Installed: return "installed";
        case ModState::Enabled:   return "enabled";
        case ModState::Disabled:  return "disabled";
        case ModState::Removed:   return "removed";
    }
    return "unknown";
}

bool ModManager::isAllowed(ModState from, ModState to) noexcept {
    // Tabella delle transizioni ammesse dalla state machine del design
    // (Layer 4). Tutto ciò che non è elencato è rifiutato (Req 4.5), incluse
    // le transizioni verso lo stesso stato e qualsiasi uscita da `Removed`
    // (stato terminale).
    switch (from) {
        case ModState::Installed:
            // enable (Req 4.6) oppure remove.
            return to == ModState::Enabled || to == ModState::Removed;
        case ModState::Enabled:
            // disable / crash / init error (Req 4.7, completato in task 12.2).
            return to == ModState::Disabled;
        case ModState::Disabled:
            // re-enable (Req 4.6) oppure remove.
            return to == ModState::Enabled || to == ModState::Removed;
        case ModState::Removed:
            // Stato terminale: nessuna transizione in uscita.
            return false;
    }
    return false;
}

void ModManager::registerMod(ModManagerModId id, EntryPointFn entry, TerminatorFn terminator) {
    auto it = mods_.find(id);
    if (it == mods_.end()) {
        ModEntry entryData{};
        entryData.state = ModState::Installed;  // stato iniziale (Req 4.4)
        entryData.entry = std::move(entry);
        entryData.terminator = std::move(terminator);
        mods_.emplace(std::move(id), std::move(entryData));
        return;
    }
    // Re-registrazione: aggiorna le callback, conserva lo stato corrente.
    it->second.entry = std::move(entry);
    it->second.terminator = std::move(terminator);
}

bool ModManager::contains(std::string_view id) const noexcept {
    return mods_.find(std::string(id)) != mods_.end();
}

std::optional<ModState> ModManager::stateOf(std::string_view id) const noexcept {
    auto it = mods_.find(std::string(id));
    if (it == mods_.end()) return std::nullopt;
    return it->second.state;
}

void ModManager::reportRejection(TransitionRejection rejection) {
    if (reportSink_) reportSink_(rejection);
    rejections_.push_back(std::move(rejection));
}

void ModManager::reportInitFailure(InitFailure failure) {
    if (initFailureSink_) initFailureSink_(failure);
    initFailures_.push_back(std::move(failure));
}

TransitionResult ModManager::transition(std::string_view id, ModState target) {
    TransitionResult result{};

    auto it = mods_.find(std::string(id));
    if (it == mods_.end()) {
        result.status = TransitionStatus::UnknownMod;
        result.state = target;  // privo di significato: la mod non esiste
        return result;
    }

    ModEntry& mod = it->second;
    const ModState current = mod.state;

    // Transizione non ammessa: rifiuto + stato invariato + segnalazione (Req 4.5).
    if (!isAllowed(current, target)) {
        TransitionRejection rejection{};
        rejection.mod = it->first;
        rejection.from = current;
        rejection.requested = target;
        rejection.message = "transizione non ammessa per la mod '" + it->first + "': " +
                            std::string(to_string(current)) + " -> " +
                            std::string(to_string(target));

        result.status = TransitionStatus::Rejected;
        result.state = current;  // mantiene lo stato corrente (Req 4.5)
        result.rejection = rejection;
        reportRejection(std::move(rejection));
        return result;
    }

    // Transizione verso Enabled: invoca l'entry point dichiarato (Req 4.6).
    if (target == ModState::Enabled) {
        if (mod.entry) {
            // Isolamento del fallimento di inizializzazione (Req 4.7, 28.4): se
            // l'entry point fallisce — restituendo errore O lanciando
            // un'eccezione — la mod NON passa a Enabled, viene portata a
            // `Disabled` e si produce una segnalazione (mod + causa, Req 4.7,
            // 28.5). L'eccezione è catturata qui e NON propagata, così il
            // caricamento delle altre mod può proseguire (Req 28.4).
            EntryPointOutcome outcome{};
            bool threw = false;
            std::string failureCause;

            try {
                outcome = mod.entry();
            } catch (const std::exception& ex) {
                threw = true;
                failureCause = ex.what();
            } catch (...) {
                threw = true;
                failureCause = "eccezione non gestita di tipo sconosciuto";
            }

            if (threw) {
                // Eccezione isolata: mod -> Disabled + segnalazione (Req 4.7, 28.4, 28.5).
                mod.state = ModState::Disabled;
                result.entryPoint = EntryPointStatus::Threw;
                result.entryPointMessage = failureCause;
                result.status = TransitionStatus::Applied;  // chiamata gestita
                result.state = ModState::Disabled;
                reportInitFailure(InitFailure{it->first, failureCause, /*threw*/ true});
                return result;
            }

            result.entryPointMessage = outcome.message;
            if (outcome.ok) {
                result.entryPoint = EntryPointStatus::Ok;
            } else {
                // Errore di inizializzazione (Req 4.7): mod -> Disabled +
                // segnalazione (mod + causa). La transizione a Enabled NON è
                // applicata; il caricamento delle altre mod prosegue (Req 28.4).
                mod.state = ModState::Disabled;
                result.entryPoint = EntryPointStatus::Error;
                result.status = TransitionStatus::Applied;  // chiamata gestita
                result.state = ModState::Disabled;
                reportInitFailure(InitFailure{it->first, outcome.message, /*threw*/ false});
                return result;
            }
        } else {
            result.entryPoint = EntryPointStatus::NotInvoked;
        }
    }

    // Applica la transizione.
    mod.state = target;
    result.status = TransitionStatus::Applied;
    result.state = target;
    return result;
}

EnableAllResult ModManager::enableAll(const std::vector<ModManagerModId>& loadOrder) {
    // Req 4.6/4.7/28.4: abilita le mod nell'ordine di caricamento risolto,
    // isolando il fallimento di ciascuna mod e proseguendo con le altre.
    EnableAllResult out;
    out.enabled.reserve(loadOrder.size());

    for (const auto& id : loadOrder) {
        auto it = mods_.find(id);
        if (it == mods_.end()) continue;  // mod non registrata: ignorata

        // `transition` cattura internamente errori ed eccezioni dell'entry
        // point e instrada la segnalazione (mod + causa); qui ricostruiamo
        // l'esito senza che alcuna eccezione possa propagarsi (Req 28.4).
        const TransitionResult r = transition(id, ModState::Enabled);

        if (r.entryPoint == EntryPointStatus::Error ||
            r.entryPoint == EntryPointStatus::Threw) {
            out.failed.push_back(InitFailure{
                id,
                r.entryPointMessage,
                /*threw*/ r.entryPoint == EntryPointStatus::Threw});
            continue;  // fallimento isolato: prosegue con le altre mod (Req 28.4)
        }

        if (r.applied() && r.state == ModState::Enabled) {
            out.enabled.push_back(id);
        }
    }

    return out;
}

std::vector<ModManagerModId> ModManager::shutdown(const std::vector<ModManagerModId>& loadOrder) {
    // Req 4.8: invoca il terminator di ciascuna mod ABILITATA in ordine INVERSO
    // rispetto all'ordine di caricamento. Restituisce l'ordine di invocazione.
    std::vector<ModManagerModId> invoked;
    invoked.reserve(loadOrder.size());

    for (auto rit = loadOrder.rbegin(); rit != loadOrder.rend(); ++rit) {
        auto it = mods_.find(*rit);
        if (it == mods_.end()) continue;          // mod non registrata
        if (it->second.state != ModState::Enabled) continue;  // solo abilitate

        if (it->second.terminator) it->second.terminator();
        invoked.push_back(*rit);
    }

    return invoked;
}

}  // namespace pulse::lifecycle
