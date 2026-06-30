// tests/rollback_store_test.cpp — unit test di RollbackStore / RollbackRecord
// (task 8.1, Requisiti 18.1, 18.4, 18.5).
//
// Copre:
//   * round-trip in-memory serialize/deserialize (byte arbitrari, versione,
//     platformId) — invariante del formato su disco;
//   * round-trip persist→reload su file in una directory temporanea (Req 18.1:
//     i record sopravvivono "al riavvio", qui modellato dal reload da disco);
//   * write-through di add() (persiste immediatamente, prima dell'install);
//   * ripristino dei byte originali via callback (Req 18.4) e tramite il
//     FakeBackend come abstraction di write-back;
//   * interruzione del ripristino su fallimento con segnalazione della
//     funzione interessata (Req 18.5);
//   * file assente => store vuoto; rilevamento dati corrotti.
//
// I file temporanei sono creati in una sotto-directory unica di
// temp_directory_path e rimossi nel TearDown (nessun residuo su disco).

#include "hooking/rollback_store.hpp"
#include "hooking/fake_backend.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

using pulse::hooking::FakeBackend;
using pulse::hooking::RollbackErrorCode;
using pulse::hooking::RollbackRecord;
using pulse::hooking::RollbackStore;
using GdVersion = pulse::loader::bindings::GdVersion;

RollbackRecord makeRecord(std::string owner, std::string symbol, std::uintptr_t address,
                          std::vector<std::uint8_t> bytes, GdVersion version,
                          std::string platformId) {
    RollbackRecord r;
    r.owner = std::move(owner);
    r.symbol = std::move(symbol);
    r.address = address;
    r.originalBytes = std::move(bytes);
    r.version = version;
    r.platformId = std::move(platformId);
    return r;
}

class RollbackStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = std::filesystem::temp_directory_path() /
               (std::string("pulse_rollback_test_") + info->name() + "_" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    [[nodiscard]] std::filesystem::path storePath() const { return dir_ / "rollback.pbk"; }

    std::filesystem::path dir_;
};

// --- Round-trip in-memory (formato su disco) -------------------------------

TEST_F(RollbackStoreTest, SerializeDeserializeRoundTripPreservesAllFields) {
    RollbackStore store{storePath()};
    // Byte arbitrari, incluso 0x00 e 0xFF, per verificare la trasparenza ai byte.
    ASSERT_TRUE(store.add(makeRecord("com.acme.mod", "MenuLayer::init", 0xDEADBEEF,
                                     {0x00, 0xFF, 0x10, 0x55, 0x00, 0xAB}, GdVersion{2, 2074},
                                     "windows-x64")));
    ASSERT_TRUE(store.add(makeRecord("org.demo", "PlayLayer::update", 0x1000,
                                     {0x90, 0x90, 0x90}, GdVersion{2, 2081}, "android-arm64")));

    const std::vector<std::uint8_t> bytes = store.serialize();
    std::vector<RollbackRecord> roundTripped;
    const auto res = RollbackStore::deserialize(bytes, roundTripped);
    ASSERT_TRUE(res) << res.error.message;

    EXPECT_EQ(roundTripped, store.records());
}

// --- Round-trip persist -> reload su file (Req 18.1) -----------------------

TEST_F(RollbackStoreTest, PersistThenReloadYieldsIdenticalRecords) {
    std::vector<RollbackRecord> expected = {
        makeRecord("mod.a", "FuncA", 0x4010, {0x48, 0x89, 0x5C, 0x24}, GdVersion{2, 2074},
                   "windows-x64"),
        makeRecord("mod.b", "FuncB", 0x8020, {0x00, 0x01, 0x02, 0x03, 0x04}, GdVersion{1, 9},
                   "macos-arm64"),
        makeRecord("mod.c", "NS::C::method", 0xFFFF0000, {}, GdVersion{2, 2081}, "ios-arm64"),
    };

    {
        RollbackStore store{storePath()};
        for (const auto& r : expected) {
            ASSERT_TRUE(store.add(r));
        }
    }

    // Simula il riavvio del gioco: nuova istanza che ricarica dal disco.
    RollbackStore reloaded{storePath()};
    const auto res = RollbackStore::load(storePath(), reloaded);
    ASSERT_TRUE(res) << res.error.message;
    EXPECT_EQ(reloaded.records(), expected);
}

TEST_F(RollbackStoreTest, AddIsWriteThroughBeforeReload) {
    {
        RollbackStore store{storePath()};
        ASSERT_TRUE(store.add(makeRecord("mod.x", "Sym", 0x2222, {0x11, 0x22}, GdVersion{2, 2074},
                                         "linux-x64")));
        // Nessuna chiamata esplicita a persist(): add() deve già aver scritto.
    }
    RollbackStore reloaded{storePath()};
    ASSERT_TRUE(RollbackStore::load(storePath(), reloaded));
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_EQ(reloaded.records().front().symbol, "Sym");
}

TEST_F(RollbackStoreTest, LoadMissingFileYieldsEmptyStore) {
    RollbackStore store{storePath()};
    const auto res = RollbackStore::load(storePath(), store);
    EXPECT_TRUE(res) << res.error.message;
    EXPECT_TRUE(store.empty());
}

TEST_F(RollbackStoreTest, DeserializeRejectsCorruptData) {
    std::vector<RollbackRecord> out;
    const std::vector<std::uint8_t> garbage = {'X', 'Y', 'Z', 'W', 0x01, 0x02};
    const auto res = RollbackStore::deserialize(garbage, out);
    EXPECT_FALSE(res);
    EXPECT_EQ(res.error.code, RollbackErrorCode::CorruptData);
    EXPECT_TRUE(out.empty());
}

// --- Ripristino dei byte originali (Req 18.4) ------------------------------

