// tests/mod_manager_test.cpp — unit test della state machine del ModManager
// (task 12.1, Requisiti 4.4, 4.5, 4.6, 4.8) e dell'isolamento del fallimento
// di inizializzazione (task 12.2, Requisiti 4.7, 28.4, 28.5).
//
// Verifica:
//   * transizioni AMMESSE applicate, transizioni NON ammesse rifiutate con
//     stato invariato + segnalazione che identifica mod e transizione (Req 4.4,
//     4.5);
//   * l'entry point è invocato all'enable e solo all'enable (Req 4.6);
//   * alla chiusura i terminator sono invocati in ordine INVERSO al
//     caricamento, solo per le mod abilitate (Req 4.8).
//
// Header del loader in loader/lifecycle/ (include relativo alla radice loader/);
// la logica è in mod_manager.cpp (compilata in pulse::loader via glob
// lifecycle/*.cpp).
#include "lifecycle/mod_manager.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

using namespace pulse::lifecycle;

namespace {

// -------------------------------------------------------------------------
// Stato iniziale e transizioni ammesse di base (Req 4.4).
// -------------------------------------------------------------------------
TEST(ModManager, RegistersInInstalledState) {
    ModManager mgr;
    mgr.registerMod("mod.a");
    ASSERT_TRUE(mgr.contains("mod.a"));
    EXPECT_EQ(mgr.stateOf("mod.a"), ModState::Installed);
}

TEST(ModManager, UnknownModTransitionReported) {
    ModManager mgr;
    const auto result = mgr.transition("ghost", ModState::Enabled);
    EXPECT_EQ(result.status, TransitionStatus::UnknownMod);
    EXPECT_FALSE(mgr.contains("ghost"));
}

TEST(ModManager, InstalledToEnabledAllowed) {
    ModManager mgr;
    mgr.registerMod("mod.a");
    const auto result = mgr.enable("mod.a");
    EXPECT_TRUE(result.applied());
    EXPECT_EQ(result.state, ModState::Enabled);
    EXPECT_EQ(mgr.stateOf("mod.a"), ModState::Enabled);
}

TEST(ModManager, EnableDisableEnableCycleAllowed) {
    ModManager mgr;
    mgr.registerMod("mod.a");
    EXPECT_TRUE(mgr.enable("mod.a").applied());
    EXPECT_TRUE(mgr.disable("mod.a").applied());
    EXPECT_EQ(mgr.stateOf("mod.a"), ModState::Disabled);
    EXPECT_TRUE(mgr.enable("mod.a").applied());
    EXPECT_EQ(mgr.stateOf("mod.a"), ModState::Enabled);
}

TEST(ModManager, RemoveAllowedFromInstalledAndDisabled) {
    ModManager mgr;
    mgr.registerMod("mod.a");
    EXPECT_TRUE(mgr.remove("mod.a").applied());  // Installed -> Removed
    EXPECT_EQ(mgr.stateOf("mod.a"), ModState::Removed);

    mgr.registerMod("mod.b");
    EXPECT_TRUE(mgr.enable("mod.b").applied());
    EXPECT_TRUE(mgr.disable("mod.b").applied());
    EXPECT_TRUE(mgr.remove("mod.b").applied());   // Disabled -> Removed
    EXPECT_EQ(mgr.stateOf("mod.b"), ModState::Removed);
}

// -------------------------------------------------------------------------
// Transizioni NON ammesse: rifiuto + stato invariato + segnalazione (Req 4.5).
// -------------------------------------------------------------------------
TEST(ModManager, RejectedTransitionKeepsStateAndReports) {
    ModManager mgr;
    std::vector<TransitionRejection> sink;
    mgr.setReportSink([&](const TransitionRejection& r) { sink.push_back(r); });
    mgr.registerMod("mod.a");

    // Installed -> Disabled NON è ammessa.
    const auto result = mgr.transition("mod.a", ModState::Disabled);

    EXPECT_TRUE(result.rejected());
    EXPECT_EQ(result.state, ModState::Installed);             // stato invariato
    EXPECT_EQ(mgr.stateOf("mod.a"), ModState::Installed);

    ASSERT_TRUE(result.rejection.has_value());
    EXPECT_EQ(result.rejection->mod, "mod.a");                // identifica la mod
    EXPECT_EQ(result.rejection->from, ModState::Installed);   // identifica la transizione
    EXPECT_EQ(result.rejection->requested, ModState::Disabled);

    // La segnalazione è instradata al sink e accumulata internamente.
    ASSERT_EQ(sink.size(), 1u);
    EXPECT_EQ(sink[0].mod, "mod.a");
    ASSERT_EQ(mgr.rejections().size(), 1u);
    EXPECT_EQ(mgr.rejections()[0].requested, ModState::Disabled);
}

TEST(ModManager, RemovedIsTerminalAllExitsRejected) {
    ModManager mgr;
    mgr.registerMod("mod.a");
    ASSERT_TRUE(mgr.remove("mod.a").applied());
    ASSERT_EQ(mgr.stateOf("mod.a"), ModState::Removed);

    for (ModState target : {ModState::Installed, ModState::Enabled, ModState::Disabled}) {
        const auto result = mgr.transition("mod.a", target);
        EXPECT_TRUE(result.rejected()) << "uscita da Removed dovrebbe essere rifiutata";
        EXPECT_EQ(mgr.stateOf("mod.a"), ModState::Removed);
    }
}

TEST(ModManager, EnabledToRemovedRejected) {
    // La state machine non ammette Enabled -> Removed diretto (serve disable).
    ModManager mgr;
    mgr.registerMod("mod.a");
    ASSERT_TRUE(mgr.enable("mod.a").applied());
    const auto result = mgr.remove("mod.a");
    EXPECT_TRUE(result.rejected());
    EXPECT_EQ(mgr.stateOf("mod.a"), ModState::Enabled);
}

TEST(ModManager, SameStateTransitionRejected) {
    ModManager mgr;
    mgr.registerMod("mod.a");
    const auto result = mgr.transition("mod.a", ModState::Installed);
    EXPECT_TRUE(result.rejected());
    EXPECT_EQ(mgr.stateOf("mod.a"), ModState::Installed);
}

TEST(ModManager, AllowedTransitionTableMatchesDesign) {
    // Tabella attesa dal diagramma del design (Layer 4).
    auto A = [](ModState f, ModState t) { return ModManager::isAllowed(f, t); };
    EXPECT_TRUE(A(ModState::Installed, ModState::Enabled));
    EXPECT_TRUE(A(ModState::Installed, ModState::Removed));
    EXPECT_TRUE(A(ModState::Enabled, ModState::Disabled));
    EXPECT_TRUE(A(ModState::Disabled, ModState::Enabled));
    EXPECT_TRUE(A(ModState::Disabled, ModState::Removed));
    // Non ammesse rappresentative.
    EXPECT_FALSE(A(ModState::Installed, ModState::Disabled));
    EXPECT_FALSE(A(ModState::Enabled, ModState::Removed));
    EXPECT_FALSE(A(ModState::Enabled, ModState::Installed));
    EXPECT_FALSE(A(ModState::Removed, ModState::Enabled));
    EXPECT_FALSE(A(ModState::Installed, ModState::Installed));
}

// -------------------------------------------------------------------------
// Entry point invocato all'enable (Req 4.6).
// -------------------------------------------------------------------------
TEST(ModManager, EntryPointInvokedOnEnable) {
    ModManager mgr;
    int calls = 0;
    mgr.registerMod("mod.a", [&]() {
        ++calls;
        return EntryPointOutcome::success();
    });

    const auto result = mgr.enable("mod.a");
    EXPECT_TRUE(result.applied());
    EXPECT_EQ(result.entryPoint, EntryPointStatus::Ok);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(mgr.stateOf("mod.a"), ModState::Enabled);
}

TEST(ModManager, EntryPointNotInvokedOnDisable) {
    ModManager mgr;
    int calls = 0;
    mgr.registerMod("mod.a", [&]() {
        ++calls;
        return EntryPointOutcome::success();
    });
    ASSERT_TRUE(mgr.enable("mod.a").applied());
    EXPECT_EQ(calls, 1);

    const auto result = mgr.disable("mod.a");
    EXPECT_TRUE(result.applied());
    EXPECT_EQ(result.entryPoint, EntryPointStatus::NotInvoked);
    EXPECT_EQ(calls, 1);  // il disable non invoca l'entry point
}

TEST(ModManager, EntryPointReInvokedOnReEnable) {
    ModManager mgr;
    int calls = 0;
    mgr.registerMod("mod.a", [&]() {
        ++calls;
        return EntryPointOutcome::success();
    });
    ASSERT_TRUE(mgr.enable("mod.a").applied());
    ASSERT_TRUE(mgr.disable("mod.a").applied());
    ASSERT_TRUE(mgr.enable("mod.a").applied());
    EXPECT_EQ(calls, 2);  // invocato a ogni enable (Req 4.6)
}

TEST(ModManager, EntryPointNotInvokedOnRejectedTransition) {
    ModManager mgr;
    int calls = 0;
    mgr.registerMod("mod.a", [&]() {
        ++calls;
        return EntryPointOutcome::success();
    });
    ASSERT_TRUE(mgr.remove("mod.a").applied());  // Installed -> Removed
    // Removed -> Enabled rifiutata: entry point NON invocato.
    const auto result = mgr.enable("mod.a");
    EXPECT_TRUE(result.rejected());
    EXPECT_EQ(calls, 0);
}

TEST(ModManager, EntryPointFailureMovesModToDisabled) {
    // Req 4.7: se l'entry point restituisce errore, la mod NON passa a Enabled
    // ma viene portata a Disabled, con una segnalazione (mod + causa) prodotta
    // e l'esito riportato al chiamante.
    ModManager mgr;
    std::vector<InitFailure> sink;
    mgr.setInitFailureSink([&](const InitFailure& f) { sink.push_back(f); });
    mgr.registerMod("mod.a", [&]() {
        return EntryPointOutcome::failure("init boom");
    });
    const auto result = mgr.enable("mod.a");
    EXPECT_EQ(result.entryPoint, EntryPointStatus::Error);
    EXPECT_EQ(result.entryPointMessage, "init boom");
    EXPECT_NE(mgr.stateOf("mod.a"), ModState::Enabled);
    EXPECT_EQ(mgr.stateOf("mod.a"), ModState::Disabled);  // Req 4.7

    // Segnalazione che identifica mod e causa (Req 4.7, 28.5).
    ASSERT_EQ(sink.size(), 1u);
    EXPECT_EQ(sink[0].mod, "mod.a");
    EXPECT_EQ(sink[0].cause, "init boom");
    EXPECT_FALSE(sink[0].threw);
    ASSERT_EQ(mgr.initFailures().size(), 1u);
    EXPECT_EQ(mgr.initFailures()[0].mod, "mod.a");
}

TEST(ModManager, EntryPointThrowingIsIsolatedAndMovesModToDisabled) {
    // Req 4.7 + 28.4: un'eccezione lanciata dall'entry point è ISOLATA (non
    // propagata), la mod è portata a Disabled e si produce una segnalazione
    // che identifica mod e causa (Req 28.5).
    ModManager mgr;
    std::vector<InitFailure> sink;
    mgr.setInitFailureSink([&](const InitFailure& f) { sink.push_back(f); });
    mgr.registerMod("mod.boom", [&]() -> EntryPointOutcome {
        throw std::runtime_error("kaboom");
    });

    TransitionResult result;
    EXPECT_NO_THROW({ result = mgr.enable("mod.boom"); });  // eccezione isolata
    EXPECT_EQ(result.entryPoint, EntryPointStatus::Threw);
    EXPECT_EQ(result.entryPointMessage, "kaboom");
    EXPECT_EQ(mgr.stateOf("mod.boom"), ModState::Disabled);

    ASSERT_EQ(sink.size(), 1u);
    EXPECT_EQ(sink[0].mod, "mod.boom");
    EXPECT_EQ(sink[0].cause, "kaboom");
    EXPECT_TRUE(sink[0].threw);
}

TEST(ModManager, EntryPointFailureFromDisabledStaysDisabled) {
    // Re-enable di una mod Disabled il cui entry point fallisce: resta Disabled.
    ModManager mgr;
    bool fail = false;
    mgr.registerMod("mod.a", [&]() {
        return fail ? EntryPointOutcome::failure("boom") : EntryPointOutcome::success();
    });
    ASSERT_TRUE(mgr.enable("mod.a").applied());
    ASSERT_TRUE(mgr.disable("mod.a").applied());
    ASSERT_EQ(mgr.stateOf("mod.a"), ModState::Disabled);

    fail = true;
    const auto result = mgr.enable("mod.a");
    EXPECT_EQ(result.entryPoint, EntryPointStatus::Error);
    EXPECT_EQ(mgr.stateOf("mod.a"), ModState::Disabled);
}

// -------------------------------------------------------------------------
// enableAll: isolamento del fallimento di init + prosecuzione (Req 4.7, 28.4,
// 28.5).
// -------------------------------------------------------------------------
TEST(ModManager, EnableAllContinuesAfterOneFailure) {
    // Una mod nel mezzo dell'ordine fallisce l'init: deve essere isolata
    // (Disabled) mentre le altre proseguono e vengono abilitate (Req 28.4).
    ModManager mgr;
    std::vector<std::string> initialized;
    const std::vector<std::string> loadOrder = {"a", "b", "c"};

    mgr.registerMod("a", [&]() { initialized.push_back("a"); return EntryPointOutcome::success(); });
    mgr.registerMod("b", [&]() {
        initialized.push_back("b");
        return EntryPointOutcome::failure("b failed");
    });
    mgr.registerMod("c", [&]() { initialized.push_back("c"); return EntryPointOutcome::success(); });

    const auto result = mgr.enableAll(loadOrder);

    // "a" e "c" abilitate, "b" isolata.
    const std::vector<std::string> expectedEnabled = {"a", "c"};
    EXPECT_EQ(result.enabled, expectedEnabled);
    ASSERT_EQ(result.failed.size(), 1u);
    EXPECT_EQ(result.failed[0].mod, "b");
    EXPECT_EQ(result.failed[0].cause, "b failed");
    EXPECT_FALSE(result.failed[0].threw);

    // Tutte le mod sono state visitate, anche dopo il fallimento di "b".
    const std::vector<std::string> expectedInit = {"a", "b", "c"};
    EXPECT_EQ(initialized, expectedInit);

    EXPECT_EQ(mgr.stateOf("a"), ModState::Enabled);
    EXPECT_EQ(mgr.stateOf("b"), ModState::Disabled);  // isolata (Req 4.7)
    EXPECT_EQ(mgr.stateOf("c"), ModState::Enabled);
}

TEST(ModManager, EnableAllIsolatesThrowingModAndContinues) {
    // Un'eccezione non gestita di una mod non deve interrompere il caricamento
    // delle altre (Req 28.4) e va registrata con mod + causa (Req 28.5).
    ModManager mgr;
    const std::vector<std::string> loadOrder = {"a", "boom", "c", "ghost"};

    mgr.registerMod("a", [&]() { return EntryPointOutcome::success(); });
    mgr.registerMod("boom", [&]() -> EntryPointOutcome { throw std::runtime_error("explode"); });
    mgr.registerMod("c", [&]() { return EntryPointOutcome::success(); });
    // "ghost" non registrata: ignorata da enableAll.

    EnableAllResult result;
    EXPECT_NO_THROW({ result = mgr.enableAll(loadOrder); });

    const std::vector<std::string> expectedEnabled = {"a", "c"};
    EXPECT_EQ(result.enabled, expectedEnabled);
    ASSERT_EQ(result.failed.size(), 1u);
    EXPECT_EQ(result.failed[0].mod, "boom");
    EXPECT_EQ(result.failed[0].cause, "explode");
    EXPECT_TRUE(result.failed[0].threw);

    EXPECT_EQ(mgr.stateOf("boom"), ModState::Disabled);
    EXPECT_FALSE(mgr.contains("ghost"));
    // Segnalazione accumulata internamente (mod + causa).
    ASSERT_EQ(mgr.initFailures().size(), 1u);
    EXPECT_EQ(mgr.initFailures()[0].mod, "boom");
}

// -------------------------------------------------------------------------
// Terminator in ordine INVERSO al caricamento, solo per le mod abilitate
// (Req 4.8).
// -------------------------------------------------------------------------
TEST(ModManager, TerminatorsInvokedInReverseLoadOrder) {
    ModManager mgr;
    std::vector<std::string> termOrder;
    const std::vector<std::string> loadOrder = {"a", "b", "c"};

    for (const auto& id : loadOrder) {
        mgr.registerMod(id, /*entry*/ {}, [&, id]() { termOrder.push_back(id); });
        ASSERT_TRUE(mgr.enable(id).applied());
    }

    const auto invoked = mgr.shutdown(loadOrder);

    const std::vector<std::string> expected = {"c", "b", "a"};
    EXPECT_EQ(termOrder, expected);  // ordine inverso al caricamento
    EXPECT_EQ(invoked, expected);
}

TEST(ModManager, ShutdownSkipsNonEnabledMods) {
    ModManager mgr;
    std::vector<std::string> termOrder;
    const std::vector<std::string> loadOrder = {"a", "b", "c"};

    for (const auto& id : loadOrder) {
        mgr.registerMod(id, /*entry*/ {}, [&, id]() { termOrder.push_back(id); });
    }
    // Solo "a" e "c" sono abilitate; "b" resta Installed.
    ASSERT_TRUE(mgr.enable("a").applied());
    ASSERT_TRUE(mgr.enable("c").applied());

    const auto invoked = mgr.shutdown(loadOrder);

    const std::vector<std::string> expected = {"c", "a"};
    EXPECT_EQ(termOrder, expected);
    EXPECT_EQ(invoked, expected);
}

TEST(ModManager, ShutdownIgnoresUnknownAndMissingTerminators) {
    ModManager mgr;
    std::vector<std::string> termOrder;
    // "a" abilitata senza terminator; "b" abilitata con terminator;
    // "ghost" non registrata ma presente nel load order.
    mgr.registerMod("a");  // nessun terminator
    mgr.registerMod("b", {}, [&]() { termOrder.push_back("b"); });
    ASSERT_TRUE(mgr.enable("a").applied());
    ASSERT_TRUE(mgr.enable("b").applied());

    const std::vector<std::string> loadOrder = {"a", "b", "ghost"};
    const auto invoked = mgr.shutdown(loadOrder);

    // Entrambe le mod abilitate sono incluse nell'ordine di invocazione
    // (reverse load order), ma solo "b" ha effettivamente un terminator.
    const std::vector<std::string> expectedInvoked = {"b", "a"};
    EXPECT_EQ(invoked, expectedInvoked);
    const std::vector<std::string> expectedTerm = {"b"};
    EXPECT_EQ(termOrder, expectedTerm);
}

}  // namespace
