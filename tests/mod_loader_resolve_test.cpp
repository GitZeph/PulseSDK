// tests/mod_loader_resolve_test.cpp — Unit test del cablaggio del
// DependencyResolver nel Mod_Loader (task 7.1, Requisiti 4.1–4.7).
//
// Verifica `resolve_load_plan`: proietta i Mod_Manifest compatibili in
// `ResolverManifest` via `toResolverManifest()`, invoca il `DependencyResolver`
// e produce `ResolvedLoadPlan{order, excluded}`:
//   * carica SOLO le mod in `order`, nell'ordine topologico (dipendenze prima)
//     (Req 4.1, 4.2);
//   * traduce ogni mod esclusa (dipendenza mancante/incompatibile, ciclo,
//     esclusione transitiva) in una `DiagnosticEntry` con la `CauseCategory`
//     corretta dell'insieme chiuso e un messaggio con la causa del resolver
//     (Req 4.3/4.4/4.5/4.6);
//   * `order` vuoto → zero mod + esito registrato (Req 4.7).
//
// Header del loader in loader/ (include relativo alla radice loader/).

#include "lifecycle/mod_loader.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lifecycle/dependency_resolver.hpp"
#include "lifecycle/manifest.hpp"

namespace {

using pulse::lifecycle::CauseCategory;
using pulse::lifecycle::DiagnosticEntry;
using pulse::lifecycle::ModOutcome;
using pulse::lifecycle::ResolvedLoadPlan;
using pulse::lifecycle::resolve_load_plan;
using pulse::lifecycle::SemVer;
using pulse::lifecycle::VersionConstraint;
using pulse::manifest::Dependency;
using pulse::manifest::EntryPoint;
using pulse::manifest::Manifest;
using pulse::manifest::ModType;

// Costruisce un Manifest minimale valido con id/version/dipendenze.
Manifest makeManifest(const std::string& id, SemVer version,
                      std::vector<Dependency> deps = {}) {
    Manifest m;
    m.schemaVersion = 1;
    m.id = id;
    m.version = version;
    m.name = id;
    m.type = ModType::Native;
    m.entryPoints = {EntryPoint{"init", "mod_init"}};
    m.dependencies = std::move(deps);
    return m;
}

Dependency dep(const std::string& id,
               VersionConstraint c = VersionConstraint::any()) {
    return Dependency{id, c};
}

struct CapturingSink {
    std::vector<std::string> messages;
    pulse::loader::DiagnosticSink sink() {
        return [this](std::string_view m) { messages.emplace_back(m); };
    }
    bool anyContains(std::string_view needle) const {
        for (const std::string& m : messages)
            if (m.find(needle) != std::string::npos) return true;
        return false;
    }
};

// Indice di `mod` in `order`, o -1 se assente.
int indexOf(const std::vector<std::string>& order, const std::string& mod) {
    auto it = std::find(order.begin(), order.end(), mod);
    return it == order.end() ? -1 : static_cast<int>(it - order.begin());
}

const DiagnosticEntry* findExcluded(const ResolvedLoadPlan& plan,
                                    const std::string& mod) {
    for (const DiagnosticEntry& e : plan.excluded)
        if (e.modId == mod) return &e;
    return nullptr;
}

// --- Req 4.2: catena di dipendenze → ogni mod dopo le sue dipendenze ---------

TEST(ModLoaderResolve, LinearChainTopologicallyOrdered) {
    // c dipende da b, b dipende da a → ordine a, b, c.
    std::vector<Manifest> compatible = {
        makeManifest("c", SemVer{1, 0, 0}, {dep("b")}),
        makeManifest("a", SemVer{1, 0, 0}),
        makeManifest("b", SemVer{1, 0, 0}, {dep("a")}),
    };

    CapturingSink sink;
    ResolvedLoadPlan plan = resolve_load_plan(compatible, sink.sink());

    ASSERT_EQ(plan.order.size(), 3u);
    EXPECT_TRUE(plan.excluded.empty());
    // Ogni mod compare dopo tutte quelle da cui dipende (Req 4.2).
    EXPECT_LT(indexOf(plan.order, "a"), indexOf(plan.order, "b"));
    EXPECT_LT(indexOf(plan.order, "b"), indexOf(plan.order, "c"));
}

// --- Req 4.2: mod indipendenti → tutte caricate (ordine deterministico) ------

TEST(ModLoaderResolve, IndependentModsAllLoaded) {
    std::vector<Manifest> compatible = {
        makeManifest("gamma", SemVer{1, 0, 0}),
        makeManifest("alpha", SemVer{1, 0, 0}),
        makeManifest("beta", SemVer{1, 0, 0}),
    };

    CapturingSink sink;
    ResolvedLoadPlan plan = resolve_load_plan(compatible, sink.sink());

    ASSERT_EQ(plan.order.size(), 3u);
    EXPECT_TRUE(plan.excluded.empty());
    // Tie-break deterministico lessicografico del resolver.
    EXPECT_EQ(plan.order, (std::vector<std::string>{"alpha", "beta", "gamma"}));
}

// --- Req 4.3: dipendenza mancante → esclusione con DependencyUnsatisfied ------

TEST(ModLoaderResolve, MissingDependencyExcluded) {
    std::vector<Manifest> compatible = {
        makeManifest("standalone", SemVer{1, 0, 0}),
        makeManifest("needy", SemVer{1, 0, 0}, {dep("absent")}),
    };

    CapturingSink sink;
    ResolvedLoadPlan plan = resolve_load_plan(compatible, sink.sink());

    // La mod priva di dipendenza è caricata; quella con dipendenza mancante no.
    EXPECT_EQ(indexOf(plan.order, "standalone"), 0);
    EXPECT_EQ(indexOf(plan.order, "needy"), -1);

    const DiagnosticEntry* e = findExcluded(plan, "needy");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->outcome, ModOutcome::Excluded);
    ASSERT_TRUE(e->cause.has_value());
    EXPECT_EQ(*e->cause, CauseCategory::DependencyUnsatisfied);  // Req 4.3
    EXPECT_TRUE(e->message.find("absent") != std::string::npos);
    EXPECT_TRUE(sink.anyContains("needy"));
}

