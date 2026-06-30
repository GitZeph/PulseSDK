// tests/settings_test.cpp — unit test delle impostazioni tipizzate (task 19.1).
//
// Verifica la dichiarazione TIPIZZATA delle impostazioni dello SDK (Req 9.1,
// 9.4, 9.6):
//   * SettingDecl con nome univoco 1..64, tipo nell'insieme supportato e
//     default conforme al tipo (Req 9.1);
//   * rifiuto dei nomi fuori dai limiti di lunghezza e dei default non conformi
//     (Req 9.1);
//   * rifiuto dei nomi duplicati con segnalazione che indica il nome in
//     conflitto (Req 9.6);
//   * caricamento di un valore persistito: fallback al default dichiarato con
//     segnalazione quando il valore non è conforme al tipo dichiarato (Req 9.4).
//
// Header-only: include solo l'header pubblico dello SDK <pulse/settings.hpp>.

#include <gtest/gtest.h>

#include <string>

#include <pulse/settings.hpp>

namespace {

using pulse::conforms;
using pulse::SettingDecl;
using pulse::SettingsRegistry;
using pulse::SettingType;
using pulse::SettingValue;

// --- Req 9.1: dichiarazione valida, tipi e default conforme -----------------

TEST(SettingsDecl, DeclaresValidTypedSettingsOfEachType) {
    SettingsRegistry registry;

    EXPECT_TRUE(registry.declare({"enabled", SettingType::Bool,
                                  SettingValue::Bool(true), {}})
                    .ok);
    EXPECT_TRUE(registry.declare({"count", SettingType::Int,
                                  SettingValue::Int(7), {}})
                    .ok);
    EXPECT_TRUE(registry.declare({"speed", SettingType::Float,
                                  SettingValue::Float(1.5), {}})
                    .ok);
    EXPECT_TRUE(registry.declare({"label", SettingType::String,
                                  SettingValue::String("hi"), {}})
                    .ok);
    EXPECT_TRUE(registry.declare({"mode", SettingType::Enum,
                                  SettingValue::Enum("fast"),
                                  {"slow", "fast"}})
                    .ok);

    EXPECT_EQ(registry.declarations().size(), 5u);
    EXPECT_TRUE(registry.contains("mode"));
    ASSERT_NE(registry.find("count"), nullptr);
    EXPECT_EQ(registry.find("count")->defaultValue, SettingValue::Int(7));
}

// --- Req 9.1: limiti di lunghezza del nome (1..64) --------------------------

TEST(SettingsDecl, RejectsNameLengthOutOfBounds) {
    SettingsRegistry registry;

    // Nome vuoto (lunghezza 0) rifiutato.
    EXPECT_FALSE(
        registry.declare({"", SettingType::Bool, SettingValue::Bool(false), {}})
            .ok);

    // Nome di 65 caratteri rifiutato.
    const std::string too_long(65, 'x');
    EXPECT_FALSE(registry
                     .declare({too_long, SettingType::Bool,
                               SettingValue::Bool(false), {}})
                     .ok);

    // Limiti inclusivi: 1 e 64 caratteri accettati.
    EXPECT_TRUE(
        registry.declare({"a", SettingType::Bool, SettingValue::Bool(false), {}})
            .ok);
    const std::string max_len(64, 'y');
    EXPECT_TRUE(registry
                    .declare({max_len, SettingType::Int, SettingValue::Int(0), {}})
                    .ok);
}

// --- Req 9.1: il default deve essere conforme al tipo dichiarato ------------

TEST(SettingsDecl, RejectsDefaultNotConformingToDeclaredType) {
    SettingsRegistry registry;

    // Tipo Int ma default booleano: rifiutato.
    EXPECT_FALSE(registry
                     .declare({"count", SettingType::Int,
                               SettingValue::Bool(true), {}})
                     .ok);

    // Enum con default che non appartiene alle etichette ammesse: rifiutato.
    EXPECT_FALSE(registry
                     .declare({"mode", SettingType::Enum,
                               SettingValue::Enum("turbo"), {"slow", "fast"}})
                     .ok);

    // Enum senza etichette ammesse: rifiutato.
    EXPECT_FALSE(registry
                     .declare({"empty_enum", SettingType::Enum,
                               SettingValue::Enum("x"), {}})
                     .ok);
}

// --- Req 9.6: rifiuto dei nomi duplicati con il nome in conflitto -----------

TEST(SettingsDecl, RejectsDuplicateNameAndReportsConflictingName) {
    SettingsRegistry registry;

    ASSERT_TRUE(registry
                    .declare({"volume", SettingType::Int, SettingValue::Int(50),
                              {}})
                    .ok);

    // Seconda dichiarazione con lo stesso nome (anche se di tipo diverso):
    // rifiutata, e la segnalazione contiene il nome in conflitto (Req 9.6).
    const auto dup = registry.declare(
        {"volume", SettingType::Float, SettingValue::Float(0.5), {}});
    EXPECT_FALSE(dup.ok);
    EXPECT_NE(dup.report.find("volume"), std::string::npos);

    // La dichiarazione originale resta invariata: una sola impostazione, di
    // tipo Int.
    EXPECT_EQ(registry.declarations().size(), 1u);
    ASSERT_NE(registry.find("volume"), nullptr);
    EXPECT_EQ(registry.find("volume")->type, SettingType::Int);
}

// --- Req 9.4: fallback al default su valore persistito non valido -----------

TEST(SettingsLoad, ReturnsPersistedValueWhenConforming) {
    SettingsRegistry registry;
    ASSERT_TRUE(registry
                    .declare({"count", SettingType::Int, SettingValue::Int(10),
                              {}})
                    .ok);

    const auto result = registry.loadPersisted("count", SettingValue::Int(42));
    EXPECT_FALSE(result.usedDefault);
    EXPECT_FALSE(result.report.has_value());
    EXPECT_EQ(result.value, SettingValue::Int(42));
}

TEST(SettingsLoad, FallsBackToDefaultOnTypeMismatchWithReport) {
    SettingsRegistry registry;
    ASSERT_TRUE(registry
                    .declare({"count", SettingType::Int, SettingValue::Int(10),
                              {}})
                    .ok);

    // Valore persistito di tipo errato (stringa per un Int): fallback + report.
    const auto result =
        registry.loadPersisted("count", SettingValue::String("oops"));
    EXPECT_TRUE(result.usedDefault);
    EXPECT_EQ(result.value, SettingValue::Int(10));
    ASSERT_TRUE(result.report.has_value());
    EXPECT_NE(result.report->find("count"), std::string::npos);

    // La dichiarazione resta invariata (Req 9.4).
    ASSERT_NE(registry.find("count"), nullptr);
    EXPECT_EQ(registry.find("count")->defaultValue, SettingValue::Int(10));
}

TEST(SettingsLoad, FallsBackWhenEnumLabelNotAllowed) {
    SettingsRegistry registry;
    ASSERT_TRUE(registry
                    .declare({"mode", SettingType::Enum,
                              SettingValue::Enum("slow"), {"slow", "fast"}})
                    .ok);

    // Etichetta persistita non ammessa: fallback al default "slow" + report.
    const auto result =
        registry.loadPersisted("mode", SettingValue::Enum("turbo"));
    EXPECT_TRUE(result.usedDefault);
    EXPECT_EQ(result.value, SettingValue::Enum("slow"));
    ASSERT_TRUE(result.report.has_value());
    EXPECT_NE(result.report->find("mode"), std::string::npos);

    // Un'etichetta ammessa viene invece accettata senza fallback.
    const auto ok = registry.loadPersisted("mode", SettingValue::Enum("fast"));
    EXPECT_FALSE(ok.usedDefault);
    EXPECT_EQ(ok.value, SettingValue::Enum("fast"));
}

// --- helper di conformità ---------------------------------------------------

TEST(SettingsConformance, EnumChecksMembershipOfLabel) {
    const SettingDecl decl{"mode", SettingType::Enum, SettingValue::Enum("a"),
                           {"a", "b"}};
    EXPECT_TRUE(conforms(decl, SettingValue::Enum("a")));
    EXPECT_TRUE(conforms(decl, SettingValue::Enum("b")));
    EXPECT_FALSE(conforms(decl, SettingValue::Enum("c")));
    EXPECT_FALSE(conforms(decl, SettingValue::Int(1)));
}

}  // namespace
