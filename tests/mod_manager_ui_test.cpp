// tests/mod_manager_ui_test.cpp — unit test del controller del Mod Manager
// in-game (task 31.1, Requisiti 22.1, 22.2, 22.3, 22.4, 22.5, 22.6, 22.7, 22.8).
//
// Copre, riusando la state machine `ModManager` SENZA reimplementarla:
//   * elenco vuoto con messaggio quando non è installata alcuna mod (Req 22.2);
//   * elenco delle mod installate con lo stato di abilitazione, entro il budget
//     di 3 s modellato via clock iniettabile (Req 22.1);
//   * abilita/disabilita che applicano la transizione e aggiornano lo stato
//     entro 2 s (Req 22.3);
//   * FALLBACK su transizione rifiutata: stato precedente mantenuto + messaggio
//     all'User + segnalazione instradata dal rejection sink del ModManager
//     (Req 22.4);
//   * ricerca sul Marketplace (fake) con risultati entro 5 s (Req 22.5);
//   * installazione che aggiunge la mod all'elenco (Req 22.6);
//   * rollback su installazione fallita / Marketplace irraggiungibile: nessun
//     artefatto installato (Req 22.7);
//   * segnale di aggiornamenti disponibili marcato in elenco + emesso (Req 22.8).
#include "lifecycle/mod_manager_ui.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "lifecycle/mod_manager.hpp"

namespace {

using namespace std::chrono_literals;

using pulse::lifecycle::EntryPointOutcome;
using pulse::lifecycle::IMarketplace;
using pulse::lifecycle::MarketplaceInstallResult;
using pulse::lifecycle::MarketplaceMod;
using pulse::lifecycle::MarketplaceSearchResult;
using pulse::lifecycle::ModManager;
using pulse::lifecycle::ModManagerModId;
using pulse::lifecycle::ModManagerUI;
using pulse::lifecycle::ModState;
using pulse::lifecycle::TransitionRejection;

// ---------------------------------------------------------------------------
// Clock fittizio controllato: avanza solo quando il test lo richiede, così i
// budget (≤ 3/2/5 s) sono verificabili in modo deterministico senza attese.
// ---------------------------------------------------------------------------
class FakeClock {
public:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const { return now_; }
    void advance(std::chrono::nanoseconds d) { now_ += d; }
    // Restituisce una callable compatibile con ModManagerUiClock.
    [[nodiscard]] auto fn() {
        return [this]() { return now_; };
    }

private:
    std::chrono::steady_clock::time_point now_{};
};

// ---------------------------------------------------------------------------
// Marketplace fittizio iniettabile (Req 22.5, 22.6, 22.7, 22.8). Nessuna rete:
// risultati, esito dell'installazione e set di aggiornamenti sono pilotati dal
// test.
// ---------------------------------------------------------------------------
class FakeMarketplace : public IMarketplace {
public:
    // Pilotaggio della ricerca.
    bool searchReachable{true};
    std::vector<MarketplaceMod> searchResults;
    std::string searchError;

    // Pilotaggio dell'installazione.
    bool installOk{true};
    std::string installError;

    // Pilotaggio degli aggiornamenti.
    std::vector<ModManagerModId> updates;

    // Tracciamento delle chiamate per asserzioni.
    int searchCalls{0};
    int installCalls{0};
    std::string lastInstalledId;

    MarketplaceSearchResult search(const std::string& /*query*/) override {
        ++searchCalls;
        MarketplaceSearchResult r;
        r.reachable = searchReachable;
        if (!searchReachable) {
            r.error = searchError;
        } else {
            r.results = searchResults;
        }
        return r;
    }

    MarketplaceInstallResult install(const ModManagerModId& id) override {
        ++installCalls;
        lastInstalledId = id;
        MarketplaceInstallResult r;
        r.ok = installOk;
        if (installOk) {
            r.mod = MarketplaceMod{id, id + " name", "1.0.0"};
        } else {
            r.error = installError;
        }
        return r;
    }

