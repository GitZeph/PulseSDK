// tests/pbind_format_test.cpp — unit test del parser/serializer `.pbind`
// (task 3.6, Req 4.1, 4.6, 4.7).
//
// Verifica:
//   * parsing del formato canonico (intestazione + blocchi [function]);
//   * round-trip parse→serialize→parse uguale a livello di campo (Req 4.6),
//     incluso l'ordinamento canonico per `symbol`;
//   * gestione sicura dei contenuti malformati: errore con causa, nessuna
//     eccezione/terminazione, set embedded ancora caricabile (Req 4.7).
//
// Test autonomo (nessuna dipendenza da GoogleTest) così da poter essere
// compilato ed eseguito direttamente sull'host; i property test P6/P7 (task
// 3.7/3.8) copriranno le proprietà universali con RapidCheck.

#include <iostream>
#include <string>
#include <string_view>

#include "bindings/bindings.hpp"
#include "bindings/embedded_bindings_provider.hpp"
#include "bindings/pbind_format.hpp"

namespace {

using namespace pulse::loader::bindings;

int g_failures = 0;
int g_checks = 0;

void checkImpl(bool cond, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::cerr << file << ":" << line << ": CHECK FAILED: " << expr << "\n";
    }
}

#define CHECK(cond) checkImpl((cond), #cond, __FILE__, __LINE__)

// Costruisce un BindingSet di esempio con due funzioni in ordine NON canonico.
BindingSet sampleSet() {
    BindingSet set{BindingKey{GdVersion{2, 2081}, "macos-arm64"}};

    FunctionBinding playLayerInit;
    playLayerInit.symbol = "PlayLayer::init";
    playLayerInit.address = 0x00B2C3D0;
    playLayerInit.signature = Signature{"bool", {"PlayLayer*", "GJGameLevel*"}};
    playLayerInit.resolved = false;
    set.add(playLayerInit);

    FunctionBinding menuLayerInit;
    menuLayerInit.symbol = "MenuLayer::init";
    menuLayerInit.address = 0x00A1B2C0;
    menuLayerInit.signature = Signature{"bool", {"MenuLayer*"}};
    menuLayerInit.resolved = true;
    set.add(menuLayerInit);

    return set;
}

// Confronto a livello di campo come definito dalla Req 4.6.
bool fieldEqual(const BindingSet& a, const BindingSet& b) {
    if (!(a.key() == b.key())) {
        return false;
    }
    if (a.functions().size() != b.functions().size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.functions().size(); ++i) {
        if (!(a.functions()[i] == b.functions()[i])) {
            return false;
        }
    }
    return true;
}

void testParseCanonical() {
    const std::string text =
        "# mod-index/bindings/2.2081/macos-arm64.pbind\n"
        "pbind_version = 1\n"
        "gd_version    = 2.2081\n"
        "platform      = macos-arm64\n"
        "\n"
        "[function]\n"
        "symbol     = MenuLayer::init\n"
        "offset     = 0x00A1B2C0\n"
        "return     = bool\n"
        "params     = MenuLayer*\n"
        "verified   = true\n";

    const auto r = parse_pbind(text);
    CHECK(r.ok());
    if (!r.ok()) {
        std::cerr << "  cause: " << r.error->message << " (line " << r.error->line << ")\n";
        return;
    }
    const BindingSet& set = *r.value;
    CHECK((set.key() == BindingKey{GdVersion{2, 2081}, "macos-arm64"}));
    CHECK(set.functions().size() == 1u);
    const auto fn = set.resolve("MenuLayer::init");
    CHECK(fn.has_value());
    if (fn) {
        CHECK(fn->address == 0x00A1B2C0u);
        CHECK(fn->resolved == true);
        CHECK(fn->signature.returnType == "bool");
        CHECK(fn->signature.parameterTypes.size() == 1u);
        CHECK(fn->signature.parameterTypes[0] == "MenuLayer*");
    }
}

void testRoundTrip() {
    const BindingSet original = sampleSet();
    const std::string serialized = serialize_pbind(original);

    const auto first = parse_pbind(serialized);
    CHECK(first.ok());
    if (!first.ok()) {
        std::cerr << "  cause: " << first.error->message << "\n";
        return;
    }
    // parse→serialize→parse uguale a livello di campo (Req 4.6).
    const std::string reserialized = serialize_pbind(*first.value);
    const auto second = parse_pbind(reserialized);
    CHECK(second.ok());
    if (second.ok()) {
        CHECK(fieldEqual(*first.value, *second.value));
    }

    // L'ordinamento canonico per symbol mette MenuLayer prima di PlayLayer.
    CHECK(first.value->functions().size() == 2u);
    if (first.value->functions().size() == 2u) {
        CHECK(first.value->functions()[0].symbol == "MenuLayer::init");
        CHECK(first.value->functions()[1].symbol == "PlayLayer::init");
    }

    // I campi originali sono preservati (nessuna voce alterata).
    const auto fn = first.value->resolve("PlayLayer::init");
    CHECK(fn.has_value());
    if (fn) {
        CHECK(fn->address == 0x00B2C3D0u);
        CHECK(fn->resolved == false);
        CHECK(fn->signature.parameterTypes.size() == 2u);
    }
}

