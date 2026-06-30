// tests/dependency_resolver_test.cpp — unit test del DependencyResolver
// (task 11.1, Requisiti 4.1, 4.2, 4.3).
//
// Copre:
//   * correttezza dell'ordine topologico: ogni mod compare dopo le sue
//     dipendenze (Req 4.1) e l'ordine è deterministico;
//   * esclusione di mod con dipendenza mancante o di versione incompatibile,
//     con segnalazione della mod e della dipendenza (Req 4.2), e proseguimento
//     del caricamento delle restanti mod;
//   * rilevamento dei cicli con esclusione e identificazione di tutti i membri
//     del ciclo (Req 4.3), distinguendo i membri del ciclo dalle mod che
//     dipendono dal ciclo.
#include "lifecycle/dependency_resolver.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace {

using pulse::lifecycle::Dependency;
using pulse::lifecycle::DependencyResolver;
using pulse::lifecycle::Exclusion;
using pulse::lifecycle::ExclusionReason;
using pulse::lifecycle::LoadPlan;
using pulse::lifecycle::ModId;
using pulse::lifecycle::ResolverManifest;
using pulse::lifecycle::SemVer;
using pulse::lifecycle::VersionConstraint;

// Helper: costruisce un manifest con id, versione e dipendenze (any-version).
ResolverManifest mod(const ModId& id, SemVer version,
                     std::vector<Dependency> deps = {}) {
    return ResolverManifest{id, version, std::move(deps)};
}

Dependency dep(const ModId& id, VersionConstraint c = VersionConstraint::any()) {
    return Dependency{id, c};
}

// Indice della prima occorrenza di `id` in `order`, o -1 se assente.
int posOf(const std::vector<ModId>& order, const ModId& id) {
    auto it = std::find(order.begin(), order.end(), id);
    return it == order.end() ? -1 : static_cast<int>(it - order.begin());
}

bool inOrder(const std::vector<ModId>& order, const ModId& id) {
    return posOf(order, id) >= 0;
}

const Exclusion* findExclusion(const LoadPlan& plan, const ModId& id) {
    for (const auto& e : plan.excluded) {
        if (e.mod == id) return &e;
    }
    return nullptr;
}

// -----------------------------------------------------------------------
// Req 4.1 — ordine topologico: ogni mod dopo le sue dipendenze.
// -----------------------------------------------------------------------
TEST(DependencyResolverTest, EmptyInputProducesEmptyPlan) {
    DependencyResolver resolver;
    LoadPlan plan = resolver.resolve({});
    EXPECT_TRUE(plan.order.empty());
    EXPECT_TRUE(plan.excluded.empty());
}

TEST(DependencyResolverTest, IndependentModsAllLoadedDeterministically) {
    DependencyResolver resolver;
    std::vector<ResolverManifest> mods = {
        mod("charlie", {1, 0, 0}),
        mod("alpha", {1, 0, 0}),
        mod("bravo", {1, 0, 0}),
    };
    LoadPlan plan = resolver.resolve(mods);

    ASSERT_EQ(plan.order.size(), 3u);
    EXPECT_TRUE(plan.excluded.empty());
    // Tie-break deterministico: ordine lessicografico tra nodi indipendenti.
    EXPECT_EQ(plan.order, (std::vector<ModId>{"alpha", "bravo", "charlie"}));
}

TEST(DependencyResolverTest, DependencyLoadedBeforeDependent) {
    DependencyResolver resolver;
    // app -> lib -> core
    std::vector<ResolverManifest> mods = {
        mod("app", {1, 0, 0}, {dep("lib")}),
        mod("lib", {1, 0, 0}, {dep("core")}),
        mod("core", {1, 0, 0}),
    };
    LoadPlan plan = resolver.resolve(mods);

    ASSERT_EQ(plan.order.size(), 3u);
    EXPECT_TRUE(plan.excluded.empty());
    EXPECT_LT(posOf(plan.order, "core"), posOf(plan.order, "lib"));
    EXPECT_LT(posOf(plan.order, "lib"), posOf(plan.order, "app"));
}

