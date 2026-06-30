// tests/online_bindings_provider_test.cpp — unit test del provider online
// (task 25.1, Req 20.1, 20.2).
//
// Verifica, con fetcher/verifier fittizi iniettati (host, senza rete):
//   * fetch verificato con successo => carica il set ONLINE per la coppia esatta;
//   * firma non valida => fallback sul set EMBEDDED;
//   * fetch fallito => fallback sul set EMBEDDED;
//   * cache hit: una seconda load della stessa coppia NON ri-scarica;
//   * coppia non esatta (no online, no embedded) => nullopt (nessun fuzzy-match);
//   * set scaricato con coppia diversa da quella richiesta => scartato (no fuzzy);
//   * resolve a corrispondenza esatta del simbolo nel set corrente;
//   * round-trip di serializzazione del formato .pbind.

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "bindings/bindings.hpp"
#include "bindings/embedded_bindings_provider.hpp"
#include "bindings/online_bindings_provider.hpp"

namespace {

using namespace pulse::loader::bindings;

BindingKey mvpKey() {
    return BindingKey{GdVersion{2, 2074}, "windows-x64"};
}

// Costruisce un set online plausibile per una coppia, con un simbolo distinto
// da quello embedded così da poter distinguere quale provider ha risposto.
BindingSet makeOnlineSet(const BindingKey& key) {
    BindingSet set{key};
    FunctionBinding fn;
    fn.symbol = "MenuLayer::onlineSymbol";
    fn.address = 0xABCD;
    fn.signature = Signature{"int", {"MenuLayer*", "float"}};
    fn.resolved = true;
    set.add(std::move(fn));
    return set;
}

// --- Fake fetcher: mappa URL -> byte. Registra il numero di fetch eseguiti. ---
class FakeFetcher final : public IBindingFetcher {
public:
    std::optional<std::vector<std::uint8_t>> fetch(const std::string& url) override {
        ++fetchCount;
        lastUrl = url;
        for (const auto& [u, bytes] : entries) {
            if (u == url) {
                return bytes;
            }
        }
        return std::nullopt;  // 404 / errore di rete
    }

    void put(const std::string& url, std::vector<std::uint8_t> bytes) {
        entries.emplace_back(url, std::move(bytes));
    }

    int fetchCount = 0;
    std::string lastUrl;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> entries;
};

// --- Fake verifier: accetta/rifiuta in base a un flag. ---
class FakeVerifier final : public ISignatureVerifier {
public:
    explicit FakeVerifier(bool ok) : accept(ok) {}
    bool verify(const std::vector<std::uint8_t>&, const std::string&) const override {
        return accept;
    }
    bool accept;
};

// --- .pbind round-trip ----------------------------------------------------

TEST(OnlineBindingsPbind, SerializeParseRoundTrip) {
    BindingSet set = makeOnlineSet(mvpKey());
    auto bytes = serializePbind("trusted-sig", set);

    auto container = parsePbind(bytes);
    ASSERT_TRUE(container.has_value());
    EXPECT_EQ(container->signature, "trusted-sig");

    auto parsed = parsePbindPayload(container->payload);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->key(), mvpKey());
    ASSERT_EQ(parsed->functions().size(), 1u);
    EXPECT_EQ(parsed->functions()[0], set.functions()[0]);
}

TEST(OnlineBindingsPbind, ParseRejectsMalformed) {
    EXPECT_FALSE(parsePbind(std::vector<std::uint8_t>{}).has_value());
    std::string junk = "NOTPBIND\nSIG:x\n";
    EXPECT_FALSE(parsePbind(std::vector<std::uint8_t>(junk.begin(), junk.end())).has_value());
}

// --- URL building ---------------------------------------------------------

TEST(OnlineBindings, BuildsExactUrlFromKey) {
    FakeFetcher fetcher;
    FakeVerifier verifier{true};
    EmbeddedBindingsProvider embedded;
    OnlineBindingsProvider provider{fetcher, verifier, embedded};

    EXPECT_EQ(provider.buildUrl(mvpKey()),
              "mod-index/bindings/2.2074/windows-x64.pbind");
}

// --- Successo online ------------------------------------------------------

TEST(OnlineBindings, VerifiedFetchLoadsOnlineSet) {
    FakeFetcher fetcher;
    FakeVerifier verifier{true};
    EmbeddedBindingsProvider embedded;
    OnlineBindingsProvider provider{fetcher, verifier, embedded};

    const std::string url = provider.buildUrl(mvpKey());
    fetcher.put(url, serializePbind("ok", makeOnlineSet(mvpKey())));

    auto set = provider.load(mvpKey());
    ASSERT_TRUE(set.has_value());
    EXPECT_EQ(set->key(), mvpKey());
    // Deve essere il set ONLINE (simbolo distintivo), non l'embedded.
    auto fn = provider.resolve("MenuLayer::onlineSymbol");
    ASSERT_TRUE(fn.has_value());
    EXPECT_EQ(fn->address, 0xABCDu);
    // Il simbolo embedded non è presente nel set online.
    EXPECT_FALSE(provider.resolve("MenuLayer::init").has_value());
}

// --- Fallback su firma non valida -----------------------------------------

