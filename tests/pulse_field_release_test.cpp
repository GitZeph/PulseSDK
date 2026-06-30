// pulse_field_release_test.cpp — unit test del cleanup al distruttore di
// PulseField (task 16.2, Requisito 6.4).
//
// Verifica `pulse::fields::releaseInstance<Class>(instance)`: alla distruzione
// di un'istanza si rilascia, entro una sola operazione, TUTTO lo stato
// iniettato di quell'istanza attraverso più campi/chiavi della stessa classe,
// senza alterare lo stato iniettato delle altre istanze (Req 6.4 + isolamento
// per-istanza Req 6.2).
//
// Header-only: include il header pubblico dello SDK <pulse/fields.hpp>.
#include <pulse/fields.hpp>

#include <string>

#include <gtest/gtest.h>

namespace {

// Classi del gioco fittizie usate come scope dei campi iniettati.
struct Actor {
    int dummy{0};
};
struct Widget {
    int dummy{0};
};

// ---------------------------------------------------------------------------
// Req 6.4 — releaseInstance rilascia, in un'unica operazione, TUTTI i campi
// (più chiavi) di UNA istanza, lasciando intatte le altre istanze.
// ---------------------------------------------------------------------------
TEST(PulseFieldReleaseTest, ReleaseInstanceClearsAllFieldsOfOneInstance) {
    pulse::PulseField<int, "rel/hp", Actor> hp;
    pulse::PulseField<std::string, "rel/name", Actor> name;
    pulse::PulseField<bool, "rel/alive", Actor> alive;
    hp.clearAll();
    name.clearAll();
    alive.clearAll();

    Actor a;
    Actor b;

    // Stato iniettato su due istanze, su più campi/chiavi.
    hp.set(&a, 100);
    name.set(&a, "alice");
    alive.set(&a, true);

    hp.set(&b, 50);
    name.set(&b, "bob");
    alive.set(&b, false);

    ASSERT_TRUE(hp.has(&a));
    ASSERT_TRUE(name.has(&a));
    ASSERT_TRUE(alive.has(&a));

    // Distruzione (simulata) di `a`: rilascia tutti e 3 i campi di `a`.
    const std::size_t released = pulse::fields::releaseInstance<Actor>(&a);
    EXPECT_EQ(released, 3u);

    // Tutto lo stato di `a` è rilasciato su OGNI chiave (Req 6.4).
    EXPECT_FALSE(hp.has(&a));
    EXPECT_FALSE(name.has(&a));
    EXPECT_FALSE(alive.has(&a));
    // Letture post-rilascio tornano al default tipizzato (Req 6.3).
    EXPECT_EQ(hp.get(&a), 0);
    EXPECT_EQ(name.get(&a), std::string{});
    EXPECT_FALSE(alive.get(&a));

    // L'istanza `b` resta completamente intatta (Req 6.2/6.4).
    EXPECT_TRUE(hp.has(&b));
    EXPECT_EQ(hp.get(&b), 50);
    EXPECT_EQ(name.get(&b), "bob");
    EXPECT_FALSE(alive.get(&b));
}

// ---------------------------------------------------------------------------
// Req 6.4 — rilasciare un sottoinsieme di istanze non altera lo stato delle
// istanze non distrutte (Property 15 in forma di esempio).
// ---------------------------------------------------------------------------
TEST(PulseFieldReleaseTest, ReleaseSubsetLeavesOthersUntouched) {
    pulse::PulseField<int, "rel2/score", Actor> score;
    pulse::PulseField<int, "rel2/level", Actor> level;
    score.clearAll();
    level.clearAll();

    Actor a;
    Actor b;
    Actor c;

    score.set(&a, 1);
    level.set(&a, 10);
    score.set(&b, 2);
    level.set(&b, 20);
    score.set(&c, 3);
    level.set(&c, 30);

    // Distruggi `a` e `c`; `b` deve restare invariato.
    pulse::fields::releaseInstance<Actor>(&a);
    pulse::fields::releaseInstance<Actor>(&c);

    EXPECT_FALSE(score.has(&a));
    EXPECT_FALSE(level.has(&a));
    EXPECT_FALSE(score.has(&c));
    EXPECT_FALSE(level.has(&c));

    EXPECT_TRUE(score.has(&b));
    EXPECT_EQ(score.get(&b), 2);
    EXPECT_EQ(level.get(&b), 20);
}

// ---------------------------------------------------------------------------
// Req 6.4 — releaseInstance su un'istanza senza stato iniettato è un no-op
// sicuro (restituisce 0); l'hook del distruttore può chiamarlo incondizionato.
// ---------------------------------------------------------------------------
TEST(PulseFieldReleaseTest, ReleaseInstanceWithNoStateIsSafeNoOp) {
    pulse::PulseField<int, "rel3/value", Actor> value;
    value.clearAll();

    Actor a;
    Actor b;
    value.set(&b, 7);  // solo b ha stato

    const std::size_t released = pulse::fields::releaseInstance<Actor>(&a);
    EXPECT_EQ(released, 0u);  // a non aveva stato → nessun campo rilasciato

    // b resta intatto.
    EXPECT_EQ(value.get(&b), 7);
}

// ---------------------------------------------------------------------------
// Req 6.4 — il cleanup è per-classe: releaseInstance<Actor> non tocca lo stato
// dei campi di una classe diversa (Widget), anche se l'indirizzo coincide.
// ---------------------------------------------------------------------------
TEST(PulseFieldReleaseTest, ReleaseIsScopedToTheClassRegistry) {
    pulse::PulseField<int, "rel4/a_field", Actor> actorField;
    pulse::PulseField<int, "rel4/w_field", Widget> widgetField;
    actorField.clearAll();
    widgetField.clearAll();

    Actor actor;
    Widget widget;

    actorField.set(&actor, 11);
    widgetField.set(&widget, 22);

    // Rilascia l'istanza Actor: solo i campi registrati per Actor sono toccati.
    pulse::fields::releaseInstance<Actor>(&actor);

    EXPECT_FALSE(actorField.has(&actor));
    EXPECT_TRUE(widgetField.has(&widget));
    EXPECT_EQ(widgetField.get(&widget), 22);
}

// ---------------------------------------------------------------------------
// Req 6.4 — il registro per classe accumula un cleaner per campo distinto;
// più oggetti PulseField della stessa istanziazione NON duplicano il cleaner.
// ---------------------------------------------------------------------------
TEST(PulseFieldReleaseTest, OneCleanerPerDistinctFieldInstantiation) {
    struct Scoped {};
    using Registry = pulse::fields::detail::FieldRegistry<Scoped>;
    Registry::reset();

    EXPECT_EQ(Registry::cleanerCount(), 0u);

    pulse::PulseField<int, "scoped/one", Scoped> one;
    pulse::PulseField<int, "scoped/two", Scoped> two;
    EXPECT_EQ(Registry::cleanerCount(), 2u);

    // Costruire un altro oggetto della STESSA istanziazione non aggiunge un
    // secondo cleaner (registrazione idempotente per tipo).
    pulse::PulseField<int, "scoped/one", Scoped> oneAgain;
    EXPECT_EQ(Registry::cleanerCount(), 2u);

    // releaseInstance invoca ciascun cleaner una sola volta.
    Scoped s;
    one.set(&s, 1);
    two.set(&s, 2);
    EXPECT_EQ(pulse::fields::releaseInstance<Scoped>(&s), 2u);
    EXPECT_FALSE(one.has(&s));
    EXPECT_FALSE(two.has(&s));
}

}  // namespace
