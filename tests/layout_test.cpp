// layout_test.cpp — unit test di pulse::ILayout / pulse::RowLayout (task 28.2).
//
// Copre le semantiche osservabili del Requisito 8 (UI / layout):
//   * apply() dispone i figli secondo le regole del layout entro il budget di
//     16 ms (Req 8.4): RowLayout li allinea da sinistra a destra mantenendo le
//     dimensioni; il tempo trascorso (clock iniettabile) è riportato nell'esito;
//   * su regole in conflitto/non valide apply() applica un fallback che
//     mantiene TUTTI i figli entro i limiti del contenitore (Req 8.5);
//   * il contenitore interessato è SEGNALATO (sink + esito) in caso di
//     fallback (Req 8.5).
//
// `Node` è un tipo opaco del gioco: il layout non lo dereferenzia mai. La
// geometria del contenitore e dei figli è fornita da un adattatore in-memory
// (FakeGeometry) così il test può osservare le posizioni risultanti senza un
// vero Node di gioco; come identità del contenitore si usa un indirizzo fittizio.
#include <pulse/ui.hpp>

#include <chrono>
#include <vector>

#include <gtest/gtest.h>

using pulse::ILayoutSink;
using pulse::INodeGeometry;
using pulse::kLayoutBudgetMs;
using pulse::LayoutStatus;
using pulse::Node;
using pulse::Rect;
using pulse::RowLayout;

namespace {

// Adattatore di geometria in-memory: tiene i rettangoli di contenitore e figli
// in semplici vettori, così il test può impostarli e osservarli dopo apply().
class FakeGeometry : public INodeGeometry {
public:
    FakeGeometry(Rect bounds, std::vector<Rect> children)
        : bounds_(bounds), children_(std::move(children)) {}

    Rect containerBounds() const override { return bounds_; }
    std::size_t childCount() const override { return children_.size(); }
    Rect childRect(std::size_t i) const override { return children_.at(i); }
    void setChildRect(std::size_t i, const Rect& r) override { children_.at(i) = r; }

    const std::vector<Rect>& children() const { return children_; }

private:
    Rect bounds_;
    std::vector<Rect> children_;
};

// Sink che registra le segnalazioni di fallback (Req 8.5).
class RecordingSink : public ILayoutSink {
public:
    void onFallback(Node* container, std::string_view message) override {
        ++calls;
        lastContainer = container;
        lastMessage = std::string(message);
    }

    int calls{0};
    Node* lastContainer{nullptr};
    std::string lastMessage;
};

// Identità opaca del contenitore: indirizzo fittizio, mai dereferenziato.
Node* fakeContainer() {
    static unsigned char storage{0};
    return reinterpret_cast<Node*>(&storage);
}

// Orologio iniettabile che avanza di un passo fisso ad ogni lettura, così il
// budget è deterministico nei test.
class FakeClock {
public:
    explicit FakeClock(std::chrono::milliseconds step) : step_(step) {}
    std::chrono::steady_clock::time_point operator()() {
        auto t = now_;
        now_ += step_;
        return t;
    }

private:
    std::chrono::steady_clock::time_point now_{};
    std::chrono::milliseconds step_;
};

}  // namespace

// Req 8.4: apply() dispone i figli in riga mantenendo le dimensioni e parte
// dall'angolo del contenitore (con padding), separandoli per `spacing`.
TEST(LayoutTest, NormalApplyRepositionsChildren) {
    FakeGeometry geom{Rect{0, 0, 100, 50},
                      {Rect{99, 99, 10, 10}, Rect{42, 7, 20, 10}}};
    RowLayout layout{geom, /*spacing*/ 5.0, /*padding*/ 0.0};

    layout.apply(fakeContainer());

    const auto& kids = geom.children();
    // Primo figlio all'origine, secondo dopo larghezza+spacing.
    EXPECT_DOUBLE_EQ(kids[0].x, 0.0);
    EXPECT_DOUBLE_EQ(kids[0].y, 0.0);
    EXPECT_DOUBLE_EQ(kids[1].x, 15.0);  // 10 (w0) + 5 (spacing)
    EXPECT_DOUBLE_EQ(kids[1].y, 0.0);
    // Dimensioni invariate.
    EXPECT_DOUBLE_EQ(kids[0].w, 10.0);
    EXPECT_DOUBLE_EQ(kids[1].w, 20.0);

    // Disposizione normale, nessun fallback, nessuna segnalazione.
    EXPECT_EQ(layout.lastResult().status, LayoutStatus::Applied);
    EXPECT_TRUE(layout.lastResult().applied());
    EXPECT_EQ(layout.lastResult().signaledContainer, nullptr);
}

