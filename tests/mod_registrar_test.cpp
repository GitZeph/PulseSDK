// tests/mod_registrar_test.cpp — unit test di pulse::loader::register_external_hook
// (external-mod-loading).
//
// Concern 1: il Loader possiede una COPIA stabile della stringa del simbolo.
// `HookRegistration::symbol` è una `std::string_view` NON proprietaria, quindi
// registrare un hook a partire da una stringa temporanea (che poi esce di
// scope) lascerebbe una view pendente se il registrar non copiasse la stringa.
// Questo test costruisce i simboli da temporanei, li lascia uscire di scope e
// poi verifica via `pulse::hooks::find` che la registrazione conservi un
// simbolo ancora uguale (prova della copia stabile), con detour/trampolino/
// priorità corretti, e che `pulse::hooks::count()` sia cresciuto di conseguenza.
//
// NB: questo eseguibile NON definisce `pulse_loader_register_hook`, quindi
// `pulse::hooks::register_hook` (chiamata internamente da register_external_hook)
// usa il path LOCALE: gli hook atterrano nel registro di questa immagine.
//
// La demo del loader linkato può pre-registrare voci nello static-init; per
// questo misuriamo i DELTA attorno a `pulse::hooks::registry()` invece di
// assumere un registro vuoto.

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

#include <pulse/hooks.hpp>

#include "core/mod_registrar.hpp"

namespace {

// Costruisce un simbolo da una stringa temporanea e lo registra passando una
// `std::string_view` che riferisce quel temporaneo; il temporaneo esce di scope
// al ritorno della funzione, mettendo alla prova la proprietà della copia.
void register_from_temporary(const std::string& base, void* detour,
                             void** trampoline, int priority) {
    std::string temp = base;                 // stringa locale temporanea
    std::string_view view = temp;            // view sul temporaneo
    (void)pulse::loader::register_external_hook(view, detour, trampoline,
                                                priority);
    // `temp` viene distrutto qui: se il registrar non copia, la view salvata
    // diventa pendente.
}

TEST(ModRegistrar, OwnsStableSymbolCopyAcrossTemporaries) {
    int detour_a = 0;
    int detour_b = 0;
    int detour_c = 0;
    void* tramp_a = nullptr;
    void* tramp_b = nullptr;
    void* tramp_c = nullptr;

    const std::size_t before = pulse::hooks::count();

    register_from_temporary("Ext::alpha", &detour_a, &tramp_a, 300);
    register_from_temporary("Ext::beta", &detour_b, &tramp_b, 500);
    register_from_temporary("Ext::gamma", &detour_c, &tramp_c, 700);

    const std::size_t after = pulse::hooks::count();
    EXPECT_EQ(after - before, 3u);  // il registro è cresciuto di 3

    // Sporca lo stack per far emergere eventuali view pendenti.
    volatile char scratch[256];
    for (std::size_t i = 0; i < sizeof(scratch); ++i) {
        scratch[i] = static_cast<char>(i);
    }
    (void)scratch;

    const auto* a = pulse::hooks::find("Ext::alpha");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->symbol, "Ext::alpha");  // la copia è ancora valida e uguale
    EXPECT_EQ(a->detour, reinterpret_cast<void*>(&detour_a));
    EXPECT_EQ(a->trampoline, reinterpret_cast<void**>(&tramp_a));
    EXPECT_EQ(a->priority, 300);

    const auto* b = pulse::hooks::find("Ext::beta");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->symbol, "Ext::beta");
    EXPECT_EQ(b->detour, reinterpret_cast<void*>(&detour_b));
    EXPECT_EQ(b->trampoline, reinterpret_cast<void**>(&tramp_b));
    EXPECT_EQ(b->priority, 500);

    const auto* c = pulse::hooks::find("Ext::gamma");
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->symbol, "Ext::gamma");
    EXPECT_EQ(c->detour, reinterpret_cast<void*>(&detour_c));
    EXPECT_EQ(c->trampoline, reinterpret_cast<void**>(&tramp_c));
    EXPECT_EQ(c->priority, 700);
}

// Il valore restituito riflette gli argomenti (descrittore di solo valore).
TEST(ModRegistrar, ReturnsRegistrationDescriptor) {
    int detour = 0;
    void* tramp = nullptr;
    std::string symbol = "Ext::returned";
    const auto reg = pulse::loader::register_external_hook(
        std::string_view{symbol}, &detour, &tramp, 450);
    EXPECT_EQ(reg.symbol, "Ext::returned");
    EXPECT_EQ(reg.detour, reinterpret_cast<void*>(&detour));
    EXPECT_EQ(reg.trampoline, reinterpret_cast<void**>(&tramp));
    EXPECT_EQ(reg.priority, 450);
}

}  // namespace
