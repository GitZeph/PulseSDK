// tests/macos_bootstrap_test.cpp — unit test del bootstrap macOS (task 23.1,
// Req 1.1, 1.2, 1.3, 1.4).
//
// Verifica `pulse::bootstrap::MacOSBootstrap`:
//   * platform() identifica macOS (Req 1.1);
//   * l'entry point del runtime iniettabile è invocato dal bootstrap, così il
//     runtime centralizzato parte prima della scena iniziale (Req 1.2, 1.4);
//   * idempotenza: una seconda inject() segnala AlreadyInjected.
//
// Le asserzioni che dipendono dal successo dell'iniezione sono valide solo su
// host Apple (dove è disponibile dyld). Su host non-Apple il bootstrap ritorna
// la diagnostica UnsupportedHost (Req 1.3) mantenendo la build compilabile e
// testabile: il test verifica entrambi i comportamenti dietro `#if __APPLE__`.

#include <gtest/gtest.h>

#include "bootstrap/macos_bootstrap.hpp"

namespace {

using pulse::bootstrap::BootstrapErrorCode;
using pulse::bootstrap::MacOSBootstrap;
using pulse::bootstrap::Platform;

TEST(MacOSBootstrapTest, ReportsMacOSPlatform) {
    MacOSBootstrap bootstrap;
    EXPECT_EQ(bootstrap.platform(), Platform::MacOS);
}

#if defined(__APPLE__)

// Su macOS l'entry point iniettato deve essere invocato e l'iniezione riuscire.
TEST(MacOSBootstrapTest, InvokesRuntimeEntryPointAndSucceeds) {
    bool entry_called = false;
    MacOSBootstrap bootstrap([&entry_called]() {
        entry_called = true;
        return true;
    });

    const auto result = bootstrap.inject();

    EXPECT_TRUE(entry_called);
    EXPECT_TRUE(result.injected);
    EXPECT_FALSE(result.error.has_value());
}

// Un entry point che segnala errore di avvio produce una diagnostica (Req 1.3).
TEST(MacOSBootstrapTest, EntryPointFailureIsDiagnosed) {
    MacOSBootstrap bootstrap([]() { return false; });

    const auto result = bootstrap.inject();

    EXPECT_FALSE(result.injected);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->code, BootstrapErrorCode::EntryPointHookFailed);
}

// Una seconda inject() nello stesso processo è un AlreadyInjected (idempotenza).
TEST(MacOSBootstrapTest, SecondInjectReportsAlreadyInjected) {
    MacOSBootstrap bootstrap([]() { return true; });

    ASSERT_TRUE(bootstrap.inject().injected);

    const auto second = bootstrap.inject();
    EXPECT_FALSE(second.injected);
    ASSERT_TRUE(second.error.has_value());
    EXPECT_EQ(second.error->code, BootstrapErrorCode::AlreadyInjected);
}

#else  // !__APPLE__

// Su host non-Apple l'iniezione non è operativa: deve fallire con
// UnsupportedHost senza side effect (Req 1.3), preservando la build.
TEST(MacOSBootstrapTest, UnsupportedHostFailsGracefully) {
    bool entry_called = false;
    MacOSBootstrap bootstrap([&entry_called]() {
        entry_called = true;
        return true;
    });

    const auto result = bootstrap.inject();

    EXPECT_FALSE(result.injected);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->code, BootstrapErrorCode::UnsupportedHost);
    EXPECT_FALSE(entry_called);
}

#endif  // __APPLE__

}  // namespace
