// tests/android_bootstrap_test.cpp — unit test del bootstrap Android (task 23.2,
// Req 1.1, 1.2).
//
// Verifica il comportamento platform-agnostic di `AndroidBootstrap`:
//   * `platform()` riflette l'ABI di compilazione (arm64-v8a vs armeabi-v7a);
//   * su host non-Android `inject()` riporta `UnsupportedHost` senza eseguire
//     l'entry point del runtime, lasciando partire GD senza mod (Req 1.3);
//   * su Android l'entry point iniettabile viene invocato prima della scena
//     iniziale (Req 1.2, 1.4) e gli esiti di successo/fallimento sono corretti.
//
// L'header del loader vive in loader/bootstrap/: il target di test aggiunge
// loader/ alla include path.
#include "bootstrap/android_bootstrap.hpp"

#include <gtest/gtest.h>

namespace {

using pulse::bootstrap::AndroidBootstrap;
using pulse::bootstrap::BootstrapErrorCode;
using pulse::bootstrap::Platform;
using pulse::bootstrap::android_abi_platform;

// platform() deve coincidere con l'ABI selezionata a compile-time (Req 1.1).
TEST(AndroidBootstrap, PlatformReflectsCompiledAbi) {
    AndroidBootstrap bootstrap;
#if defined(__aarch64__)
    EXPECT_EQ(bootstrap.platform(), Platform::AndroidArm64);
#else
    EXPECT_EQ(bootstrap.platform(), Platform::AndroidArmV7);
#endif
    EXPECT_EQ(bootstrap.platform(), android_abi_platform());
}

// L'entry point del runtime è iniettabile e disaccoppiato dal Loader Core.
TEST(AndroidBootstrap, EntryPointBehaviour) {
    bool called = false;
    AndroidBootstrap bootstrap{[&called]() {
        called = true;
        return true;
    }};

    const auto result = bootstrap.inject();

#if defined(__ANDROID__)
    // Su Android l'aggancio early-load avvia il runtime prima della scena
    // iniziale (Req 1.2, 1.4): l'entry point viene invocato e l'iniezione
    // riesce.
    EXPECT_TRUE(called);
    EXPECT_TRUE(result.injected);
    EXPECT_FALSE(result.error.has_value());

    // Idempotenza: una seconda iniezione è un errore diagnostico (Req 1.3).
    const auto again = bootstrap.inject();
    EXPECT_FALSE(again.injected);
    ASSERT_TRUE(again.error.has_value());
    EXPECT_EQ(again.error->code, BootstrapErrorCode::AlreadyInjected);
#else
    // Sull'host non-Android l'implementazione non è operativa: niente entry
    // point, fallimento diagnostico UnsupportedHost (Req 1.3).
    EXPECT_FALSE(called);
    EXPECT_FALSE(result.injected);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->code, BootstrapErrorCode::UnsupportedHost);
#endif
}

// Un entry point che fallisce produce una diagnostica EntryPointHookFailed
// (rilevante solo dove l'aggancio early-load è operativo, ossia su Android).
TEST(AndroidBootstrap, EntryPointFailureIsDiagnosed) {
    AndroidBootstrap bootstrap{[]() { return false; }};
    const auto result = bootstrap.inject();

#if defined(__ANDROID__)
    EXPECT_FALSE(result.injected);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->code, BootstrapErrorCode::EntryPointHookFailed);
#else
    EXPECT_FALSE(result.injected);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->code, BootstrapErrorCode::UnsupportedHost);
#endif
}

}  // namespace