void testEmptyFunctionsRoundTrip() {
    BindingSet set{BindingKey{GdVersion{2, 2074}, "windows-x64"}};
    const auto r = parse_pbind(serialize_pbind(set));
    CHECK(r.ok());
    if (r.ok()) {
        CHECK(fieldEqual(set, *r.value));
        CHECK(r.value->functions().empty());
    }
}

void testNoParamsRoundTrip() {
    BindingSet set{BindingKey{GdVersion{2, 2081}, "macos-arm64"}};
    FunctionBinding fn;
    fn.symbol = "cocos2d::CCDirector::sharedDirector";
    fn.address = 0x10;
    fn.signature = Signature{"CCDirector*", {}};  // zero parametri
    fn.resolved = true;
    set.add(fn);

    const auto r = parse_pbind(serialize_pbind(set));
    CHECK(r.ok());
    if (r.ok()) {
        CHECK(fieldEqual(set, *r.value));
        const auto got = r.value->resolve("cocos2d::CCDirector::sharedDirector");
        CHECK(got.has_value());
        if (got) {
            CHECK(got->signature.parameterTypes.empty());
        }
    }
}

// Ogni contenuto malformato deve restituire un errore con causa NON vuota,
// senza lanciare/terminare il processo (Req 4.7).
void testMalformedReturnsError(std::string_view label, std::string_view content) {
    const auto r = parse_pbind(content);
    CHECK(!r.ok());
    CHECK(r.error.has_value());
    if (r.error) {
        CHECK(!r.error->message.empty());
    }
    if (r.ok()) {
        std::cerr << "  malformed input accepted unexpectedly: " << label << "\n";
    }
}

void testMalformed() {
    testMalformedReturnsError("missing pbind_version",
                              "gd_version = 2.2081\nplatform = macos-arm64\n");
    testMalformedReturnsError("missing gd_version",
                              "pbind_version = 1\nplatform = macos-arm64\n");
    testMalformedReturnsError("missing platform",
                              "pbind_version = 1\ngd_version = 2.2081\n");
    testMalformedReturnsError(
        "unparseable offset",
        "pbind_version = 1\ngd_version = 2.2081\nplatform = macos-arm64\n"
        "[function]\nsymbol = A::b\noffset = xyz\nreturn = void\nverified = true\n");
    testMalformedReturnsError(
        "malformed gd_version",
        "pbind_version = 1\ngd_version = 2x2081\nplatform = macos-arm64\n");
    testMalformedReturnsError(
        "unsupported pbind_version",
        "pbind_version = 2\ngd_version = 2.2081\nplatform = macos-arm64\n");
    testMalformedReturnsError(
        "line without '='",
        "pbind_version = 1\ngd_version = 2.2081\nplatform = macos-arm64\nGARBAGE\n");
    testMalformedReturnsError(
        "function missing symbol",
        "pbind_version = 1\ngd_version = 2.2081\nplatform = macos-arm64\n"
        "[function]\noffset = 0x10\nreturn = void\nverified = true\n");
    testMalformedReturnsError(
        "bad verified value",
        "pbind_version = 1\ngd_version = 2.2081\nplatform = macos-arm64\n"
        "[function]\nsymbol = A::b\noffset = 0x10\nreturn = void\nverified = maybe\n");
    testMalformedReturnsError(
        "unknown header key",
        "pbind_version = 1\ngd_version = 2.2081\nplatform = macos-arm64\nbogus = 3\n");
}

// Dopo un parse malformato il processo continua e il set embedded resta
// caricabile (Req 4.7).
void testEmbeddedStillLoadableAfterMalformed() {
    const auto bad = parse_pbind("totally::garbage without structure");
    CHECK(!bad.ok());  // non termina il processo: arriviamo qui

    EmbeddedBindingsProvider provider;
    const auto set = provider.load(BindingKey{GdVersion{2, 2074}, "windows-x64"});
    CHECK(set.has_value());
    if (set) {
        CHECK(provider.resolve("MenuLayer::init").has_value());
    }
}

}  // namespace

int main() {
    testParseCanonical();
    testRoundTrip();
    testEmptyFunctionsRoundTrip();
    testNoParamsRoundTrip();
    testMalformed();
    testEmbeddedStillLoadableAfterMalformed();

    std::cout << "pbind_format_test: " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed\n";
    if (g_failures != 0) {
        std::cerr << "pbind_format_test: " << g_failures << " FAILED\n";
        return 1;
    }
    std::cout << "pbind_format_test: ALL PASSED\n";
    return 0;
}
