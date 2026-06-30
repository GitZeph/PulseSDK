// tests/mvp_loader_test.cpp — test del cablaggio centralizzato dell'MVP (task 4.4).
//
// Verifica il flusso bootstrap → core → bindings → backend → PULSE_HOOK
// realizzato da `pulse::loader::mvp::MvpLoader`:
//   * rilevata GD_Version e caricato il set di bindings esatto, l'indirizzo di
//     `MenuLayer::init` è risolto e il detour PULSE_HOOK vi è installato sul
//     backend iniettato (Req 1.2, 1.4, 1.5, 2.2, 20.2);
//   * il trampolino verso l'originale è cablato, così il detour esegue e poi
//     invoca l'originale preservando parametri e valore di ritorno, con logging
//     dell'esecuzione del detour e dell'originale (Req 2.2, 5.3);
//   * i fallimenti (versione non rilevata, bindings assenti, simbolo non
//     risolto, install fallito) sono diagnosticati senza crash.
//
// L'host (macOS/Linux) non dispone del binario di GD né di MinHook: il flusso è
// reso testabile iniettando un detector fittizio (GD 2.2074), il `FakeBackend`
// in-memory e l'override del platformId verso "windows-x64".

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <pulse/hooks.hpp>

#include "bindings/embedded_bindings_provider.hpp"
#include "hooking/fake_backend.hpp"
#include "mvp/menulayer_init_hook.hpp"
#include "mvp/mvp_loader.hpp"

