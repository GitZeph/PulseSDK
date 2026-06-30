// tests/property36_cross_platform_lookup_test.cpp
// Feature: pulse-sdk, Property 36 — Lookup dei bindings con corrispondenza
// esatta e nessun hook su indirizzi non risolti, ESTESO a più coppie
// (GD_Version, piattaforma) tramite l'OnlineBindingsProvider (task 25.2).
// Validates: Requirements 20.1, 20.2, 20.3, 20.4 (Requisiti 20.1–20.4)
//
// Contesto: il task 9.2 ha già coperto P36 sul singolo set embedded
// (2.2074, windows-x64). Questo test ESTENDE la stessa proprietà a più coppie
// (GD_Version, piattaforma) usando l'OnlineBindingsProvider (Layer 2,
// task 25.1), che indicizza i set scaricati per coppia esatta con cache e
// fallback. Per girare host senza rete, il trasporto (`IBindingFetcher`) e la
// verifica di firma (`ISignatureVerifier`) sono fittizi e iniettati; come
// fallback si usa un provider nullo, così la sola sorgente di set è quella
// "provisionata" nel fake fetcher.
//
// Property 36 (design.md): "Per ogni coppia (GD_Version, piattaforma) e per
// ogni insieme di simboli, `resolve` restituisce un binding se e solo se la
// coppia esatta è indicizzata e il simbolo è risolto; il numero di hook
// installati su indirizzi non risolti è sempre 0."
//
// Su input randomizzati con RapidCheck (≥100 iterazioni per default), su molte
// coppie (GD_Version, piattaforma) generate casualmente:
//
//   PARTE 1 — Lookup ESATTO cross-platform (Req 20.1, 20.2):
//     Per ogni coppia PROVISIONATA, `load` carica esattamente quel set e
//     `resolve` del simbolo indicizzato restituisce il binding corretto
//     (indirizzo atteso); i near-miss di simbolo non risolvono (no fuzzy-match).
//
//   PARTE 2 — Nessun binding e zero hook su coppie NON provisionate
//   (Req 20.2, 20.3, 20.4):
//     Per una coppia NON provisionata (versione e/o piattaforma diversa),
//     `load` restituisce nullopt (nessun fuzzy-match) e `resolve` non risolve
//     nulla; instradando l'esito attraverso HookGate + FakeBackend, il numero
//     di hook installati su indirizzi non risolti è sempre 0.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "bindings/bindings.hpp"
#include "bindings/online_bindings_provider.hpp"
#include "hooking/fake_backend.hpp"
#include "hooking/hook_gate.hpp"

namespace {

using pulse::hooking::FakeBackend;
using pulse::hooking::HookGate;
using pulse::loader::bindings::BindingKey;
using pulse::loader::bindings::BindingSet;
using pulse::loader::bindings::FunctionBinding;
using pulse::loader::bindings::GdVersion;
using pulse::loader::bindings::IBindingFetcher;
using pulse::loader::bindings::IBindingsProvider;
using pulse::loader::bindings::ISignatureVerifier;
using pulse::loader::bindings::OnlineBindingsProvider;
using pulse::loader::bindings::serializePbind;
using pulse::loader::bindings::Signature;

int g_detour = 0;
void* const kDetour = &g_detour;

// --- Doppi di test: trasporto, verifica e fallback (host, senza rete). -----

// Fake fetcher deterministico: mappa URL -> byte serializzati di un .pbind.
class FakeFetcher final : public IBindingFetcher {
public:
    std::optional<std::vector<std::uint8_t>> fetch(const std::string& url) override {
        for (const auto& [u, bytes] : entries_) {
            if (u == url) {
                return bytes;
            }
        }
        return std::nullopt;  // coppia non provisionata => 404
    }

