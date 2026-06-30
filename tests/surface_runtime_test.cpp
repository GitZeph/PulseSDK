// tests/surface_runtime_test.cpp — unit test del runtime sottile della
// GD_API_Surface (task 13.1-13.4; Req 6, 7, 8, 9).
//
// Verifica, riusando `binding_verifier` e i provider esistenti (con provider
// finti per la policy d'ordine):
//   * 13.1 Surface_Runtime_Report: un `API_Element` è risolvibile SSE il binding
//     è risolto da `binding_verifier`; partial resolution (i risolti restano
//     installabili, gli irrisolti sono elencati senza interromperli).
//   * 13.2 Classificazione per coppia: stesso insieme di simboli su coppie
//     diverse, solo il flag `resolvable` cambia; nessuna cross-coppia.
//   * 13.3 SurfaceResolver embedded-first: precedenza embedded, fallback online,
//     stessa semantica `binding_verifier` fra provider (provider iniettati).
//   * 13.4 Gate di provenienza runtime: lettura per riferimento senza
//     ricomputazione; `verified = true` con provenienza incompleta → declassato
//     a non risolvibile + errore di auditabilità con simbolo+coppia.

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bindings/binding_verifier.hpp"
#include "bindings/bindings.hpp"
#include "surface/surface_provenance.hpp"
#include "surface/surface_report.hpp"
#include "surface/surface_resolver.hpp"

namespace {

using namespace pulse::loader::bindings;
using namespace pulse::loader::surface;

const BindingKey kMacPair{GdVersion{2, 2081}, "macos-arm64"};
const BindingKey kWinPair{GdVersion{2, 2074}, "windows-x64"};

constexpr std::uintptr_t kGoodOffset = 0x00316688;

FunctionBinding makeBinding(const std::string& symbol, std::uintptr_t addr,
                            bool claimedVerified) {
    FunctionBinding fn;
    fn.symbol = symbol;
    fn.address = addr;
    fn.signature = Signature{"bool", {"MenuLayer*"}};
    fn.resolved = claimedVerified;  // claim della verifica offline (.pbind verified)
    return fn;
}

// Provider finto in-memory: serve un singolo `BindingSet` per coppia esatta,
// onorando il contratto `IBindingsProvider` (load esatto + resolve esatto).
class FakeProvider final : public IBindingsProvider {
public:
    void addSet(BindingSet set) { sets_.push_back(std::move(set)); }

    std::optional<BindingSet> load(const BindingKey& key) override {
        for (const auto& set : sets_) {
            if (set.key() == key) {
                current_ = set;
                return set;
            }
        }
        current_.reset();
        return std::nullopt;
    }

    std::optional<FunctionBinding> resolve(std::string_view symbol) const override {
        if (!current_) {
            return std::nullopt;
        }
        return current_->resolve(symbol);
    }

private:
    std::vector<BindingSet> sets_;
    std::optional<BindingSet> current_;
};

bool listed(const std::vector<SurfaceElementStatus>& v, std::string_view symbol) {
    return std::any_of(v.begin(), v.end(),
                       [&](const SurfaceElementStatus& s) { return s.symbol == symbol; });
}

}  // namespace

// ===========================================================================
// 13.1 — Surface_Runtime_Report: risolvibile SSE verificato (Req 6.1, 8.4).
// ===========================================================================