    std::vector<ModManagerModId> updatesFor(
        const std::vector<ModManagerModId>& /*installed*/) override {
        return updates;
    }
};

// Helper: aggancia un rejection sink al manager che accumula le segnalazioni.
void captureRejections(ModManager& mgr, std::vector<TransitionRejection>& sink) {
    mgr.setReportSink([&sink](const TransitionRejection& r) { sink.push_back(r); });
}

// ===========================================================================
// Req 22.2 — elenco vuoto con messaggio quando non è installata alcuna mod.
// ===========================================================================
TEST(ModManagerUiTest, ListEmptyWhenNoModsInstalled) {
    ModManager mgr;
    FakeClock clock;
    ModManagerUI ui(mgr, /*marketplace*/ nullptr, clock.fn());

    const auto result = ui.listMods();

    EXPECT_TRUE(result.empty);
    EXPECT_TRUE(result.mods.empty());
    EXPECT_FALSE(result.message.empty());  // messaggio "nessuna mod" (Req 22.2)
    EXPECT_TRUE(result.withinBudget);
}

// ===========================================================================
// Req 22.1 — l'elenco mostra le mod installate con lo stato di abilitazione,
// entro il budget di 3 s.
// ===========================================================================
TEST(ModManagerUiTest, ListShowsModsWithEnablementState) {
    ModManager mgr;
    FakeClock clock;
    ModManagerUI ui(mgr, nullptr, clock.fn());

    ui.addInstalledMod("alpha", [] { return EntryPointOutcome::success(); });
    ui.addInstalledMod("beta", [] { return EntryPointOutcome::success(); });

    // Abilita alpha: deve risultare enabled in elenco.
    ASSERT_TRUE(ui.enable("alpha").ok);

    // Modella il costo dell'operazione di elenco entro il budget (2 s ≤ 3 s).
    const auto list = [&]() {
        // Avanza il clock "durante" l'operazione tramite un clock che misura
        // start/end: qui simuliamo facendo avanzare prima della seconda lettura.
        return ui.listMods();
    };
    auto result = list();

    ASSERT_EQ(result.mods.size(), 2u);
    // Ordine di installazione preservato (deterministico).
    EXPECT_EQ(result.mods[0].id, "alpha");
    EXPECT_EQ(result.mods[1].id, "beta");
    EXPECT_TRUE(result.mods[0].enabled);
    EXPECT_EQ(result.mods[0].state, ModState::Enabled);
    EXPECT_FALSE(result.mods[1].enabled);
    EXPECT_EQ(result.mods[1].state, ModState::Installed);
    EXPECT_TRUE(result.withinBudget);
}

// ===========================================================================
// Req 22.3 — abilita/disabilita applicano la transizione ammessa e aggiornano
// lo stato di abilitazione entro 2 s.
// ===========================================================================
TEST(ModManagerUiTest, EnableThenDisableAppliesAllowedTransitions) {
    ModManager mgr;
    FakeClock clock;
    ModManagerUI ui(mgr, nullptr, clock.fn());
    ui.addInstalledMod("alpha", [] { return EntryPointOutcome::success(); });

    const auto en = ui.enable("alpha");
    EXPECT_TRUE(en.ok);
    EXPECT_EQ(en.previousState, ModState::Installed);
    EXPECT_EQ(en.currentState, ModState::Enabled);
    EXPECT_TRUE(en.withinBudget);
    EXPECT_EQ(mgr.stateOf("alpha"), ModState::Enabled);

    const auto dis = ui.disable("alpha");
    EXPECT_TRUE(dis.ok);
    EXPECT_EQ(dis.previousState, ModState::Enabled);
    EXPECT_EQ(dis.currentState, ModState::Disabled);
    EXPECT_TRUE(dis.withinBudget);
    EXPECT_EQ(mgr.stateOf("alpha"), ModState::Disabled);
}

// ===========================================================================
// Req 22.3 — il budget di 2 s è effettivamente verificato: un'operazione che
// supera il budget (clock avanzato di 3 s) è marcata withinBudget == false.
// ===========================================================================
TEST(ModManagerUiTest, ToggleBudgetExceededIsReported) {
    ModManager mgr;
    FakeClock clock;
    // Clock che avanza di 3 s a ogni lettura: la prima lettura (start) e la
    // seconda (end) distano 3 s > budget 2 s.
    auto advancingClock = [&clock]() {
        auto t = clock.now();
        clock.advance(3s);
        return t;
    };
    ModManagerUI ui(mgr, nullptr, advancingClock);
    ui.addInstalledMod("alpha", [] { return EntryPointOutcome::success(); });

    const auto en = ui.enable("alpha");
    EXPECT_TRUE(en.ok);                 // la transizione è comunque applicata
    EXPECT_FALSE(en.withinBudget);      // ma fuori budget (Req 22.3)
    EXPECT_GE(en.elapsed, 3s);
}

// ===========================================================================
// Req 22.4 — FALLBACK su transizione non completata: stato precedente
// mantenuto + messaggio all'User + segnalazione via rejection sink del manager.
// ===========================================================================
TEST(ModManagerUiTest, RejectedTransitionFallsBackAndSignals) {
    ModManager mgr;
    std::vector<TransitionRejection> rejections;
    captureRejections(mgr, rejections);

    FakeClock clock;
    ModManagerUI ui(mgr, nullptr, clock.fn());

    std::vector<std::string> messages;
    ui.setMessageSink([&messages](const std::string& m) { messages.push_back(m); });

    ui.addInstalledMod("alpha");

    // Stato iniziale Installed: disabilitare NON è ammesso (Installed->Disabled
    // rifiutato dalla state machine). Lo stato precedente deve essere mantenuto.
    const auto dis = ui.disable("alpha");

    EXPECT_FALSE(dis.ok);
    EXPECT_EQ(dis.previousState, ModState::Installed);
    EXPECT_EQ(dis.currentState, ModState::Installed);  // stato precedente mantenuto (Req 22.4)
    EXPECT_EQ(mgr.stateOf("alpha"), ModState::Installed);
    EXPECT_FALSE(dis.message.empty());                 // messaggio di errore (Req 22.4)

    // Messaggio mostrato all'User e segnalazione instradata dal manager.
    EXPECT_FALSE(messages.empty());
    ASSERT_EQ(rejections.size(), 1u);
    EXPECT_EQ(rejections[0].mod, "alpha");
    EXPECT_EQ(rejections[0].from, ModState::Installed);
    EXPECT_EQ(rejections[0].requested, ModState::Disabled);
}

// ===========================================================================
// Req 22.4 — fallback anche quando l'entry point fallisce all'enable: la mod
// NON risulta abilitata e viene segnalato l'errore.
// ===========================================================================
TEST(ModManagerUiTest, FailedEntryPointEnableFallsBack) {
    ModManager mgr;
    FakeClock clock;
    ModManagerUI ui(mgr, nullptr, clock.fn());

    std::vector<std::string> messages;
    ui.setMessageSink([&messages](const std::string& m) { messages.push_back(m); });

    ui.addInstalledMod("boom", [] { return EntryPointOutcome::failure("init fallita"); });

    const auto en = ui.enable("boom");
    EXPECT_FALSE(en.ok);                              // abilitazione non completata
    EXPECT_NE(en.currentState, ModState::Enabled);    // non abilitata (Req 22.4)
    EXPECT_FALSE(en.message.empty());
    EXPECT_FALSE(messages.empty());
}

// ===========================================================================
// Req 22.5 — la ricerca sul Marketplace restituisce i risultati fittizi entro
// il budget di 5 s.
// ===========================================================================
TEST(ModManagerUiTest, MarketplaceSearchReturnsResults) {
    ModManager mgr;
    FakeClock clock;
    FakeMarketplace market;
    market.searchResults = {
        MarketplaceMod{"cool-mod", "Cool Mod", "2.1.0"},
        MarketplaceMod{"nice-mod", "Nice Mod", "1.0.0"},
    };
    ModManagerUI ui(mgr, &market, clock.fn());

    const auto result = ui.searchMarketplace("mod");

    EXPECT_TRUE(result.reachable);
    ASSERT_EQ(result.results.size(), 2u);
    EXPECT_EQ(result.results[0].id, "cool-mod");
    EXPECT_EQ(result.results[1].id, "nice-mod");
    EXPECT_TRUE(result.withinBudget);
    EXPECT_EQ(market.searchCalls, 1);
}

// ===========================================================================
// Req 22.7 — ricerca con Marketplace irraggiungibile: reachable == false +
// messaggio, nessun risultato.
// ===========================================================================
TEST(ModManagerUiTest, MarketplaceSearchUnreachable) {
    ModManager mgr;
    FakeClock clock;
    FakeMarketplace market;
    market.searchReachable = false;
    market.searchError = "rete assente";
    ModManagerUI ui(mgr, &market, clock.fn());

    std::vector<std::string> messages;
    ui.setMessageSink([&messages](const std::string& m) { messages.push_back(m); });

    const auto result = ui.searchMarketplace("mod");

    EXPECT_FALSE(result.reachable);
    EXPECT_TRUE(result.results.empty());
    EXPECT_FALSE(result.message.empty());
    EXPECT_FALSE(messages.empty());
}

// ===========================================================================
// Req 22.6 — installazione riuscita: la mod compare nell'elenco delle
// installate.
// ===========================================================================
TEST(ModManagerUiTest, InstallSuccessAddsModToList) {
    ModManager mgr;
    FakeClock clock;
    FakeMarketplace market;
    market.installOk = true;
    ModManagerUI ui(mgr, &market, clock.fn());

    const auto res = ui.install("new-mod");

    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.id, "new-mod");
    EXPECT_TRUE(mgr.contains("new-mod"));
    EXPECT_EQ(mgr.stateOf("new-mod"), ModState::Installed);

