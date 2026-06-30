// tests/binding_verification_test.cpp — unit test della risoluzione exact-pair
// e della semantica "resolved sse verificato" (task 3.9, Req 4.2, 4.3, 4.4, 4.5).
//
// Verifica:
//   * `address_is_verified` / `mark_resolved_if_verified`: un binding è risolto
//     SSE l'indirizzo è verificato (non-zero, != sentinel, prologo conforme) E
//     la voce dichiarava la verifica offline (Req 4.2, 4.3).
//   * `EmbeddedBindingsProvider`: il set embedded MVP resta risolto (offset
//     non-zero, != sentinel); `addVerifiedSet` affianca un `.pbind` reale
//     ricalcolando `resolved` per ogni voce (Req 4.3).
//   * Lookup exact-pair: `load` carica SSE la coppia (GD_Version, piattaforma)
//     coincide esattamente; nessuna sostituzione da versione/piattaforma diversa
//     (Req 4.4). `resolve` esatto del simbolo, nessun fuzzy-match.
//   * Zero hook su indirizzi non risolti: instradando i binding non risolti
//     attraverso `HookGate` con un `FakeBackend`, nessun hook viene installato
//     (Req 4.5).

#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "bindings/binding_verifier.hpp"
#include "bindings/bindings.hpp"
#include "bindings/embedded_bindings_provider.hpp"
#include "bindings/pbind_format.hpp"
#include "hooking/fake_backend.hpp"
#include "hooking/hook_gate.hpp"

namespace {

using namespace pulse::loader::bindings;
using pulse::hooking::FakeBackend;
using pulse::hooking::HookGate;

int g_detour = 0;
void* const kDetour = &g_detour;

// Verificatore di prologo controllabile: risponde secondo un flag così i test
// possono esercitare la dimensione "prologo conforme alla firma".
class ControllablePrologueVerifier final : public IPrologueVerifier {
public:
    explicit ControllablePrologueVerifier(bool ok) : ok_(ok) {}
    bool prologueMatchesSignature(const FunctionBinding&) const override { return ok_; }

private:
    bool ok_;
};

FunctionBinding makeBinding(const std::string& symbol, std::uintptr_t addr,
                            bool claimedVerified) {
    FunctionBinding fn;
    fn.symbol = symbol;
    fn.address = addr;
    fn.signature = Signature{"bool", {"MenuLayer*"}};
    fn.resolved = claimedVerified;  // claim della verifica offline (.pbind verified)
    return fn;
}

const BindingKey kPrioritizedKey{GdVersion{2, 2081}, "macos-arm64"};

// `.pbind` reale di esempio per (2.2081, macos-arm64): offset verificato
// non-placeholder, verified=true (vedi design 4.1 / Data Models).
constexpr const char* kRealPbind =
    "pbind_version = 1\n"
    "gd_version = 2.2081\n"
    "platform = macos-arm64\n"
    "\n"
    "[function]\n"
    "symbol = MenuLayer::init\n"
    "offset = 0x00A1B2C0\n"
    "return = bool\n"
    "params = MenuLayer*\n"
    "verified = true\n";

BindingSet parseOrDie(const char* content) {
    PbindParseResult r = parse_pbind(content);
    EXPECT_TRUE(r.ok()) << (r.error ? r.error->message : "");
    return std::move(*r.value);
}

}  // namespace

// ===========================================================================
// address_is_verified / mark_resolved_if_verified (Req 4.2, 4.3).
// ===========================================================================

TEST(BindingVerifier, VerifiedRequiresNonZeroOffset) {
    TrustingPrologueVerifier ok;
    EXPECT_FALSE(address_is_verified(makeBinding("S::f", 0, true), ok));
    EXPECT_TRUE(address_is_verified(makeBinding("S::f", 0x1000, true), ok));
}

TEST(BindingVerifier, VerifiedRejectsSentinelOffset) {
    TrustingPrologueVerifier ok;
    EXPECT_FALSE(
        address_is_verified(makeBinding("S::f", kPlaceholderSentinel, true), ok));
}

TEST(BindingVerifier, VerifiedRequiresPrologueConformance) {
    ControllablePrologueVerifier bad{false};
    EXPECT_FALSE(address_is_verified(makeBinding("S::f", 0x1000, true), bad));
    ControllablePrologueVerifier good{true};
    EXPECT_TRUE(address_is_verified(makeBinding("S::f", 0x1000, true), good));
}