TEST(SurfaceReport, ResolvableIffVerified) {
    BindingSet set{kMacPair};
    set.add(makeBinding("MenuLayer::init", kGoodOffset, /*verified=*/true));  // risolto
    set.add(makeBinding("PlayLayer::init", kGoodOffset, /*verified=*/false)); // non verificato
    set.add(makeBinding("A::zero", 0, /*verified=*/true));                    // offset 0
    set.add(makeBinding("B::sentinel", kPlaceholderSentinel, /*verified=*/true)); // sentinel

    const std::vector<std::string_view> symbols = {"MenuLayer::init", "PlayLayer::init",
                                                   "A::zero", "B::sentinel"};
    SurfaceRuntimeReport report = classify_surface(symbols, set);

    EXPECT_EQ(report.resolvableCount(), 1u);
    EXPECT_TRUE(listed(report.resolvable, "MenuLayer::init"));

    // Ogni voce non verificata/placeholder è non risolvibile (coerente con
    // address_is_verified).
    EXPECT_EQ(report.unresolvableCount(), 3u);
    EXPECT_TRUE(listed(report.unresolvable, "PlayLayer::init"));
    EXPECT_TRUE(listed(report.unresolvable, "A::zero"));
    EXPECT_TRUE(listed(report.unresolvable, "B::sentinel"));
}

TEST(SurfaceReport, SymbolAbsentFromPairIsUnresolvable) {
    BindingSet set{kMacPair};
    set.add(makeBinding("MenuLayer::init", kGoodOffset, true));

    // Simbolo della superficie senza binding nella coppia → non risolvibile.
    SurfaceRuntimeReport report =
        classify_surface({"MenuLayer::init", "GJBaseGameLayer::update"}, set);

    EXPECT_TRUE(listed(report.resolvable, "MenuLayer::init"));
    EXPECT_TRUE(listed(report.unresolvable, "GJBaseGameLayer::update"));
}

TEST(SurfaceReport, PartialResolutionDoesNotBlockResolved) {
    // Un mix di risolti e irrisolti: i risolti restano installabili, gli
    // irrisolti sono elencati separatamente senza impedire i risolti (Req 6.3, 7.4).
    BindingSet set{kMacPair};
    set.add(makeBinding("R1::ok", 0x1000, true));
    set.add(makeBinding("U1::no", 0, true));
    set.add(makeBinding("R2::ok", 0x2000, true));
    set.add(makeBinding("U2::no", kPlaceholderSentinel, true));

    SurfaceRuntimeReport report =
        classify_surface({"R1::ok", "U1::no", "R2::ok", "U2::no"}, set);

    EXPECT_EQ(report.resolvableCount(), 2u);
    EXPECT_TRUE(listed(report.resolvable, "R1::ok"));
    EXPECT_TRUE(listed(report.resolvable, "R2::ok"));
    EXPECT_EQ(report.unresolvableCount(), 2u);
}

TEST(SurfaceReport, ResolvableElementsCarryNonNullProvenance) {
    // provenance non-null SSE risolvibile (Req 8.1); unresolvable → nullptr.
    BindingSet set{kMacPair};
    set.add(makeBinding("MenuLayer::init", kGoodOffset, true));
    set.add(makeBinding("PlayLayer::init", 0, true));

    SurfaceRuntimeReport report = classify_surface({"MenuLayer::init", "PlayLayer::init"}, set);

    ASSERT_EQ(report.resolvableCount(), 1u);
    EXPECT_NE(report.resolvable[0].provenance, nullptr);
    EXPECT_TRUE(report.resolvable[0].provenance->complete);
    ASSERT_EQ(report.unresolvableCount(), 1u);
    EXPECT_EQ(report.unresolvable[0].provenance, nullptr);
}

// ===========================================================================
// 13.2 — Classificazione per coppia, nessuna cross-coppia (Req 7.1, 7.2, 7.3).
// ===========================================================================

