// tests/demo_mod_test.cpp — test del cablaggio della demo mod sull'HookEngine
// reale (task 3.15, Requisiti 9.1, 9.2, 9.3, 9.6).
//
// Verifica `pulse::loader::mvp::DemoMod`, che riusa il detour dimostrativo
// (`mvp/menulayer_init_hook.*`) cablandolo sul COORDINATORE reale degli hook
// (`pulse::hooking::HookEngine`) e sui bindings reali risolti
// dall'`EmbeddedBindingsProvider`:
//   * Req 9.1 — risolto `MenuLayer::init` dai bindings, viene installato
//     ESATTAMENTE un hook su quella funzione attraverso l'HookEngine;
//   * Req 9.2 — il detour emette un log via la facility diagnostica che
//     identifica la demo mod (`pulse.demo`) e l'hook come sorgente;
//   * Req 9.3 — il detour invoca l'originale tramite trampolino e ne ritorna il
//     valore non modificato (osservato con un trampolino finto host);
//   * Req 9.6 — se `MenuLayer::init` è non risolto: log della causa, NESSUN
//     hook, l'engine resta a 0 hook (GD prosegue senza mod).
//
// L'host (macOS/Linux) non dispone del binario di GD: il flusso è reso
// testabile dal `FakeBackend` in-memory dietro l'HookEngine e da un trampolino
// finto stand-in dell'originale per l'osservabilità (DemoLogSink).

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <pulse/hooks.hpp>

#include "bindings/bindings.hpp"
#include "bindings/embedded_bindings_provider.hpp"
#include "hooking/fake_backend.hpp"
#include "hooking/hook_engine.hpp"
#include "mvp/demo_mod.hpp"
#include "mvp/menulayer_init_hook.hpp"