TEST(DependencyResolverTest, DiamondDependencyRespectsAllEdges) {
    DependencyResolver resolver;
    // top depends on left and right; both depend on base.
    std::vector<ResolverManifest> mods = {
        mod("top", {1, 0, 0}, {dep("left"), dep("right")}),
        mod("left", {1, 0, 0}, {dep("base")}),
        mod("right", {1, 0, 0}, {dep("base")}),
        mod("base", {1, 0, 0}),
    };
    LoadPlan plan = resolver.resolve(mods);

    ASSERT_EQ(plan.order.size(), 4u);
    EXPECT_TRUE(plan.excluded.empty());
    EXPECT_LT(posOf(plan.order, "base"), posOf(plan.order, "left"));
    EXPECT_LT(posOf(plan.order, "base"), posOf(plan.order, "right"));
    EXPECT_LT(posOf(plan.order, "left"), posOf(plan.order, "top"));
    EXPECT_LT(posOf(plan.order, "right"), posOf(plan.order, "top"));
}

TEST(DependencyResolverTest, OrderIsDeterministicAcrossInputPermutations) {
    DependencyResolver resolver;
    std::vector<ResolverManifest> a = {
        mod("app", {1, 0, 0}, {dep("lib")}),
        mod("lib", {1, 0, 0}, {dep("core")}),
        mod("core", {1, 0, 0}),
        mod("util", {1, 0, 0}, {dep("core")}),
    };
    std::vector<ResolverManifest> b = {
        mod("util", {1, 0, 0}, {dep("core")}),
        mod("core", {1, 0, 0}),
        mod("lib", {1, 0, 0}, {dep("core")}),
        mod("app", {1, 0, 0}, {dep("lib")}),
    };
    EXPECT_EQ(resolver.resolve(a).order, resolver.resolve(b).order);
}

// -----------------------------------------------------------------------
// Req 4.2 — esclusione per dipendenza mancante o incompatibile.
// -----------------------------------------------------------------------
TEST(DependencyResolverTest, MissingDependencyExcludesModAndReportsIt) {
    DependencyResolver resolver;
    std::vector<ResolverManifest> mods = {
        mod("app", {1, 0, 0}, {dep("ghost")}),  // ghost non installata
        mod("solo", {1, 0, 0}),
    };
    LoadPlan plan = resolver.resolve(mods);

    // La mod sana prosegue il caricamento (Req 4.2).
    EXPECT_TRUE(inOrder(plan.order, "solo"));
    EXPECT_FALSE(inOrder(plan.order, "app"));

    const Exclusion* ex = findExclusion(plan, "app");
    ASSERT_NE(ex, nullptr);
    EXPECT_EQ(ex->reason.kind, ExclusionReason::Kind::MissingDependency);
    EXPECT_EQ(ex->reason.dependency, "ghost");
}

TEST(DependencyResolverTest, IncompatibleVersionExcludesModAndReportsFound) {
    DependencyResolver resolver;
    // app richiede core >= 2.0.0, ma è installato core 1.5.0.
    std::vector<ResolverManifest> mods = {
        mod("app", {1, 0, 0}, {dep("core", VersionConstraint::atLeast({2, 0, 0}))}),
        mod("core", {1, 5, 0}),
    };
    LoadPlan plan = resolver.resolve(mods);

    EXPECT_TRUE(inOrder(plan.order, "core"));  // core stessa è caricabile
    EXPECT_FALSE(inOrder(plan.order, "app"));

    const Exclusion* ex = findExclusion(plan, "app");
    ASSERT_NE(ex, nullptr);
    EXPECT_EQ(ex->reason.kind, ExclusionReason::Kind::IncompatibleDependency);
    EXPECT_EQ(ex->reason.dependency, "core");
    ASSERT_TRUE(ex->reason.foundVersion.has_value());
    EXPECT_EQ(*ex->reason.foundVersion, (SemVer{1, 5, 0}));
}