TEST(SurfacePerPair, SameSymbolSetResolvabilityDiffersPerPair) {
    const std::vector<std::string_view> symbols = {"MenuLayer::init", "PlayLayer::init"};

    // Coppia A (mac): MenuLayer::init risolto, PlayLayer::init no.
    BindingSet macSet{kMacPair};
    macSet.add(makeBinding("MenuLayer::init", kGoodOffset, true));
    macSet.add(makeBinding("PlayLayer::init", 0, true));

    // Coppia B (win): inverso — stesso insieme di simboli, diversa risolvibilità.
    BindingSet winSet{kWinPair};
    winSet.add(makeBinding("MenuLayer::init", 0, true));
    winSet.add(makeBinding("PlayLayer::init", 0x4000, true));

    SurfaceRuntimeReport macReport = classify_surface(symbols, macSet);
    SurfaceRuntimeReport winReport = classify_surface(symbols, winSet);

    // Stesso insieme di simboli esposto su entrambe le coppie (Req 7.1).
    EXPECT_EQ(macReport.resolvableCount() + macReport.unresolvableCount(), symbols.size());
    EXPECT_EQ(winReport.resolvableCount() + winReport.unresolvableCount(), symbols.size());

    // Solo il flag resolvable differisce per coppia (Req 7.3).
    EXPECT_TRUE(listed(macReport.resolvable, "MenuLayer::init"));
    EXPECT_TRUE(listed(macReport.unresolvable, "PlayLayer::init"));
    EXPECT_TRUE(listed(winReport.unresolvable, "MenuLayer::init"));
    EXPECT_TRUE(listed(winReport.resolvable, "PlayLayer::init"));
}

TEST(SurfacePerPair, ResolvabilityDerivedOnlyFromThatPairsSet) {
    // La risolvibilità per la coppia del set passato NON dipende da altre coppie:
    // classify usa esclusivamente `setForPair` (Req 7.2). Alterare un'altra
    // coppia (un set diverso) non entra nel calcolo perché non viene passato.
    BindingSet macSet{kMacPair};
    macSet.add(makeBinding("MenuLayer::init", kGoodOffset, true));

    SurfaceRuntimeReport report = classify_surface({"MenuLayer::init"}, macSet);
    EXPECT_TRUE(listed(report.resolvable, "MenuLayer::init"));
    // La coppia del report è quella del set passato.
    ASSERT_EQ(report.resolvableCount(), 1u);
    EXPECT_EQ(report.resolvable[0].provenance->pair, kMacPair);
}

// ===========================================================================
// 13.3 — SurfaceResolver embedded-first (Req 9.1, 9.2, 9.3, 9.4).
// ===========================================================================

TEST(SurfaceResolverPolicy, EmbeddedWinsWhenItResolves) {
    FakeProvider embedded;
    FakeProvider online;

    BindingSet embSet{kMacPair};
    embSet.add(makeBinding("MenuLayer::init", 0x1111, true));  // embedded risolto
    embedded.addSet(embSet);

    BindingSet onlSet{kMacPair};
    onlSet.add(makeBinding("MenuLayer::init", 0x2222, true));  // online risolto
    online.addSet(onlSet);

    SurfaceResolver resolver{embedded, online};
    auto fn = resolver.resolve(kMacPair, "MenuLayer::init");
    ASSERT_TRUE(fn.has_value());
    EXPECT_TRUE(fn->resolved);
    EXPECT_EQ(fn->address, 0x1111u);  // embedded vince (Req 9.4)
}

TEST(SurfaceResolverPolicy, FallsBackToOnlineOnlyWhenEmbeddedNotResolved) {
    FakeProvider embedded;
    FakeProvider online;

    BindingSet embSet{kMacPair};
    embSet.add(makeBinding("MenuLayer::init", 0, true));  // embedded NON risolto (offset 0)
    embedded.addSet(embSet);

    BindingSet onlSet{kMacPair};
    onlSet.add(makeBinding("MenuLayer::init", 0x2222, true));  // online risolto
    online.addSet(onlSet);

    SurfaceResolver resolver{embedded, online};
    auto fn = resolver.resolve(kMacPair, "MenuLayer::init");
    ASSERT_TRUE(fn.has_value());
    EXPECT_TRUE(fn->resolved);
    EXPECT_EQ(fn->address, 0x2222u);  // fallback online (Req 9.2)
}

