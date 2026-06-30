// tests/pbind_malformed_property_test.cpp
// Feature: pulse-gd-integration, Property 7 — Gestione sicura di `.pbind` malformati.
// Validates: Requirements 4.7 (Requisiti 4.7)
//
// Property 7 (design.md sezione 4.3 / tasks.md, task 3.8): "contenuto malformato
// → errore con causa, set embedded ancora caricabile, processo non terminato".
//
// Il requisito 4.7 formula l'invariante come: "IF a Binding_Set_File is
// malformed, THEN THE Bindings_System SHALL reject the file with an error
// indicating that the file is malformed and identifying the offending cause,
// SHALL keep the embedded binding set loadable, AND SHALL NOT terminate the
// host process".
//
// Strategia di test (RapidCheck, ≥100 iterazioni per default):
//   - Property 7a (malformazioni strutturate, garantite): a partire da un
//     `.pbind` VALIDO generato casualmente, applichiamo una mutazione scelta a
//     caso GARANTITA malformante (header obbligatorio rimosso, valore non
//     valido, chiave sconosciuta, riga non "key = value", campo di funzione
//     mancante/non valido, ...) e verifichiamo che `parse_pbind`:
//        (a) NON lanci eccezioni e ritorni un risultato (nessuna terminazione);
//        (b) ritorni `error` valorizzato con un messaggio di causa NON vuoto;
//        (c) lasci comunque caricabile il set embedded (provider indipendente).
//   - Property 7b (fuzz arbitrario): su contenuti pseudo-casuali arbitrari
//     verifichiamo la robustezza — `parse_pbind` non lancia mai e, quando
//     ritorna un errore, la causa è non vuota; il set embedded resta caricabile.
//
// Il parser non ha stato globale: per ribadire l'invariante "set embedded
// ancora caricabile" costruiamo un `EmbeddedBindingsProvider` indipendente
// dopo ogni parse e verifichiamo che la coppia embedded carichi e risolva
// `MenuLayer::init`.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bindings/bindings.hpp"
#include "bindings/embedded_bindings_provider.hpp"
#include "bindings/pbind_format.hpp"

namespace {

using pulse::loader::bindings::BindingKey;
using pulse::loader::bindings::BindingSet;
using pulse::loader::bindings::EmbeddedBindingsProvider;
using pulse::loader::bindings::FunctionBinding;
using pulse::loader::bindings::GdVersion;
using pulse::loader::bindings::parse_pbind;
using pulse::loader::bindings::PbindParseResult;
using pulse::loader::bindings::serialize_pbind;
using pulse::loader::bindings::Signature;

// Coppia chiave del set embedded del MVP (vedi embedded_bindings_provider.cpp):
// GD 2.2074 su windows-x64, con `MenuLayer::init` risolto. Usata per verificare
// che, dopo aver tentato di parsare un `.pbind` malformato, il set embedded
// resti caricabile e risolvibile (Req 4.7).
const BindingKey kEmbeddedKey{GdVersion{2, 2074}, "windows-x64"};

// Alfabeto di token "puliti" per symbol/return/params/platform: esclude i
// caratteri strutturali del formato (spazi, '=', ',', '#', newline) così la
// BASE generata è sempre un `.pbind` VALIDO prima della mutazione.
const std::string kTokenAlphabet =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_:*<>&";

rc::Gen<std::string> tokenGen() {
    return rc::gen::nonEmpty(
        rc::gen::container<std::string>(rc::gen::elementOf(kTokenAlphabet)));
}

// Genera un `BindingSet` VALIDO con almeno una funzione, così le mutazioni a
// livello di blocco [function] sono sempre applicabili.
BindingSet genValidSetWithFunction() {
    const int major = *rc::gen::inRange(0, 100).as("gd_version.major");
    const int minor = *rc::gen::inRange(0, 100000).as("gd_version.minor");
    const std::string platform = *tokenGen().as("platform");

    BindingSet set{BindingKey{GdVersion{major, minor}, platform}};

    const std::size_t count =
        *rc::gen::inRange<std::size_t>(1, 6).as("numero di funzioni");
    for (std::size_t i = 0; i < count; ++i) {
        FunctionBinding fn;
        fn.symbol = *tokenGen().as("symbol");
        fn.address = *rc::gen::arbitrary<std::uintptr_t>().as("offset");
        const std::string returnType = *tokenGen().as("return");
        const std::vector<std::string> params =
            *rc::gen::container<std::vector<std::string>>(tokenGen()).as("params");
        fn.signature = Signature{returnType, params};
        fn.resolved = *rc::gen::arbitrary<bool>().as("verified");
        set.add(std::move(fn));
    }
    return set;
}

// Suddivide su '\n' preservando il delimitatore finale come elemento vuoto, così
// la riunione con '\n' riproduce la stringa originale.
std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (true) {
        const auto nl = s.find('\n', pos);
        if (nl == std::string::npos) {
            lines.push_back(s.substr(pos));
            break;
        }
        lines.push_back(s.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            out += '\n';
        }
        out += lines[i];
    }
    return out;
}

