// tests/crash_sentinel_test.cpp — unit test di CrashSentinel
// (task 8.2, Requisiti 18.2, 18.3, 18.5).
//
// Copre, con tempo e persistenza iniettati (clock controllato + directory
// temporanea), il contratto della sentinella crash / auto-disable:
//   * crash entro i primi 60 s -> al riavvio la mod e' disabilitata, i byte
//     originali ripristinati e l'User riceve un messaggio (Req 18.2/18.3);
//   * il ripristino dei byte e' effettivamente eseguito (write-back) e tocca
//     SOLO gli hook della mod imputata (Req 18.4);
//   * fallimento del ripristino -> interruzione, mod che resta disabilitata e
//     segnalazione della funzione interessata (Req 18.5);
//   * crash DOPO i 60 s e chiusura pulita -> nessun auto-disable (Req 18.2);
//   * la lista delle mod disabilitate sopravvive al "riavvio" (impedire il
//     caricamento all'avvio successivo, Req 18.2);
//   * round-trip serialize/deserialize del marker.
//
// I file temporanei sono creati in una sotto-directory unica di
// temp_directory_path e rimossi nel TearDown (nessun residuo su disco).

#include "hooking/crash_sentinel.hpp"
#include "hooking/rollback_store.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using pulse::hooking::CrashSentinel;
using pulse::hooking::RecoveryReport;
using pulse::hooking::RollbackRecord;
using pulse::hooking::RollbackStore;
using pulse::hooking::SentinelOutcome;
using pulse::hooking::SessionMarker;
using GdVersion = pulse::loader::bindings::GdVersion;

RollbackRecord makeRecord(std::string owner, std::string symbol, std::uintptr_t address,
                          std::vector<std::uint8_t> bytes) {
    RollbackRecord r;
    r.owner = std::move(owner);
    r.symbol = std::move(symbol);
    r.address = address;
    r.originalBytes = std::move(bytes);
    r.version = GdVersion{2, 2074};
    r.platformId = "windows-x64";
    return r;
}

