// tests/read_original_test.cpp — unit test focalizzato sul round-trip di
// `readOriginal` (task 4.6, Req 2.2).
//
// Proprietà sotto test (round-trip del prologo): i byte restituiti da
// `readOriginal` coincidono ESATTAMENTE con i byte del prologo originale che
// vengono (o verrebbero) sovrascritti da `install`, anche DOPO che `install`
// ha patchato i byte "live". Questo è il presupposto del rollback persistente
// (Req 18.1): l'engine cattura i byte originali via readOriginal per poterli
// ripristinare, quindi readOriginal non deve mai riflettere la patch.
//
// Nota: complementare a tests/fake_backend_test.cpp — qui l'asserzione è
// esplicitamente "i byte letti == i byte effettivamente sovrascritti da
// install", catturando il prologo pre-install come riferimento.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "hooking/fake_backend.hpp"
#include "hooking/minhook_backend.hpp"

namespace {

using pulse::hooking::FakeBackend;
using pulse::hooking::HookErrorCode;

using Bytes = FakeBackend::Bytes;

int g_detour = 0;
void* const kDetour = &g_detour;

// --- round-trip: readOriginal == prologo sovrascritto, dopo install --------
//
// Cattura i byte "live" del prologo PRIMA di install (= ciò che install
// sovrascriverà), installa (i live cambiano), poi verifica che readOriginal
// restituisca ancora esattamente quei byte pre-install.
TEST(ReadOriginalRoundTrip, ReturnsOverwrittenPrologueBytesAfterInstall) {
    constexpr std::uintptr_t kTarget = 0x4000;
    const Bytes prologue{0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22};

    FakeBackend backend;
    backend.seedOriginal(kTarget, prologue);

    // I byte che install sovrascriverà sono esattamente il prologo "live".
    const auto overwritten = backend.liveBytes(kTarget);
    ASSERT_TRUE(overwritten.has_value());
    ASSERT_EQ(*overwritten, prologue);

    // install patcha i byte live (il prologo viene sovrascritto dallo stub).
    ASSERT_TRUE(backend.install(kTarget, kDetour).has_value());
    const auto patched = backend.liveBytes(kTarget);
    ASSERT_TRUE(patched.has_value());
    ASSERT_NE(*patched, prologue) << "install deve aver sovrascritto il prologo";

    // readOriginal restituisce ESATTAMENTE i byte sovrascritti, non i patchati.
    auto read = backend.readOriginal(kTarget, prologue.size());
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read.value().bytes(), *overwritten);
    EXPECT_EQ(read.value().size(), prologue.size());
}

// --- round-trip stabile: stesso risultato prima e dopo install -------------
//
// readOriginal deve essere idempotente rispetto a install: leggere prima o
// dopo l'installazione produce gli stessi identici byte.
TEST(ReadOriginalRoundTrip, IdenticalBeforeAndAfterInstall) {
    constexpr std::uintptr_t kTarget = 0x8100;
    const Bytes prologue{0x90, 0x90, 0x55, 0x48, 0x89, 0xE5, 0x01, 0x02};

    FakeBackend backend;
    backend.seedOriginal(kTarget, prologue);

    auto before = backend.readOriginal(kTarget, prologue.size());
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before.value().bytes(), prologue);

    ASSERT_TRUE(backend.install(kTarget, kDetour).has_value());

    auto after = backend.readOriginal(kTarget, prologue.size());
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after.value().bytes(), before.value().bytes());
}

// --- round-trip su prefisso: legge esattamente i primi `len` byte ----------
//
// Quando install sovrascriverebbe solo i primi `len` byte del prologo,
// readOriginal(len) deve restituire esattamente quel prefisso originale.
TEST(ReadOriginalRoundTrip, ReturnsExactPrefixOfOriginalPrologue) {
    constexpr std::uintptr_t kTarget = 0xC200;
    const Bytes prologue{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

    FakeBackend backend;
    backend.seedOriginal(kTarget, prologue);
    ASSERT_TRUE(backend.install(kTarget, kDetour).has_value());

    constexpr std::size_t kLen = 4;
    auto read = backend.readOriginal(kTarget, kLen);
    ASSERT_TRUE(read.has_value());

    const Bytes expectedPrefix(prologue.begin(), prologue.begin() + kLen);
    EXPECT_EQ(read.value().bytes(), expectedPrefix);
    EXPECT_EQ(read.value().size(), kLen);
}

// --- invariante documentata del backend MinHook su host non supportato -----
//
// Il design documenta che, fuori da Windows x64, MinHookBackend è uno stub:
// readOriginal riporta `Unsupported` e available() è false (Req 2.2/26.3).
// Su Windows x64 il path reale legge i byte e non deve riportare Unsupported.
TEST(ReadOriginalRoundTrip, MinHookDocumentedInvariant) {
    auto backend = pulse::hooking::make_minhook_backend();
    ASSERT_NE(backend, nullptr);
    EXPECT_EQ(backend->name(), "pulse-minhook");

    auto read = backend->readOriginal(0x1000, 4);
#if defined(_WIN32) && defined(PULSE_HOOK_BACKEND_MINHOOK)
    // Sul path reale può riuscire o fallire per protezione di memoria, ma
    // l'indirizzo non è "non supportato".
    if (!read.has_value()) {
        EXPECT_NE(read.error().code, HookErrorCode::Unsupported);
    }
#else
    EXPECT_FALSE(backend->available());
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().code, HookErrorCode::Unsupported);
#endif
}

}  // namespace