TEST(DependencyResolverTest, CompatibleVersionWithinRangeIsAccepted) {
    DependencyResolver resolver;
    // app richiede core in [1.0.0, 2.0.0); installato core 1.5.0 → ok.
    std::vector<ResolverManifest> mods = {
        mod("app", {1, 0, 0},
            {dep("core", VersionConstraint::range({1, 0, 0}, {2, 0, 0}))}),
        mod("core", {1, 5, 0}),
    };
    LoadPlan plan = resolver.resolve(mods);

    EXPECT_TRUE(plan.excluded.empty());
    ASSERT_EQ(plan.order.size(), 2u);
    EXPECT_LT(posOf(plan.order, "core"), posOf(plan.order, "app"));
}

TEST(DependencyResolverTest, TransitiveExclusionPropagatesToDependents) {
    DependencyResolver resolver;
    // app -> lib -> ghost(mancante). Sia app sia lib sono escluse.
    std::vector<ResolverManifest> mods = {
        mod("app", {1, 0, 0}, {dep("lib")}),
        mod("lib", {1, 0, 0}, {dep("ghost")}),
        mod("safe", {1, 0, 0}),
    };
    LoadPlan plan = resolver.resolve(mods);

    EXPECT_TRUE(inOrder(plan.order, "safe"));
    EXPECT_FALSE(inOrder(plan.order, "app"));
    EXPECT_FALSE(inOrder(plan.order, "lib"));

    const Exclusion* libEx = findExclusion(plan, "lib");
    ASSERT_NE(libEx, nullptr);
    EXPECT_EQ(libEx->reason.kind, ExclusionReason::Kind::MissingDependency);
    EXPECT_EQ(libEx->reason.dependency, "ghost");

    const Exclusion* appEx = findExclusion(plan, "app");
    ASSERT_NE(appEx, nullptr);
    EXPECT_EQ(appEx->reason.kind, ExclusionReason::Kind::DependencyExcluded);
    EXPECT_EQ(appEx->reason.dependency, "lib");
}

// -----------------------------------------------------------------------
// Req 4.3 — rilevamento e isolamento dei cicli.
// -----------------------------------------------------------------------
TEST(DependencyResolverTest, SimpleCycleExcludesAllMembersAndReportsThem) {
    DependencyResolver resolver;
    // a -> b -> a (ciclo). plus una mod sana.
    std::vector<ResolverManifest> mods = {
        mod("a", {1, 0, 0}, {dep("b")}),
        mod("b", {1, 0, 0}, {dep("a")}),
        mod("free", {1, 0, 0}),
    };
    LoadPlan plan = resolver.resolve(mods);

    EXPECT_TRUE(inOrder(plan.order, "free"));
    EXPECT_FALSE(inOrder(plan.order, "a"));
    EXPECT_FALSE(inOrder(plan.order, "b"));

    const Exclusion* aEx = findExclusion(plan, "a");
    const Exclusion* bEx = findExclusion(plan, "b");
    ASSERT_NE(aEx, nullptr);
    ASSERT_NE(bEx, nullptr);
    EXPECT_EQ(aEx->reason.kind, ExclusionReason::Kind::DependencyCycle);
    EXPECT_EQ(bEx->reason.kind, ExclusionReason::Kind::DependencyCycle);
    // Entrambi riportano l'elenco completo dei membri del ciclo (Req 4.3).
    EXPECT_EQ(aEx->reason.cycle, (std::vector<ModId>{"a", "b"}));
    EXPECT_EQ(bEx->reason.cycle, (std::vector<ModId>{"a", "b"}));
}

