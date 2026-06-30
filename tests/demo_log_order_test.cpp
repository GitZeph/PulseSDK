// tests/demo_log_order_test.cpp — ordine di log della demo mod (task 3.16,
// Requisito 9.5, host-testabile).
//
// Verifica l'example 9.5 del design: quando il detour dimostrativo
// (`mvp/menulayer_init_hook.*`, cablato dalla `DemoMod` sull'HookEngine reale)
// scatta, la facility diagnostica registra l'esecuzione del DETOUR **prima**
// dell'esecuzione dell'ORIGINALE. Sull'host di build/test il codice reale di
// `MenuLayer::init` non esiste: si usa il `DemoLogSink` per osservare i log del
// detour e un **trampolino finto** iniettabile come stand-in dell'originale.
//
// Strategia di osservazione dell'ORDINE: sia i log del detour sia l'esecuzione
// dell'originale (trampolino finto) appendono un marcatore alla STESSA sequenza
// condivisa. Il detour logga il proprio ingresso, poi invoca `callOriginal`
// (trampolino finto). Asserendo che il marcatore del log del detour preceda il
// marcatore dell'originale nella sequenza, si dimostra l'invariante 9.5
// "detour prima dell'originale".

#include <gtest/gtest.h>

#include <cstddef>
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
using pulse::loader::mvp::DemoMod;
using pulse::loader::mvp::DemoModStatus;
using pulse::loader::mvp::MenuLayer;

// Crea un provider embedded già caricato sulla coppia (2.2074, windows-x64),
// così `resolve("MenuLayer::init")` risolve l'indirizzo verificato (allineato
// a EmbeddedBindingsProvider e a demo_mod_test.cpp).
std::shared_ptr<EmbeddedBindingsProvider> makeLoadedProvider() {
    auto provider = std::make_shared<EmbeddedBindingsProvider>();
    const BindingKey key{GdVersion{2, 2074}, "windows-x64"};
    (void)provider->load(key);
    return provider;
}

// Marcatori distinti per la sequenza di eventi condivisa.
constexpr const char* kDetourLogMarker = "DETOUR_LOG";
constexpr const char* kOriginalExecMarker = "ORIGINAL_EXEC";

// --- Req 9.5: il detour è loggato PRIMA dell'esecuzione dell'originale -------
TEST(DemoLogOrder, DetourIsLoggedBeforeOriginalExecutes) {
    FakeBackend backend;
    HookEngine engine{backend};
    DemoMod demo{engine, makeLoadedProvider()};

    // Sequenza condivisa che intreccia i log del detour e l'esecuzione
    // dell'originale (trampolino finto), così l'ordine relativo è osservabile.
    std::vector<std::string> order;

    // Il sink del demo appende un marcatore di log del detour a ogni messaggio.
    pulse::loader::mvp::set_demo_log_sink(
        [&order](std::string_view /*message*/) { order.emplace_back(kDetourLogMarker); });

    // Trampolino finto (stand-in dell'originale): registra la propria
    // esecuzione nella stessa sequenza e applica l'effetto osservabile.
    static std::vector<std::string>* s_order = &order;
    demo.set_fake_trampoline(+[](MenuLayer* self) -> bool {
        if (self != nullptr) self->initialized = true;
        s_order->emplace_back(kOriginalExecMarker);
        return true;
    });

    ASSERT_EQ(demo.install().status, DemoModStatus::HookInstalled);

    // Invoca il detour registrato, come farebbe il gioco al posto dell'originale.
    const auto* reg = pulse::hooks::find("MenuLayer_init");
    ASSERT_NE(reg, nullptr);
    auto detour = reinterpret_cast<bool (*)(MenuLayer*)>(reg->detour);

    MenuLayer layer;
    const bool ret = detour(&layer);

    // L'originale (trampolino finto) è stato eseguito e il suo valore è
    // propagato invariato.
    EXPECT_TRUE(ret);
    EXPECT_TRUE(layer.initialized);

    // L'ordine deve contenere almeno il log d'ingresso del detour e
    // l'esecuzione dell'originale.
    ASSERT_GE(order.size(), 2u);

    // Indice del PRIMO log del detour e indice dell'esecuzione dell'originale.
    std::size_t firstDetourLog = order.size();
    std::size_t originalExec = order.size();
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i] == kDetourLogMarker && firstDetourLog == order.size()) {
            firstDetourLog = i;
        }
        if (order[i] == kOriginalExecMarker && originalExec == order.size()) {
            originalExec = i;
        }
    }

    ASSERT_NE(firstDetourLog, order.size()) << "atteso almeno un log del detour";
    ASSERT_NE(originalExec, order.size()) << "atteso l'esecuzione dell'originale";

    // Invariante Req 9.5: il detour è registrato PRIMA che l'originale esegua.
    EXPECT_LT(firstDetourLog, originalExec)
        << "il log del detour deve precedere l'esecuzione dell'originale (Req 9.5)";

    pulse::loader::mvp::set_demo_log_sink(nullptr);  // ripristina lo stato globale
}

}  // namespace