TEST(BindingVerifier, ResolvedIffVerifiedAndClaimed) {
    TrustingPrologueVerifier ok;

    // Claim verificato + indirizzo verificato => resolved.
    EXPECT_TRUE(mark_resolved_if_verified(makeBinding("S::f", 0x1000, true), ok).resolved);

    // Claim NON verificato => non risolto anche con indirizzo valido.
    EXPECT_FALSE(mark_resolved_if_verified(makeBinding("S::f", 0x1000, false), ok).resolved);

    // Claim verificato ma indirizzo nullo/sentinel => non risolto (difesa).
    EXPECT_FALSE(mark_resolved_if_verified(makeBinding("S::f", 0, true), ok).resolved);
    EXPECT_FALSE(
        mark_resolved_if_verified(makeBinding("S::f", kPlaceholderSentinel, true), ok)
            .resolved);

    // Prologo non conforme => non risolto anche con claim verificato.
    ControllablePrologueVerifier bad{false};
    EXPECT_FALSE(mark_resolved_if_verified(makeBinding("S::f", 0x1000, true), bad).resolved);
}

TEST(BindingVerifier, VerifyBindingSetPreservesKeyAndRecomputesResolved) {
    BindingSet set{kPrioritizedKey};
    set.add(makeBinding("A::a", 0x1000, true));   // verified, valid -> resolved
    set.add(makeBinding("B::b", 0, true));        // verified ma offset 0 -> no
    set.add(makeBinding("C::c", 0x2000, false));  // non verified -> no

    TrustingPrologueVerifier ok;
    BindingSet verified = verify_binding_set(set, ok);

    EXPECT_EQ(verified.key(), kPrioritizedKey);
    ASSERT_EQ(verified.functions().size(), 3u);
    EXPECT_TRUE(verified.resolve("A::a")->resolved);
    EXPECT_FALSE(verified.resolve("B::b")->resolved);
    EXPECT_FALSE(verified.resolve("C::c")->resolved);
}

// ===========================================================================
// EmbeddedBindingsProvider: set embedded MVP + affiancamento del `.pbind` reale.
// ===========================================================================

TEST(EmbeddedProviderVerified, MvpSetRemainsResolved) {
    EmbeddedBindingsProvider provider;
    ASSERT_TRUE(provider.load(BindingKey{GdVersion{2, 2074}, "windows-x64"}).has_value());
    auto fn = provider.resolve("MenuLayer::init");
    ASSERT_TRUE(fn.has_value());
    EXPECT_TRUE(fn->resolved);
    EXPECT_NE(fn->address, 0u);
    EXPECT_NE(fn->address, kPlaceholderSentinel);
}

TEST(EmbeddedProviderVerified, DefaultProviderHasNoPrioritizedPair) {
    // Il provider di default NON espone la coppia (2.2081, macos-arm64): essa è
    // affiancata solo a runtime via `addVerifiedSet` (preserva l'invariante del
    // set embedded singolo). Exact-pair: nessuna sostituzione.
    EmbeddedBindingsProvider provider;
    EXPECT_FALSE(provider.load(kPrioritizedKey).has_value());
}

TEST(EmbeddedProviderVerified, AddVerifiedSetFromRealPbindResolvesExactPair) {
    EmbeddedBindingsProvider provider;
    provider.addVerifiedSet(parseOrDie(kRealPbind));

    // Lookup exact-pair: carica SSE la coppia coincide esattamente (Req 4.4).
    auto loaded = provider.load(kPrioritizedKey);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->key(), kPrioritizedKey);

    // `.pbind` verificato (offset non-zero, verified=true) => resolved (Req 4.2/4.3).
    auto fn = provider.resolve("MenuLayer::init");
    ASSERT_TRUE(fn.has_value());
    EXPECT_TRUE(fn->resolved);
    EXPECT_EQ(fn->address, 0x00A1B2C0u);
    EXPECT_EQ(fn->signature.returnType, "bool");
}

TEST(EmbeddedProviderVerified, AddVerifiedSetNoSubstitutionAcrossVersionOrPlatform) {
    EmbeddedBindingsProvider provider;
    provider.addVerifiedSet(parseOrDie(kRealPbind));

    // Versione/piattaforma diverse: nessuna corrispondenza, nessun fuzzy-match,
    // nessuna sostituzione dalla coppia affiancata (Req 4.4).
    EXPECT_FALSE(provider.load(BindingKey{GdVersion{2, 2074}, "macos-arm64"}).has_value());
    EXPECT_FALSE(provider.load(BindingKey{GdVersion{2, 2081}, "macos-x64"}).has_value());
    EXPECT_FALSE(provider.load(BindingKey{GdVersion{2, 2081}, "Macos-Arm64"}).has_value());
}

