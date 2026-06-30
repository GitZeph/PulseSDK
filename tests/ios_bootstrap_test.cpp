// tests/ios_bootstrap_test.cpp — unit test del bootstrap iOS (task 23.3,
// Req 1.1, 1.2, 1.3).
//
// Verifica il contratto di `pulse::bootstrap::IOSBootstrap`:
//   * platform() identifica iOS arm64 (Req 1.1);
//   * sull'host di sviluppo non-iOS (macOS desktop / Linux) inject() ritorna un
//     fallimento diagnostico `UnsupportedHost` senza marcare l'iniezione,
//     lasciando partire il gioco senza mod (Req 1.3) e mantenendo la build
//     cross-platform compilabile;
//   * sul target iOS (gating TARGET_OS_IPHONE) inject() invoca l'entry point
//     centralizzato del runtime prima della scena iniziale (Req 1.2, 1.4) e
//     l'idempotenza è preservata (seconda chiamata -> AlreadyInjected).
#include "bootstrap/ios_bootstrap.hpp"

#include <gtest/gtest.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace pulse::bootstrap {
namespace {

#if defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
constexpr bool kIsIosTarget = true;
#else
constexpr bool kIsIosTarget = false;
#endif

TEST(IOSBootstrap, PlatformIsIosArm64) {
    IOSBootstrap bootstrap;
    EXPECT_EQ(bootstrap.platform(), Platform::IOSArm64);
    EXPECT_EQ(platform_id(bootstrap.platform()), "ios-arm64");
}

TEST(IOSBootstrap, InjectBehaviourMatchesHost) {
    bool entry_called = false;
    IOSBootstrap bootstrap([&entry_called] {
        entry_called = true;
        return true;
    });

    const auto result = bootstrap.inject();

    if (kIsIosTarget) {
        // Su iOS l'early-load avvia il runtime prima della scena iniziale.
        EXPECT_TRUE(result.injected);
        EXPECT_FALSE(result.error.has_value());
        EXPECT_TRUE(entry_called);

        // Idempotenza: un secondo inject() è un errore diagnostico, non un
        // nuovo aggancio.
        const auto again = bootstrap.inject();
        EXPECT_FALSE(again.injected);
        ASSERT_TRUE(again.error.has_value());
        EXPECT_EQ(again.error->code, BootstrapErrorCode::AlreadyInjected);
    } else {
        // Sull'host non-iOS (incluso macOS desktop) il path è disattivato e
        // l'entry point non viene eseguito; il gioco parte senza mod (Req 1.3).
        EXPECT_FALSE(result.injected);
        ASSERT_TRUE(result.error.has_value());
        EXPECT_EQ(result.error->code, BootstrapErrorCode::UnsupportedHost);
        EXPECT_FALSE(entry_called);
    }
}

}  // namespace
}  // namespace pulse::bootstrap