// Indice della prima riga (trim grezzo sul prefisso) che inizia con `prefix`.
// Restituisce -1 se assente.
int findLineStartingWith(const std::vector<std::string>& lines,
                         const std::string& prefix) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        std::size_t b = l.find_first_not_of(" \t");
        if (b == std::string::npos) {
            continue;
        }
        if (l.compare(b, prefix.size(), prefix) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Indice della prima riga il cui contenuto (trim) è esattamente `[function]`.
int findFirstFunctionHeader(const std::vector<std::string>& lines) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        std::size_t b = l.find_first_not_of(" \t");
        if (b == std::string::npos) {
            continue;
        }
        std::size_t e = l.find_last_not_of(" \t\r");
        if (l.substr(b, e - b + 1) == "[function]") {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// All'interno del primo blocco [function], indice della prima riga che inizia
// con `keyPrefix` (es. "offset"). Restituisce -1 se assente.
int findFunctionKeyLine(const std::vector<std::string>& lines,
                        const std::string& keyPrefix) {
    const int header = findFirstFunctionHeader(lines);
    if (header < 0) {
        return -1;
    }
    for (std::size_t i = static_cast<std::size_t>(header) + 1; i < lines.size();
         ++i) {
        const std::string& l = lines[i];
        std::size_t b = l.find_first_not_of(" \t");
        if (b == std::string::npos) {
            continue;
        }
        if (l.compare(b, 11, "[function]") == 0) {
            break;  // inizio del blocco successivo
        }
        if (l.compare(b, keyPrefix.size(), keyPrefix) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Tipi di mutazione GARANTITE malformanti (rendono l'intero contenuto invalido).
enum class Mutation {
    DropPbindVersion,        // header obbligatorio mancante
    DropGdVersion,           // header obbligatorio mancante
    DropPlatform,            // header obbligatorio mancante
    BadPbindVersion,         // valore non intero
    UnsupportedPbindVersion, // versione del formato non supportata
    BadGdVersion,            // manca il separatore major.minor
    EmptyPlatform,           // valore vuoto non ammesso
    UnknownHeaderKey,        // chiave d'intestazione sconosciuta
    GarbageHeaderLine,       // riga non "key = value" e non "[function]"
    BadOffset,               // offset di funzione non numerico
    BadVerified,             // verified diverso da true/false
    DropSymbol,              // blocco [function] privo di 'symbol'
    EmptySymbol,             // symbol vuoto non ammesso
    UnknownFunctionKey,      // chiave di [function] sconosciuta
    kCount
};

// Applica `m` al `.pbind` VALIDO `valid`, restituendo un contenuto malformato.
std::string applyMutation(const std::string& valid, Mutation m) {
    std::vector<std::string> lines = splitLines(valid);

    switch (m) {
        case Mutation::DropPbindVersion: {
            const int i = findLineStartingWith(lines, "pbind_version");
            if (i >= 0) lines.erase(lines.begin() + i);
            break;
        }
        case Mutation::DropGdVersion: {
            const int i = findLineStartingWith(lines, "gd_version");
            if (i >= 0) lines.erase(lines.begin() + i);
            break;
        }
        case Mutation::DropPlatform: {
            const int i = findLineStartingWith(lines, "platform");
            if (i >= 0) lines.erase(lines.begin() + i);
            break;
        }
        case Mutation::BadPbindVersion: {
            const int i = findLineStartingWith(lines, "pbind_version");
            if (i >= 0) lines[i] = "pbind_version = notanumber";
            break;
        }
        case Mutation::UnsupportedPbindVersion: {
            const int i = findLineStartingWith(lines, "pbind_version");
            if (i >= 0) lines[i] = "pbind_version = 999";
            break;
        }
        case Mutation::BadGdVersion: {
            const int i = findLineStartingWith(lines, "gd_version");
            if (i >= 0) lines[i] = "gd_version = 22081";  // nessun '.'
            break;
        }
        case Mutation::EmptyPlatform: {
            const int i = findLineStartingWith(lines, "platform");
            if (i >= 0) lines[i] = "platform = ";
            break;
        }
        case Mutation::UnknownHeaderKey: {
            // Inserita nella regione d'intestazione (prima di ogni [function]).
            lines.insert(lines.begin() + 1, "frobnicate = bogus");
            break;
        }
        case Mutation::GarbageHeaderLine: {
            // Riga priva di '=' e diversa da "[function]" nella regione header.
            lines.insert(lines.begin() + 1, "this line has no equals sign");
            break;
        }
        case Mutation::BadOffset: {
            const int i = findFunctionKeyLine(lines, "offset");
            if (i >= 0) lines[i] = "offset = zznothex";
            break;
        }
        case Mutation::BadVerified: {
            const int i = findFunctionKeyLine(lines, "verified");
            if (i >= 0) lines[i] = "verified = maybe";
            break;
        }
        case Mutation::DropSymbol: {
            const int i = findFunctionKeyLine(lines, "symbol");
            if (i >= 0) lines.erase(lines.begin() + i);
            break;
        }
        case Mutation::EmptySymbol: {
            const int i = findFunctionKeyLine(lines, "symbol");
            if (i >= 0) lines[i] = "symbol = ";
            break;
        }
        case Mutation::UnknownFunctionKey: {
            const int header = findFirstFunctionHeader(lines);
            if (header >= 0) {
                lines.insert(lines.begin() + header + 1, "wibble = 1");
            }
            break;
        }
        case Mutation::kCount:
            break;
    }
    return joinLines(lines);
}

// Verifica che il set embedded resti caricabile e risolvibile (Req 4.7).
// Usa un provider indipendente: il parser è privo di stato globale, quindi
// qualunque tentativo di parse non può corromperlo.
void assertEmbeddedStillLoadable() {
    EmbeddedBindingsProvider provider;
    const auto loaded = provider.load(kEmbeddedKey);
    RC_ASSERT(loaded.has_value());
    const auto fn = provider.resolve("MenuLayer::init");
    RC_ASSERT(fn.has_value());
    RC_ASSERT(fn->resolved);
}

}  // namespace

// ===========================================================================
// Property 7a — un `.pbind` malformato è SEMPRE rifiutato con un errore la cui
// causa è non vuota, senza terminare il processo, e il set embedded resta
// caricabile. Feature: pulse-gd-integration, Property 7. Validates: Req 4.7.
// ===========================================================================
RC_GTEST_PROP(Property7MalformedPbind,
              MalformedContentRejectedWithCauseAndEmbeddedStillLoadable,
              ()) {
    // BASE valida generata casualmente (con almeno una funzione).
    const BindingSet base = genValidSetWithFunction();
    const std::string valid = serialize_pbind(base);

    // Precondizione: la BASE è effettivamente valida (altrimenti la mutazione
    // non testerebbe la transizione valido→malformato).
    const auto baseParsed = parse_pbind(valid);
    RC_PRE(baseParsed.ok());

    // Mutazione garantita malformante scelta a caso.
    const auto kind = static_cast<Mutation>(
        *rc::gen::inRange<int>(0, static_cast<int>(Mutation::kCount))
             .as("mutation kind"));
    const std::string malformed = applyMutation(valid, kind);

    // (a) Nessuna eccezione/terminazione: parse_pbind ritorna un risultato.
    const PbindParseResult result = parse_pbind(malformed);

    // (b) Esito di errore con causa non vuota (Req 4.7).
    RC_ASSERT(!result.ok());
    RC_ASSERT(!result.value.has_value());
    RC_ASSERT(result.error.has_value());
    RC_ASSERT(!result.error->message.empty());

    // (c) Il set embedded resta caricabile e risolvibile.
    assertEmbeddedStillLoadable();
}

// ===========================================================================
// Property 7b — su contenuto arbitrario (fuzz) `parse_pbind` non lancia mai;
// quando ritorna un errore, la causa è non vuota; il set embedded resta
// caricabile. Feature: pulse-gd-integration, Property 7. Validates: Req 4.7.
// ===========================================================================
RC_GTEST_PROP(Property7MalformedPbind,
              ArbitraryFuzzNeverThrowsAndKeepsEmbeddedLoadable,
              (const std::string& fuzz)) {
    // Nessuna eccezione/terminazione su input arbitrario (Req 4.7).
    const PbindParseResult result = parse_pbind(fuzz);

    // Robustezza: se rifiutato, la causa è non vuota; se accettato (caso raro
    // ma valido), il valore è presente. In entrambi i casi nessuna terminazione.
    if (!result.ok()) {
        RC_ASSERT(result.error.has_value());
        RC_ASSERT(!result.error->message.empty());
        RC_ASSERT(!result.value.has_value());
    } else {
        RC_ASSERT(result.value.has_value());
        RC_ASSERT(!result.error.has_value());
    }

    // Il set embedded resta sempre caricabile e risolvibile (Req 4.7).
    assertEmbeddedStillLoadable();
}