TEST(SurfaceResolverPolicy, EmbeddedAbsentSymbolFallsBackToOnline) {
    FakeProvider embedded;  // nessun set per la coppia
    FakeProvider online;

    BindingSet onlSet{kMacPair};
    onlSet.add(makeBinding("MenuLayer::init", 0x2222, true));
    online.addSet(onlSet);

    SurfaceResolver resolver{embedded, online};
    auto fn = resolver.resolve(kMacPair, "MenuLayer::init");
    ASSERT_TRUE(fn.has_value());
    EXPECT_EQ(fn->address, 0x2222u);
}

TEST(SurfaceResolverPolicy, NeitherResolvedReturnsUnresolvedOrNullopt) {
    FakeProvider embedded;
    FakeProvider online;

    BindingSet embSet{kMacPair};
    embSet.add(makeBinding("MenuLayer::init", 0, true));  // embedded non risolto
    embedded.addSet(embSet);
    // online non ha la coppia → nessun binding online.

    SurfaceResolver resolver{embedded, online};
    auto fn = resolver.resolve(kMacPair, "MenuLayer::init");
    ASSERT_TRUE(fn.has_value());          // restituisce l'embedded non risolto
    EXPECT_FALSE(fn->resolved);

    // Simbolo assente del tutto → nullopt (nessuna corrispondenza esatta).
    EXPECT_FALSE(resolver.resolve(kMacPair, "Nope::missing").has_value());
}

TEST(SurfaceResolverPolicy, SameVerifierSemanticsRegardlessOfProvider) {
    // Req 9.3: un binding con offset sentinel ma verified=true NON è risolto,
    // sia che provenga dall'embedded sia dall'online.
    FakeProvider embedded;
    FakeProvider online;

    BindingSet embSet{kMacPair};
    embSet.add(makeBinding("MenuLayer::init", kPlaceholderSentinel, true));
    embedded.addSet(embSet);

    BindingSet onlSet{kMacPair};
    onlSet.add(makeBinding("MenuLayer::init", kPlaceholderSentinel, true));
    online.addSet(onlSet);

    SurfaceResolver resolver{embedded, online};
    auto fn = resolver.resolve(kMacPair, "MenuLayer::init");
    ASSERT_TRUE(fn.has_value());
    EXPECT_FALSE(fn->resolved);  // stessa regola binding_verifier per entrambi
}

TEST(SurfaceResolverPolicy, EffectiveSetFeedsReportWithEmbeddedFirst) {
    FakeProvider embedded;
    FakeProvider online;

    BindingSet embSet{kMacPair};
    embSet.add(makeBinding("MenuLayer::init", 0x1111, true));  // embedded risolto
    embSet.add(makeBinding("PlayLayer::init", 0, true));       // embedded non risolto
    embedded.addSet(embSet);

    BindingSet onlSet{kMacPair};
    onlSet.add(makeBinding("PlayLayer::init", 0x3333, true));  // online risolto (fallback)
    online.addSet(onlSet);

    SurfaceResolver resolver{embedded, online};
    const std::vector<std::string_view> symbols = {"MenuLayer::init", "PlayLayer::init"};
    BindingSet effective = resolver.effectiveSet(kMacPair, symbols);

    SurfaceRuntimeReport report = classify_surface(symbols, effective);
    EXPECT_EQ(report.resolvableCount(), 2u);
    EXPECT_EQ(effective.resolve("MenuLayer::init")->address, 0x1111u);  // embedded
    EXPECT_EQ(effective.resolve("PlayLayer::init")->address, 0x3333u);  // online fallback
}

// ===========================================================================
// 13.4 — Gate di provenienza runtime (Req 8.1, 8.2, 8.3).
// ===========================================================================

