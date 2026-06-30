// tests/pbind_roundtrip_property_test.cpp
// Feature: pulse-gd-integration, Property 6 — Round-trip del Binding_Set_File `.pbind`.
// Validates: Requirements 4.6 (Requisiti 4.6)
//
// Property 6 (design.md sezione 4.3 / tasks.md, task 3.7): "parse→serialize→parse
// uguale a livello di campo (version, platform, per funzione symbol/offset/
// signature) senza voci aggiunte/rimosse/alterate".
//
// Il requisito 4.6 formula l'invariante come: "WHEN a Binding_Set_File is
// serialized and then parsed, THE Bindings_System SHALL produce a binding set
// equal to the original at field level — equal GD_Version, equal Target_Platform,
// and for each function an equal identifier, offset, and signature — with no
// added, removed, or altered entries".
//
// Su input randomizzati con RapidCheck (≥100 iterazioni per default) generiamo
// un `BindingSet` arbitrario VALIDO (versione, piattaforma, e un insieme di
// funzioni con symbol/offset/signature/verified) e verifichiamo che:
//   (a) `parse_pbind(serialize_pbind(set))` riesca SEMPRE (nessun errore);
//   (b) il `BindingSet` ottenuto sia uguale all'originale a livello di campo —
//       stessa coppia (GD_Version, piattaforma), stesse funzioni (per ciascuna
//       symbol/offset/signature/verified), senza voci aggiunte/rimosse/alterate;
//   (c) il round-trip sia idempotente: una seconda iterazione
//       parse→serialize→parse produca lo stesso risultato a livello di campo.
//
// Il serializer canonicalizza l'ordine delle funzioni per `symbol`; per un
// confronto robusto all'ordine confrontiamo entrambi i lati dopo lo stesso
// ordinamento canonico (stable_sort per `symbol`), così l'invariante
// "stessi campi, nessuna voce aggiunta/rimossa/alterata" è verificata
// indipendentemente dall'ordine di inserimento.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bindings/bindings.hpp"
#include "bindings/pbind_format.hpp"

namespace {

using pulse::loader::bindings::BindingKey;
using pulse::loader::bindings::BindingSet;
using pulse::loader::bindings::FunctionBinding;
using pulse::loader::bindings::GdVersion;
using pulse::loader::bindings::parse_pbind;
using pulse::loader::bindings::serialize_pbind;
using pulse::loader::bindings::Signature;

// Alfabeto di token "puliti": esclude deliberatamente i caratteri che il
// formato testuale `.pbind` usa come struttura o che il parser normalizza —
// spazi/tab/CR/LF (trim + line-based), '=' (separatore key/value), ',' (lista
// di parametri) e '#' (commento). Sono ammessi i caratteri che compaiono nei
// simboli e nelle firme reali ("MenuLayer::init", "MenuLayer*", "GJGameLevel*").
// Generare token entro questo spazio mantiene l'input nello spazio VALIDO del
// formato, dove il round-trip a livello di campo deve valere (Req 4.6).
const std::string kTokenAlphabet =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_:*<>&";

// Generatore di token non vuoto entro `kTokenAlphabet`.
rc::Gen<std::string> tokenGen() {
    return rc::gen::nonEmpty(
        rc::gen::container<std::string>(rc::gen::elementOf(kTokenAlphabet)));
}

// Confronto a livello di campo come definito dal Req 4.6, indipendente
// dall'ordine: stessa chiave (GD_Version + piattaforma) e, dopo ordinamento
// canonico per `symbol`, stesse funzioni (symbol/offset/signature/verified)
// senza voci aggiunte/rimosse/alterate.
bool fieldEqualCanonical(const BindingSet& a, const BindingSet& b) {
    if (!(a.key() == b.key())) {
        return false;
    }
    if (a.functions().size() != b.functions().size()) {
        return false;  // voci aggiunte o rimosse
    }
    std::vector<FunctionBinding> fa = a.functions();
    std::vector<FunctionBinding> fb = b.functions();
    const auto bySymbol = [](const FunctionBinding& x, const FunctionBinding& y) {
        return x.symbol < y.symbol;
    };
    std::stable_sort(fa.begin(), fa.end(), bySymbol);
    std::stable_sort(fb.begin(), fb.end(), bySymbol);
    for (std::size_t i = 0; i < fa.size(); ++i) {
        if (!(fa[i] == fb[i])) {  // FunctionBinding::operator== confronta tutti i campi
            return false;         // voce alterata
        }
    }
    return true;
}

// Genera un `BindingSet` arbitrario VALIDO entro lo spazio del formato `.pbind`.
BindingSet genBindingSet() {
    const int major = *rc::gen::inRange(0, 100).as("gd_version.major");
    const int minor = *rc::gen::inRange(0, 100000).as("gd_version.minor");
    const std::string platform = *tokenGen().as("platform");

    BindingSet set{BindingKey{GdVersion{major, minor}, platform}};

    const std::size_t count =
        *rc::gen::inRange<std::size_t>(0, 12).as("numero di funzioni");
    for (std::size_t i = 0; i < count; ++i) {
        FunctionBinding fn;
        fn.symbol = *tokenGen().as("symbol");
        fn.address = *rc::gen::arbitrary<std::uintptr_t>().as("offset");
        const std::string returnType = *tokenGen().as("return");
        const std::vector<std::string> params =
            *rc::gen::container<std::vector<std::string>>(tokenGen())
                 .as("params");
        fn.signature = Signature{returnType, params};
        fn.resolved = *rc::gen::arbitrary<bool>().as("verified");
        set.add(std::move(fn));
    }
    return set;
}

}  // namespace

// ===========================================================================
// Property 6 — serialize→parse riproduce il `BindingSet` originale a livello di
// campo (version, platform, per funzione symbol/offset/signature) senza voci
// aggiunte/rimosse/alterate; il round-trip è inoltre idempotente.
// Feature: pulse-gd-integration, Property 6. Validates: Requirements 4.6.
// ===========================================================================
RC_GTEST_PROP(Property6PbindRoundTrip,
              SerializeThenParseEqualsOriginalAtFieldLevel,
              ()) {
    const BindingSet original = genBindingSet();

    // (a) parse(serialize(original)) riesce sempre: lo spazio generato è valido.
    const std::string serialized = serialize_pbind(original);
    const auto first = parse_pbind(serialized);
    RC_ASSERT(first.ok());
    RC_ASSERT(first.value.has_value());

    // (b) uguaglianza a livello di campo: stessa coppia + stesse funzioni,
    //     nessuna voce aggiunta/rimossa/alterata (Req 4.6).
    RC_ASSERT(fieldEqualCanonical(original, *first.value));

    // Conteggio esplicito: nessuna voce aggiunta o rimossa.
    RC_ASSERT(first.value->functions().size() == original.functions().size());

    // (c) idempotenza del round-trip: parse→serialize→parse stabile a livello
    //     di campo (e a livello di byte sulla forma canonica).
    const std::string reserialized = serialize_pbind(*first.value);
    const auto second = parse_pbind(reserialized);
    RC_ASSERT(second.ok());
    RC_ASSERT(second.value.has_value());
    RC_ASSERT(fieldEqualCanonical(*first.value, *second.value));
    RC_ASSERT(fieldEqualCanonical(original, *second.value));
    RC_ASSERT(reserialized == serialized);  // forma canonica stabile
}
