// pulse_field_test.cpp — unit test di PulseField<T, Key, Class> (task 16.1,
// Requisiti 6.1, 6.2, 6.3).
//
// Verifica la porzione storage/get/set/default/registro:
//   * default tipizzato quando il campo non è mai stato scritto (Req 6.3);
//   * round-trip set→get per-istanza (Req 6.2);
//   * isolamento per-istanza: scrivere su un'istanza non altera le altre (Req 6.2);
//   * più chiavi/campi indipendenti sulla stessa classe (Req 6.1);
//   * registro chiavi per classe e capacità fino a 256 campi (Req 6.1).
//
// Header-only: include il header pubblico dello SDK <pulse/fields.hpp>.
#include <pulse/fields.hpp>

#include <string>

#include <gtest/gtest.h>

namespace {

// Classi del gioco fittizie usate come scope dei campi iniettati.
struct Player {
    int dummy{0};
};
struct Enemy {
    int dummy{0};
};

// ---------------------------------------------------------------------------
// Req 6.3 — un campo mai assegnato restituisce il default tipizzato.
// ---------------------------------------------------------------------------
TEST(PulseFieldTest, UnwrittenFieldReturnsTypedDefault) {
    pulse::PulseField<int, "test/default_int", Player> intField;
    pulse::PulseField<std::string, "test/default_str", Player> strField;
    pulse::PulseField<bool, "test/default_bool", Player> boolField;
    intField.clearAll();
    strField.clearAll();
    boolField.clearAll();

    Player player;

    EXPECT_EQ(intField.get(&player), 0);
    EXPECT_EQ(strField.get(&player), std::string{});
    EXPECT_FALSE(boolField.get(&player));
    // La lettura non crea stato: nessuna istanza memorizzata.
    EXPECT_FALSE(intField.has(&player));
    EXPECT_EQ(intField.liveInstanceCount(), 0u);
}

// ---------------------------------------------------------------------------
// Req 6.2 — set memorizza per l'istanza e get restituisce il valore scritto.
// ---------------------------------------------------------------------------
TEST(PulseFieldTest, SetThenGetRoundTrip) {
    pulse::PulseField<int, "test/round_trip", Player> field;
    field.clearAll();

    Player player;
    field.set(&player, 42);

    EXPECT_TRUE(field.has(&player));
    EXPECT_EQ(field.get(&player), 42);

    // Sovrascrittura: l'ultimo valore vince.
    field.set(&player, 7);
    EXPECT_EQ(field.get(&player), 7);
}

// ---------------------------------------------------------------------------
// Req 6.2 — isolamento per-istanza: scrivere su un'istanza non altera le altre.
// ---------------------------------------------------------------------------
TEST(PulseFieldTest, PerInstanceIsolation) {
    pulse::PulseField<int, "test/isolation", Player> field;
    field.clearAll();

    Player a;
    Player b;
    Player c;

    field.set(&a, 100);
    field.set(&b, 200);
    // c non è mai stato scritto.

    EXPECT_EQ(field.get(&a), 100);
    EXPECT_EQ(field.get(&b), 200);
    EXPECT_EQ(field.get(&c), 0);  // default tipizzato (Req 6.3)

    // Modificare a non tocca b né c.
    field.set(&a, 101);
    EXPECT_EQ(field.get(&a), 101);
    EXPECT_EQ(field.get(&b), 200);
    EXPECT_EQ(field.get(&c), 0);

    // clear(a) rilascia solo lo stato di a (base del cleanup, Req 6.4).
    EXPECT_TRUE(field.clear(&a));
    EXPECT_FALSE(field.has(&a));
    EXPECT_EQ(field.get(&a), 0);
    EXPECT_EQ(field.get(&b), 200);  // b resta intatto (Req 6.2)
}

// ---------------------------------------------------------------------------
// Req 6.1 — più campi (chiavi) distinti sulla stessa classe sono indipendenti.
// ---------------------------------------------------------------------------
TEST(PulseFieldTest, MultipleKeysAreIndependent) {
    pulse::PulseField<int, "test/jumpCount", Player> jumpCount;
    pulse::PulseField<int, "test/coinCount", Player> coinCount;
    pulse::PulseField<std::string, "test/nickname", Player> nickname;
    jumpCount.clearAll();
    coinCount.clearAll();
    nickname.clearAll();

    Player player;
    jumpCount.set(&player, 3);
    coinCount.set(&player, 50);
    nickname.set(&player, "neo");

    EXPECT_EQ(jumpCount.get(&player), 3);
    EXPECT_EQ(coinCount.get(&player), 50);
    EXPECT_EQ(nickname.get(&player), "neo");

    // Modificare un campo non altera gli altri campi della stessa istanza.
    jumpCount.set(&player, 4);
    EXPECT_EQ(jumpCount.get(&player), 4);
    EXPECT_EQ(coinCount.get(&player), 50);
    EXPECT_EQ(nickname.get(&player), "neo");
}

// ---------------------------------------------------------------------------
// Req 6.2 — campi con la stessa chiave ma su classi diverse sono isolati
// (storage e registro per classe distinti).
// ---------------------------------------------------------------------------
TEST(PulseFieldTest, SameKeyDifferentClassesAreIsolated) {
    pulse::PulseField<int, "test/shared_key", Player> playerField;
    pulse::PulseField<int, "test/shared_key", Enemy> enemyField;
    playerField.clearAll();
    enemyField.clearAll();

    Player player;
    Enemy enemy;

    playerField.set(&player, 11);
    enemyField.set(&enemy, 22);

    EXPECT_EQ(playerField.get(&player), 11);
    EXPECT_EQ(enemyField.get(&enemy), 22);
}

// ---------------------------------------------------------------------------
// Req 6.3 — getRef inizializza con il default e consente la mutazione in-place.
// ---------------------------------------------------------------------------
TEST(PulseFieldTest, GetRefInitializesWithDefaultAndMutates) {
    pulse::PulseField<int, "test/get_ref", Player> field;
    field.clearAll();

    Player player;
    int& ref = field.getRef(&player);
    EXPECT_EQ(ref, 0);  // default tipizzato (Req 6.3)

    ref = 99;
    EXPECT_EQ(field.get(&player), 99);
    EXPECT_TRUE(field.has(&player));
}

// ---------------------------------------------------------------------------
// Req 6.1 — il registro raccoglie le chiavi distinte PER CLASSE.
// ---------------------------------------------------------------------------
TEST(PulseFieldTest, KeyRegistryTracksKeysPerClass) {
    // Classe dedicata per isolare il registro da altri test.
    struct Boss {};
    using Registry = pulse::fields::detail::FieldRegistry<Boss>;
    Registry::reset();

    EXPECT_EQ(Registry::fieldCount(), 0u);

    pulse::PulseField<int, "boss/hp", Boss> hp;
    pulse::PulseField<int, "boss/phase", Boss> phase;

    EXPECT_EQ(Registry::fieldCount(), 2u);
    EXPECT_TRUE(Registry::contains("boss/hp"));
    EXPECT_TRUE(Registry::contains("boss/phase"));
    EXPECT_FALSE(Registry::contains("boss/unknown"));

    // La registrazione della stessa chiave è idempotente (nessun duplicato).
    pulse::PulseField<int, "boss/hp", Boss> hpAgain;
    EXPECT_EQ(Registry::fieldCount(), 2u);
}

// ---------------------------------------------------------------------------
// Req 6.1 — capacità: fino a 256 campi distinti per classe.
// ---------------------------------------------------------------------------
TEST(PulseFieldTest, RegistrySupportsUpTo256Fields) {
    struct CapacityClass {};
    using Registry = pulse::fields::detail::FieldRegistry<CapacityClass>;
    Registry::reset();

    EXPECT_EQ(Registry::kMaxFields, 256u);

    // Registra 256 chiavi distinte: tutte accettate.
    for (int i = 0; i < 256; ++i) {
        const std::string key = "cap/field_" + std::to_string(i);
        EXPECT_TRUE(Registry::registerKey(key)) << "chiave " << i;
    }
    EXPECT_EQ(Registry::fieldCount(), 256u);
    EXPECT_TRUE(Registry::atCapacity());

    // Una 257ª chiave NUOVA viene rifiutata (oltre il limite di 256, Req 6.1).
    EXPECT_FALSE(Registry::registerKey("cap/field_256"));
    EXPECT_EQ(Registry::fieldCount(), 256u);

    // Ri-registrare una chiave già presente resta consentito (idempotente).
    EXPECT_TRUE(Registry::registerKey("cap/field_0"));
    EXPECT_EQ(Registry::fieldCount(), 256u);
}

}  // namespace
