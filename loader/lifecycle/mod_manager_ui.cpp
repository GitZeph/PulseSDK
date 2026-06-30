// loader/lifecycle/mod_manager_ui.cpp — implementazione del controller del
// Mod Manager in-game (Requisito 22).
//
// Compilato in `pulse::loader` tramite il glob `lifecycle/*.cpp`.
#include "lifecycle/mod_manager_ui.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace pulse::lifecycle {

namespace {

// Clock di default: orologio monotono di sistema.
ModManagerUiClock default_clock() {
    return []() { return std::chrono::steady_clock::now(); };
}

}  // namespace

ModManagerUI::ModManagerUI(ModManager& manager, IMarketplace* marketplace,
                           ModManagerUiClock clock)
    : manager_(manager),
      marketplace_(marketplace),
      clock_(clock ? std::move(clock) : default_clock()) {}

bool ModManagerUI::isInstalled(const ModManagerModId& id) const {
    return std::find(installOrder_.begin(), installOrder_.end(), id) != installOrder_.end();
}

void ModManagerUI::addInstalledMod(ModManagerModId id, EntryPointFn entry,
                                   TerminatorFn terminator) {
    const bool known = isInstalled(id);
    // Registra (o aggiorna le callback) nel ModManager riusato (Req 4.4).
    manager_.registerMod(id, std::move(entry), std::move(terminator));
    if (!known) installOrder_.push_back(std::move(id));
}

ModListResult ModManagerUI::listMods() {
    // Req 22.1/22.2: produce l'elenco con lo stato di abilitazione, misurando
    // il tempo rispetto al budget (≤ 3 s).
    const auto start = now();

    ModListResult out;
    out.mods.reserve(installOrder_.size());

    for (const auto& id : installOrder_) {
        const auto state = manager_.stateOf(id);
        if (!state) continue;  // disallineamento improbabile: ignora
        ModListEntry entry;
        entry.id = id;
        entry.state = *state;
        entry.enabled = (*state == ModState::Enabled);  // stato di abilitazione (Req 22.1)
        entry.updateAvailable = hasUpdate(id);          // segnalazione update (Req 22.8)
        out.mods.push_back(std::move(entry));
    }

    // Req 22.2: nessuna mod installata => elenco vuoto con messaggio.
    if (out.mods.empty()) {
        out.empty = true;
        out.message = "Nessuna mod installata.";
    }

    const auto end = now();
    out.elapsed = end - start;
    out.withinBudget = out.elapsed <= kModListBudget;  // Req 22.1
    return out;
}

ModToggleResult ModManagerUI::enable(const ModManagerModId& id) {
    // Req 22.3: applica la transizione verso Enabled e aggiorna lo stato; su
    // fallimento mantiene lo stato precedente e segnala (Req 22.4).
    const auto start = now();

    ModToggleResult out;
    const auto prior = manager_.stateOf(id);
    out.previousState = prior.value_or(ModState::Installed);

    const TransitionResult r = manager_.enable(id);

    // Successo SOLO se la mod è effettivamente Enabled dopo la chiamata: una
    // transizione rifiutata mantiene lo stato precedente (Req 4.5) e un
    // fallimento dell'entry point porta a Disabled (Req 4.7) — in entrambi i
    // casi l'abilitazione NON è stata completata (Req 22.4).
    out.currentState = r.state;
    out.ok = r.applied() && r.state == ModState::Enabled &&
             r.entryPoint != EntryPointStatus::Error &&
             r.entryPoint != EntryPointStatus::Threw;

    if (!out.ok) {
        // Fallback: stato di abilitazione precedente mantenuto + messaggio
        // (Req 22.4). La segnalazione di transizione rifiutata è già instradata
        // dal sink del ModManager (Req 4.5).
        out.currentState = out.previousState;
        if (r.rejected() && r.rejection) {
            out.message = "Abilitazione non completata: " + r.rejection->message;
        } else if (r.entryPoint == EntryPointStatus::Threw ||
                   r.entryPoint == EntryPointStatus::Error) {
            out.message = "Abilitazione non completata per la mod '" + id +
                          "': inizializzazione fallita (" + r.entryPointMessage + ").";
        } else {
            out.message = "Abilitazione non completata per la mod '" + id + "'.";
        }
        emitMessage(out.message);
    }

    const auto end = now();
    out.elapsed = end - start;
    out.withinBudget = out.elapsed <= kModToggleBudget;  // Req 22.3
    return out;
}

ModToggleResult ModManagerUI::disable(const ModManagerModId& id) {
    // Req 22.3: applica la transizione verso Disabled; su fallimento mantiene
    // lo stato precedente e segnala (Req 22.4).
    const auto start = now();

    ModToggleResult out;
    const auto prior = manager_.stateOf(id);
    out.previousState = prior.value_or(ModState::Installed);

    const TransitionResult r = manager_.disable(id);
    out.currentState = r.state;
    out.ok = r.applied() && r.state == ModState::Disabled;

    if (!out.ok) {
        out.currentState = out.previousState;  // mantiene lo stato precedente (Req 22.4)
        if (r.rejected() && r.rejection) {
            out.message = "Disabilitazione non completata: " + r.rejection->message;
        } else {
            out.message = "Disabilitazione non completata per la mod '" + id + "'.";
        }
        emitMessage(out.message);
    }

    const auto end = now();
    out.elapsed = end - start;
    out.withinBudget = out.elapsed <= kModToggleBudget;  // Req 22.3
    return out;
}