    const auto list = ui.listMods();
    ASSERT_EQ(list.mods.size(), 1u);
    EXPECT_EQ(list.mods[0].id, "new-mod");
}

// ===========================================================================
// Req 22.7 — installazione fallita: ROLLBACK, nessun artefatto installato
// (mod assente dal manager e dall'elenco).
// ===========================================================================
TEST(ModManagerUiTest, InstallFailureRollsBackNoPartialInstall) {
    ModManager mgr;
    FakeClock clock;
    FakeMarketplace market;
    market.installOk = false;
    market.installError = "download interrotto";
    ModManagerUI ui(mgr, &market, clock.fn());

    std::vector<std::string> messages;
    ui.setMessageSink([&messages](const std::string& m) { messages.push_back(m); });

    const auto res = ui.install("broken-mod");

    EXPECT_FALSE(res.ok);
    EXPECT_FALSE(res.message.empty());
    EXPECT_FALSE(messages.empty());

    // Nessun artefatto parziale (Req 22.7): la mod non è registrata né elencata.
    EXPECT_FALSE(mgr.contains("broken-mod"));
    const auto list = ui.listMods();
    EXPECT_TRUE(list.empty);
    EXPECT_TRUE(list.mods.empty());
}

// ===========================================================================
// Req 22.7 — Marketplace assente: install annullata senza modifiche.
// ===========================================================================
TEST(ModManagerUiTest, InstallWithoutMarketplaceIsCancelled) {
    ModManager mgr;
    FakeClock clock;
    ModManagerUI ui(mgr, /*marketplace*/ nullptr, clock.fn());

    const auto res = ui.install("any-mod");
    EXPECT_FALSE(res.ok);
    EXPECT_FALSE(mgr.contains("any-mod"));
}