class CrashSentinelTest : public ::testing::Test {
protected:
    void SetUp() override {
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = std::filesystem::temp_directory_path() /
               (std::string("pulse_sentinel_test_") + info->name() + "_" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        now_ = 1000;  // base arbitraria del clock iniettato
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    [[nodiscard]] std::filesystem::path markerPath() const { return dir_ / "session.marker"; }
    [[nodiscard]] std::filesystem::path disabledPath() const { return dir_ / "disabled.mods"; }
    [[nodiscard]] std::filesystem::path rollbackPath() const { return dir_ / "rollback.pbk"; }

    // Clock iniettato controllato dal test.
    [[nodiscard]] pulse::hooking::SentinelClock clock() {
        return [this] { return now_; };
    }

    std::unique_ptr<CrashSentinel> makeSentinel() {
        return std::make_unique<CrashSentinel>(markerPath(), disabledPath(), clock());
    }

    std::filesystem::path dir_;
    std::int64_t now_ = 0;
};

// --- Round-trip del marker -------------------------------------------------

TEST_F(CrashSentinelTest, MarkerSerializeDeserializeRoundTrip) {
    SessionMarker m;
    m.startupTime = 12345;
    m.lastHeartbeat = 12399;
    m.graceSurvived = true;
    m.cleanShutdown = false;
    m.activeMod = std::string("com.acme.crashy");

    const auto bytes = CrashSentinel::serializeMarker(m);
    SessionMarker out;
    const auto res = CrashSentinel::deserializeMarker(bytes, out);
    ASSERT_TRUE(res) << res.error.message;
    EXPECT_EQ(out, m);
}

TEST_F(CrashSentinelTest, MarkerRoundTripWithoutActiveMod) {
    SessionMarker m;
    m.startupTime = 7;
    m.lastHeartbeat = 7;
    SessionMarker out;
    ASSERT_TRUE(CrashSentinel::deserializeMarker(CrashSentinel::serializeMarker(m), out));
    EXPECT_EQ(out, m);
    EXPECT_FALSE(out.activeMod.has_value());
}

// --- Crash entro 60 s -> auto-disable al riavvio (Req 18.2, 18.3, 18.4) ----

TEST_F(CrashSentinelTest, CrashWithin60sAutoDisablesAndRestoresOnNextStart) {
    // RollbackStore con i record persistiti dei due hook della mod che crasha,
    // piu' un hook di un'altra mod che NON deve essere toccato.
    RollbackStore store{rollbackPath()};
    ASSERT_TRUE(store.add(makeRecord("com.acme.crashy", "MenuLayer::init", 0x1000, {0xAA, 0xBB})));
    ASSERT_TRUE(store.add(makeRecord("com.acme.crashy", "PlayLayer::update", 0x2000, {0xCC})));
    ASSERT_TRUE(store.add(makeRecord("org.other.safe", "GJ::foo", 0x3000, {0xDD})));

    // --- Sessione 1: avvio, una mod diventa attiva, poi "crash" (no shutdown).
    {
        auto sentinel = makeSentinel();
        ASSERT_TRUE(sentinel->beginSession());          // t=1000
        now_ += 5;                                       // 5 s dopo l'avvio
        ASSERT_TRUE(sentinel->recordActiveMod("com.acme.crashy"));
        // Nessun markCleanShutdown / markGraceSurvived: simula il crash.
    }

    // --- Sessione 2: riavvio. La sentinella recupera dalla sessione precedente.
    std::map<std::uintptr_t, std::vector<std::uint8_t>> written;
    const auto writeFn = [&](std::uintptr_t addr, const std::vector<std::uint8_t>& bytes) {
        written[addr] = bytes;
        return true;
    };
    std::vector<std::string> userMessages;
    const auto userSink = [&](std::string_view msg) { userMessages.emplace_back(msg); };

    now_ = 5000;  // nuovo avvio molto piu' tardi
    auto sentinel2 = makeSentinel();
    const RecoveryReport rep = sentinel2->recoverFromPreviousSession(store, writeFn, userSink);

    // Auto-disable della mod imputata (Req 18.2).
    EXPECT_EQ(rep.outcome, SentinelOutcome::AutoDisabled);
    ASSERT_TRUE(rep.blamedMod.has_value());
    EXPECT_EQ(*rep.blamedMod, "com.acme.crashy");
    EXPECT_TRUE(sentinel2->isModDisabled("com.acme.crashy"));
    EXPECT_FALSE(sentinel2->isModDisabled("org.other.safe"));

    // Ripristino effettuato SOLO sugli hook della mod imputata (Req 18.4).
    EXPECT_EQ(rep.restoredFunctions, 2u);
    ASSERT_EQ(written.size(), 2u);
    EXPECT_EQ(written[0x1000], (std::vector<std::uint8_t>{0xAA, 0xBB}));
    EXPECT_EQ(written[0x2000], (std::vector<std::uint8_t>{0xCC}));
    EXPECT_EQ(written.count(0x3000), 0u);  // hook dell'altra mod NON toccato

    // Messaggio all'User col nome della mod e il motivo (Req 18.3).
    ASSERT_EQ(userMessages.size(), 1u);
    EXPECT_NE(userMessages[0].find("com.acme.crashy"), std::string::npos);
    EXPECT_NE(rep.userMessage.find("com.acme.crashy"), std::string::npos);
}

// --- La disabilitazione sopravvive al riavvio (Req 18.2) -------------------

TEST_F(CrashSentinelTest, DisabledListPersistsAcrossRestart) {
    RollbackStore store{rollbackPath()};
    ASSERT_TRUE(store.add(makeRecord("mod.boom", "F", 0x10, {0x01})));

    const auto writeFn = [](std::uintptr_t, const std::vector<std::uint8_t>&) { return true; };

    {
        auto s = makeSentinel();
        ASSERT_TRUE(s->beginSession());
        now_ += 3;
        ASSERT_TRUE(s->recordActiveMod("mod.boom"));
    }
    now_ = 9000;
    {
        auto s = makeSentinel();
        const auto rep = s->recoverFromPreviousSession(store, writeFn);
        ASSERT_EQ(rep.outcome, SentinelOutcome::AutoDisabled);
        EXPECT_TRUE(s->isModDisabled("mod.boom"));
    }
    // Un avvio successivo (nuova istanza) deve continuare a vedere la mod
    // disabilitata: la lista e' stata persistita su disco.
    {
        auto s = makeSentinel();
        EXPECT_TRUE(s->isModDisabled("mod.boom"));
        // enableMod la riabilita e persiste la rimozione.
        ASSERT_TRUE(s->enableMod("mod.boom"));
        EXPECT_FALSE(s->isModDisabled("mod.boom"));
    }
    {
        auto s = makeSentinel();
        EXPECT_FALSE(s->isModDisabled("mod.boom"));
    }
}

// --- Fallimento del ripristino: abort + segnalazione funzione (Req 18.5) ---

TEST_F(CrashSentinelTest, RestoreFailureAbortsKeepsDisabledAndReportsFunction) {
    RollbackStore store{rollbackPath()};
    ASSERT_TRUE(store.add(makeRecord("mod.boom", "FuncOk", 0x100, {0xAA})));
    ASSERT_TRUE(store.add(makeRecord("mod.boom", "FuncBoom", 0x200, {0xBB})));
    ASSERT_TRUE(store.add(makeRecord("mod.boom", "FuncNeverReached", 0x300, {0xCC})));

    {
        auto s = makeSentinel();
        ASSERT_TRUE(s->beginSession());
        now_ += 10;
        ASSERT_TRUE(s->recordActiveMod("mod.boom"));
    }

    int calls = 0;
    const auto writeFn = [&](std::uintptr_t addr, const std::vector<std::uint8_t>&) {
        ++calls;
        return addr != 0x200;  // fallisce su FuncBoom
    };
    std::vector<std::string> userMessages;
    const auto userSink = [&](std::string_view msg) { userMessages.emplace_back(msg); };

    now_ = 9000;
    auto s = makeSentinel();
    const auto rep = s->recoverFromPreviousSession(store, writeFn, userSink);

    EXPECT_EQ(rep.outcome, SentinelOutcome::RestoreFailed);
    // Interrotto sul secondo record: FuncNeverReached non e' stato chiamato.
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(rep.restoredFunctions, 1u);
    ASSERT_TRUE(rep.failedFunction.has_value());
    EXPECT_EQ(*rep.failedFunction, "FuncBoom");  // funzione interessata (Req 18.5)

    // La mod resta disabilitata nonostante il fallimento del ripristino (Req 18.5).
    EXPECT_TRUE(s->isModDisabled("mod.boom"));

    // Messaggio all'User col fallimento e la funzione interessata (Req 18.5).
    ASSERT_EQ(userMessages.size(), 1u);
    EXPECT_NE(userMessages[0].find("FuncBoom"), std::string::npos);
}

// --- Crash DOPO i 60 s: nessun auto-disable (Req 18.2) ---------------------

TEST_F(CrashSentinelTest, CrashAfterGraceWindowDoesNotAutoDisable) {
    RollbackStore store{rollbackPath()};
    ASSERT_TRUE(store.add(makeRecord("mod.boom", "F", 0x10, {0x01})));

    bool restoreCalled = false;
    const auto writeFn = [&](std::uintptr_t, const std::vector<std::uint8_t>&) {
        restoreCalled = true;
        return true;
    };

    {
        auto s = makeSentinel();
        ASSERT_TRUE(s->beginSession());  // t=1000
        now_ += 5;
        ASSERT_TRUE(s->recordActiveMod("mod.boom"));
        now_ += 100;                     // > 60 s dopo l'avvio
        ASSERT_TRUE(s->heartbeat());     // marca la finestra di grazia superata
        // Poi "crash" (nessuna chiusura pulita).
    }

    now_ = 9000;
    auto s = makeSentinel();
    const auto rep = s->recoverFromPreviousSession(store, writeFn);
    EXPECT_EQ(rep.outcome, SentinelOutcome::CrashOutsideWindow);
    EXPECT_FALSE(s->isModDisabled("mod.boom"));
    EXPECT_FALSE(restoreCalled);
}

// --- Chiusura pulita: nessun auto-disable ----------------------------------

TEST_F(CrashSentinelTest, CleanShutdownDoesNotAutoDisable) {
    RollbackStore store{rollbackPath()};
    ASSERT_TRUE(store.add(makeRecord("mod.boom", "F", 0x10, {0x01})));
    const auto writeFn = [](std::uintptr_t, const std::vector<std::uint8_t>&) { return true; };

    {
        auto s = makeSentinel();
        ASSERT_TRUE(s->beginSession());
        now_ += 5;
        ASSERT_TRUE(s->recordActiveMod("mod.boom"));
        ASSERT_TRUE(s->markCleanShutdown());  // chiusura ordinata
    }

    now_ = 9000;
    auto s = makeSentinel();
    const auto rep = s->recoverFromPreviousSession(store, writeFn);
    EXPECT_EQ(rep.outcome, SentinelOutcome::CleanShutdown);
    EXPECT_FALSE(s->isModDisabled("mod.boom"));
}

// --- Nessuna sessione precedente -------------------------------------------

TEST_F(CrashSentinelTest, NoPreviousSessionIsNoOp) {
    RollbackStore store{rollbackPath()};
    const auto writeFn = [](std::uintptr_t, const std::vector<std::uint8_t>&) { return true; };
    auto s = makeSentinel();
    const auto rep = s->recoverFromPreviousSession(store, writeFn);
    EXPECT_EQ(rep.outcome, SentinelOutcome::NoPreviousSession);
}

// --- Crash entro 60 s ma nessuna mod attiva --------------------------------

TEST_F(CrashSentinelTest, CrashWithin60sButNoActiveModIsNotAttributed) {
    RollbackStore store{rollbackPath()};
    const auto writeFn = [](std::uintptr_t, const std::vector<std::uint8_t>&) { return true; };

    {
        auto s = makeSentinel();
        ASSERT_TRUE(s->beginSession());  // nessuna recordActiveMod
        now_ += 5;
        ASSERT_TRUE(s->heartbeat());
    }
    now_ = 9000;
    auto s = makeSentinel();
    const auto rep = s->recoverFromPreviousSession(store, writeFn);
    EXPECT_EQ(rep.outcome, SentinelOutcome::NoActiveModToBlame);
}

// --- Il marker e' consumato dopo la recovery (idempotenza) -----------------

TEST_F(CrashSentinelTest, MarkerIsConsumedAfterRecovery) {
    RollbackStore store{rollbackPath()};
    ASSERT_TRUE(store.add(makeRecord("mod.boom", "F", 0x10, {0x01})));
    const auto writeFn = [](std::uintptr_t, const std::vector<std::uint8_t>&) { return true; };

    {
        auto s = makeSentinel();
        ASSERT_TRUE(s->beginSession());
        now_ += 3;
        ASSERT_TRUE(s->recordActiveMod("mod.boom"));
    }
    now_ = 9000;
    {
        auto s = makeSentinel();
        const auto rep = s->recoverFromPreviousSession(store, writeFn);
        ASSERT_EQ(rep.outcome, SentinelOutcome::AutoDisabled);
    }
    // Una seconda recovery (senza nuova sessione) non deve trovare nulla.
    {
        auto s = makeSentinel();
        const auto rep = s->recoverFromPreviousSession(store, writeFn);
        EXPECT_EQ(rep.outcome, SentinelOutcome::NoPreviousSession);
    }
}

}  // namespace
