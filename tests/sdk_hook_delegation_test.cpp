// tests/sdk_hook_delegation_test.cpp — verifica la delega adattiva di
// `pulse::hooks::register_hook` (external-mod-loading).
//
// Concern 2: quando il processo espone `pulse_loader_register_hook` (come fa il
// Loader_Artifact a visibilità di default), `register_hook` deve DELEGARE a
// quell'entry point — così l'hook atterra nel registro del Loader — e NON deve
// far crescere il registro LOCALE di questa immagine.
//
// Simuliamo il caso cross-immagine dentro un solo eseguibile DEFINENDO qui
// `pulse_loader_register_hook` (extern "C", visibilità di default) che cattura
// gli argomenti in variabili globali. `register_hook` lo risolve via
// `dlsym(RTLD_DEFAULT, ...)`.
//
// IMPORTANTE: definire `pulse_loader_register_hook` fa delegare TUTTE le
// chiamate a `register_hook` di questo binario. Per questo il test è in un
// eseguibile DEDICATO che linka solo `pulse::sdk` (header-only): nessun loader
// e nessun register_external_hook qui, quindi non c'è ambiguità.
//
// `register_hook` memorizza il risultato del dlsym in uno static function-local
// (risolto alla prima chiamata). Poiché questo è un eseguibile separato (solo il
// suo processo è interessato) e misuriamo il DELTA del registro strettamente
// attorno alla nostra chiamata, l'asserzione non dipende dall'ordine rispetto ad
// altri suite.

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

#include <pulse/hooks.hpp>

namespace {

// Globali di cattura popolate dall'entry point simulato del loader.
int g_calls = 0;
std::string g_symbol;
void* g_detour = nullptr;
void** g_trampoline = nullptr;
int g_priority = 0;

}  // namespace

// Entry point C-ABI simulato del Loader_Artifact. Visibilità di default così
// `dlsym(RTLD_DEFAULT, ...)` lo risolve nell'immagine principale.
extern "C" __attribute__((visibility("default"))) void pulse_loader_register_hook(
    const char* symbol, void* detour, void** trampoline, int priority) {
    ++g_calls;
    g_symbol = (symbol != nullptr) ? symbol : "";
    g_detour = detour;
    g_trampoline = trampoline;
    g_priority = priority;
}

namespace {

TEST(SdkHookDelegation, DelegatesToLoaderAndSkipsLocalRegistry) {
    int detour = 0;
    void* tramp = nullptr;

    g_calls = 0;
    g_symbol.clear();
    g_detour = nullptr;
    g_trampoline = nullptr;
    g_priority = 0;

    const std::size_t before = pulse::hooks::registry().size();

    const auto reg = pulse::hooks::register_hook(
        "Test::sym", &detour, reinterpret_cast<void**>(&tramp), 600);

    const std::size_t after = pulse::hooks::registry().size();

    // Ha delegato all'entry point del loader con argomenti corrispondenti.
    EXPECT_EQ(g_calls, 1);
    EXPECT_EQ(g_symbol, "Test::sym");
    EXPECT_EQ(g_detour, reinterpret_cast<void*>(&detour));
    EXPECT_EQ(g_trampoline, reinterpret_cast<void**>(&tramp));
    EXPECT_EQ(g_priority, 600);

    // NON ha fatto crescere il registro locale (delta strettamente attorno alla
    // chiamata): la registrazione autorevole vive nel registro del loader.
    EXPECT_EQ(after, before);

    // Il valore restituito resta un descrittore utile al chiamante.
    EXPECT_EQ(reg.symbol, "Test::sym");
    EXPECT_EQ(reg.detour, reinterpret_cast<void*>(&detour));
    EXPECT_EQ(reg.priority, 600);
}

TEST(SdkHookDelegation, RepeatedCallsKeepDelegating) {
    int d1 = 0;
    int d2 = 0;
    void* t1 = nullptr;
    void* t2 = nullptr;

    const std::size_t before = pulse::hooks::registry().size();

    g_calls = 0;
    pulse::hooks::register_hook("Test::one", &d1,
                                reinterpret_cast<void**>(&t1));
    pulse::hooks::register_hook("Test::two", &d2,
                                reinterpret_cast<void**>(&t2), 700);

    EXPECT_EQ(g_calls, 2);
    EXPECT_EQ(g_symbol, "Test::two");
    EXPECT_EQ(g_priority, 700);
    EXPECT_EQ(pulse::hooks::registry().size(), before);  // sempre nessuna crescita locale
}

}  // namespace
