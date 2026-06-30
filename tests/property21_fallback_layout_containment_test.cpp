// tests/property21_fallback_layout_containment_test.cpp
// Feature: pulse-sdk, Property 21 — Contenimento del layout di fallback.
// Validates: Requirements 8.5 (Requisiti 8.5)
//
// Property 21 (design.md / Req 8.5): per ogni contenitore con regole di layout
// in CONFLITTO o NON VALIDE, la disposizione di fallback mantiene TUTTI i nodi
// figli entro i limiti del contenitore (clamp di posizione e dimensione) e
// produce una segnalazione che identifica il contenitore (sink + esito).
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si generano limiti di contenitore casuali (origine e dimensioni non
//     negative) e un insieme randomizzato di figli con posizioni/dimensioni
//     arbitrarie, inclusi figli SOVRADIMENSIONATI (più grandi del contenitore)
//     e fuori dai bordi;
//   * si generano `spacing`/`padding` che possono essere NEGATIVI (regole non
//     valide) oppure non negativi (potenziale conflitto se i figli non entrano
//     in riga), così da esercitare sia i casi che forzano il fallback sia i
//     casi di disposizione normale;
//   * `Node` è un tipo opaco del gioco mai dereferenziato: la geometria è
//     fornita da un adattatore in-memory (FakeGeometry) e il contenitore è
//     un'identità fittizia (indirizzo stabile).
//
// Invarianti verificati:
//   (a) se le regole sono in conflitto/non valide, apply() applica il fallback
//       (status == Fallback) e OGNI figlio risulta interamente contenuto nei
//       limiti del contenitore (bounds.contains(child)), con dimensioni non
//       superiori a quelle del contenitore;
//   (b) in caso di fallback il contenitore è SEGNALATO: sia tramite il sink
//       (una chiamata con il container corretto e messaggio non vuoto) sia
//       tramite l'esito (signaledContainer == container, message non vuoto);
//   (c) se invece le regole sono valide, apply() NON ricade sul fallback
//       (status == Applied, nessuna segnalazione) e i figli, già entro i
//       limiti, restano contenuti dopo la disposizione normale.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include <pulse/ui.hpp>

namespace {

using pulse::ILayoutSink;
using pulse::INodeGeometry;
using pulse::LayoutStatus;
using pulse::Node;
using pulse::Rect;
using pulse::RowLayout;

// Adattatore di geometria in-memory: tiene i rettangoli di contenitore e figli
// in vettori, così il test li imposta e li osserva dopo apply(). Specchia il
// FakeGeometry dello unit test (tests/layout_test.cpp).
class FakeGeometry : public INodeGeometry {
public:
    FakeGeometry(Rect bounds, std::vector<Rect> children)
        : bounds_(bounds), children_(std::move(children)) {}

    Rect containerBounds() const override { return bounds_; }
    std::size_t childCount() const override { return children_.size(); }
    Rect childRect(std::size_t i) const override { return children_.at(i); }
    void setChildRect(std::size_t i, const Rect& r) override {
        children_.at(i) = r;
    }

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

// Generatore di un Rect figlio con coordinate/dimensioni arbitrarie ma
// limitate, per coprire figli interni, fuori dai bordi e sovradimensionati.
rc::Gen<Rect> genChild() {
    return rc::gen::apply(
        [](double x, double y, double w, double h) {
            return Rect{x, y, w, h};
        },
        rc::gen::inRange(-100, 400).as("child.x"),
        rc::gen::inRange(-100, 400).as("child.y"),
        rc::gen::inRange(0, 400).as("child.w"),
        rc::gen::inRange(0, 400).as("child.h"));
}

}  // namespace

// --- Property 21 — contenimento del layout di fallback --------------------
// Feature: pulse-sdk, Property 21. Validates: Requirements 8.5.
RC_GTEST_PROP(Property21FallbackLayoutContainment,
              FallbackKeepsAllChildrenWithinBoundsAndSignalsContainer,
              ()) {
    // Limiti del contenitore: origine arbitraria, dimensioni non negative.
    const Rect bounds{
        static_cast<double>(*rc::gen::inRange(-50, 200).as("bounds.x")),
        static_cast<double>(*rc::gen::inRange(-50, 200).as("bounds.y")),
        static_cast<double>(*rc::gen::inRange(0, 200).as("bounds.w")),
        static_cast<double>(*rc::gen::inRange(0, 200).as("bounds.h"))};

    // Insieme randomizzato di figli (anche vuoto), con sovradimensionati e
    // fuori dai bordi grazie agli intervalli di genChild().
    const auto children =
        *rc::gen::container<std::vector<Rect>>(genChild()).as("figli");

    // Regole che possono essere NON VALIDE (negative) o valide (>= 0), così da
    // esercitare sia il fallback (negative / conflitto) sia la via normale.
    const double spacing =
        static_cast<double>(*rc::gen::inRange(-10, 50).as("spacing"));
    const double padding =
        static_cast<double>(*rc::gen::inRange(-10, 50).as("padding"));

    FakeGeometry geom{bounds, children};
    RecordingSink sink;
    RowLayout layout{geom, spacing, padding, &sink};

    layout.apply(fakeContainer());

    const auto& result = layout.lastResult();

    if (result.status == LayoutStatus::Fallback) {
        // (a) Ogni figlio è interamente contenuto nei limiti del contenitore,
        // con dimensioni clampate entro il contenitore.
        for (const Rect& c : geom.children()) {
            RC_ASSERT(bounds.contains(c));
            RC_ASSERT(c.w <= bounds.w + 1e-9);
            RC_ASSERT(c.h <= bounds.h + 1e-9);
        }

        // (b) Il contenitore è segnalato sia via sink sia via esito.
        RC_ASSERT(result.fellBack());
        RC_ASSERT(result.signaledContainer == fakeContainer());
        RC_ASSERT(!result.message.empty());
        RC_ASSERT(sink.calls == 1);
        RC_ASSERT(sink.lastContainer == fakeContainer());
        RC_ASSERT(!sink.lastMessage.empty());
    } else {
        // (c) Disposizione normale: nessuna segnalazione e figli contenuti.
        RC_ASSERT(result.status == LayoutStatus::Applied);
        RC_ASSERT(result.applied());
        RC_ASSERT(result.signaledContainer == nullptr);
        RC_ASSERT(sink.calls == 0);
        for (const Rect& c : geom.children()) {
            RC_ASSERT(bounds.contains(c));
        }
    }
}
