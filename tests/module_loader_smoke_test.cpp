// tests/module_loader_smoke_test.cpp — smoke test (example) del seam
// Module_Loader (External Mod Loading, task 5.3, Requisiti 11.1, 11.2, 11.4).
//
// Questo è un test di esempio/smoke (GoogleTest), NON un property test. Verifica
// in modo robusto su tutte le configurazioni il contratto STRUTTURALE del seam
// `IModuleLoader` definito dai task 5.1/5.2:
//
//   * `make_platform_module_loader()` restituisce un loader non nullo che espone
//     `available()`/`name()`/`load`/`resolveEntryPoint`/`unload` (Req 11.1);
//   * la query di disponibilità riporta `true` SOLO quando la piattaforma
//     corrente fornisce l'implementazione reale: macOS arm64 con il backend
//     Dobby abilitato → true; ogni altra build (macOS senza Dobby, host
//     non-bersaglio) → false (Req 11.1/11.2);
//   * gli stub non-Apple (Windows/Android/iOS) riportano `available() == false`
//     e le loro primitive degradano in modo confinato senza caricare codice
//     (Req 11.4);
//   * il contratto load → resolveEntryPoint (hit + `SymbolNotFound`) → unload è
//     esercitato in CI via `FakeModuleLoader` (host-testabile, nessun dlopen);
//   * dove la piattaforma Apple è attiva (path reale compilato anche con Dobby
//     OFF), l'estrazione su file temporaneo con permessi ristretti (0600), la
//     risoluzione dell'entry point esportato e il cleanup allo `unload` sono
//     verificati con una dylib fixture benigna e caricabile.
//
// Il comportamento `dlopen`/`dlsym`/`dlclose` runtime end-to-end sul GD reale è
// verificato in Fase E; qui si fa lo smoke della FORMA del seam (design §3).
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).
#include "lifecycle/module_loader.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

using pulse::lifecycle::AndroidModuleLoader;
using pulse::lifecycle::Bytes;
using pulse::lifecycle::FakeModuleLoader;
using pulse::lifecycle::IOsModuleLoader;
using pulse::lifecycle::MacOsModuleLoader;
using pulse::lifecycle::make_platform_module_loader;
using pulse::lifecycle::ModuleHandle;
using pulse::lifecycle::WindowsModuleLoader;

// ---------------------------------------------------------------------------
// Factory + query di disponibilità (Req 11.1, 11.2).
// ---------------------------------------------------------------------------

TEST(ModuleLoaderSmoke, FactoryReturnsNonNullLoaderExposingTheSeam) {
    auto loader = make_platform_module_loader();
    ASSERT_NE(loader, nullptr);
    // L'interfaccia espone name()/available(); load/resolveEntryPoint/unload
    // sono parte del tipo astratto (verificate sotto via Fake e su Apple reale).
    EXPECT_FALSE(loader->name().empty());
}

TEST(ModuleLoaderSmoke, AvailabilityMatchesCurrentPlatform) {
    auto loader = make_platform_module_loader();
    ASSERT_NE(loader, nullptr);
#if defined(__APPLE__) && defined(PULSE_HOOK_BACKEND_DOBBY)
    // macOS arm64 con Dobby abilitato → unica implementazione reale (Req 11.2).
    EXPECT_TRUE(loader->available());
#else
    // macOS senza Dobby (build host di default), Windows/Android, o host
    // non-bersaglio → nessuna implementazione reale (Req 11.4).
    EXPECT_FALSE(loader->available());
#endif
}

// ---------------------------------------------------------------------------
// Stub non-Apple: available()==false e nessun caricamento (Req 11.4).
// ---------------------------------------------------------------------------

TEST(ModuleLoaderSmoke, NonApplePlatformStubsReportUnavailable) {
    WindowsModuleLoader win;
    AndroidModuleLoader android;
    IOsModuleLoader ios;

    EXPECT_FALSE(win.available());
    EXPECT_FALSE(android.available());
    EXPECT_FALSE(ios.available());

    EXPECT_FALSE(win.name().empty());
    EXPECT_FALSE(android.name().empty());
    EXPECT_FALSE(ios.name().empty());

    // Le primitive degradano in modo confinato (Unsupported), senza caricare
    // alcun codice.
    Bytes image{0x01, 0x02, 0x03};
    auto winLoad = win.load("mod.win", image);
    auto androidLoad = android.load("mod.android", image);
    auto iosLoad = ios.load("mod.ios", image);
    EXPECT_FALSE(winLoad.has_value());
    EXPECT_FALSE(androidLoad.has_value());
    EXPECT_FALSE(iosLoad.has_value());
}