namespace {

using pulse::loader::GdVersion;
using pulse::loader::IVersionDetector;
using pulse::loader::bindings::EmbeddedBindingsProvider;
using pulse::loader::bindings::IBindingsProvider;
using pulse::hooking::FakeBackend;
using pulse::hooking::IHookBackend;
using pulse::loader::mvp::MenuLayer;
using pulse::loader::mvp::MvpConfig;
using pulse::loader::mvp::MvpLoader;
using pulse::loader::mvp::MvpStatus;

// Offset embedded di MenuLayer::init per (2.2074, windows-x64) — deve combaciare
// con quello in EmbeddedBindingsProvider.
constexpr std::uintptr_t kMenuLayerInitOffset = 0x003140D0;

// --- detector fittizio: restituisce (o meno) una GD_Version configurata -----
class FakeVersionDetector final : public IVersionDetector {
public:
    explicit FakeVersionDetector(std::optional<GdVersion> version)
        : version_(version) {}
    std::optional<GdVersion> detect() override { return version_; }

private:
    std::optional<GdVersion> version_;
};

// Costruisce un MvpLoader configurato per puntare al set (2.2074, windows-x64)
// dall'host di test, raccogliendo i log diagnostici in `logs`.
std::unique_ptr<MvpLoader> makeLoader(std::optional<GdVersion> detected,
                                      std::shared_ptr<FakeBackend>& backendOut,
                                      std::vector<std::string>& logs,
                                      std::string platformOverride = "windows-x64",
                                      std::string_view bindingSymbol =
                                          pulse::loader::mvp::kMenuLayerInitBindingSymbol) {
    auto detector = std::make_shared<FakeVersionDetector>(detected);
    auto backend = std::make_unique<FakeBackend>();
    backendOut = std::shared_ptr<FakeBackend>(backend.get(), [](FakeBackend*) {});

    auto provider = std::make_shared<EmbeddedBindingsProvider>();

    MvpConfig config;
    config.platformIdOverride = std::move(platformOverride);
    config.target.bindingSymbol = bindingSymbol;

    auto sink = [&logs](std::string_view msg) { logs.emplace_back(msg); };

    return std::make_unique<MvpLoader>(std::move(detector), std::move(backend),
                                       std::move(provider), std::move(config), sink);
}

// --- happy path: detour installato e trampolino cablato --------------------
TEST(MvpLoader, WiresFlowAndInstallsMenuLayerInitHook) {
    std::shared_ptr<FakeBackend> backend;
    std::vector<std::string> logs;
    auto loader = makeLoader(GdVersion{2, 2074}, backend, logs);

    const auto result = loader->run();

    EXPECT_EQ(result.status, MvpStatus::Success);
    EXPECT_TRUE(result.injected);
    EXPECT_EQ(result.hookedAddress, kMenuLayerInitOffset);

    // Il backend ha ricevuto l'install all'indirizzo risolto dai bindings.
    EXPECT_TRUE(backend->isInstalled(kMenuLayerInitOffset));
    EXPECT_EQ(backend->installedCount(), 1u);

    // Il contesto runtime è esposto (Req 1.5).
    EXPECT_EQ(loader->context().gdVersion, (GdVersion{2, 2074}));

    // Il detour dimostrativo è registrato presso il registro dello SDK.
    EXPECT_NE(pulse::hooks::find("MenuLayer_init"), nullptr);
}

// --- esecuzione detour → originale con logging (Req 2.2, 5.3) --------------
TEST(MvpLoader, DetourRunsThenOriginalWithLogging) {
    std::shared_ptr<FakeBackend> backend;
    std::vector<std::string> logs;
    auto loader = makeLoader(GdVersion{2, 2074}, backend, logs);
    ASSERT_EQ(loader->run().status, MvpStatus::Success);

    // Il trampolino del FakeBackend è un indirizzo sintetico non eseguibile
    // (il codice reale del gioco non è disponibile sull'host). Per dimostrare
    // l'esecuzione end-to-end del detour seguito dall'originale, cabliamo il
    // trampolino verso un originale di prova reale: è esattamente ciò che un
    // backend reale farebbe restituendo un trampolino eseguibile.
    static int originalCalls = 0;
    originalCalls = 0;
    auto original = +[](MenuLayer* self) -> bool {
        ++originalCalls;
        self->initialized = true;  // effetto osservabile dell'originale
        return true;
    };
    ASSERT_TRUE(pulse::hooks::bind_trampoline(
        "MenuLayer_init", reinterpret_cast<void*>(original)));

    // Cattura i messaggi di log del demo (detour + originale).
    std::vector<std::string> demoLogs;
    pulse::loader::mvp::set_demo_log_sink(
        [&demoLogs](std::string_view m) { demoLogs.emplace_back(m); });

    // Invoca il detour registrato (come farebbe il gioco al posto dell'originale).
    const auto* reg = pulse::hooks::find("MenuLayer_init");
    ASSERT_NE(reg, nullptr);
    auto detour = reinterpret_cast<bool (*)(MenuLayer*)>(reg->detour);

    MenuLayer layer;
    const bool ret = detour(&layer);

    // Il detour ha eseguito, poi l'originale, preservando self e il ritorno.
    EXPECT_TRUE(ret);
    EXPECT_EQ(originalCalls, 1);
    EXPECT_TRUE(layer.initialized);

    // Sono stati loggati sia il detour sia l'esito dell'originale.
    ASSERT_EQ(demoLogs.size(), 2u);
    EXPECT_NE(demoLogs[0].find("detour"), std::string::npos);
    EXPECT_NE(demoLogs[1].find("originale"), std::string::npos);

    pulse::loader::mvp::set_demo_log_sink(nullptr);  // ripristina
}

// --- GD_Version non rilevata: caricamento abortito (Req 1.7) ---------------
TEST(MvpLoader, AbortsWhenVersionNotDetected) {
    std::shared_ptr<FakeBackend> backend;
    std::vector<std::string> logs;
    auto loader = makeLoader(std::nullopt, backend, logs);

    const auto result = loader->run();

    EXPECT_EQ(result.status, MvpStatus::VersionDetectionFailed);
    EXPECT_FALSE(result.injected);
    EXPECT_EQ(backend->installedCount(), 0u);  // nessun hook installato
}

// --- nessun set di bindings per la coppia (gating Req 20.3) ----------------
TEST(MvpLoader, FailsWhenBindingsNotFoundForPlatform) {
    std::shared_ptr<FakeBackend> backend;
    std::vector<std::string> logs;
    // Override verso una piattaforma senza set embedded.
    auto loader = makeLoader(GdVersion{2, 2074}, backend, logs, "macos-arm64");

    const auto result = loader->run();

    EXPECT_EQ(result.status, MvpStatus::BindingsNotFound);
    EXPECT_FALSE(result.injected);
    EXPECT_EQ(backend->installedCount(), 0u);
}

// --- simbolo non risolvibile nel set: hook annullato (Req 20.4) ------------
TEST(MvpLoader, FailsWhenSymbolUnresolved) {
    std::shared_ptr<FakeBackend> backend;
    std::vector<std::string> logs;
    auto loader =
        makeLoader(GdVersion{2, 2074}, backend, logs, "windows-x64", "PlayLayer::init");

    const auto result = loader->run();

    EXPECT_EQ(result.status, MvpStatus::SymbolUnresolved);
    EXPECT_FALSE(result.injected);
    EXPECT_EQ(backend->installedCount(), 0u);
}

// --- install del backend fallito: diagnosticato senza crash (Req 2.5) ------
TEST(MvpLoader, ReportsBackendInstallFailure) {
    auto detector = std::make_shared<FakeVersionDetector>(GdVersion{2, 2074});
    auto backend = std::make_unique<FakeBackend>();
    backend->failAllInstalls(true);
    FakeBackend* backendPtr = backend.get();

    auto provider = std::make_shared<EmbeddedBindingsProvider>();
    MvpConfig config;
    config.platformIdOverride = "windows-x64";

    MvpLoader loader(std::move(detector), std::move(backend), std::move(provider),
                     std::move(config));

    const auto result = loader.run();

    EXPECT_EQ(result.status, MvpStatus::InstallFailed);
    EXPECT_FALSE(result.injected);
    EXPECT_EQ(backendPtr->installedCount(), 0u);
}

// --- riconciliazione GdVersion core ↔ bindings -----------------------------
TEST(MvpLoader, ReconcilesGdVersionAcrossModules) {
    const auto v = pulse::loader::mvp::to_bindings_version(GdVersion{2, 2074});
    EXPECT_EQ(v.major, 2);
    EXPECT_EQ(v.minor, 2074);
}

// --- bootstrap → runtime: su host non-Windows fallback senza crash (Req 1.3) -
TEST(MvpLoader, BootstrapAndRunIsGracefulOnNonWindowsHost) {
    std::shared_ptr<FakeBackend> backend;
    std::vector<std::string> logs;
    auto loader = makeLoader(GdVersion{2, 2074}, backend, logs);

    const auto boot = loader->bootstrap_and_run();

#if defined(_WIN32)
    EXPECT_TRUE(boot.injected);
#else
    // Il WindowsBootstrap riporta UnsupportedHost senza eseguire l'entry point,
    // lasciando partire GD senza mod (Req 1.3).
    EXPECT_FALSE(boot.injected);
    ASSERT_TRUE(boot.error.has_value());
#endif
}

}  // namespace