namespace {

using pulse::hooking::FakeBackend;
using pulse::hooking::HookEngine;
using pulse::loader::bindings::BindingKey;
using pulse::loader::bindings::EmbeddedBindingsProvider;
using pulse::loader::bindings::GdVersion;
using pulse::loader::bindings::IBindingsProvider;
using pulse::loader::mvp::DemoMod;
using pulse::loader::mvp::DemoModStatus;
using pulse::loader::mvp::MenuLayer;

// Offset embedded di MenuLayer::init per (2.2074, windows-x64) — deve combaciare
// con quello in EmbeddedBindingsProvider.
constexpr std::uintptr_t kMenuLayerInitOffset = 0x003140D0;

// Crea un provider embedded già caricato sulla coppia (2.2074, windows-x64),
// così `resolve("MenuLayer::init")` risolve l'indirizzo verificato.
std::shared_ptr<EmbeddedBindingsProvider> makeLoadedProvider() {
    auto provider = std::make_shared<EmbeddedBindingsProvider>();
    const BindingKey key{GdVersion{2, 2074}, "windows-x64"};
    (void)provider->load(key);  // popola il set "corrente" per resolve()
    return provider;
}

// --- Req 9.1: esattamente un hook su MenuLayer::init via HookEngine ---------
TEST(DemoMod, InstallsExactlyOneHookOnMenuLayerInit) {
    FakeBackend backend;
    std::vector<std::string> logs;
    HookEngine engine{backend, [&logs](std::string_view m) { logs.emplace_back(m); }};

    DemoMod demo{engine, makeLoadedProvider(),
                 pulse::loader::mvp::kMenuLayerInitBindingSymbol,
                 pulse::loader::mvp::kMenuLayerInitRegistrationSymbol,
                 [&logs](std::string_view m) { logs.emplace_back(m); }};

    const auto result = demo.install();

    EXPECT_EQ(result.status, DemoModStatus::HookInstalled);
    EXPECT_TRUE(result.installed());
    EXPECT_EQ(result.hookedAddress, kMenuLayerInitOffset);

    // ESATTAMENTE un hook su una sola funzione bersaglio (Req 9.1).
    EXPECT_EQ(engine.installedTargets(), 1u);
    EXPECT_EQ(engine.totalHooks(), 1u);
    EXPECT_EQ(result.installedHooks, 1u);
    EXPECT_TRUE(engine.isTargetInstalled(kMenuLayerInitOffset));
    EXPECT_TRUE(backend.isInstalled(kMenuLayerInitOffset));

    // Il detour dimostrativo è registrato presso il registro dello SDK.
    EXPECT_NE(pulse::hooks::find("MenuLayer_init"), nullptr);
}

// --- Req 9.2/9.3/9.5: il detour logga (demo mod + hook) e invoca l'originale -
TEST(DemoMod, DetourIdentifiesDemoModThenInvokesOriginalReturningItsValue) {
    FakeBackend backend;
    HookEngine engine{backend};
    DemoMod demo{engine, makeLoadedProvider()};

    ASSERT_EQ(demo.install().status, DemoModStatus::HookInstalled);

    // Cattura i log del demo (sorgente identificata in Req 9.2).
    std::vector<std::string> demoLogs;
    pulse::loader::mvp::set_demo_log_sink(
        [&demoLogs](std::string_view m) { demoLogs.emplace_back(m); });

    // Invoca il detour registrato, come farebbe il gioco al posto dell'originale.
    const auto* reg = pulse::hooks::find("MenuLayer_init");
    ASSERT_NE(reg, nullptr);
    auto detour = reinterpret_cast<bool (*)(MenuLayer*)>(reg->detour);

    MenuLayer layer;
    const bool ret = detour(&layer);

    // Req 9.3: il detour invoca l'originale (trampolino finto) e ne ritorna il
    // valore non modificato; l'effetto osservabile dell'originale è applicato.
    EXPECT_TRUE(ret);
    EXPECT_TRUE(layer.initialized);

    // Req 9.2/9.5: due messaggi, il primo (detour) PRIMA del secondo (originale),
    // entrambi identificano la demo mod come sorgente.
    ASSERT_EQ(demoLogs.size(), 2u);
    EXPECT_NE(demoLogs[0].find("detour"), std::string::npos);
    EXPECT_NE(demoLogs[1].find("originale"), std::string::npos);
    EXPECT_NE(demoLogs[0].find(std::string{pulse::loader::mvp::kDemoModId}),
              std::string::npos);

    pulse::loader::mvp::set_demo_log_sink(nullptr);  // ripristina lo stato globale
}

// --- Req 9.3: trampolino finto iniettato e valore di ritorno preservato -----
TEST(DemoMod, FakeTrampolinePreservesOriginalReturnValue) {
    FakeBackend backend;
    HookEngine engine{backend};
    DemoMod demo{engine, makeLoadedProvider()};

    // Trampolino finto che ritorna `false`: il detour deve propagarlo invariato.
    demo.set_fake_trampoline(+[](MenuLayer* self) -> bool {
        if (self != nullptr) self->initialized = true;
        return false;
    });

    ASSERT_EQ(demo.install().status, DemoModStatus::HookInstalled);

    const auto* reg = pulse::hooks::find("MenuLayer_init");
    ASSERT_NE(reg, nullptr);
    auto detour = reinterpret_cast<bool (*)(MenuLayer*)>(reg->detour);

    MenuLayer layer;
    EXPECT_FALSE(detour(&layer));  // valore dell'originale preservato (Req 9.3)
    EXPECT_TRUE(layer.initialized);
}

// --- Req 9.6: simbolo non risolto → log, 0 hook, GD prosegue ----------------
TEST(DemoMod, UnresolvedSymbolInstallsNoHookAndProceeds) {
    FakeBackend backend;
    std::vector<std::string> logs;
    HookEngine engine{backend};

    // Provider NON caricato: `resolve("MenuLayer::init")` ritorna nullopt.
    auto provider = std::make_shared<EmbeddedBindingsProvider>();
    DemoMod demo{engine, provider,
                 pulse::loader::mvp::kMenuLayerInitBindingSymbol,
                 pulse::loader::mvp::kMenuLayerInitRegistrationSymbol,
                 [&logs](std::string_view m) { logs.emplace_back(m); }};

    const auto result = demo.install();

    EXPECT_EQ(result.status, DemoModStatus::SymbolUnresolved);
    EXPECT_FALSE(result.installed());
    EXPECT_EQ(result.installedHooks, 0u);

    // Nessun hook installato (Req 9.6): l'engine e il backend restano vuoti.
    EXPECT_EQ(engine.installedTargets(), 0u);
    EXPECT_EQ(engine.totalHooks(), 0u);
    EXPECT_EQ(backend.installedCount(), 0u);

    // È stata registrata una diagnostica che identifica la funzione non risolta.
    bool logged = false;
    for (const auto& line : logs) {
        if (line.find("MenuLayer::init") != std::string::npos &&
            line.find("non risolta") != std::string::npos) {
            logged = true;
            break;
        }
    }
    EXPECT_TRUE(logged);
}

// --- fallimento del backend: diagnosticato senza crash, 0 hook --------------
TEST(DemoMod, ReportsBackendInstallFailureWithoutCrash) {
    FakeBackend backend;
    backend.failAllInstalls(true);
    HookEngine engine{backend};

    DemoMod demo{engine, makeLoadedProvider()};
    const auto result = demo.install();

    EXPECT_EQ(result.status, DemoModStatus::InstallFailed);
    EXPECT_FALSE(result.installed());
    EXPECT_EQ(result.installedHooks, 0u);
    EXPECT_EQ(engine.installedTargets(), 0u);
    EXPECT_EQ(backend.installedCount(), 0u);
}

}  // namespace