// ---------------------------------------------------------------------------
// Contratto load → resolveEntryPoint → unload via FakeModuleLoader (host).
// ---------------------------------------------------------------------------

TEST(ModuleLoaderSmoke, SeamLoadResolveUnloadContractViaFake) {
    FakeModuleLoader fake;
    EXPECT_TRUE(fake.available());
    EXPECT_FALSE(fake.name().empty());

    const Bytes image{0xCA, 0xFE, 0xBA, 0xBE};
    FakeModuleLoader::ModuleSpec spec;
    spec.exports.push_back({"mod_entry", {0x11, 0x22, 0x33}});
    fake.program(image, spec);

    // load → handle valido.
    auto loaded = fake.load("mod.alpha", image);
    ASSERT_TRUE(loaded.has_value());
    ModuleHandle handle = loaded.value();
    EXPECT_TRUE(handle.valid());

    // resolveEntryPoint del simbolo esportato → indirizzo non nullo.
    auto entry = fake.resolveEntryPoint(handle, "mod_entry");
    ASSERT_TRUE(entry.has_value());
    EXPECT_NE(entry.value(), nullptr);

    // Entry point non esportato → SymbolNotFound, nessuna invocazione (Req 5.3).
    auto missing = fake.resolveEntryPoint(handle, "does_not_exist");
    EXPECT_FALSE(missing.has_value());

    // unload → handle invalidato.
    auto unloaded = fake.unload(handle);
    EXPECT_TRUE(unloaded.has_value());
    EXPECT_FALSE(handle.valid());
}

#if defined(__APPLE__)

// Legge interamente un file binario in un buffer di byte.
std::vector<std::uint8_t> readAllBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------
// Estrazione reale su file temporaneo con permessi ristretti + cleanup
// (Req 11.2/11.3, 5.2, 8.7). Il path reale di MacOsModuleLoader è compilato su
// tutte le build Apple (anche con Dobby OFF), quindi load/resolve/unload sono
// esercitabili qui con una dylib fixture benigna e caricabile.
// ---------------------------------------------------------------------------
TEST(ModuleLoaderSmoke, MacOsRealExtractionRestrictedPermsAndCleanup) {
    const std::filesystem::path fixture{PULSE_MODULE_LOADER_SMOKE_FIXTURE};
    ASSERT_TRUE(std::filesystem::exists(fixture))
        << "fixture dylib mancante: " << fixture;

    const auto moduleBytes = readAllBytes(fixture);
    ASSERT_FALSE(moduleBytes.empty());

    MacOsModuleLoader loader;

    // load: estrae i byte su un file temporaneo per-sessione e fa dlopen.
    auto loaded = loader.load("pulse.smoke", moduleBytes);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    ModuleHandle handle = loaded.value();
    ASSERT_TRUE(handle.valid());

    // Il modulo è stato estratto su un file temporaneo realmente presente.
    const std::filesystem::path extracted = handle.extractedPath;
    ASSERT_FALSE(extracted.empty());
    ASSERT_TRUE(std::filesystem::exists(extracted)) << extracted;

    // Permessi ristretti: solo il proprietario (0600), niente group/other.
    namespace fs = std::filesystem;
    const fs::perms p = fs::status(extracted).permissions();
    EXPECT_EQ(p & fs::perms::owner_read, fs::perms::owner_read);
    EXPECT_EQ(p & fs::perms::owner_write, fs::perms::owner_write);
    EXPECT_EQ(p & fs::perms::group_all, fs::perms::none);
    EXPECT_EQ(p & fs::perms::others_all, fs::perms::none);

    // resolveEntryPoint del simbolo esportato dalla fixture (Req 5.2).
    auto entry = loader.resolveEntryPoint(handle, "pulse_module_loader_smoke_entry");
    ASSERT_TRUE(entry.has_value()) << entry.error().message;
    EXPECT_NE(entry.value(), nullptr);

    // Simbolo non esportato → SymbolNotFound (Req 5.3).
    auto missing = loader.resolveEntryPoint(handle, "pulse_symbol_absent");
    EXPECT_FALSE(missing.has_value());

    // unload: dlclose + rimozione del file temporaneo (cleanup, Req 8.7).
    auto unloaded = loader.unload(handle);
    EXPECT_TRUE(unloaded.has_value()) << unloaded.error().message;
    EXPECT_FALSE(handle.valid());
    EXPECT_FALSE(std::filesystem::exists(extracted))
        << "il file temporaneo non è stato rimosso allo unload: " << extracted;
}

#endif  // __APPLE__

}  // namespace
