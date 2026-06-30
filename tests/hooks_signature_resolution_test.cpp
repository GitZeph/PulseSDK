// tests/hooks_signature_resolution_test.cpp — verifica della firma a
// compile-time e risoluzione del simbolo a load-time (task 17.1).
//
// Copre:
//   * il concept `SignatureMatches` (Req 5.2): vero quando la firma dichiarata
//     coincide con quella esposta da `BindingTraits<Symbol>::Fn`, falso quando
//     differisce (ritorno o parametri), permissivo quando il simbolo non ha un
//     binding noto a compile-time. Asserito sia con `static_assert` sia via
//     `requires` (caso positivo);
//   * l'espansione di `PULSE_HOOK` su un simbolo con binding compatibile: deve
//     COMPILARE (la variante incompatibile fallisce in compilazione ed è
//     coperta dal compile-fail test del task 17.2, in una TU separata);
//   * `PULSE_HOOK_PRIORITY` registra la priorità di catena (Req 3.2);
//   * la risoluzione a load-time `resolve_all` (Req 5.4): annulla SOLO l'hook
//     irrisolto (lo marca irrisolto e lo segnala) lasciando installati gli
//     altri hook risolti, senza interrompere la passata.

#include <gtest/gtest.h>

#include <pulse/hooks.hpp>

// ---------------------------------------------------------------------------
// Tipo bersaglio fittizio + specializzazione di BindingTraits PRIMA di
// qualunque PULSE_HOOK, così lo static_assert della macro vede il binding.
// ---------------------------------------------------------------------------
namespace {
struct GameLayer {
    int frames = 0;
};
}  // namespace

namespace pulse::hooks {
// Il layer dei bindings espone la firma canonica del bersaglio per il simbolo
// "GameLayer_update": void(GameLayer*, float).
template <>
struct BindingTraits<FixedString("GameLayer_update")> {
    using Fn = void(GameLayer*, float);
};
}  // namespace pulse::hooks

// ---------------------------------------------------------------------------
// Caso POSITIVO della macro: firma dichiarata == firma del binding → COMPILA.
// (I nomi dei parametri non fanno parte del tipo funzione e sono ignorati.)
// ---------------------------------------------------------------------------
PULSE_HOOK(GameLayer_update, void, (GameLayer* self, float dt)) {
    if (self != nullptr) {
        self->frames += static_cast<int>(dt);
    }
}

// ---------------------------------------------------------------------------
// Hook SENZA binding noto (controllo permissivo) usati per la risoluzione a
// load-time. Una priorità esplicita su uno di essi verifica anche Req 3.2.
// ---------------------------------------------------------------------------
PULSE_HOOK(res_alpha, int, (int x)) { return x; }
PULSE_HOOK_PRIORITY(res_beta, 800, int, (int x)) { return x; }
PULSE_HOOK(res_gamma, int, (int x)) { return x; }

