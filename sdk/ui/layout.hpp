// sdk/ui/layout.hpp — sistema di layout dell'interfaccia (Layer 5,
// Requisito 8). Implementazione header-only di `ILayout` e di un layout
// concreto (RowLayout) con disposizione di fallback, esposta ai Developer
// tramite l'header pubblico <pulse/ui.hpp> (che promuove i tipi nel namespace
// `pulse`).
//
// Semantiche osservabili implementate qui:
//
//   * apply(container) — riposiziona i nodi figli del contenitore secondo le
//       regole dichiarate dal layout entro un budget di 16 ms (Req 8.4). Il
//       budget è modellato in modo testabile sull'host tramite un orologio
//       iniettabile e il tempo trascorso è riportato nel `LayoutResult`.
//   * Fallback (Req 8.5) — se le regole del layout sono in CONFLITTO fra loro
//       o NON VALIDE, apply() applica una disposizione di fallback che
//       mantiene TUTTI i figli all'interno dei limiti del contenitore
//       (clamp di posizione e dimensione) e SEGNALA il contenitore interessato
//       tramite un sink di errore e l'esito (`LayoutResult`).
//
// `Node` è un tipo del gioco non disponibile sull'host: come per il
// NodeRegistry è modellato come tipo OPACO forward-declared (vedi
// sdk/ui/node_id.hpp). Per restare host-testabile il layout non dereferenzia
// mai `Node*`: la geometria del contenitore e dei figli è raggiunta tramite un
// adattatore iniettabile `INodeGeometry`, così il test può osservare le
// posizioni risultanti SENZA un vero Node di gioco. Nel gioco reale si fornisce
// un `INodeGeometry` che incapsula l'accesso al vero albero dei nodi.
//
// Stack: C++20/23 (Requisito 26.1). Header-only, nessuna dipendenza esterna.
#ifndef PULSE_UI_LAYOUT_HPP
#define PULSE_UI_LAYOUT_HPP

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include "node_id.hpp"  // pulse::ui::Node (tipo opaco), pulse::ui::ModId

namespace pulse::ui {

// ---------------------------------------------------------------------------
// Geometria di base.
// ---------------------------------------------------------------------------

// Rettangolo di delimitazione di un nodo: origine (x, y) in alto-a-sinistra e
// dimensioni (w, h) non negative. Tipo valore semplice, indipendente dal gioco.
struct Rect {
    double x{0.0};
    double y{0.0};
    double w{0.0};
    double h{0.0};

    // Bordi derivati.
    [[nodiscard]] double right() const noexcept { return x + w; }
    [[nodiscard]] double bottom() const noexcept { return y + h; }

    // True se `inner` è interamente contenuto in `*this` (limiti inclusi),
    // tenendo conto di un piccolo margine numerico.
    [[nodiscard]] bool contains(const Rect& inner) const noexcept {
        constexpr double eps = 1e-9;
        return inner.x >= x - eps && inner.y >= y - eps &&
               inner.right() <= right() + eps &&
               inner.bottom() <= bottom() + eps;
    }
};

// ---------------------------------------------------------------------------
// Adattatore di geometria dei nodi (host-testabile).
//
// Astrae l'accesso ai rettangoli del contenitore e dei suoi figli. Il layout
// opera SOLO attraverso questa interfaccia: non dereferenzia mai `Node*`, così
// il codice compila ed è testabile sull'host con una geometria fittizia.
// ---------------------------------------------------------------------------
class INodeGeometry {
public:
    virtual ~INodeGeometry() = default;

    // Limiti del contenitore entro cui disporre i figli.
    [[nodiscard]] virtual Rect containerBounds() const = 0;

    // Numero di nodi figli da disporre.
    [[nodiscard]] virtual std::size_t childCount() const = 0;

    // Rettangolo corrente del figlio di indice `index` (< childCount()).
    [[nodiscard]] virtual Rect childRect(std::size_t index) const = 0;

    // Riposiziona/ridimensiona il figlio di indice `index`.
    virtual void setChildRect(std::size_t index, const Rect& rect) = 0;
};

// ---------------------------------------------------------------------------
// Sink di segnalazione del fallback (Req 8.5).
//
// Riceve la notifica quando un layout ricade sul fallback, identificando il
// contenitore interessato. È opzionale: se assente, l'esito resta comunque
// disponibile nel `LayoutResult`.
// ---------------------------------------------------------------------------
class ILayoutSink {
public:
    virtual ~ILayoutSink() = default;

