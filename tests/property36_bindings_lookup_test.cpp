// tests/property36_bindings_lookup_test.cpp
// Feature: pulse-sdk, Property 36 — Lookup dei bindings con corrispondenza
// esatta e nessun hook su indirizzi non risolti.
// Validates: Requirements 20.1, 20.2, 20.3, 20.4 (Requisiti 20.1–20.4)
//
// Property 36 (design.md): "Per ogni coppia (GD_Version, piattaforma) e per
// ogni insieme di simboli, `resolve` restituisce un binding se e solo se la
// coppia esatta è indicizzata e il simbolo è risolto; il numero di hook
// installati su indirizzi non risolti è sempre 0."
//
// Il test si articola in due parti, su input randomizzati con RapidCheck
// (≥100 iterazioni per default):
//
//   PARTE 1 — Lookup a corrispondenza ESATTA (Req 20.1, 20.2):
//     * BindingSet::resolve restituisce un valore SE E SOLO SE il simbolo
//       interrogato coincide esattamente (case-sensitive) con un simbolo
//       indicizzato; varianti vicine (case diverso, spazio finale, sottostringa,
//       suffisso) NON risolvono (nessun fuzzy-match).
//     * IBindingsProvider/EmbeddedBindingsProvider::load restituisce il set SE
//       E SOLO SE la coppia (GD_Version, piattaforma) coincide esattamente;
//       near-miss di versione/piattaforma (incluse differenze di solo case)
//       non caricano nulla.
//
//   PARTE 2 — Nessun hook su indirizzi non risolti (Req 20.3, 20.4):
//     * Instradando un insieme randomizzato di binding (alcuni risolti, alcuni
//       non risolti, alcuni assenti) attraverso HookGate/gate_install con un
//       FakeBackend, l'invariante numeroHookSuIndirizziNonRisolti == 0 vale
//       sempre: il backend riceve install SOLO per i binding risolti; quelli
//       non risolti/assenti sono bloccati prima di toccare il backend.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bindings/bindings.hpp"
#include "bindings/embedded_bindings_provider.hpp"
#include "hooking/fake_backend.hpp"
#include "hooking/hook_gate.hpp"

namespace {

using pulse::hooking::FakeBackend;
using pulse::hooking::HookGate;
using pulse::loader::bindings::BindingKey;
using pulse::loader::bindings::BindingSet;
using pulse::loader::bindings::EmbeddedBindingsProvider;
using pulse::loader::bindings::FunctionBinding;
using pulse::loader::bindings::GdVersion;
using pulse::loader::bindings::Signature;

int g_detour = 0;
void* const kDetour = &g_detour;

// Coppia chiave del set embedded del MVP (vedi embedded_bindings_provider.cpp).
const BindingKey kMvpKey{GdVersion{2, 2074}, "windows-x64"};

bool containsSymbol(const std::vector<std::string>& symbols,
                    const std::string& query) {
    return std::find(symbols.begin(), symbols.end(), query) != symbols.end();
}

// Costruisce un BindingSet risolto a partire da una chiave e da una lista di
// simboli univoci; assegna a ciascuno un indirizzo non nullo e distinto.
BindingSet buildResolvedSet(const BindingKey& key,
                            const std::vector<std::string>& symbols) {
    BindingSet set{key};
    std::uintptr_t addr = 0x1000;
    for (const auto& sym : symbols) {
        FunctionBinding fn;
        fn.symbol = sym;
        fn.address = addr;
        fn.signature = Signature{"void", {}};
        fn.resolved = true;
        set.add(std::move(fn));
        addr += 0x40;
    }
    return set;
}

}  // namespace

// ===========================================================================
// PARTE 1a — BindingSet::resolve: corrispondenza esatta del simbolo (iff).
// Feature: pulse-sdk, Property 36. Validates: Requirements 20.1, 20.2.
// ===========================================================================
RC_GTEST_PROP(Property36BindingsLookup,
              ResolveReturnsBindingIffExactSymbolMatch,
              ()) {
    // Simboli indicizzati: lista univoca e non vuota, alfabeto ristretto
    // (lettere minuscole a..j) così le mutazioni near-miss sono significative.
    const auto symbolGen = rc::gen::nonEmpty(
        rc::gen::container<std::string>(rc::gen::inRange<char>('a', 'k')));
    const auto symbols =
        *rc::gen::nonEmpty(rc::gen::unique<std::vector<std::string>>(symbolGen))
             .as("simboli indicizzati");

    const BindingSet set = buildResolvedSet(kMvpKey, symbols);

    // (1) Ogni simbolo indicizzato risolve esattamente a sé stesso.
    for (const auto& sym : symbols) {
        const auto hit = set.resolve(sym);
        RC_ASSERT(hit.has_value());
        RC_ASSERT(hit->symbol == sym);
        RC_ASSERT(hit->resolved);
    }

    // (2) IFF: per un simbolo interrogato qualsiasi, resolve ha valore se e
    //     solo se il simbolo è esattamente tra quelli indicizzati.
    const auto query = *symbolGen.as("simbolo interrogato");
    RC_ASSERT(set.resolve(query).has_value() == containsSymbol(symbols, query));

    // (3) Near-miss espliciti costruiti da un simbolo indicizzato: non devono
    //     risolvere (no fuzzy-match), salvo coincidano per caso con un simbolo
    //     realmente indicizzato.
    const auto& base = symbols.front();

    std::string upper = base;
    for (char& c : upper) c = static_cast<char>(std::toupper(c));
    if (!containsSymbol(symbols, upper)) {
        RC_ASSERT(!set.resolve(upper).has_value());  // case diverso
    }

    const std::string trailingSpace = base + " ";
    if (!containsSymbol(symbols, trailingSpace)) {
        RC_ASSERT(!set.resolve(trailingSpace).has_value());  // spazio finale
    }

    const std::string suffixed = base + "z";
    if (!containsSymbol(symbols, suffixed)) {
        RC_ASSERT(!set.resolve(suffixed).has_value());  // suffisso aggiunto
    }

    if (base.size() > 1) {
        const std::string prefix = base.substr(0, base.size() - 1);
        if (!containsSymbol(symbols, prefix)) {
            RC_ASSERT(!set.resolve(prefix).has_value());  // sottostringa
        }
    }
}

