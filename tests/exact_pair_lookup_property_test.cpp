// tests/exact_pair_lookup_property_test.cpp
// Feature: pulse-gd-integration, Property 5 — Lookup esatto della coppia e zero
// hook su indirizzi non risolti.
// Validates: Requirements 4.4, 4.5, 5.2 (Requisiti 4.4, 4.5, 5.2)
//
// Property 5 (design.md sezione 4 / tasks.md, task 3.11): "Per ogni coppia
// (GD_Version, Target_Platform) e per ogni insieme di simboli, il provider
// restituisce un binding SE E SOLO SE la coppia esatta è indicizzata e il
// simbolo è risolto (nessun fuzzy-match né sostituzione da versione/piattaforma
// diversa), e il numero di hook installati su indirizzi non risolti è sempre 0."
//
// *(Estende pulse-sdk Property 36 al PROVIDER REALE — EmbeddedBindingsProvider
// con `addVerifiedSet` che affianca i `.pbind` reali e applica la verifica
// "resolved sse verificato" del task 3.9 — e alla DETECTION ESATTA: `load`
// chiede i bindings per la coppia (GD_Version, Target_Platform) ESATTA rilevata,
// senza alcun fallback a versione/piattaforma diverse, Req 5.2.)*
//
// Requisiti coperti:
//   - 4.4: nessuna corrispondenza per una coppia priva di `.pbind` esatto;
//          nessuna sostituzione/fuzzy-match da una coppia diversa.
//   - 5.2: i bindings sono richiesti per la coppia ESATTA, senza fallback.
//   - 4.5: zero hook installati su indirizzi non risolti.
//
// Su input randomizzati con RapidCheck (≥100 iterazioni per default) si
// approvvigiona un `EmbeddedBindingsProvider` REALE con un insieme di coppie
// (GD_Version, piattaforma) DISTINTE, ciascuna con un MIX di bindings verificati
// e non verificati (offset zero / sentinel / valido, claim di verifica
// true/false). I simboli sono GLOBALMENTE univoci per coppia (prefisso con
// l'indice della coppia), così l'assenza di sostituzione tra coppie è
// osservabile. Le versioni generate hanno `major >= 3`, quindi non collidono
// MAI con il set embedded del MVP (2.2074, "windows-x64").

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "bindings/bindings.hpp"
#include "bindings/binding_verifier.hpp"
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
using pulse::loader::bindings::kPlaceholderSentinel;
using pulse::loader::bindings::Signature;

int g_detour = 0;
void* const kDetour = &g_detour;

// Chiave embedded del MVP (vedi embedded_bindings_provider.cpp): le coppie
// generate hanno major >= 3 e non possono coincidere con questa (major 2).
const BindingKey kMvpKey{GdVersion{2, 2074}, "windows-x64"};

// Categoria di un binding generato lungo le dimensioni che il verificatore
// "resolved sse verificato" (Req 4.2/4.3) distingue.
enum class Cat {
    VerifiedValid,        // claim=true, offset valido        -> resolved == true
    UnverifiedClaimFalse, // claim=false, offset valido       -> resolved == false
    UnverifiedZero,       // claim=true, offset 0             -> resolved == false
    UnverifiedSentinel,   // claim=true, offset sentinel      -> resolved == false
};

// Descrittore atteso di un singolo binding indicizzato.
struct Entry {
    std::uintptr_t address;
    bool resolved;  // valore atteso DOPO la verifica (addVerifiedSet)
};

// Una coppia approvvigionata: chiave esatta + mappa simbolo->atteso.
struct Provisioned {
    BindingKey key;
    std::map<std::string, Entry> symbols;
};

using KeyTuple = std::tuple<int, int, std::string>;

BindingKey toKey(const KeyTuple& t) {
    return BindingKey{GdVersion{std::get<0>(t), std::get<1>(t)}, std::get<2>(t)};
}

// Generatore di una coppia (major>=3) — non collide mai con il set embedded.
rc::Gen<KeyTuple> keyGen() {
    return rc::gen::tuple(
        rc::gen::inRange(3, 12),
        rc::gen::inRange(0, 10000),
        rc::gen::element<std::string>("macos-arm64", "windows-x64",
                                      "android-arm64", "ios-arm64", "macos-x64",
                                      "android-armv7"));
}

