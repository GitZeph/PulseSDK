// tests/smoke_test.cpp — smoke test minimale della toolchain (Req 26.2, 26.3).
//
// Scopo: validare che la catena di build/test sia funzionante end-to-end:
//   * GoogleTest compila e linka (asserzioni esemplificative);
//   * RapidCheck compila e linka (property-based testing, ≥100 iterazioni);
//   * gli artefatti `pulse::sdk` (header) e `pulse::loader` (libreria statica)
//     sono compilabili e collegabili insieme al codice di test.
//
// Non verifica logica di dominio: serve come canarino per la toolchain su
// Windows/macOS/Linux.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "pulse/version.hpp"
#include "pulse_loader/loader.hpp"

namespace {

// --- GoogleTest: l'SDK è incluso e linkabile ------------------------------
TEST(ToolchainSmoke, SdkVersionMatchesHeaderMacros) {
    const auto v = pulse::sdk_version();
    EXPECT_EQ(v.major, PULSE_SDK_VERSION_MAJOR);
    EXPECT_EQ(v.minor, PULSE_SDK_VERSION_MINOR);
    EXPECT_EQ(v.patch, PULSE_SDK_VERSION_PATCH);
}

// --- GoogleTest: la libreria statica del loader linka --------------------
TEST(ToolchainSmoke, LoaderLinksAgainstSdk) {
    // loader_sdk_version() vive in pulse_loader: chiamarlo prova il link.
    const auto from_loader = pulse::loader::loader_sdk_version();
    const auto from_sdk = pulse::sdk_version();
    EXPECT_EQ(from_loader.major, from_sdk.major);
    EXPECT_EQ(from_loader.minor, from_sdk.minor);
    EXPECT_EQ(from_loader.patch, from_sdk.patch);
}

// --- GoogleTest: il SO host rilevato è uno di quelli supportati -----------
TEST(ToolchainSmoke, HostOsIsSupported) {
    const auto os = pulse::loader::host_os();
    EXPECT_TRUE(os == pulse::loader::HostOs::Windows ||
                os == pulse::loader::HostOs::MacOS ||
                os == pulse::loader::HostOs::Linux);
}

// --- RapidCheck: smoke property per validare il framework PBT --------------
// Feature: pulse-sdk, toolchain smoke property (≥100 iterazioni di default).
RC_GTEST_PROP(ToolchainSmoke, IntegerAdditionIsCommutative, (int a, int b)) {
    RC_ASSERT(a + b == b + a);
}

}  // namespace
