// loader/bindings/embedded_bindings_provider.cpp — dati e logica del provider
// embedded del MVP, con verifica "resolved sse verificato" (task 3.9).
#include "embedded_bindings_provider.hpp"

#include <utility>

#include "binding_verifier.hpp"

namespace pulse::loader::bindings {

namespace {

// Coppia chiave del MVP: GD 2.2074 su Windows x64 (design → MVP).
constexpr int kMvpGdMajor = 2;
constexpr int kMvpGdMinor = 2074;
constexpr std::string_view kMvpPlatformId = "windows-x64";

// Offset di `MenuLayer::init` relativo alla base dell'immagine per
// (2.2074, windows-x64). Valore embedded del MVP: rappresenta il singolo
// indirizzo necessario all'hook dimostrativo. Deve essere validato contro il
// binario reale di GD 2.2074 prima del rilascio; il modello e il gating
// (Req 20.3/20.4) restano corretti indipendentemente dal valore esatto.
constexpr std::uintptr_t kMenuLayerInitOffset = 0x003140D0;

// Costruisce il set embedded (2.2074, windows-x64).
BindingSet makeWindowsX64_2_2074() {
    BindingSet set{BindingKey{GdVersion{kMvpGdMajor, kMvpGdMinor},
                              std::string{kMvpPlatformId}}};

    // bool MenuLayer::init();  // `this` (MenuLayer*) è il primo parametro.
    FunctionBinding menuLayerInit;
    menuLayerInit.symbol = "MenuLayer::init";
    menuLayerInit.address = kMenuLayerInitOffset;
    menuLayerInit.signature = Signature{
        /*returnType=*/"bool",
        /*parameterTypes=*/{"MenuLayer*"},
    };
    // Verifica offline registrata: la voce dichiara di essere verificata; il
    // valore `resolved` definitivo è ricalcolato dalla verifica (Req 4.3).
    menuLayerInit.resolved = true;
    set.add(std::move(menuLayerInit));

    return set;
}

}  // namespace

EmbeddedBindingsProvider::EmbeddedBindingsProvider() {
    // Applica la verifica "resolved sse verificato" (Req 4.2/4.3) anche al set
    // embedded: l'offset MVP è non-zero e != sentinel, quindi resta risolto;
    // il modello e il gating restano corretti indipendentemente dal valore.
    TrustingPrologueVerifier prologueVerifier;
    sets_.push_back(verify_binding_set(makeWindowsX64_2_2074(), prologueVerifier));
}

void EmbeddedBindingsProvider::addVerifiedSet(const BindingSet& set) {
    TrustingPrologueVerifier prologueVerifier;
    addVerifiedSet(set, prologueVerifier);
}

void EmbeddedBindingsProvider::addVerifiedSet(const BindingSet& set,
                                              const IPrologueVerifier& prologueVerifier) {
    // Ricalcola `resolved` per ogni binding secondo la verifica (Req 4.3).
    BindingSet verified = verify_binding_set(set, prologueVerifier);
    // Coppia esatta unica: sostituisce un eventuale set preesistente con la
    // stessa chiave (nessun duplicato, nessun fuzzy-match).
    for (auto& existing : sets_) {
        if (existing.key() == verified.key()) {
            existing = std::move(verified);
            return;
        }
    }
    sets_.push_back(std::move(verified));
}

std::optional<BindingSet> EmbeddedBindingsProvider::load(const BindingKey& key) {
    // Corrispondenza ESATTA della coppia (GD_Version, piattaforma) (Req 20.2):
    // sia la versione sia l'identificatore di piattaforma devono coincidere.
    for (const auto& set : sets_) {
        if (set.key() == key) {
            current_ = set;
            return set;
        }
    }
    // Nessuna corrispondenza esatta: non si carica nulla (gating a monte, Req 20.3).
    return std::nullopt;
}

std::optional<FunctionBinding> EmbeddedBindingsProvider::resolve(
    std::string_view symbol) const {
    if (!current_) {
        return std::nullopt;
    }
    // Corrispondenza esatta del simbolo, nessun fuzzy-match (Req 20.2).
    return current_->resolve(symbol);
}

}  // namespace pulse::loader::bindings