TEST(EmbeddedProviderVerified, UnverifiedPbindEntryStaysUnresolved) {
    // `.pbind` che dichiara verified=false: il binding resta non risolto.
    const char* unverified =
        "pbind_version = 1\n"
        "gd_version = 2.2081\n"
        "platform = macos-arm64\n"
        "\n"
        "[function]\n"
        "symbol = PlayLayer::init\n"
        "offset = 0x00B00000\n"
        "return = bool\n"
        "params = PlayLayer*, GJGameLevel*\n"
        "verified = false\n";

    EmbeddedBindingsProvider provider;
    provider.addVerifiedSet(parseOrDie(unverified));
    ASSERT_TRUE(provider.load(kPrioritizedKey).has_value());

    auto fn = provider.resolve("PlayLayer::init");
    ASSERT_TRUE(fn.has_value());
    EXPECT_FALSE(fn->resolved);  // non verificato => non risolto (Req 4.3)
}

TEST(EmbeddedProviderVerified, SentinelOffsetClaimingVerifiedStaysUnresolved) {
    // Difesa in profondità: una voce con offset sentinel ma verified=true non
    // deve mai diventare risolta (Req 4.2).
    BindingSet set{kPrioritizedKey};
    set.add(makeBinding("MenuLayer::init", kPlaceholderSentinel, true));

    EmbeddedBindingsProvider provider;
    provider.addVerifiedSet(set);
    ASSERT_TRUE(provider.load(kPrioritizedKey).has_value());

    auto fn = provider.resolve("MenuLayer::init");
    ASSERT_TRUE(fn.has_value());
    EXPECT_FALSE(fn->resolved);
}

TEST(EmbeddedProviderVerified, ResolveIsExactSymbolNoFuzzyMatch) {
    EmbeddedBindingsProvider provider;
    provider.addVerifiedSet(parseOrDie(kRealPbind));
    ASSERT_TRUE(provider.load(kPrioritizedKey).has_value());

    EXPECT_TRUE(provider.resolve("MenuLayer::init").has_value());
    EXPECT_FALSE(provider.resolve("MenuLayer::Init").has_value());
    EXPECT_FALSE(provider.resolve("menulayer::init").has_value());
    EXPECT_FALSE(provider.resolve("MenuLayer::init ").has_value());
    EXPECT_FALSE(provider.resolve("init").has_value());
}

TEST(EmbeddedProviderVerified, AddVerifiedSetReplacesSamePairNoDuplicate) {
    EmbeddedBindingsProvider provider;

    // Primo affiancamento: voce non verificata.
    BindingSet first{kPrioritizedKey};
    first.add(makeBinding("MenuLayer::init", 0x00A1B2C0, false));
    provider.addVerifiedSet(first);

    auto before = (provider.load(kPrioritizedKey), provider.resolve("MenuLayer::init"));
    ASSERT_TRUE(before.has_value());
    EXPECT_FALSE(before->resolved);

    // Secondo affiancamento sulla STESSA coppia: voce verificata. Sostituisce.
    provider.addVerifiedSet(parseOrDie(kRealPbind));
    auto loaded = provider.load(kPrioritizedKey);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->functions().size(), 1u);  // nessun duplicato di coppia
    auto after = provider.resolve("MenuLayer::init");
    ASSERT_TRUE(after.has_value());
    EXPECT_TRUE(after->resolved);
}

// ===========================================================================
// Zero hook su indirizzi non risolti (Req 4.5): i binding non risolti dal
// provider sono bloccati da HookGate prima di toccare il backend.
// ===========================================================================

TEST(EmbeddedProviderVerified, ZeroHooksOnUnresolvedBindings) {
    // `.pbind` con un mix: una voce verificata (risolta) e una non verificata.
    const char* mixed =
        "pbind_version = 1\n"
        "gd_version = 2.2081\n"
        "platform = macos-arm64\n"
        "\n"
        "[function]\n"
        "symbol = MenuLayer::init\n"
        "offset = 0x00A1B2C0\n"
        "return = bool\n"
        "params = MenuLayer*\n"
        "verified = true\n"
        "\n"
        "[function]\n"
        "symbol = PlayLayer::init\n"
        "offset = 0x00B00000\n"
        "return = bool\n"
        "params = PlayLayer*\n"
        "verified = false\n";

    EmbeddedBindingsProvider provider;
    provider.addVerifiedSet(parseOrDie(mixed));
    ASSERT_TRUE(provider.load(kPrioritizedKey).has_value());

    FakeBackend backend;
    HookGate gate{backend};

    const auto resolvedFn = provider.resolve("MenuLayer::init");    // risolto
    const auto unresolvedFn = provider.resolve("PlayLayer::init");  // non risolto

    gate.install(resolvedFn, kDetour);
    gate.install(unresolvedFn, kDetour);

    // Solo il binding risolto è installato; zero hook su indirizzi non risolti.
    EXPECT_EQ(gate.installedCount(), 1u);
    EXPECT_EQ(gate.hooksOnUnresolvedAddresses(), 0u);
    EXPECT_EQ(gate.blockedUnresolvedCount(), 1u);
    ASSERT_TRUE(unresolvedFn.has_value());
    EXPECT_FALSE(backend.isInstalled(unresolvedFn->address));
}