    void put(const std::string& url, std::vector<std::uint8_t> bytes) {
        entries_.emplace_back(url, std::move(bytes));
    }

private:
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> entries_;
};

// Verificatore fittizio che accetta sempre (la firma non è oggetto di P36).
class AcceptingVerifier final : public ISignatureVerifier {
public:
    bool verify(const std::vector<std::uint8_t>&, const std::string&) const override {
        return true;
    }
};

// Fallback nullo: nessuna corrispondenza, così l'unica sorgente di set è il
// fake fetcher (evita il set embedded che inquinerebbe lo spazio "non
// provisionato").
class NullBindingsProvider final : public IBindingsProvider {
public:
    std::optional<BindingSet> load(const BindingKey&) override { return std::nullopt; }
    std::optional<FunctionBinding> resolve(std::string_view) const override {
        return std::nullopt;
    }
};

// Piattaforme supportate plausibili (Req 20.1: coppie indicizzate per piattaforma).
const std::vector<std::string>& platforms() {
    static const std::vector<std::string> kPlatforms = {
        "windows-x64", "windows-x86", "macos-x64",  "macos-arm64",
        "android-arm64", "android-armv7", "ios-arm64", "linux-x64"};
    return kPlatforms;
}

// Simbolo indicizzato deterministico per una coppia: univoco per (versione,
// piattaforma) così la verifica del "binding corretto" non è ambigua.
std::string symbolFor(const BindingKey& key) {
    return "Func::v" + std::to_string(key.version.major) + "_" +
           std::to_string(key.version.minor) + "@" + key.platformId;
}

// Indirizzo risolto deterministico e non nullo per una coppia.
std::uintptr_t addressFor(const BindingKey& key) {
    std::uintptr_t h = 0x1000;
    h = h * 31 + static_cast<std::uintptr_t>(key.version.major) + 1;
    h = h * 31 + static_cast<std::uintptr_t>(key.version.minor) + 1;
    for (const char c : key.platformId) {
        h = h * 31 + static_cast<std::uintptr_t>(static_cast<unsigned char>(c));
    }
    return (h | 0x1u);  // garantisce indirizzo non nullo
}

// Costruisce il set risolto per una coppia (un simbolo distintivo).
BindingSet buildSetForKey(const BindingKey& key) {
    BindingSet set{key};
    FunctionBinding fn;
    fn.symbol = symbolFor(key);
    fn.address = addressFor(key);
    // Firma con almeno un parametro: il formato .pbind serializza i tipi dei
    // parametri come ultimo campo (TAB-separato), quindi una lista non vuota
    // round-trip-a fedelmente attraverso il trasporto fittizio.
    fn.signature = Signature{"void", {"void*"}};
    fn.resolved = true;
    set.add(std::move(fn));
    return set;
}

bool sameKey(const BindingKey& a, const BindingKey& b) {
    return a.version == b.version && a.platformId == b.platformId;
}

bool containsKey(const std::vector<BindingKey>& keys, const BindingKey& key) {
    return std::any_of(keys.begin(), keys.end(),
                       [&](const BindingKey& k) { return sameKey(k, key); });
}

// Genera una coppia (GD_Version, piattaforma) su uno spazio ampio così che una
// coppia interrogata a caso sia quasi sempre "non provisionata".
rc::Gen<BindingKey> keyGen() {
    return rc::gen::map(
        rc::gen::tuple(rc::gen::inRange(0, 6),       // GD major
                       rc::gen::inRange(0, 3000),    // GD minor
                       rc::gen::inRange<std::size_t>(0, platforms().size())),
        [](const std::tuple<int, int, std::size_t>& t) {
            return BindingKey{GdVersion{std::get<0>(t), std::get<1>(t)},
                              platforms()[std::get<2>(t)]};
        });
}

// Genera un insieme NON vuoto di coppie provisionate, deduplicate per coppia
// esatta (l'OnlineBindingsProvider indicizza per coppia univoca, Req 20.1).
std::vector<BindingKey> generateProvisionedKeys() {
    const auto raw = *rc::gen::nonEmpty(rc::gen::container<std::vector<BindingKey>>(keyGen()))
                          .as("coppie provisionate (grezze)");
    std::vector<BindingKey> unique;
    for (const auto& k : raw) {
        if (!containsKey(unique, k)) {
            unique.push_back(k);
        }
    }
    return unique;
}

}  // namespace