TEST(SurfaceProvenanceGate, ResolvableProvenanceIsCompleteAndReadByReference) {
    BindingSet set{kMacPair};
    set.add(makeBinding("MenuLayer::init", kGoodOffset, true));

    SurfaceProvenanceStore store;
    store.put(ProvenanceRecord{"MenuLayer::init", kMacPair, "contributor-A",
                               /*crossCheckDocumented=*/true, /*prologueDocumented=*/true});

    SurfaceRuntimeReport report =
        classify_surface({"MenuLayer::init"}, set, default_prologue_verifier(), &store);

    ASSERT_EQ(report.resolvableCount(), 1u);
    const ProvenanceRef* ref = report.resolvable[0].provenance;
    ASSERT_NE(ref, nullptr);
    EXPECT_TRUE(ref->complete);
    ASSERT_NE(ref->record, nullptr);
    // Lettura per riferimento: punta al record memorizzato nello store, senza
    // ricomputazione (Req 8.3).
    EXPECT_EQ(ref->record, store.get("MenuLayer::init", kMacPair));
    EXPECT_EQ(ref->record->addressSource, "contributor-A");
}

TEST(SurfaceProvenanceGate, IncompleteProvenanceDeclassifiesAndAuditsSymbolPair) {
    BindingSet set{kMacPair};
    set.add(makeBinding("MenuLayer::init", kGoodOffset, true));  // verified+offset valido

    SurfaceProvenanceStore store;
    // verified=true ma provenienza incompleta (prologo non documentato).
    store.put(ProvenanceRecord{"MenuLayer::init", kMacPair, "contributor-A",
                               /*crossCheckDocumented=*/true, /*prologueDocumented=*/false});

    SurfaceRuntimeReport report =
        classify_surface({"MenuLayer::init"}, set, default_prologue_verifier(), &store);

    // Declassato a non risolvibile (Req 8.2) nonostante verified=true.
    EXPECT_EQ(report.resolvableCount(), 0u);
    ASSERT_EQ(report.unresolvableCount(), 1u);
    EXPECT_EQ(report.unresolvable[0].symbol, "MenuLayer::init");

    // Errore di auditabilità con simbolo + coppia (Req 8.2).
    ASSERT_EQ(report.auditErrors.size(), 1u);
    EXPECT_EQ(report.auditErrors[0].symbol, "MenuLayer::init");
    EXPECT_EQ(report.auditErrors[0].pair, kMacPair);
}

TEST(SurfaceProvenanceGate, NoStoreImpliesCompleteFromVerifiedFlag) {
    // Senza store di provenienza, la completezza è implicata dal flag verified
    // del .pbind + gate build-time: l'elemento risolto è risolvibile.
    BindingSet set{kMacPair};
    set.add(makeBinding("MenuLayer::init", kGoodOffset, true));

    SurfaceRuntimeReport report = classify_surface({"MenuLayer::init"}, set);
    ASSERT_EQ(report.resolvableCount(), 1u);
    ASSERT_NE(report.resolvable[0].provenance, nullptr);
    EXPECT_TRUE(report.resolvable[0].provenance->complete);
    EXPECT_EQ(report.resolvable[0].provenance->record, nullptr);
    EXPECT_TRUE(report.auditErrors.empty());
}

TEST(SurfaceProvenanceGate, StoreGetIsIdempotentSameReference) {
    SurfaceProvenanceStore store;
    store.put(ProvenanceRecord{"MenuLayer::init", kMacPair, "src", true, true});

    const ProvenanceRecord* a = store.get("MenuLayer::init", kMacPair);
    const ProvenanceRecord* b = store.get("MenuLayer::init", kMacPair);
    EXPECT_EQ(a, b);  // due letture → stesso record (nessuna ricomputazione)
    EXPECT_NE(a, nullptr);

    // Coppia diversa / simbolo diverso → nessun record.
    EXPECT_EQ(store.get("MenuLayer::init", kWinPair), nullptr);
    EXPECT_EQ(store.get("PlayLayer::init", kMacPair), nullptr);
}