// Approvvigiona un EmbeddedBindingsProvider REALE su un insieme di coppie
// distinte generate, restituendo le attese per-coppia. `nextValidAddr` assegna
// indirizzi validi GLOBALMENTE univoci (non-zero, distanti dal sentinel), così
// nessun hook reale entra in collisione (AlreadyHooked) instradandolo sul backend.
std::vector<Provisioned> provision(EmbeddedBindingsProvider& provider) {
    // Coppie DISTINTE (uniche) generate casualmente.
    const auto keyTuples =
        *rc::gen::nonEmpty(rc::gen::unique<std::vector<KeyTuple>>(keyGen()))
             .as("coppie (GD_Version, piattaforma) distinte");

    std::vector<Provisioned> result;
    std::uintptr_t nextAddr = 0x1000;  // contatore globale di indirizzi validi

    for (std::size_t i = 0; i < keyTuples.size(); ++i) {
        const BindingKey key = toKey(keyTuples[i]);
        BindingSet set{key};
        Provisioned prov;
        prov.key = key;

        const std::size_t m =
            *rc::gen::inRange<std::size_t>(1, 7).as("numero di simboli");
        for (std::size_t j = 0; j < m; ++j) {
            // Simbolo GLOBALMENTE univoco per coppia: nessun simbolo è condiviso
            // tra coppie diverse, rendendo osservabile l'assenza di sostituzione.
            const std::string symbol =
                "K" + std::to_string(i) + "_Sym" + std::to_string(j);

            const Cat cat = *rc::gen::element(
                                 Cat::VerifiedValid, Cat::UnverifiedClaimFalse,
                                 Cat::UnverifiedZero, Cat::UnverifiedSentinel)
                                 .as("categoria binding");

            std::uintptr_t address = 0;
            bool claim = false;
            bool expectedResolved = false;
            switch (cat) {
                case Cat::VerifiedValid:
                    address = nextAddr;
                    nextAddr += 0x40;
                    claim = true;
                    expectedResolved = true;
                    break;
                case Cat::UnverifiedClaimFalse:
                    address = nextAddr;
                    nextAddr += 0x40;
                    claim = false;
                    expectedResolved = false;
                    break;
                case Cat::UnverifiedZero:
                    address = 0;
                    claim = true;
                    expectedResolved = false;
                    break;
                case Cat::UnverifiedSentinel:
                    address = kPlaceholderSentinel;
                    claim = true;
                    expectedResolved = false;
                    break;
            }

            FunctionBinding fn;
            fn.symbol = symbol;
            fn.address = address;
            fn.signature = Signature{"bool", {symbol + "*"}};
            // `resolved` in ingresso codifica il claim di verifica offline
            // (campo `verified` del `.pbind`); addVerifiedSet lo ricalcola.
            fn.resolved = claim;
            set.add(std::move(fn));

            prov.symbols.emplace(symbol, Entry{address, expectedResolved});
        }

        // Provider REALE: affianca il set con la verifica "resolved sse
        // verificato" (TrustingPrologueVerifier su host/CI) — task 3.9.
        provider.addVerifiedSet(set);
        result.push_back(std::move(prov));
    }

    return result;
}

bool isProvisioned(const std::vector<Provisioned>& prov, const BindingKey& key) {
    for (const auto& p : prov) {
        if (p.key == key) return true;
    }
    return false;
}

}  // namespace

