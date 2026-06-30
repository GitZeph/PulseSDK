// tests/binding_resolved_iff_verified_property_test.cpp
// Feature: pulse-gd-integration, Property 4 — Binding risolto se e solo se verificato.
// Validates: Requirements 4.3 (Requisiti 4.3)
//
// Property 4 (design.md sezione 4.1 / tasks.md, task 3.10): "mix di voci
// verificate/non verificate → `resolved` iff verificato".
//
// Il requisito 4.3 formula l'invariante come: "WHEN the Bindings_System resolves
// a function for a verified (GD_Version, Target_Platform) pair, THE Bindings_System
// SHALL mark each binding whose address has been verified against the real
// Geometry Dash binary as resolved, and SHALL leave all other (unverified)
// bindings unresolved".
//
// Un indirizzo è **verificato e non-placeholder** (Req 4.2) sse, simultaneamente:
//   (1) l'offset è NON-ZERO;
//   (2) l'offset è DIVERSO dal sentinel/placeholder del set embedded;
//   (3) il PROLOGO all'indirizzo risolto è CONFORME alla firma registrata;
// e la voce DICHIARAVA la verifica offline (il campo `verified` del `.pbind`,
// mappato sul `resolved` in ingresso del FunctionBinding).
//
// Su input randomizzati con RapidCheck (≥100 iterazioni per default) generiamo
// un `BindingSet` con un MIX di voci che variano lungo tutte le dimensioni
// rilevanti — categoria dell'offset (zero / sentinel / valido), claim di
// verifica (true/false), conformità del prologo (true/false) — e verifichiamo,
// attraverso `verify_binding_set` (la stessa logica usata da
// `EmbeddedBindingsProvider::addVerifiedSet`), che per OGNI voce risultante:
//
//   resolved == (claimedVerified ∧ offset≠0 ∧ offset≠sentinel ∧ prologoConforme)
//
// cioè `resolved` è vero SE E SOLO SE la voce è genuinamente verificata.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "bindings/binding_verifier.hpp"
#include "bindings/bindings.hpp"

namespace {

using pulse::loader::bindings::BindingKey;
using pulse::loader::bindings::BindingSet;
using pulse::loader::bindings::FunctionBinding;
using pulse::loader::bindings::GdVersion;
using pulse::loader::bindings::IPrologueVerifier;
using pulse::loader::bindings::kPlaceholderSentinel;
using pulse::loader::bindings::Signature;
using pulse::loader::bindings::verify_binding_set;

// Verificatore di prologo guidato da una tabella per-simbolo: risponde
// "conforme" SSE la tabella lo indica per quel simbolo. Permette di generare un
// MIX in cui ogni voce ha una conformità del prologo scelta indipendentemente,
// esercitando la dimensione (3) della verifica (Req 4.2/4.3).
class TablePrologueVerifier final : public IPrologueVerifier {
public:
    explicit TablePrologueVerifier(std::unordered_map<std::string, bool> conforms)
        : conforms_(std::move(conforms)) {}

    bool prologueMatchesSignature(const FunctionBinding& binding) const override {
        const auto it = conforms_.find(binding.symbol);
        return it != conforms_.end() && it->second;
    }

private:
    std::unordered_map<std::string, bool> conforms_;
};

// Categorie di offset che il predicato di verifica distingue (Req 4.2).
enum class OffsetCategory { Zero, Sentinel, Valid };

// Sceglie un offset concreto per la categoria. Per "Valid" genera un valore
// non-zero e diverso dal sentinel (lo spazio dei valori genuinamente risolvibili).
std::uintptr_t pickOffset(OffsetCategory cat) {
    switch (cat) {
        case OffsetCategory::Zero:
            return 0;
        case OffsetCategory::Sentinel:
            return kPlaceholderSentinel;
        case OffsetCategory::Valid: {
            // 1 .. sentinel-1: sempre non-zero e != sentinel.
            const auto v = *rc::gen::inRange<std::uintptr_t>(
                                1, kPlaceholderSentinel)
                                .as("offset valido (non-zero, != sentinel)");
            return v;
        }
    }
    return 0;
}

}  // namespace

// ===========================================================================
// Property 4 — su un mix di voci, `resolved` è vero SSE la voce è genuinamente
// verificata (claim verificato ∧ offset non-zero ∧ offset != sentinel ∧
// prologo conforme alla firma).
// Feature: pulse-gd-integration, Property 4. Validates: Requirements 4.3.
// ===========================================================================
RC_GTEST_PROP(Property4BindingResolvedIffVerified,
              ResolvedIffGenuinelyVerified,
              ()) {
    const int major = *rc::gen::inRange(0, 100).as("gd_version.major");
    const int minor = *rc::gen::inRange(0, 100000).as("gd_version.minor");
    const std::string platform =
        *rc::gen::element<std::string>("macos-arm64", "windows-x64",
                                       "android-arm64", "ios-arm64")
             .as("platform");

    BindingSet set{BindingKey{GdVersion{major, minor}, platform}};

    // Stato atteso per ciascuna voce, indicizzato per simbolo univoco.
    struct Expectation {
        bool claimedVerified;
        OffsetCategory offsetCategory;
        bool prologueConforms;
    };
    std::unordered_map<std::string, Expectation> expectations;
    std::unordered_map<std::string, bool> prologueTable;

    const std::size_t count =
        *rc::gen::inRange<std::size_t>(1, 16).as("numero di voci");
    for (std::size_t i = 0; i < count; ++i) {
        // Simbolo univoco per voce: garantisce un mapping 1:1 con la tabella del
        // prologo e con le attese, così il MIX è osservabile per-voce.
        const std::string symbol = "Sym_" + std::to_string(i);

        const bool claimedVerified =
            *rc::gen::arbitrary<bool>().as("claim di verifica (.pbind verified)");
        const OffsetCategory offsetCategory =
            *rc::gen::element(OffsetCategory::Zero, OffsetCategory::Sentinel,
                              OffsetCategory::Valid)
                 .as("categoria offset");
        const bool prologueConforms =
            *rc::gen::arbitrary<bool>().as("prologo conforme alla firma");

        FunctionBinding fn;
        fn.symbol = symbol;
        fn.address = pickOffset(offsetCategory);
        fn.signature = Signature{"bool", {symbol + "*"}};
        // Il `resolved` in ingresso codifica il claim di verifica offline.
        fn.resolved = claimedVerified;
        set.add(std::move(fn));

        expectations.emplace(
            symbol, Expectation{claimedVerified, offsetCategory, prologueConforms});
        prologueTable.emplace(symbol, prologueConforms);
    }

    TablePrologueVerifier verifier{prologueTable};
    const BindingSet verified = verify_binding_set(set, verifier);

    // La chiave è preservata e nessuna voce è aggiunta/rimossa.
    RC_ASSERT(verified.key() == set.key());
    RC_ASSERT(verified.functions().size() == set.functions().size());

    // Invariante "iff": per ogni voce, resolved sse genuinamente verificata.
    for (const auto& fn : verified.functions()) {
        const auto it = expectations.find(fn.symbol);
        RC_ASSERT(it != expectations.end());
        const Expectation& e = it->second;

        const bool genuinelyVerified =
            e.claimedVerified &&
            e.offsetCategory == OffsetCategory::Valid &&  // non-zero ∧ != sentinel
            e.prologueConforms;

        RC_ASSERT(fn.resolved == genuinelyVerified);
    }
}