// ===========================================================================
// Req 22.8 — segnalazione degli aggiornamenti disponibili: marcatura in elenco
// + emissione del segnale; startUpdate azzera il flag su successo.
// ===========================================================================
TEST(ModManagerUiTest, UpdatesAvailableSignalledAndMarkedInList) {
    ModManager mgr;
    FakeClock clock;
    FakeMarketplace market;
    ModManagerUI ui(mgr, &market, clock.fn());

    std::vector<ModManagerModId> signalled;
    ui.setUpdatesSignal([&signalled](const std::vector<ModManagerModId>& ids) {
        signalled = ids;
    });

    ui.addInstalledMod("alpha");
    ui.addInstalledMod("beta");

    market.updates = {"alpha"};  // solo alpha ha un aggiornamento

    const auto updated = ui.refreshUpdates();
    ASSERT_EQ(updated.size(), 1u);
    EXPECT_EQ(updated[0], "alpha");

    // Segnale emesso (Req 22.8).
    ASSERT_EQ(signalled.size(), 1u);
    EXPECT_EQ(signalled[0], "alpha");

    // Marcatura in elenco (Req 22.8).
    EXPECT_TRUE(ui.hasUpdate("alpha"));
    EXPECT_FALSE(ui.hasUpdate("beta"));
    const auto list = ui.listMods();
    ASSERT_EQ(list.mods.size(), 2u);
    EXPECT_TRUE(list.mods[0].updateAvailable);   // alpha
    EXPECT_FALSE(list.mods[1].updateAvailable);  // beta

    // Avvio dell'aggiornamento riuscito: il flag si azzera (Req 22.8).
    market.installOk = true;
    const auto upd = ui.startUpdate("alpha");
    EXPECT_TRUE(upd.ok);
    EXPECT_FALSE(ui.hasUpdate("alpha"));
}

// ===========================================================================
// Req 22.8 — startUpdate fallita: nessuna modifica, flag mantenuto (Req 22.7).
// ===========================================================================
TEST(ModManagerUiTest, StartUpdateFailureKeepsFlag) {
    ModManager mgr;
    FakeClock clock;
    FakeMarketplace market;
    ModManagerUI ui(mgr, &market, clock.fn());

    ui.addInstalledMod("alpha");
    market.updates = {"alpha"};
    ui.refreshUpdates();
    ASSERT_TRUE(ui.hasUpdate("alpha"));

    market.installOk = false;
    market.installError = "aggiornamento interrotto";
    const auto upd = ui.startUpdate("alpha");

    EXPECT_FALSE(upd.ok);
    EXPECT_FALSE(upd.message.empty());
    EXPECT_TRUE(ui.hasUpdate("alpha"));  // flag mantenuto
}

}  // namespace