// Req 8.4: il budget di 16 ms è modellato in modo testabile; una disposizione
// rapida (passo 1 ms) rientra nel budget, una lenta (passo 32 ms) no.
TEST(LayoutTest, BudgetMeasuredWithInjectedClock) {
    FakeGeometry fast{Rect{0, 0, 100, 50}, {Rect{0, 0, 10, 10}}};
    FakeClock fastClock{std::chrono::milliseconds(1)};
    RowLayout fastLayout{fast, 0.0, 0.0, nullptr, std::ref(fastClock)};
    fastLayout.apply(fakeContainer());
    EXPECT_LE(fastLayout.lastResult().elapsedMs, kLayoutBudgetMs);
    EXPECT_TRUE(fastLayout.lastResult().withinBudget);

    FakeGeometry slow{Rect{0, 0, 100, 50}, {Rect{0, 0, 10, 10}}};
    FakeClock slowClock{std::chrono::milliseconds(32)};
    RowLayout slowLayout{slow, 0.0, 0.0, nullptr, std::ref(slowClock)};
    slowLayout.apply(fakeContainer());
    EXPECT_GT(slowLayout.lastResult().elapsedMs, kLayoutBudgetMs);
    EXPECT_FALSE(slowLayout.lastResult().withinBudget);
}

// Req 8.5: regole NON VALIDE (spacing negativo) -> fallback che mantiene i
// figli nei limiti del contenitore + segnalazione del contenitore.
TEST(LayoutTest, InvalidRulesTriggerClampingFallbackAndSignal) {
    FakeGeometry geom{Rect{0, 0, 100, 50},
                      {Rect{200, 200, 10, 10}, Rect{-30, -30, 10, 10}}};
    RecordingSink sink;
    RowLayout layout{geom, /*spacing*/ -1.0, /*padding*/ 0.0, &sink};

    layout.apply(fakeContainer());

    // Fallback applicato.
    EXPECT_EQ(layout.lastResult().status, LayoutStatus::Fallback);
    EXPECT_TRUE(layout.lastResult().fellBack());

    // Ogni figlio è ora interamente contenuto nei limiti del contenitore.
    const Rect bounds = geom.containerBounds();
    for (const Rect& c : geom.children()) {
        EXPECT_TRUE(bounds.contains(c)) << "figlio fuori dai limiti dopo fallback";
    }

    // Il contenitore è stato segnalato (sink + esito).
    EXPECT_EQ(sink.calls, 1);
    EXPECT_EQ(sink.lastContainer, fakeContainer());
    EXPECT_FALSE(sink.lastMessage.empty());
    EXPECT_EQ(layout.lastResult().signaledContainer, fakeContainer());
    EXPECT_FALSE(layout.lastResult().message.empty());
}

// Req 8.5: regole IN CONFLITTO (i figli non entrano nel contenitore) ->
// fallback che riduce/riposiziona ogni figlio dentro i limiti + segnalazione.
TEST(LayoutTest, ConflictingRulesClampOversizedChildrenAndSignal) {
    // Due figli larghi 80 ciascuno non entrano in un contenitore largo 100.
    FakeGeometry geom{Rect{10, 10, 100, 40},
                      {Rect{0, 0, 80, 30}, Rect{0, 0, 80, 60}}};
    RecordingSink sink;
    RowLayout layout{geom, /*spacing*/ 0.0, /*padding*/ 0.0, &sink};

    layout.apply(fakeContainer());

    EXPECT_TRUE(layout.lastResult().fellBack());

    const Rect bounds = geom.containerBounds();
    for (const Rect& c : geom.children()) {
        EXPECT_TRUE(bounds.contains(c)) << "figlio fuori dai limiti dopo fallback";
        // Dimensioni ridotte entro il contenitore.
        EXPECT_LE(c.w, bounds.w + 1e-9);
        EXPECT_LE(c.h, bounds.h + 1e-9);
    }

    EXPECT_EQ(sink.calls, 1);
    EXPECT_EQ(sink.lastContainer, fakeContainer());
}

// Verifica supplementare: senza sink la segnalazione resta comunque
// disponibile nell'esito (Req 8.5).
TEST(LayoutTest, FallbackSignalAvailableInResultWithoutSink) {
    FakeGeometry geom{Rect{0, 0, 10, 10}, {Rect{0, 0, 999, 999}}};
    RowLayout layout{geom, 0.0, 0.0, /*sink*/ nullptr};

    layout.apply(fakeContainer());

    EXPECT_TRUE(layout.lastResult().fellBack());
    EXPECT_EQ(layout.lastResult().signaledContainer, fakeContainer());
    EXPECT_TRUE(geom.containerBounds().contains(geom.children().front()));
}