// --- Req 4.3: dipendenza di versione incompatibile → DependencyUnsatisfied ----

TEST(ModLoaderResolve, IncompatibleVersionDependencyExcluded) {
    // needy richiede lib >= 2.0.0 ma è installata lib 1.0.0.
    std::vector<Manifest> compatible = {
        makeManifest("lib", SemVer{1, 0, 0}),
        makeManifest("needy", SemVer{1, 0, 0},
                     {dep("lib", VersionConstraint::atLeast(SemVer{2, 0, 0}))}),
    };

    CapturingSink sink;
    ResolvedLoadPlan plan = resolve_load_plan(compatible, sink.sink());

    EXPECT_EQ(indexOf(plan.order, "lib"), 0);
    EXPECT_EQ(indexOf(plan.order, "needy"), -1);

    const DiagnosticEntry* e = findExcluded(plan, "needy");
    ASSERT_NE(e, nullptr);
    ASSERT_TRUE(e->cause.has_value());
    EXPECT_EQ(*e->cause, CauseCategory::DependencyUnsatisfied);  // Req 4.3
    // Il messaggio riporta la versione trovata (causa del resolver).
    EXPECT_TRUE(e->message.find("1.0.0") != std::string::npos);
}

// --- Req 4.4: ciclo di dipendenze → tutti i membri esclusi con DependencyCycle

TEST(ModLoaderResolve, CycleExcludesAllMembers) {
    // x <-> y ciclo; z indipendente è caricato.
    std::vector<Manifest> compatible = {
        makeManifest("x", SemVer{1, 0, 0}, {dep("y")}),
        makeManifest("y", SemVer{1, 0, 0}, {dep("x")}),
        makeManifest("z", SemVer{1, 0, 0}),
    };

    CapturingSink sink;
    ResolvedLoadPlan plan = resolve_load_plan(compatible, sink.sink());

    // z prosegue, x e y esclusi (Req 4.4, 4.6).
    EXPECT_EQ(indexOf(plan.order, "z"), 0);
    EXPECT_EQ(indexOf(plan.order, "x"), -1);
    EXPECT_EQ(indexOf(plan.order, "y"), -1);

    for (const std::string& m : {std::string("x"), std::string("y")}) {
        const DiagnosticEntry* e = findExcluded(plan, m);
        ASSERT_NE(e, nullptr) << "manca diagnostica per " << m;
        ASSERT_TRUE(e->cause.has_value());
        EXPECT_EQ(*e->cause, CauseCategory::DependencyCycle);  // Req 4.4
        // Il messaggio elenca i membri del ciclo (causa del resolver).
        EXPECT_TRUE(e->message.find("x") != std::string::npos);
        EXPECT_TRUE(e->message.find("y") != std::string::npos);
    }
}