TEST(OnlineBindings, SignatureFailureFallsBackToEmbedded) {
    FakeFetcher fetcher;
    FakeVerifier verifier{false};  // verifica sempre fallita
    EmbeddedBindingsProvider embedded;
    OnlineBindingsProvider provider{fetcher, verifier, embedded};

    const std::string url = provider.buildUrl(mvpKey());
    fetcher.put(url, serializePbind("bad", makeOnlineSet(mvpKey())));

    auto set = provider.load(mvpKey());
    ASSERT_TRUE(set.has_value());
    EXPECT_EQ(set->key(), mvpKey());
    // Deve essere il set EMBEDDED (MenuLayer::init), non quello online.
    EXPECT_TRUE(provider.resolve("MenuLayer::init").has_value());
    EXPECT_FALSE(provider.resolve("MenuLayer::onlineSymbol").has_value());
}

// --- Fallback su fetch fallito --------------------------------------------

TEST(OnlineBindings, FetchFailureFallsBackToEmbedded) {
    FakeFetcher fetcher;  // nessuna entry => fetch fallisce
    FakeVerifier verifier{true};
    EmbeddedBindingsProvider embedded;
    OnlineBindingsProvider provider{fetcher, verifier, embedded};

    auto set = provider.load(mvpKey());
    ASSERT_TRUE(set.has_value());
    EXPECT_TRUE(provider.resolve("MenuLayer::init").has_value());
}

// --- Cache hit evita il ri-fetch ------------------------------------------

TEST(OnlineBindings, CacheHitAvoidsRefetch) {
    FakeFetcher fetcher;
    FakeVerifier verifier{true};
    EmbeddedBindingsProvider embedded;
    OnlineBindingsProvider provider{fetcher, verifier, embedded};

    const std::string url = provider.buildUrl(mvpKey());
    fetcher.put(url, serializePbind("ok", makeOnlineSet(mvpKey())));

    ASSERT_TRUE(provider.load(mvpKey()).has_value());
    EXPECT_EQ(fetcher.fetchCount, 1);

    // Seconda load della stessa coppia: deve servire dalla cache senza fetch.
    ASSERT_TRUE(provider.load(mvpKey()).has_value());
    EXPECT_EQ(fetcher.fetchCount, 1);
    EXPECT_TRUE(provider.resolve("MenuLayer::onlineSymbol").has_value());
}

// --- Coppia non esatta => nullopt (nessun fuzzy-match) --------------------

TEST(OnlineBindings, NonExactKeyReturnsNulloptNoFuzzyMatch) {
    FakeFetcher fetcher;
    FakeVerifier verifier{true};
    EmbeddedBindingsProvider embedded;
    OnlineBindingsProvider provider{fetcher, verifier, embedded};

    // Online ha solo windows-x64; embedded ha solo (2.2074, windows-x64).
    fetcher.put(provider.buildUrl(mvpKey()), serializePbind("ok", makeOnlineSet(mvpKey())));

    // Versione vicina ma non identica: nessuna corrispondenza esatta.
    EXPECT_FALSE(provider.load(BindingKey{GdVersion{2, 2073}, "windows-x64"}).has_value());
    // Piattaforma diversa.
    EXPECT_FALSE(provider.load(BindingKey{GdVersion{2, 2074}, "android-arm64"}).has_value());
    // Differenza solo di case nella piattaforma.
    EXPECT_FALSE(provider.load(BindingKey{GdVersion{2, 2074}, "Windows-X64"}).has_value());
}

// --- Set scaricato con coppia errata => scartato (no fuzzy) ----------------

TEST(OnlineBindings, MismatchedDownloadedKeyIsRejected) {
    FakeFetcher fetcher;
    FakeVerifier verifier{true};
    EmbeddedBindingsProvider embedded;
    OnlineBindingsProvider provider{fetcher, verifier, embedded};

    // L'URL richiesto è per (2,2074); ma il payload dichiara una coppia diversa.
    const BindingKey requested = mvpKey();
    const BindingKey wrong{GdVersion{2, 2073}, "windows-x64"};
    fetcher.put(provider.buildUrl(requested), serializePbind("ok", makeOnlineSet(wrong)));

    // Il set scaricato non corrisponde alla coppia richiesta: si scarta e si
    // ricade sull'embedded (che possiede (2,2074, windows-x64)).
    auto set = provider.load(requested);
    ASSERT_TRUE(set.has_value());
    EXPECT_EQ(set->key(), requested);
    EXPECT_TRUE(provider.resolve("MenuLayer::init").has_value());
    EXPECT_FALSE(provider.resolve("MenuLayer::onlineSymbol").has_value());
}

// --- resolve esatto -------------------------------------------------------

TEST(OnlineBindings, ResolveIsExactNoFuzzyMatch) {
    FakeFetcher fetcher;
    FakeVerifier verifier{true};
    EmbeddedBindingsProvider embedded;
    OnlineBindingsProvider provider{fetcher, verifier, embedded};

    fetcher.put(provider.buildUrl(mvpKey()), serializePbind("ok", makeOnlineSet(mvpKey())));
    ASSERT_TRUE(provider.load(mvpKey()).has_value());

    EXPECT_TRUE(provider.resolve("MenuLayer::onlineSymbol").has_value());
    EXPECT_FALSE(provider.resolve("MenuLayer::OnlineSymbol").has_value());  // case
    EXPECT_FALSE(provider.resolve("onlineSymbol").has_value());             // sottostringa
    EXPECT_FALSE(provider.resolve("MenuLayer::onlineSymbol ").has_value()); // spazio
}

TEST(OnlineBindings, ResolveReturnsNulloptWithoutLoad) {
    FakeFetcher fetcher;
    FakeVerifier verifier{true};
    EmbeddedBindingsProvider embedded;
    OnlineBindingsProvider provider{fetcher, verifier, embedded};
    EXPECT_FALSE(provider.resolve("MenuLayer::init").has_value());
}

}  // namespace
