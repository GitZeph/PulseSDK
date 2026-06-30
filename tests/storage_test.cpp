// storage_test.cpp — unit test di pulse::storage::ModStorage (task 20.1).
//
// Copre le semantiche osservabili del Requisito 10:
//   * round-trip set/get della scrittura più recente (Req 10.1, presupposto di
//     10.2 validato a fondo dal property test 20.2);
//   * chiave assente -> std::nullopt senza errore bloccante (Req 10.4);
//   * rifiuto su capacità superata che preserva invariati i dati già presenti
//     e segnala il superamento (Req 10.5);
//   * isolamento per identità della Mod (Req 10.1);
//   * capacità di default ≥ 10 MB (Req 10.1).
#include <pulse/storage.hpp>

#include <string>

#include <gtest/gtest.h>

using pulse::storage::kDefaultCapacityBytes;
using pulse::storage::ModStorage;
using pulse::storage::StorageErrorCode;
using pulse::storage::Value;

// Round-trip: scrivere una chiave e rileggerla restituisce il valore scritto.
TEST(ModStorageTest, RoundTripSetGetReturnsWrittenValue) {
    ModStorage storage("mod.alpha");

    ASSERT_TRUE(storage.set("level", Value("data-123")));

    auto read = storage.get("level");
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, Value("data-123"));
}

// La scrittura più recente per una chiave sovrascrive la precedente.
TEST(ModStorageTest, MostRecentWriteWins) {
    ModStorage storage("mod.alpha");

    ASSERT_TRUE(storage.set("k", Value("v1")));
    ASSERT_TRUE(storage.set("k", Value("v2-longer")));

    auto read = storage.get("k");
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, Value("v2-longer"));
    EXPECT_EQ(storage.size(), 1u);
}

// Valori binari (contenenti '\0') vengono preservati intatti.
TEST(ModStorageTest, BinarySafeValues) {
    ModStorage storage("mod.alpha");
    const Value binary("a\0b\0c", 5);

    ASSERT_TRUE(storage.set("blob", binary));
    auto read = storage.get("blob");
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->size(), 5u);
    EXPECT_EQ(*read, binary);
}

// Req 10.4: leggere una chiave inesistente restituisce un valore assente senza
// generare un errore bloccante.
TEST(ModStorageTest, AbsentKeyReturnsNulloptWithoutError) {
    ModStorage storage("mod.alpha");

    EXPECT_FALSE(storage.contains("missing"));
    EXPECT_EQ(storage.get("missing"), std::nullopt);

    // Anche dopo aver scritto altre chiavi, una chiave assente resta nullopt.
    ASSERT_TRUE(storage.set("present", Value("x")));
    EXPECT_EQ(storage.get("still-missing"), std::nullopt);
}

// Req 10.5: una scrittura che supererebbe la capacità è rifiutata, i dati già
// presenti restano invariati e l'errore segnala il superamento di capacità.
TEST(ModStorageTest, CapacityExceededRejectionPreservesPriorData) {
    // Capacità iniettata a 16 byte: spazio per "k1"(2)+"hello"(5)=7 usati.
    ModStorage storage("mod.alpha", /*capacityBytes=*/16);

    ASSERT_TRUE(storage.set("k1", Value("hello")));
    EXPECT_EQ(storage.usedBytes(), 7u);

    // "k2"(2) + valore di 20 byte = 22 -> proiezione 29 > 16: deve fallire.
    const Value tooBig(20, 'z');
    auto result = storage.set("k2", tooBig);
    ASSERT_FALSE(result.isOk());
    EXPECT_EQ(result.error().code, StorageErrorCode::CapacityExceeded);

    // I dati preesistenti sono invariati e la chiave rifiutata non esiste.
    EXPECT_FALSE(storage.contains("k2"));
    EXPECT_EQ(storage.get("k2"), std::nullopt);
    auto kept = storage.get("k1");
    ASSERT_TRUE(kept.has_value());
    EXPECT_EQ(*kept, Value("hello"));
    EXPECT_EQ(storage.usedBytes(), 7u);
    EXPECT_EQ(storage.size(), 1u);
}

// Una sovrascrittura che rientra nella capacità è accettata e aggiorna l'uso;
// una sovrascrittura che la supererebbe è rifiutata preservando il valore.
TEST(ModStorageTest, OverwriteCapacityAccounting) {
    ModStorage storage("mod.alpha", /*capacityBytes=*/10);

    ASSERT_TRUE(storage.set("k", Value("12345")));  // 1 + 5 = 6 usati.
    EXPECT_EQ(storage.usedBytes(), 6u);

    // Sovrascrittura entro capacità: "k"(1) + 9 = 10 == capacità.
    ASSERT_TRUE(storage.set("k", Value("123456789")));
    EXPECT_EQ(storage.usedBytes(), 10u);

    // Sovrascrittura oltre capacità: "k"(1) + 10 = 11 > 10 -> rifiuto.
    auto result = storage.set("k", Value(10, 'x'));
    ASSERT_FALSE(result.isOk());
    EXPECT_EQ(result.error().code, StorageErrorCode::CapacityExceeded);
    // Il valore precedente è preservato.
    EXPECT_EQ(*storage.get("k"), Value("123456789"));
    EXPECT_EQ(storage.usedBytes(), 10u);
}

// Req 10.1: due ModStorage con identità diverse non condividono i dati.
TEST(ModStorageTest, PerModIsolation) {
    ModStorage alpha("mod.alpha");
    ModStorage beta("mod.beta");

    ASSERT_TRUE(alpha.set("shared-key", Value("from-alpha")));
    ASSERT_TRUE(beta.set("shared-key", Value("from-beta")));

    EXPECT_EQ(*alpha.get("shared-key"), Value("from-alpha"));
    EXPECT_EQ(*beta.get("shared-key"), Value("from-beta"));

    // Una chiave scritta solo in alpha è assente in beta.
    ASSERT_TRUE(alpha.set("only-alpha", Value("x")));
    EXPECT_EQ(beta.get("only-alpha"), std::nullopt);

    EXPECT_EQ(alpha.modId(), "mod.alpha");
    EXPECT_EQ(beta.modId(), "mod.beta");
}

// Req 10.1: la capacità di default è almeno 10 MB per Mod.
TEST(ModStorageTest, DefaultCapacityAtLeast10MB) {
    ModStorage storage("mod.alpha");
    EXPECT_GE(storage.capacityBytes(), 10u * 1024u * 1024u);
    EXPECT_EQ(storage.capacityBytes(), kDefaultCapacityBytes);
    EXPECT_EQ(storage.usedBytes(), 0u);
    EXPECT_EQ(storage.size(), 0u);
}