// ===========================================================================
// Property 5a — Lookup ESATTO della coppia + del simbolo, senza sostituzione.
// Il provider reale restituisce un binding SSE la coppia esatta è indicizzata e
// il simbolo è esattamente presente; nessun fuzzy-match su versione/piattaforma/
// simbolo e nessuna sostituzione da una coppia diversa. Il `resolved` riflette
// la verifica (Req 4.2/4.3). (Estende pulse-sdk Property 36 al provider reale.)
// Feature: pulse-gd-integration, Property 5. Validates: Requirements 4.4, 5.2.
// ===========================================================================
RC_GTEST_PROP(Property5ExactPairLookup,
              ProviderReturnsBindingIffExactPairAndSymbolIndexed,
              ()) {
    EmbeddedBindingsProvider provider;
    const std::vector<Provisioned> prov = provision(provider);

    // (1) Per OGNI coppia approvvigionata: load carica esattamente quel set e
    //     resolve restituisce ogni simbolo indicizzato con il `resolved` atteso.
    for (const auto& p : prov) {
        const auto loaded = provider.load(p.key);
        RC_ASSERT(loaded.has_value());
        RC_ASSERT(loaded->key() == p.key);

        for (const auto& [symbol, entry] : p.symbols) {
            const auto r = provider.resolve(symbol);
            RC_ASSERT(r.has_value());            // coppia esatta + simbolo presente
            RC_ASSERT(r->symbol == symbol);
            RC_ASSERT(r->address == entry.address);
            RC_ASSERT(r->resolved == entry.resolved);  // resolved sse verificato
        }

        // (2) NESSUNA SOSTITUZIONE tra coppie: un simbolo che appartiene SOLO a
        //     un'altra coppia non risolve in questo set (simboli globalmente
        //     univoci per coppia). Verifica l'invariante "no fuzzy-match/sub".
        for (const auto& other : prov) {
            if (other.key == p.key) continue;
            for (const auto& [otherSymbol, _] : other.symbols) {
                RC_ASSERT(!provider.resolve(otherSymbol).has_value());
            }
        }

        // (3) Near-miss del SIMBOLO sul set corrente: nessuna risoluzione
        //     (case diverso, spazio finale, suffisso) salvo coincidenze reali.
        const std::string base = p.symbols.begin()->first;
        const std::string withSpace = base + " ";
        if (p.symbols.find(withSpace) == p.symbols.end()) {
            RC_ASSERT(!provider.resolve(withSpace).has_value());
        }
        const std::string suffixed = base + "Z";
        if (p.symbols.find(suffixed) == p.symbols.end()) {
            RC_ASSERT(!provider.resolve(suffixed).has_value());
        }
        std::string upper = base;
        for (char& c : upper) c = static_cast<char>(std::toupper(c));
        if (p.symbols.find(upper) == p.symbols.end()) {
            RC_ASSERT(!provider.resolve(upper).has_value());
        }
    }

    // (4) Near-miss della COPPIA (Req 4.4, 5.2): mutazioni della chiave di una
    //     coppia approvvigionata NON caricano nulla, salvo coincidano con
    //     un'altra coppia realmente approvvigionata. Nessun fallback.
    for (const auto& p : prov) {
        const BindingKey majorMiss{GdVersion{p.key.version.major + 100,
                                             p.key.version.minor},
                                   p.key.platformId};
        if (!isProvisioned(prov, majorMiss)) {
            RC_ASSERT(!provider.load(majorMiss).has_value());
        }

        const BindingKey minorMiss{GdVersion{p.key.version.major,
                                             p.key.version.minor + 100000},
                                   p.key.platformId};
        if (!isProvisioned(prov, minorMiss)) {
            RC_ASSERT(!provider.load(minorMiss).has_value());
        }

        const std::string altPlatform =
            p.key.platformId == "linux-x64" ? "freebsd-x64" : "linux-x64";
        const BindingKey platformMiss{p.key.version, altPlatform};
        if (!isProvisioned(prov, platformMiss)) {
            RC_ASSERT(!provider.load(platformMiss).has_value());
        }
    }
}

// ===========================================================================
// Property 5b — Zero hook su indirizzi NON risolti (Req 4.5).
// Instradando ogni binding risolto dal provider reale (più richieste per simboli
// assenti) attraverso HookGate + FakeBackend, l'invariante
// numeroHookSuIndirizziNonRisolti == 0 vale SEMPRE: il backend riceve un install
// SOLO per i binding `resolved`; i binding non verificati (resolved==false) e i
// simboli assenti sono bloccati prima di toccare il backend.
// Feature: pulse-gd-integration, Property 5. Validates: Requirements 4.5.
// ===========================================================================
RC_GTEST_PROP(Property5ExactPairLookup,
              ZeroHooksInstalledOnUnresolvedAddresses,
              ()) {
    EmbeddedBindingsProvider provider;
    const std::vector<Provisioned> prov = provision(provider);

    FakeBackend backend;
    HookGate gate{backend};

    std::size_t expectedInstalls = 0;
    std::vector<std::uintptr_t> unresolvedAddresses;

    for (const auto& p : prov) {
        const auto loaded = provider.load(p.key);
        RC_ASSERT(loaded.has_value());

        for (const auto& [symbol, entry] : p.symbols) {
            const auto r = provider.resolve(symbol);
            RC_ASSERT(r.has_value());
            // Instrada il binding risolto dal provider reale attraverso il gate.
            gate.install(r, kDetour);

            if (entry.resolved) {
                ++expectedInstalls;
            } else if (entry.address != 0u) {
                // Indirizzo non risolto (non-zero): non deve MAI risultare hookato.
                unresolvedAddresses.push_back(entry.address);
            }
        }

        // Richiesta per un simbolo ASSENTE (resolve -> nullopt): il gate blocca
        // senza toccare il backend (nessuna corrispondenza esatta).
        RC_ASSERT(!provider.resolve("K_absent_" + p.key.platformId).has_value());
        gate.install(provider.resolve("K_absent_" + p.key.platformId), kDetour);
    }

    // INVARIANTE (Req 4.5): zero hook su indirizzi non risolti, sempre.
    RC_ASSERT(gate.hooksOnUnresolvedAddresses() == 0u);

    // Il backend riceve un tentativo di install SOLO per i binding risolti.
    RC_ASSERT(backend.installAttempts() == expectedInstalls);
    RC_ASSERT(backend.installedCount() == expectedInstalls);
    RC_ASSERT(gate.installedCount() == expectedInstalls);

    // Nessun indirizzo non risolto risulta installato sul backend.
    for (const auto addr : unresolvedAddresses) {
        RC_ASSERT(!backend.isInstalled(addr));
    }
}