TEST(DependencyResolverTest, ThreeNodeCycleReportsAllThreeMembers) {
    DependencyResolver resolver;
    // x -> y -> z -> x.
    std::vector<ResolverManifest> mods = {
        mod("x", {1, 0, 0}, {dep("y")}),
        mod("y", {1, 0, 0}, {dep("z")}),
        mod("z", {1, 0, 0}, {dep("x")}),
    };
    LoadPlan plan = resolver.resolve(mods);

    EXPECT_TRUE(plan.order.empty());
    ASSERT_EQ(plan.excluded.size(), 3u);
    for (const ModId& id : {"x", "y", "z"}) {
        const Exclusion* ex = findExclusion(plan, id);
        ASSERT_NE(ex, nullptr) << id;
        EXPECT_EQ(ex->reason.kind, ExclusionReason::Kind::DependencyCycle);
        EXPECT_EQ(ex->reason.cycle, (std::vector<ModId>{"x", "y", "z"}));
    }
}

TEST(DependencyResolverTest, SelfDependencyIsDetectedAsCycle) {
    DependencyResolver resolver;
    std::vector<ResolverManifest> mods = {
        mod("loner", {1, 0, 0}, {dep("loner")}),
    };
    LoadPlan plan = resolver.resolve(mods);

    EXPECT_TRUE(plan.order.empty());
    const Exclusion* ex = findExclusion(plan, "loner");
    ASSERT_NE(ex, nullptr);
    EXPECT_EQ(ex->reason.kind, ExclusionReason::Kind::DependencyCycle);
    EXPECT_EQ(ex->reason.cycle, (std::vector<ModId>{"loner"}));
}

TEST(DependencyResolverTest, DependentOnCycleIsExcludedButNotAsCycleMember) {
    DependencyResolver resolver;
    // a <-> b è il ciclo; c dipende da a ma non è membro del ciclo.
    std::vector<ResolverManifest> mods = {
        mod("a", {1, 0, 0}, {dep("b")}),
        mod("b", {1, 0, 0}, {dep("a")}),
        mod("c", {1, 0, 0}, {dep("a")}),
    };
    LoadPlan plan = resolver.resolve(mods);

    EXPECT_TRUE(plan.order.empty());

    const Exclusion* cEx = findExclusion(plan, "c");
    ASSERT_NE(cEx, nullptr);
    // c è escluso perché dipende da una mod esclusa, non come membro del ciclo.
    EXPECT_EQ(cEx->reason.kind, ExclusionReason::Kind::DependencyExcluded);
    EXPECT_EQ(cEx->reason.dependency, "a");

    const Exclusion* aEx = findExclusion(plan, "a");
    ASSERT_NE(aEx, nullptr);
    EXPECT_EQ(aEx->reason.kind, ExclusionReason::Kind::DependencyCycle);
    EXPECT_EQ(aEx->reason.cycle, (std::vector<ModId>{"a", "b"}));
}

TEST(DependencyResolverTest, ValidModsLoadAlongsideCycleAndMissingExclusions) {
    DependencyResolver resolver;
    // Scenario misto: una catena valida, un ciclo, una dipendenza mancante.
    std::vector<ResolverManifest> mods = {
        mod("core", {1, 0, 0}),
        mod("ui", {1, 0, 0}, {dep("core")}),
        mod("p", {1, 0, 0}, {dep("q")}),
        mod("q", {1, 0, 0}, {dep("p")}),
        mod("broken", {1, 0, 0}, {dep("absent")}),
    };
    LoadPlan plan = resolver.resolve(mods);

    // Catena valida caricata in ordine.
    EXPECT_LT(posOf(plan.order, "core"), posOf(plan.order, "ui"));
    EXPECT_FALSE(inOrder(plan.order, "p"));
    EXPECT_FALSE(inOrder(plan.order, "q"));
    EXPECT_FALSE(inOrder(plan.order, "broken"));

    EXPECT_EQ(findExclusion(plan, "p")->reason.kind,
              ExclusionReason::Kind::DependencyCycle);
    EXPECT_EQ(findExclusion(plan, "q")->reason.kind,
              ExclusionReason::Kind::DependencyCycle);
    EXPECT_EQ(findExclusion(plan, "broken")->reason.kind,
              ExclusionReason::Kind::MissingDependency);
}

}  // namespace