// ===========================================================================
// PARTE 1 — Lookup ESATTO cross-platform: per ogni coppia provisionata, load
// carica esattamente quel set e resolve restituisce il binding corretto.
// Feature: pulse-sdk, Property 36. Validates: Requirements 20.1, 20.2.
// ===========================================================================
RC_GTEST_PROP(Property36CrossPlatform,
              ExactMatchLoadAndResolveAcrossPairs,
              ()) {
    const std::vector<BindingKey> provisioned = generateProvisionedKeys();

    FakeFetcher fetcher;
    AcceptingVerifier verifier;
    NullBindingsProvider fallback;
    OnlineBindingsProvider provider{fetcher, verifier, fallback};

    // Provisiona ogni coppia all'URL ESATTO costruito dalla coppia stessa.
    for (const auto& key : provisioned) {
        fetcher.put(provider.buildUrl(key), serializePbind("sig", buildSetForKey(key)));
    }

    for (const auto& key : provisioned) {
        // load deve restituire ESATTAMENTE il set della coppia richiesta.
        const auto loaded = provider.load(key);
        RC_ASSERT(loaded.has_value());
        RC_ASSERT(sameKey(loaded->key(), key));

        // resolve del simbolo indicizzato restituisce il binding corretto.
        const std::string symbol = symbolFor(key);
        const auto hit = provider.resolve(symbol);
        RC_ASSERT(hit.has_value());
        RC_ASSERT(hit->symbol == symbol);
        RC_ASSERT(hit->resolved);
        RC_ASSERT(hit->address == addressFor(key));

        // Near-miss del simbolo: nessun fuzzy-match (case, spazio, suffisso).
        std::string upper = symbol;
        for (char& c : upper) c = static_cast<char>(std::toupper(c));
        if (upper != symbol) {
            RC_ASSERT(!provider.resolve(upper).has_value());
        }
        RC_ASSERT(!provider.resolve(symbol + " ").has_value());
        RC_ASSERT(!provider.resolve(symbol + "X").has_value());
    }
}

// ===========================================================================
// PARTE 2 — Coppia NON provisionata: load non risolve nulla (no fuzzy-match) e
// instradando l'esito attraverso HookGate il numero di hook su indirizzi non
// risolti è sempre 0.
// Feature: pulse-sdk, Property 36. Validates: Requirements 20.2, 20.3, 20.4.
// ===========================================================================
RC_GTEST_PROP(Property36CrossPlatform,
              NonProvisionedPairYieldsNoBindingAndZeroHooks,
              ()) {
    const std::vector<BindingKey> provisioned = generateProvisionedKeys();
    const BindingKey query = *keyGen().as("coppia interrogata");

    FakeFetcher fetcher;
    AcceptingVerifier verifier;
    NullBindingsProvider fallback;
    OnlineBindingsProvider provider{fetcher, verifier, fallback};

    for (const auto& key : provisioned) {
        fetcher.put(provider.buildUrl(key), serializePbind("sig", buildSetForKey(key)));
    }

    const bool isProvisioned = containsKey(provisioned, query);

    // IFF: load carica un set se e solo se la coppia interrogata è provisionata
    // con corrispondenza ESATTA (Req 20.2: nessun fuzzy-match su versione o
    // piattaforma).
    const auto loaded = provider.load(query);
    RC_ASSERT(loaded.has_value() == isProvisioned);

    FakeBackend backend;
    HookGate gate{backend};

    if (!isProvisioned) {
        // Nessun set caricato => resolve non risolve alcun simbolo.
        RC_ASSERT(!provider.resolve(symbolFor(query)).has_value());

        // Instradando il binding (assente) attraverso il gate: nessun hook
        // tocca il backend e l'invariante resta 0 (Req 20.3, 20.4).
        gate.install(provider.resolve(symbolFor(query)), kDetour);
        RC_ASSERT(gate.installedCount() == 0u);
        RC_ASSERT(backend.installAttempts() == 0u);
        RC_ASSERT(backend.installedCount() == 0u);
    } else {
        // Caso di copertura: coppia provisionata => load esatto e resolve ok.
        RC_ASSERT(sameKey(loaded->key(), query));
        RC_ASSERT(provider.resolve(symbolFor(query)).has_value());
        gate.install(provider.resolve(symbolFor(query)), kDetour);
        RC_ASSERT(gate.installedCount() == 1u);
    }

    // INVARIANTE Req 20.4: zero hook su indirizzi non risolti, sempre.
    RC_ASSERT(gate.hooksOnUnresolvedAddresses() == 0u);
}