SearchResult ModManagerUI::searchMarketplace(const std::string& query) {
    // Req 22.5/22.7: ricerca con budget ≤ 5 s; Marketplace assente o
    // irraggiungibile => reachable == false con messaggio.
    const auto start = now();

    SearchResult out;
    if (marketplace_ == nullptr) {
        out.reachable = false;
        out.message = "Marketplace non disponibile.";
        emitMessage(out.message);
        const auto end0 = now();
        out.elapsed = end0 - start;
        out.withinBudget = out.elapsed <= kMarketplaceSearchBudget;
        return out;
    }

    const MarketplaceSearchResult mr = marketplace_->search(query);
    out.reachable = mr.reachable;
    if (!mr.reachable) {
        out.message = mr.error.empty() ? "Marketplace irraggiungibile." : mr.error;
        emitMessage(out.message);
    } else {
        out.results = mr.results;
    }

    const auto end = now();
    out.elapsed = end - start;
    out.withinBudget = out.elapsed <= kMarketplaceSearchBudget;  // Req 22.5
    return out;
}

InstallResult ModManagerUI::install(const ModManagerModId& id) {
    // Req 22.6/22.7: installa aggiungendo la mod all'elenco; su fallimento
    // ANNULLA senza alcuna modifica parziale.
    InstallResult out;
    out.id = id;

    if (marketplace_ == nullptr) {
        out.ok = false;
        out.message = "Marketplace non disponibile: installazione annullata.";
        emitMessage(out.message);
        return out;
    }

    // Stato pre-operazione per garantire il rollback "tutto o niente" (Req 22.7).
    const bool wasInstalled = isInstalled(id);

    const MarketplaceInstallResult mr = marketplace_->install(id);
    if (!mr.ok) {
        // Rollback: nessuna registrazione, nessuna voce in elenco (Req 22.7).
        // Nulla è stato modificato a monte, quindi non resta alcun artefatto
        // parziale; l'asserzione esplicita protegge da regressioni future.
        if (!wasInstalled && isInstalled(id)) {
            installOrder_.erase(
                std::remove(installOrder_.begin(), installOrder_.end(), id),
                installOrder_.end());
        }
        out.ok = false;
        out.message = mr.error.empty()
                          ? ("Installazione della mod '" + id + "' non completata: annullata.")
                          : ("Installazione della mod '" + id + "' non completata: " + mr.error);
        emitMessage(out.message);
        return out;
    }

    // Successo: registra nel ModManager (stato Installed, Req 4.4) e aggiunge
    // alla lista delle installate (Req 22.6).
    const ModManagerModId finalId = mr.mod.id.empty() ? id : mr.mod.id;
    addInstalledMod(finalId);
    out.ok = true;
    out.id = finalId;
    return out;
}

std::vector<ModManagerModId> ModManagerUI::refreshUpdates() {
    // Req 22.8: interroga il Marketplace per gli aggiornamenti disponibili
    // delle mod installate, marca le voci ed emette il segnale.
    updates_.clear();
    if (marketplace_ != nullptr && !installOrder_.empty()) {
        updates_ = marketplace_->updatesFor(installOrder_);
        // Mantiene solo gli id effettivamente installati.
        updates_.erase(std::remove_if(updates_.begin(), updates_.end(),
                                      [this](const ModManagerModId& id) {
                                          return !isInstalled(id);
                                      }),
                       updates_.end());
    }

    if (updatesSignal_ && !updates_.empty()) updatesSignal_(updates_);
    return updates_;
}

bool ModManagerUI::hasUpdate(const ModManagerModId& id) const {
    return std::find(updates_.begin(), updates_.end(), id) != updates_.end();
}

InstallResult ModManagerUI::startUpdate(const ModManagerModId& id) {
    // Req 22.8: avvia l'aggiornamento. Come install, su fallimento annulla
    // senza modifiche (Req 22.7) e mantiene il flag di update.
    InstallResult out;
    out.id = id;

    if (marketplace_ == nullptr) {
        out.ok = false;
        out.message = "Marketplace non disponibile: aggiornamento annullato.";
        emitMessage(out.message);
        return out;
    }

    const MarketplaceInstallResult mr = marketplace_->install(id);
    if (!mr.ok) {
        out.ok = false;
        out.message = mr.error.empty()
                          ? ("Aggiornamento della mod '" + id + "' non completato: annullato.")
                          : ("Aggiornamento della mod '" + id + "' non completato: " + mr.error);
        emitMessage(out.message);
        return out;  // flag di update mantenuto
    }

    // Successo: azzera il flag di aggiornamento per questa mod (Req 22.8).
    updates_.erase(std::remove(updates_.begin(), updates_.end(), id), updates_.end());
    out.ok = true;
    return out;
}

}  // namespace pulse::lifecycle