// --- Req 4.5: dipendenza transitiva da mod esclusa → anch'essa esclusa --------

TEST(ModLoaderResolve, TransitiveExclusionPropagates) {
    // top → mid → absent(mancante). mid escluso (dep mancante), top escluso
    // perché dipende da mid escluso (Req 4.5). other prosegue.
    std::vector<Manifest> compatible = {
        makeManifest("top", SemVer{1, 0, 0}, {dep("mid")}),
        makeManifest("mid", SemVer{1, 0, 0}, {dep("absent")}),
        makeManifest("other", SemVer{1, 0, 0}),
    };

    CapturingSink sink;
    ResolvedLoadPlan plan = resolve_load_plan(compatible, sink.sink());

    EXPECT_EQ(indexOf(plan.order, "other"), 0);
    EXPECT_EQ(indexOf(plan.order, "top"), -1);
    EXPECT_EQ(indexOf(plan.order, "mid"), -1);

    const DiagnosticEntry* mid = findExcluded(plan, "mid");
    ASSERT_NE(mid, nullptr);
    EXPECT_EQ(*mid->cause, CauseCategory::DependencyUnsatisfied);  // Req 4.3

    const DiagnosticEntry* top = findExcluded(plan, "top");
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(*top->cause, CauseCategory::DependencyUnsatisfied);  // Req 4.5
    EXPECT_TRUE(top->message.find("mid") != std::string::npos);
}

// --- Req 4.7: order vuoto → zero mod + esito registrato -----------------------

TEST(ModLoaderResolve, EmptyInputYieldsEmptyOrderRecorded) {
    CapturingSink sink;
    ResolvedLoadPlan plan = resolve_load_plan({}, sink.sink());

    EXPECT_TRUE(plan.order.empty());
    EXPECT_TRUE(plan.excluded.empty());
    // L'esito "zero mod" è registrato (Req 4.7).
    EXPECT_TRUE(sink.anyContains("order vuoto") || sink.anyContains("zero mod"));
}

TEST(ModLoaderResolve, AllExcludedYieldsEmptyOrderRecorded) {
    // Unica mod con dipendenza mancante → order vuoto (Req 4.7) + esclusione.
    std::vector<Manifest> compatible = {
        makeManifest("lonely", SemVer{1, 0, 0}, {dep("ghost")}),
    };

    CapturingSink sink;
    ResolvedLoadPlan plan = resolve_load_plan(compatible, sink.sink());

    EXPECT_TRUE(plan.order.empty());
    ASSERT_EQ(plan.excluded.size(), 1u);
    EXPECT_EQ(plan.excluded[0].modId, "lonely");
    EXPECT_EQ(*plan.excluded[0].cause, CauseCategory::DependencyUnsatisfied);
    EXPECT_TRUE(sink.anyContains("order vuoto") || sink.anyContains("zero mod"));
}

// --- sink nullo: nessun crash, diagnostica resta nel piano --------------------

TEST(ModLoaderResolve, NullSinkKeepsDiagnosticsInPlan) {
    std::vector<Manifest> compatible = {
        makeManifest("needy", SemVer{1, 0, 0}, {dep("absent")}),
    };

    ResolvedLoadPlan plan = resolve_load_plan(compatible, nullptr);  // sink nullo
    EXPECT_TRUE(plan.order.empty());
    ASSERT_EQ(plan.excluded.size(), 1u);
    EXPECT_EQ(plan.excluded[0].modId, "needy");
}

// --- to_string / cause_category_for sanity ------------------------------------

TEST(ModLoaderResolve, ToStringCoversClosedSets) {
    using pulse::lifecycle::to_string;
    EXPECT_EQ(to_string(ModOutcome::Loaded), "caricata");
    EXPECT_EQ(to_string(ModOutcome::Excluded), "esclusa");
    EXPECT_EQ(to_string(ModOutcome::Isolated), "isolata");
    EXPECT_EQ(to_string(CauseCategory::DependencyCycle), "ciclo di dipendenze");
    EXPECT_EQ(to_string(CauseCategory::DependencyUnsatisfied),
              "dipendenza non soddisfatta");
}

}  // namespace