    // Invocato quando le regole sono in conflitto/non valide e si applica il
    // fallback: identifica il `container` interessato e fornisce un messaggio.
    virtual void onFallback(Node* container, std::string_view message) = 0;
};

// ---------------------------------------------------------------------------
// Esito di apply().
// ---------------------------------------------------------------------------

enum class LayoutStatus {
    // Regole valide e coerenti: i figli sono stati disposti normalmente (Req 8.4).
    Applied,
    // Regole in conflitto/non valide: applicata la disposizione di fallback
    // che mantiene i figli nei limiti del contenitore (Req 8.5).
    Fallback,
};

// Budget di disposizione: 16 millisecondi (Req 8.4).
inline constexpr double kLayoutBudgetMs = 16.0;

struct LayoutResult {
    LayoutStatus status{LayoutStatus::Applied};

    // Tempo trascorso nella disposizione, in millisecondi (clock iniettabile).
    double elapsedMs{0.0};

    // True se la disposizione è rientrata nel budget di 16 ms (Req 8.4).
    bool withinBudget{true};

    // Contenitore segnalato in caso di fallback (Req 8.5), altrimenti nullptr.
    Node* signaledContainer{nullptr};

    // Messaggio diagnostico (vuoto in assenza di fallback).
    std::string message;

    // True se la disposizione normale è stata applicata senza fallback.
    [[nodiscard]] bool applied() const noexcept {
        return status == LayoutStatus::Applied;
    }

    // True se è stata applicata la disposizione di fallback (Req 8.5).
    [[nodiscard]] bool fellBack() const noexcept {
        return status == LayoutStatus::Fallback;
    }
};

// Orologio iniettabile per misurare il budget in modo deterministico sui test.
// Restituisce un istante monotòno; di default usa std::chrono::steady_clock.
using LayoutClock = std::function<std::chrono::steady_clock::time_point()>;

// ---------------------------------------------------------------------------
// ILayout — interfaccia di un algoritmo di layout (Req 8.4, 8.5).
//
// Conforme alla firma di design `apply(Node* container)`. Poiché `Node` è
// opaco e la geometria reale non è disponibile sull'host, l'oggetto layout è
// costruito con un `INodeGeometry` iniettato (host-testabile) e, opzionalmente,
// un `ILayoutSink` e un orologio. `apply(container)` usa `container` solo come
// identità opaca da segnalare/registrare nell'esito.
// ---------------------------------------------------------------------------
class ILayout {
public:
    virtual ~ILayout() = default;

    // Riposiziona i figli del contenitore entro 16 ms (Req 8.4). Su regole in
    // conflitto/non valide applica il fallback che mantiene i figli nei limiti
    // e segnala il contenitore (Req 8.5). L'esito dettagliato è esposto da
    // lastResult().
    virtual void apply(Node* container) = 0;

    // Esito dell'ultima chiamata ad apply() (tempo trascorso, fallback, ecc.).
    [[nodiscard]] const LayoutResult& lastResult() const noexcept {
        return lastResult_;
    }

protected:
    LayoutResult lastResult_{};
};

// ---------------------------------------------------------------------------
// LayoutBase — scheletro comune ai layout concreti.
//
// Gestisce: misura del budget tramite orologio iniettabile, distinzione fra
// regole valide/coerenti e regole in conflitto/non valide, applicazione del
// fallback (clamp di tutti i figli nei limiti del contenitore) e segnalazione
// del contenitore. Le sottoclassi forniscono solo le due routine di calcolo.
// ---------------------------------------------------------------------------
class LayoutBase : public ILayout {
public:
    explicit LayoutBase(INodeGeometry& geometry, ILayoutSink* sink = nullptr,
                        LayoutClock clock = {})
        : geometry_(geometry), sink_(sink), clock_(std::move(clock)) {}

    void apply(Node* container) final {
        const auto start = now();

        LayoutResult result{};
        if (rulesValid()) {
            // Regole valide e coerenti: disposizione normale (Req 8.4).
            applyNormal();
            result.status = LayoutStatus::Applied;
        } else {
            // Regole in conflitto/non valide: fallback che mantiene i figli nei
            // limiti del contenitore + segnalazione del contenitore (Req 8.5).
            applyFallback();
            result.status = LayoutStatus::Fallback;
            result.signaledContainer = container;
            result.message =
                "layout in conflitto/non valido: applicato fallback che "
                "mantiene i figli nei limiti del contenitore";
            if (sink_ != nullptr) {
                sink_->onFallback(container, result.message);
            }
        }

        const auto end = now();
        result.elapsedMs =
            std::chrono::duration<double, std::milli>(end - start).count();
        result.withinBudget = result.elapsedMs <= kLayoutBudgetMs;

        lastResult_ = std::move(result);
    }

protected:
    // True se le regole di questo layout sono valide e mutuamente coerenti, in
    // modo da poter applicare la disposizione normale (Req 8.4). False se sono
    // in conflitto o non valide e occorre il fallback (Req 8.5).
    [[nodiscard]] virtual bool rulesValid() const = 0;