namespace {

using pulse::hooks::BindingTraits;
using pulse::hooks::FixedString;
using pulse::hooks::SignatureMatches;

// --- SignatureMatches: verifica a compile-time (Req 5.2) -------------------

// Vero: firma identica a quella del binding.
static_assert(SignatureMatches<FixedString("GameLayer_update"),
                               void(GameLayer*, float)>,
              "la firma identica al binding deve corrispondere");

// Falso: tipo di ritorno diverso.
static_assert(!SignatureMatches<FixedString("GameLayer_update"),
                                int(GameLayer*, float)>,
              "un tipo di ritorno diverso non deve corrispondere");

// Falso: tipo di parametro diverso.
static_assert(!SignatureMatches<FixedString("GameLayer_update"),
                                void(GameLayer*, int)>,
              "un tipo di parametro diverso non deve corrispondere");

// Falso: numero di parametri diverso.
static_assert(!SignatureMatches<FixedString("GameLayer_update"),
                                void(GameLayer*)>,
              "un numero di parametri diverso non deve corrispondere");

// Permissivo: simbolo senza binding noto → soddisfatto (verifica a load-time).
static_assert(SignatureMatches<FixedString("symbol_without_binding"),
                               int(double, char)>,
              "senza binding noto il controllo è permissivo");

// Caso positivo asserito anche tramite un contesto `requires`.
template <auto Symbol, class Declared>
    requires SignatureMatches<Symbol, Declared>
constexpr bool compiles_when_signature_matches() {
    return true;
}

static_assert(
    compiles_when_signature_matches<FixedString("GameLayer_update"),
                                    void(GameLayer*, float)>(),
    "il vincolo requires deve essere soddisfatto dalla firma corretta");

TEST(SignatureMatches, MatchesBindingAtCompileTime) {
    // I controlli sostanziali sono static_assert sopra; qui rendiamo il caso
    // osservabile anche a runtime per il report di test.
    EXPECT_TRUE((SignatureMatches<FixedString("GameLayer_update"),
                                  void(GameLayer*, float)>));
    EXPECT_FALSE((SignatureMatches<FixedString("GameLayer_update"),
                                   int(GameLayer*, float)>));
    EXPECT_TRUE((SignatureMatches<FixedString("symbol_without_binding"),
                                  int(double, char)>));
}

// --- L'hook con firma compatibile è registrato e installabile --------------
TEST(SignatureMatches, CompatibleHookIsRegistered) {
    const auto* reg = pulse::hooks::find("GameLayer_update");
    ASSERT_NE(reg, nullptr);
    EXPECT_EQ(reg->symbol, "GameLayer_update");
    EXPECT_NE(reg->detour, nullptr);
    EXPECT_EQ(reg->priority, 500);  // default (Req 3.2)
}

// --- PULSE_HOOK_PRIORITY registra la priorità di catena (Req 3.2) ----------
TEST(PulseHookPriority, RecordsChainPriority) {
    const auto* beta = pulse::hooks::find("res_beta");
    ASSERT_NE(beta, nullptr);
    EXPECT_EQ(beta->priority, 800);

    const auto* alpha = pulse::hooks::find("res_alpha");
    ASSERT_NE(alpha, nullptr);
    EXPECT_EQ(alpha->priority, 500);  // forma minimale → default
}

// --- Risoluzione a load-time: annulla SOLO l'hook irrisolto (Req 5.4) ------
TEST(LoadTimeResolution, CancelsOnlyUnresolvedHookAndKeepsOthers) {
    // Indirizzi fittizi distinti per i simboli risolvibili.
    static int sentinel_a = 0;
    static int sentinel_g = 0;
    static int sentinel_u = 0;

    // Resolver: risolve tutti tranne "res_beta" (simbolo non risolvibile).
    const auto report = pulse::hooks::resolve_all(
        [&](std::string_view symbol) -> void* {
            if (symbol == "res_beta") {
                return nullptr;  // irrisolvibile → l'hook va annullato
            }
            if (symbol == "res_alpha") return &sentinel_a;
            if (symbol == "res_gamma") return &sentinel_g;
            // Qualunque altro simbolo registrato (es. GameLayer_update) risolve.
            return &sentinel_u;
        });

    // Esattamente l'hook irrisolto è segnalato; gli altri sono risolti.
    EXPECT_FALSE(report.all_resolved());
    EXPECT_EQ(report.unresolved_count(), 1u);
    ASSERT_EQ(report.unresolved.size(), 1u);
    EXPECT_EQ(report.unresolved.front(), "res_beta");

    // Lo stato per-registrazione riflette la cancellazione del solo hook
    // irrisolto: gli altri restano risolti/installabili.
    const auto* beta = pulse::hooks::find("res_beta");
    ASSERT_NE(beta, nullptr);
    EXPECT_FALSE(beta->resolved);          // annullato
    EXPECT_EQ(beta->target, nullptr);

    const auto* alpha = pulse::hooks::find("res_alpha");
    ASSERT_NE(alpha, nullptr);
    EXPECT_TRUE(alpha->resolved);          // resta valido
    EXPECT_EQ(alpha->target, &sentinel_a);

    const auto* gamma = pulse::hooks::find("res_gamma");
    ASSERT_NE(gamma, nullptr);
    EXPECT_TRUE(gamma->resolved);          // resta valido
    EXPECT_EQ(gamma->target, &sentinel_g);

    const auto* game = pulse::hooks::find("GameLayer_update");
    ASSERT_NE(game, nullptr);
    EXPECT_TRUE(game->resolved);

    // Tutti i simboli registrati eccetto quello irrisolto sono risolti.
    EXPECT_EQ(report.resolved_count(), pulse::hooks::count() - 1u);
}

}  // namespace
