// tests/hooks_test.cpp — unit test della macro PULSE_HOOK minimale (task 4.3).
//
// Verifica la versione single-hook (MVP) del hooking dichiarativo dello SDK:
//   * PULSE_HOOK espande in un detour REGISTRATO presso il registro globale
//     usando solo un riferimento simbolico alla funzione bersaglio (Req 5.1);
//   * il detour è reindirizzabile e può invocare l'originale (Req 2.2);
//   * callOriginal preserva PARAMETRI e VALORE DI RITORNO della firma (Req 5.3),
//     per firme diverse (valore di ritorno scalare, booleano, puntatore "self").
//
// L'Hooking Engine reale (task 4.4) viene simulato: si "installa" l'hook
// cablando il trampolino verso una funzione originale di prova con
// pulse::hooks::bind_trampoline, poi si invoca il detour registrato.

#include <gtest/gtest.h>

#include <cstdint>

#include <pulse/hooks.hpp>

namespace {

// --- Funzioni "originali" di prova + contatori di invocazione --------------

int g_add_calls = 0;
int original_add(int a, int b) {
    ++g_add_calls;
    return a + b;
}

int g_init_calls = 0;
struct FakeLayer {
    int value = 0;
};
bool original_init(FakeLayer* self) {
    ++g_init_calls;
    self->value = 42;  // effetto osservabile dell'originale
    return true;
}

int g_identity_calls = 0;
const char* original_identity(const char* s) {
    ++g_identity_calls;
    return s;
}

}  // namespace

// --- Dichiarazione degli hook (a livello di file, come in una mod reale) ---

// Hook con ritorno scalare e due parametri: il detour aggiunge 100 al valore
// originale, dimostrando che parametri e ritorno passano inalterati.
PULSE_HOOK(test_add, int, (int a, int b)) {
    return callOriginal(a, b) + 100;
}

// Hook in stile metodo: il primo parametro è il puntatore `self`. Il detour
// invoca l'originale (che muta self) e ne propaga il valore di ritorno.
PULSE_HOOK(test_init, bool, (FakeLayer* self)) {
    if (!callOriginal(self)) {
        return false;
    }
    return true;
}

// Hook con ritorno a puntatore: verifica la preservazione di un valore di
// ritorno non scalare.
PULSE_HOOK(test_identity, const char*, (const char* s)) {
    return callOriginal(s);
}

namespace {

using pulse::hooks::HookRegistration;

// Recupera la firma tipizzata del detour per invocarlo nei test.
template <class Fn>
Fn detour_as(std::string_view symbol) {
    const HookRegistration* reg = pulse::hooks::find(symbol);
    EXPECT_NE(reg, nullptr);
    return reinterpret_cast<Fn>(reg->detour);
}

// --- registrazione: i tre hook sono presenti nel registro (Req 5.1) -------
TEST(PulseHook, HooksAreRegisteredBySymbol) {
    EXPECT_NE(pulse::hooks::find("test_add"), nullptr);
    EXPECT_NE(pulse::hooks::find("test_init"), nullptr);
    EXPECT_NE(pulse::hooks::find("test_identity"), nullptr);
    EXPECT_EQ(pulse::hooks::find("not_declared"), nullptr);

    const auto* reg = pulse::hooks::find("test_add");
    ASSERT_NE(reg, nullptr);
    EXPECT_EQ(reg->symbol, "test_add");
    EXPECT_NE(reg->detour, nullptr);      // il detour è generato (Req 2.2)
    EXPECT_NE(reg->trampoline, nullptr);  // lo slot del trampolino esiste
}

// --- bind_trampoline cabla l'originale; il detour lo richiama (Req 2.2/5.3) -
TEST(PulseHook, CallOriginalPreservesParamsAndReturnValue) {
    ASSERT_TRUE(pulse::hooks::bind_trampoline(
        "test_add", reinterpret_cast<void*>(&original_add)));

    auto detour = detour_as<int (*)(int, int)>("test_add");
    ASSERT_NE(detour, nullptr);

    g_add_calls = 0;
    const int result = detour(2, 3);

    EXPECT_EQ(g_add_calls, 1);   // l'originale è stato invocato (Req 5.3)
    EXPECT_EQ(result, 105);      // callOriginal(2,3)=5 -> +100 = 105
}

// --- firma in stile metodo: self preservato e mutato dall'originale --------
TEST(PulseHook, CallOriginalPreservesSelfPointerAndBoolReturn) {
    ASSERT_TRUE(pulse::hooks::bind_trampoline(
        "test_init", reinterpret_cast<void*>(&original_init)));

    auto detour = detour_as<bool (*)(FakeLayer*)>("test_init");
    ASSERT_NE(detour, nullptr);

    FakeLayer layer;
    g_init_calls = 0;
    const bool ok = detour(&layer);

    EXPECT_TRUE(ok);
    EXPECT_EQ(g_init_calls, 1);
    EXPECT_EQ(layer.value, 42);  // l'originale ha agito sull'istanza passata
}

// --- valore di ritorno non scalare (puntatore) preservato ------------------
TEST(PulseHook, CallOriginalPreservesPointerReturn) {
    ASSERT_TRUE(pulse::hooks::bind_trampoline(
        "test_identity", reinterpret_cast<void*>(&original_identity)));

    auto detour = detour_as<const char* (*)(const char*)>("test_identity");
    ASSERT_NE(detour, nullptr);

    const char* input = "pulse";
    g_identity_calls = 0;
    const char* out = detour(input);

    EXPECT_EQ(g_identity_calls, 1);
    EXPECT_EQ(out, input);  // stesso puntatore restituito senza alterazioni
}

// --- bind_trampoline su simbolo non dichiarato fallisce --------------------
TEST(PulseHook, BindTrampolineUnknownSymbolFails) {
    int dummy = 0;
    EXPECT_FALSE(pulse::hooks::bind_trampoline(
        "not_declared", reinterpret_cast<void*>(&dummy)));
}

}  // namespace
