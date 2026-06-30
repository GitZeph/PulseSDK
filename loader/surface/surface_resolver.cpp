// loader/surface/surface_resolver.cpp — policy d'ordine embedded-first sui
// provider riusati (task 13.3, Req 9.1, 9.2, 9.3, 9.4, 9.5).
#include "surface/surface_resolver.hpp"

#include "surface/surface_report.hpp"  // default_prologue_verifier

namespace pulse::loader::surface {

SurfaceResolver::SurfaceResolver(IBindingsProvider& embedded, IBindingsProvider& online)
    : SurfaceResolver(embedded, online, default_prologue_verifier(),
                      kPlaceholderSentinel) {}

SurfaceResolver::SurfaceResolver(IBindingsProvider& embedded, IBindingsProvider& online,
                                 const IPrologueVerifier& prologueVerifier,
                                 std::uintptr_t sentinel)
    : embedded_(embedded),
      online_(online),
      prologueVerifier_(prologueVerifier),
      sentinel_(sentinel) {}

std::optional<FunctionBinding> SurfaceResolver::resolve(const BindingKey& pair,
                                                        std::string_view symbol) const {
    // Applica la STESSA semantica `binding_verifier` a qualunque binding,
    // qualunque sia il provider (Req 9.3): `mark_resolved_if_verified` ricalcola
    // `resolved` dalla regola `verified && offset valido && prologo conforme`.
    const auto verify =
        [&](std::optional<FunctionBinding> b) -> std::optional<FunctionBinding> {
        if (!b.has_value()) {
            return std::nullopt;
        }
        return bindings::mark_resolved_if_verified(*b, prologueVerifier_, sentinel_);
    };

    // Embedded primario (Req 9.1): carica la coppia esatta e risolve il simbolo.
    embedded_.load(pair);
    std::optional<FunctionBinding> embedded = verify(embedded_.resolve(symbol));
    if (embedded.has_value() && embedded->resolved) {
        // Embedded fornisce un binding risolto → vince, anche se l'online lo
        // fornisse a sua volta (Req 9.4). Nessun bisogno di interrogare l'online.
        return embedded;
    }

    // Fallback online SOLO perché l'embedded non ha fornito un binding risolto
    // (Req 9.2): carica la coppia esatta e risolve il simbolo.
    online_.load(pair);
    std::optional<FunctionBinding> online = verify(online_.resolve(symbol));
    if (online.has_value() && online->resolved) {
        return online;
    }

    // Nessun binding risolto: restituisce il binding non risolto dell'embedded
    // se presente (per la classificazione/segnalazione a valle), altrimenti
    // quello dell'online, altrimenti nessuna corrispondenza esatta.
    if (embedded.has_value()) {
        return embedded;
    }
    return online;
}

BindingSet SurfaceResolver::effectiveSet(
    const BindingKey& pair, const std::vector<std::string_view>& symbols) const {
    BindingSet set{pair};
    for (std::string_view symbol : symbols) {
        if (auto binding = resolve(pair, symbol)) {
            set.add(*binding);
        }
    }
    return set;
}

}  // namespace pulse::loader::surface