TEST_F(RollbackStoreTest, RestoreAllWritesBackOriginalBytesViaCallback) {
    RollbackStore store{storePath()};
    ASSERT_TRUE(store.add(makeRecord("mod.a", "FuncA", 0x100, {0xAA, 0xBB, 0xCC}, GdVersion{2, 2074},
                                     "windows-x64")));
    ASSERT_TRUE(store.add(makeRecord("mod.a", "FuncB", 0x200, {0x01, 0x02}, GdVersion{2, 2074},
                                     "windows-x64")));

    // Memoria simulata: la callback registra i byte scritti per indirizzo.
    std::map<std::uintptr_t, std::vector<std::uint8_t>> written;
    const auto writeFn = [&](std::uintptr_t addr, const std::vector<std::uint8_t>& bytes) {
        written[addr] = bytes;
        return true;
    };

    const auto outcome = store.restoreAll(writeFn);
    ASSERT_TRUE(outcome.ok());
    EXPECT_EQ(outcome.restored, 2u);
    ASSERT_EQ(written.size(), 2u);
    EXPECT_EQ(written[0x100], (std::vector<std::uint8_t>{0xAA, 0xBB, 0xCC}));
    EXPECT_EQ(written[0x200], (std::vector<std::uint8_t>{0x01, 0x02}));
}

TEST_F(RollbackStoreTest, RestoreViaFakeBackendRoundTripRestoresOriginalBytes) {
    // Modella l'install reale: il FakeBackend semina i byte originali, install
    // li "patcha", e il rollback (via RollbackStore) deve riscrivere gli originali.
    FakeBackend backend;
    const std::uintptr_t target = 0xCAFE;
    const std::vector<std::uint8_t> original = {0x55, 0x48, 0x89, 0xE5};
    backend.seedOriginal(target, original);

    // Conserva i byte originali nello store PRIMA dell'install (Req 18.1).
    RollbackStore store{storePath()};
    auto read = backend.readOriginal(target, original.size());
    ASSERT_TRUE(read.has_value());
    ASSERT_TRUE(store.add(makeRecord("mod.a", "Target", target, read.value().bytes(),
                                     GdVersion{2, 2074}, "windows-x64")));

    // Install: i byte live divergono dagli originali.
    ASSERT_TRUE(backend.install(target, reinterpret_cast<void*>(0x1)).has_value());
    EXPECT_NE(backend.liveBytes(target).value(), original);

    // Rollback via store: write-back diretto nella memoria simulata.
    const auto writeFn = [&](std::uintptr_t addr, const std::vector<std::uint8_t>& bytes) {
        backend.seedOriginal(addr, bytes);  // ripristina i byte live == bytes
        return true;
    };
    const auto outcome = store.restoreAll(writeFn);
    ASSERT_TRUE(outcome.ok());
    EXPECT_EQ(backend.liveBytes(target).value(), original);
}

// --- Interruzione su fallimento del ripristino (Req 18.5) ------------------

TEST_F(RollbackStoreTest, RestoreAllAbortsAndReportsFunctionOnFailure) {
    RollbackStore store{storePath()};
    ASSERT_TRUE(store.add(makeRecord("mod.a", "FuncOk", 0x100, {0xAA}, GdVersion{2, 2074},
                                     "windows-x64")));
    ASSERT_TRUE(store.add(makeRecord("mod.a", "FuncBoom", 0x200, {0xBB}, GdVersion{2, 2074},
                                     "windows-x64")));
    ASSERT_TRUE(store.add(makeRecord("mod.a", "FuncNeverReached", 0x300, {0xCC},
                                     GdVersion{2, 2074}, "windows-x64")));

    int calls = 0;
    const auto writeFn = [&](std::uintptr_t addr, const std::vector<std::uint8_t>&) {
        ++calls;
        return addr != 0x200;  // fallisce su FuncBoom
    };

    const auto outcome = store.restoreAll(writeFn);
    EXPECT_FALSE(outcome.ok());
    EXPECT_TRUE(outcome.aborted);
    EXPECT_EQ(outcome.restored, 1u);  // solo FuncOk ripristinato
    EXPECT_EQ(calls, 2);              // interrotto su FuncBoom, FuncNeverReached non chiamato
    ASSERT_TRUE(outcome.error.has_value());
    EXPECT_EQ(outcome.error->code, RollbackErrorCode::RestoreFailed);
    EXPECT_EQ(outcome.error->symbol, "FuncBoom");  // funzione interessata (Req 18.5)
}

TEST_F(RollbackStoreTest, RestoreSingleRecordReportsSymbolOnFailure) {
    RollbackStore store{storePath()};
    const auto record = makeRecord("mod.a", "OnlyFunc", 0x999, {0x42}, GdVersion{2, 2074},
                                   "windows-x64");
    ASSERT_TRUE(store.add(record));

    const auto failFn = [](std::uintptr_t, const std::vector<std::uint8_t>&) { return false; };
    const auto outcome = store.restore(record, failFn);
    EXPECT_FALSE(outcome.ok());
    ASSERT_TRUE(outcome.error.has_value());
    EXPECT_EQ(outcome.error->symbol, "OnlyFunc");
}

TEST_F(RollbackStoreTest, ClearRemovesFileAndRecords) {
    RollbackStore store{storePath()};
    ASSERT_TRUE(store.add(makeRecord("mod.a", "Sym", 0x1, {0x01}, GdVersion{2, 2074},
                                     "windows-x64")));
    ASSERT_TRUE(std::filesystem::exists(storePath()));

    ASSERT_TRUE(store.clear());
    EXPECT_TRUE(store.empty());
    EXPECT_FALSE(std::filesystem::exists(storePath()));
}

}  // namespace