    // Disposizione normale dei figli secondo le regole del layout (Req 8.4).
    virtual void applyNormal() = 0;

    // Disposizione di fallback: porta OGNI figlio entro i limiti del
    // contenitore, riducendone le dimensioni se più grande del contenitore e
    // riposizionandolo (clamp) per non sforare i bordi (Req 8.5).
    void applyFallback() {
        const Rect bounds = geometry_.containerBounds();
        const std::size_t n = geometry_.childCount();
        for (std::size_t i = 0; i < n; ++i) {
            geometry_.setChildRect(i, clampIntoBounds(geometry_.childRect(i), bounds));
        }
    }

    // Riduce/riposiziona `child` perché sia interamente contenuto in `bounds`.
    [[nodiscard]] static Rect clampIntoBounds(Rect child, const Rect& bounds) {
        // Dimensioni: non più grandi del contenitore (e non negative).
        child.w = std::clamp(child.w, 0.0, bounds.w);
        child.h = std::clamp(child.h, 0.0, bounds.h);
        // Posizione: l'origine resta entro [bounds, bounds_max - size].
        child.x = std::clamp(child.x, bounds.x, bounds.right() - child.w);
        child.y = std::clamp(child.y, bounds.y, bounds.bottom() - child.h);
        return child;
    }

    [[nodiscard]] INodeGeometry& geometry() const noexcept { return geometry_; }

    INodeGeometry& geometry_;
    ILayoutSink* sink_{nullptr};
    LayoutClock clock_;

private:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const {
        return clock_ ? clock_() : std::chrono::steady_clock::now();
    }
};

// ---------------------------------------------------------------------------
// RowLayout — layout concreto a riga orizzontale (Req 8.4, 8.5).
//
// Dispone i figli da sinistra a destra a partire dall'angolo del contenitore
// (più un `padding`), separati da `spacing`, mantenendo le dimensioni di
// ciascun figlio. Regole non valide o in conflitto:
//   * NON VALIDE: padding o spacing negativi.
//   * IN CONFLITTO: i figli, alle loro dimensioni, non entrano nei limiti del
//       contenitore (la riga sfora a destra, oppure un figlio è più alto/largo
//       del contenitore). In questi casi si applica il fallback (Req 8.5).
// ---------------------------------------------------------------------------
class RowLayout : public LayoutBase {
public:
    RowLayout(INodeGeometry& geometry, double spacing = 0.0,
              double padding = 0.0, ILayoutSink* sink = nullptr,
              LayoutClock clock = {})
        : LayoutBase(geometry, sink, std::move(clock)),
          spacing_(spacing),
          padding_(padding) {}

protected:
    [[nodiscard]] bool rulesValid() const override {
        // Regole non valide: parametri negativi.
        if (padding_ < 0.0 || spacing_ < 0.0) {
            return false;
        }

        const Rect bounds = geometry_.containerBounds();
        const std::size_t n = geometry_.childCount();
        const double innerW = bounds.w - 2.0 * padding_;
        const double innerH = bounds.h - 2.0 * padding_;
        if (innerW < 0.0 || innerH < 0.0) {
            return false;  // il padding stesso eccede il contenitore.
        }

        // Conflitto: somma delle larghezze + spaziature oltre lo spazio utile,
        // oppure un figlio più alto dello spazio utile in altezza.
        double used = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const Rect c = geometry_.childRect(i);
            if (c.h > innerH + 1e-9 || c.w > innerW + 1e-9) {
                return false;
            }
            used += c.w;
            if (i + 1 < n) {
                used += spacing_;
            }
        }
        return used <= innerW + 1e-9;
    }

    void applyNormal() override {
        const Rect bounds = geometry_.containerBounds();
        const std::size_t n = geometry_.childCount();
        double cursorX = bounds.x + padding_;
        const double rowY = bounds.y + padding_;
        for (std::size_t i = 0; i < n; ++i) {
            Rect c = geometry_.childRect(i);
            c.x = cursorX;
            c.y = rowY;
            geometry_.setChildRect(i, c);
            cursorX += c.w + spacing_;
        }
    }

private:
    double spacing_{0.0};
    double padding_{0.0};
};

}  // namespace pulse::ui

#endif  // PULSE_UI_LAYOUT_HPP
