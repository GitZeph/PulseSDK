// tests/settings_ui_test.cpp — unit test dell'UI auto-generata delle
// impostazioni e della persistenza sull'edit (task 31.2, Req 9.2, 9.3, 9.5,
// 10.3, 10.6).
//
// Verifica il controller host-testabile pulse::SettingsUiController:
//   * (Req 9.2) un controllo di input per CIASCUNA impostazione dichiarata,
//     con il ControlKind corretto e, per le enum, l'elenco delle etichette;
//   * (Req 9.3) un edit conforme è persistito ed è riletto al ciclo successivo,
//     entro il budget di 1 s (clock iniettato);
//   * (Req 9.5) un edit non conforme è rifiutato: il valore precedente è
//     conservato, viene emesso un messaggio e nulla è persistito;
//   * (Req 10.3) round-trip dei valori persistiti (anche float esatto);
//   * (Req 10.6) su rimozione mod i dati sono eliminati SOLO con conferma
//     dell'User, conservati altrimenti.

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include <pulse/settings_ui.hpp>

namespace {

using pulse::SettingDecl;
using pulse::SettingsRegistry;
using pulse::SettingType;
using pulse::SettingValue;
using pulse::SettingsUiController;
using pulse::ControlKind;
using pulse::storage::ModStorage;

// Costruisce un registro con un'impostazione per ciascun tipo supportato.
SettingsRegistry makeRegistry() {
    SettingsRegistry registry;
    EXPECT_TRUE(registry
                    .declare(SettingDecl{"flag", SettingType::Bool,
                                         SettingValue::Bool(false), {}})
                    .ok);
    EXPECT_TRUE(registry
                    .declare(SettingDecl{"count", SettingType::Int,
                                         SettingValue::Int(7), {}})
                    .ok);
    EXPECT_TRUE(registry
                    .declare(SettingDecl{"ratio", SettingType::Float,
                                         SettingValue::Float(0.5), {}})
                    .ok);
    EXPECT_TRUE(registry
                    .declare(SettingDecl{"label", SettingType::String,
                                         SettingValue::String("ciao"), {}})
                    .ok);
    EXPECT_TRUE(registry
                    .declare(SettingDecl{"mode", SettingType::Enum,
                                         SettingValue::Enum("easy"),
                                         {"easy", "normal", "hard"}})
                    .ok);
    return registry;
}

// --- Req 9.2 — un controllo per impostazione, del tipo corretto ------------

TEST(SettingsUiControllerTest, GeneratesOneControlPerDeclaredSetting) {
    SettingsRegistry registry = makeRegistry();
    ModStorage store("mod.id");
    SettingsUiController controller(registry, store);

    const auto controls = controller.generateControls();

    // Un controllo per impostazione, nell'ordine di dichiarazione.
    ASSERT_EQ(controls.size(), 5u);
    EXPECT_EQ(controls[0].name, "flag");
    EXPECT_EQ(controls[1].name, "count");
    EXPECT_EQ(controls[2].name, "ratio");
    EXPECT_EQ(controls[3].name, "label");
    EXPECT_EQ(controls[4].name, "mode");

    // Il ControlKind deriva dal SettingType.
    EXPECT_EQ(controls[0].kind, ControlKind::Toggle);
    EXPECT_EQ(controls[1].kind, ControlKind::NumericInt);
    EXPECT_EQ(controls[2].kind, ControlKind::NumericFloat);
    EXPECT_EQ(controls[3].kind, ControlKind::Text);
    EXPECT_EQ(controls[4].kind, ControlKind::Dropdown);

    // Il dropdown enum espone le etichette ammesse.
    EXPECT_EQ(controls[4].enumOptions,
              (std::vector<std::string>{"easy", "normal", "hard"}));

    // In assenza di valore persistito, il controllo mostra il default.
    EXPECT_EQ(controls[1].currentValue, SettingValue::Int(7));
    EXPECT_EQ(controls[4].currentValue, SettingValue::Enum("easy"));
}

// --- Req 9.3 / 10.3 — edit valido persiste, riletto entro budget -----------

TEST(SettingsUiControllerTest, ValidEditPersistsAndIsReadBack) {
    SettingsRegistry registry = makeRegistry();
    ModStorage store("mod.id");
    SettingsUiController controller(registry, store);

    const auto result = controller.edit("count", SettingValue::Int(42));
    EXPECT_TRUE(result.accepted);
    EXPECT_TRUE(result.persisted);
    EXPECT_TRUE(result.withinBudget);
    EXPECT_EQ(result.value, SettingValue::Int(42));

    // Letto al ciclo successivo (Req 9.3) e visibile nei controlli rigenerati.
    EXPECT_EQ(controller.currentValue("count"), SettingValue::Int(42));
    const auto controls = controller.generateControls();
    EXPECT_EQ(controls[1].currentValue, SettingValue::Int(42));

    // Round-trip esatto di un float (Req 10.3).
    const double precise = 3.141592653589793;
    EXPECT_TRUE(controller.edit("ratio", SettingValue::Float(precise)).accepted);
    EXPECT_EQ(controller.currentValue("ratio"), SettingValue::Float(precise));
    EXPECT_DOUBLE_EQ(controller.currentValue("ratio").asFloat(), precise);
}

// --- Req 9.3 — persistenza entro il budget di 1 s (clock iniettato) --------

TEST(SettingsUiControllerTest, PersistenceWithinOneSecondBudget) {
    SettingsRegistry registry = makeRegistry();
    ModStorage store("mod.id");

    // Clock che avanza di 250 ms ad ogni lettura: l'edit consuma due letture
    // (start/end) => 250 ms di elapsed, entro il budget di 1 s.
    auto now = std::chrono::steady_clock::time_point{};
    SettingsUiController withinBudget(
        registry, store,
        [&now] {
            const auto t = now;
            now += std::chrono::milliseconds(250);
            return t;
        });
    EXPECT_TRUE(withinBudget.edit("flag", SettingValue::Bool(true)).withinBudget);

    // Clock che avanza di 5 s tra start ed end: oltre il budget => withinBudget
    // falso (la persistenza è comunque registrata, ma fuori budget).
    SettingsRegistry registry2 = makeRegistry();
    ModStorage store2("mod.id");
    auto now2 = std::chrono::steady_clock::time_point{};
    SettingsUiController overBudget(
        registry2, store2,
        [&now2] {
            const auto t = now2;
            now2 += std::chrono::seconds(5);
            return t;
        });
    const auto over = overBudget.edit("flag", SettingValue::Bool(true));
    EXPECT_TRUE(over.accepted);
    EXPECT_FALSE(over.withinBudget);
}

// --- Req 9.5 — edit non conforme rifiutato, precedente conservato ----------

TEST(SettingsUiControllerTest, InvalidEditRejectedKeepsPreviousAndEmitsMessage) {
    SettingsRegistry registry = makeRegistry();
    ModStorage store("mod.id");
    std::vector<std::string> messages;
    SettingsUiController controller(
        registry, store, SettingsUiController::defaultClock(),
        SettingsUiController::Confirm{},
        [&messages](const std::string& m) { messages.push_back(m); });

    // Persiste prima un valore valido.
    ASSERT_TRUE(controller.edit("count", SettingValue::Int(10)).accepted);

    // Edit non conforme: tipo errato (String su Int).
    const auto rejected = controller.edit("count", SettingValue::String("x"));
    EXPECT_FALSE(rejected.accepted);
    EXPECT_FALSE(rejected.persisted);
    EXPECT_TRUE(rejected.message.has_value());
    // Il valore precedente è conservato (Req 9.5).
    EXPECT_EQ(rejected.value, SettingValue::Int(10));
    EXPECT_EQ(controller.currentValue("count"), SettingValue::Int(10));
    // È stato emesso un messaggio per il rifiuto.
    EXPECT_FALSE(messages.empty());

    // Etichetta enum non ammessa: rifiutata, default conservato.
    const auto badEnum = controller.edit("mode", SettingValue::Enum("insane"));
    EXPECT_FALSE(badEnum.accepted);
    EXPECT_EQ(controller.currentValue("mode"), SettingValue::Enum("easy"));

    // Niente è stato persistito per il valore non valido.
    EXPECT_FALSE(store.contains("mode"));
}

// --- Req 10.6 — eliminazione dati solo dopo conferma -----------------------

TEST(SettingsUiControllerTest, RemovalPreservesDataWhenNotConfirmed) {
    SettingsRegistry registry = makeRegistry();
    ModStorage store("mod.id");
    SettingsUiController controller(
        registry, store, SettingsUiController::defaultClock(),
        [] { return false; });  // User NON conferma

    ASSERT_TRUE(controller.edit("count", SettingValue::Int(99)).accepted);
    ASSERT_TRUE(store.contains("count"));

    const auto removal = controller.deletePersistedData();
    EXPECT_FALSE(removal.confirmed);
    EXPECT_FALSE(removal.deleted);
    // I dati sono conservati finché l'User non conferma (Req 10.6).
    EXPECT_TRUE(store.contains("count"));
    EXPECT_EQ(controller.currentValue("count"), SettingValue::Int(99));
}

TEST(SettingsUiControllerTest, RemovalDeletesDataWhenConfirmed) {
    SettingsRegistry registry = makeRegistry();
    ModStorage store("mod.id");
    SettingsUiController controller(
        registry, store, SettingsUiController::defaultClock(),
        [] { return true; });  // User conferma

    ASSERT_TRUE(controller.edit("count", SettingValue::Int(99)).accepted);
    ASSERT_TRUE(controller.edit("flag", SettingValue::Bool(true)).accepted);
    ASSERT_TRUE(store.contains("count"));

    const auto removal = controller.deletePersistedData();
    EXPECT_TRUE(removal.confirmed);
    EXPECT_TRUE(removal.deleted);
    EXPECT_EQ(removal.removedKeys, 2u);
    // I dati persistiti sono stati eliminati.
    EXPECT_FALSE(store.contains("count"));
    EXPECT_FALSE(store.contains("flag"));
    // Dopo l'eliminazione, i controlli tornano ai default.
    EXPECT_EQ(controller.currentValue("count"), SettingValue::Int(7));
}

// --- Edit di impostazione sconosciuta: rifiutato con messaggio -------------

TEST(SettingsUiControllerTest, EditUnknownSettingRejected) {
    SettingsRegistry registry = makeRegistry();
    ModStorage store("mod.id");
    SettingsUiController controller(registry, store);

    const auto result = controller.edit("nope", SettingValue::Int(1));
    EXPECT_FALSE(result.accepted);
    EXPECT_FALSE(result.persisted);
    EXPECT_TRUE(result.message.has_value());
    EXPECT_FALSE(store.contains("nope"));
}

}  // namespace
