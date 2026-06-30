// tests/bindings_test.cpp — unit test del Bindings System (Layer 2, task 3.1).
//
// Verifica il modello dei bindings e l'EmbeddedBindingsProvider:
//   * load a corrispondenza ESATTA della coppia (GD_Version, piattaforma) (Req 20.2);
//   * resolve a corrispondenza ESATTA del simbolo, senza fuzzy-match (Req 20.2);
//   * presenza del set MVP (2.2074, windows-x64) con MenuLayer::init (Req 20.1).

#include <gtest/gtest.h>

#include "bindings/bindings.hpp"
#include "bindings/embedded_bindings_provider.hpp"

namespace {

using namespace pulse::loader::bindings;

BindingKey mvpKey() {
    return BindingKey{GdVersion{2, 2074}, "windows-x64"};
}

// --- load: corrispondenza esatta della coppia ----------------------------

TEST(EmbeddedBindings, LoadsExactMvpKey) {
    EmbeddedBindingsProvider provider;
    auto set = provider.load(mvpKey());
    ASSERT_TRUE(set.has_value());
    EXPECT_EQ(set->key(), mvpKey());
    EXPECT_FALSE(set->functions().empty());
}

TEST(EmbeddedBindings, LoadRejectsWrongVersion) {
    EmbeddedBindingsProvider provider;
    // Versione diversa: nessuna corrispondenza esatta, nessun fuzzy-match.
    EXPECT_FALSE(provider.load(BindingKey{GdVersion{2, 2073}, "windows-x64"}).has_value());
    EXPECT_FALSE(provider.load(BindingKey{GdVersion{2, 2081}, "windows-x64"}).has_value());
}

TEST(EmbeddedBindings, LoadRejectsWrongPlatform) {
    EmbeddedBindingsProvider provider;
    // Piattaforma diversa: nessuna corrispondenza esatta.
    EXPECT_FALSE(provider.load(BindingKey{GdVersion{2, 2074}, "macos"}).has_value());
    EXPECT_FALSE(provider.load(BindingKey{GdVersion{2, 2074}, "android-arm64"}).has_value());
    // Differenza solo di case: NON deve corrispondere (match esatto, case-sensitive).
    EXPECT_FALSE(provider.load(BindingKey{GdVersion{2, 2074}, "Windows-X64"}).has_value());
}

// --- resolve: corrispondenza esatta del simbolo --------------------------

TEST(EmbeddedBindings, ResolvesMenuLayerInitAfterLoad) {
    EmbeddedBindingsProvider provider;
    ASSERT_TRUE(provider.load(mvpKey()).has_value());

    auto fn = provider.resolve("MenuLayer::init");
    ASSERT_TRUE(fn.has_value());
    EXPECT_EQ(fn->symbol, "MenuLayer::init");
    EXPECT_TRUE(fn->resolved);
    EXPECT_NE(fn->address, 0u);
    EXPECT_EQ(fn->signature.returnType, "bool");
    ASSERT_EQ(fn->signature.parameterTypes.size(), 1u);
    EXPECT_EQ(fn->signature.parameterTypes[0], "MenuLayer*");
}

TEST(EmbeddedBindings, ResolveIsExactNoFuzzyMatch) {
    EmbeddedBindingsProvider provider;
    ASSERT_TRUE(provider.load(mvpKey()).has_value());

    // Varianti vicine ma non identiche NON devono risolvere (no fuzzy-match).
    EXPECT_FALSE(provider.resolve("MenuLayer::Init").has_value());     // case diverso
    EXPECT_FALSE(provider.resolve("menulayer::init").has_value());     // case diverso
    EXPECT_FALSE(provider.resolve("MenuLayer::init ").has_value());    // spazio finale
    EXPECT_FALSE(provider.resolve("init").has_value());                // sottostringa
    EXPECT_FALSE(provider.resolve("MenuLayer::init2").has_value());    // prefisso
    EXPECT_FALSE(provider.resolve("PlayLayer::init").has_value());     // simbolo assente
}

TEST(EmbeddedBindings, ResolveReturnsNulloptWithoutLoad) {
    EmbeddedBindingsProvider provider;
    // Nessun set caricato: resolve deve restituire nullopt.
    EXPECT_FALSE(provider.resolve("MenuLayer::init").has_value());
}

// --- BindingSet::resolve in isolamento -----------------------------------

TEST(BindingSet, ResolveExactMatchOnly) {
    BindingSet set{mvpKey()};
    FunctionBinding fn;
    fn.symbol = "SomeClass::method";
    fn.address = 0x1000;
    fn.signature = Signature{"void", {}};
    fn.resolved = true;
    set.add(fn);

    EXPECT_TRUE(set.resolve("SomeClass::method").has_value());
    EXPECT_FALSE(set.resolve("SomeClass::Method").has_value());
    EXPECT_FALSE(set.resolve("SomeClass").has_value());
}

}  // namespace