// ===========================================================================
// PARTE 1b — EmbeddedBindingsProvider::load: corrispondenza esatta della
// coppia (GD_Version, piattaforma) (iff). Su match esatto, resolve del simbolo
// indicizzato funziona e i near-miss di simbolo no.
// Feature: pulse-sdk, Property 36. Validates: Requirements 20.1, 20.2.
// ===========================================================================
RC_GTEST_PROP(Property36BindingsLookup,
              LoadReturnsSetIffExactKeyMatch,
              ()) {
    const auto major = *rc::gen::inRange(0, 6).as("GD major");
    const auto minor = *rc::gen::inRange(0, 3000).as("GD minor");
    const auto platform = *rc::gen::element<std::string>(
        "windows-x64", "Windows-X64", "windows-x86", "macos", "macos-arm64",
        "android-arm64", "android-armv7", "ios-arm64", "linux-x64", "")
        .as("platformId");

    const BindingKey key{GdVersion{major, minor}, platform};
    const bool isExactMvp = (key == kMvpKey);

    EmbeddedBindingsProvider provider;
    const auto loaded = provider.load(key);

    // IFF: load carica il set se e solo se la coppia coincide esattamente.
    RC_ASSERT(loaded.has_value() == isExactMvp);

    if (isExactMvp) {
        RC_ASSERT(loaded->key() == kMvpKey);

        // Sul set caricato, il simbolo indicizzato risolve esattamente...
        const auto resolved = provider.resolve("MenuLayer::init");
        RC_ASSERT(resolved.has_value());
        RC_ASSERT(resolved->resolved);
        RC_ASSERT(resolved->address != 0u);

        // ...mentre i near-miss del simbolo non risolvono (no fuzzy-match).
        RC_ASSERT(!provider.resolve("MenuLayer::Init").has_value());
        RC_ASSERT(!provider.resolve("menulayer::init").has_value());
        RC_ASSERT(!provider.resolve("MenuLayer::init ").has_value());
        RC_ASSERT(!provider.resolve("init").has_value());
        RC_ASSERT(!provider.resolve("MenuLayer::init2").has_value());
    }
}

// ===========================================================================
// PARTE 2 — Nessun hook su indirizzi non risolti (Req 20.3, 20.4).
// Instrada binding randomizzati (risolti / non risolti / assenti) attraverso
// HookGate con un FakeBackend e verifica l'invariante
// numeroHookSuIndirizziNonRisolti == 0 su tutti gli input.
// Feature: pulse-sdk, Property 36. Validates: Requirements 20.3, 20.4.
// ===========================================================================
RC_GTEST_PROP(Property36BindingsLookup,
              ZeroHooksInstalledOnUnresolvedOrAbsentBindings,
              ()) {
    // Ogni elemento: (present, resolved). present=false modella un binding
    // assente (nessuna corrispondenza esatta nel set, optional vuoto).
    const auto specs =
        *rc::gen::container<std::vector<std::pair<bool, bool>>>(
             rc::gen::pair(rc::gen::arbitrary<bool>(),
                           rc::gen::arbitrary<bool>()))
             .as("binding (present, resolved)");

    FakeBackend backend;
    HookGate gate{backend};

    std::size_t expectedInstalls = 0;
    std::vector<std::uintptr_t> unresolvedAddresses;

    std::uintptr_t addr = 0x1000;  // non nullo, distinto per indice
    for (const auto& [present, resolved] : specs) {
        const std::uintptr_t target = addr;
        addr += 0x40;

        if (!present) {
            // Binding assente: optional vuoto -> bloccato dal gate.
            gate.install(std::optional<FunctionBinding>{}, kDetour);
            continue;
        }

        FunctionBinding fn;
        fn.symbol = "Sym::f";
        fn.address = target;
        fn.signature = Signature{"void", {}};
        fn.resolved = resolved;

        gate.install(std::optional<FunctionBinding>{fn}, kDetour);

        if (resolved) {
            ++expectedInstalls;
        } else {
            unresolvedAddresses.push_back(target);
        }
    }

    // INVARIANTE Req 20.4: zero hook su indirizzi non risolti, sempre.
    RC_ASSERT(gate.hooksOnUnresolvedAddresses() == 0u);

    // Il backend riceve un tentativo di install SOLO per i binding risolti:
    // quelli non risolti/assenti sono bloccati prima di toccarlo.
    RC_ASSERT(backend.installAttempts() == expectedInstalls);
    RC_ASSERT(backend.installedCount() == expectedInstalls);
    RC_ASSERT(gate.installedCount() == expectedInstalls);
    RC_ASSERT(gate.blockedUnresolvedCount() == specs.size() - expectedInstalls);

    // Nessun indirizzo non risolto risulta installato sul backend.
    for (const auto a : unresolvedAddresses) {
        RC_ASSERT(!backend.isInstalled(a));
    }
}
