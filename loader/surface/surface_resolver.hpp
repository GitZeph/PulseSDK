// loader/surface/surface_resolver.hpp — policy d'ordine embedded-first della
// GD_API_Surface (task 13.3, Req 9.1, 9.2, 9.3, 9.4, 9.5).
//
// `SurfaceResolver` è una **policy d'ordine** SOPRA due `IBindingsProvider`
// riusati, senza modificarne il contratto `load`/`resolve` (Req 9.5):
//
//   * risolve PRIMA tramite l'`EmbeddedBindingsProvider` (primario, Req 9.1);
//   * ricade sull'`OnlineBindingsProvider` SOLO se l'embedded non fornisce un
//     binding risolto per la coppia (Req 9.2);
//   * quando ENTRAMBI forniscono un binding risolto, vince l'embedded (Req 9.4);
//   * applica la STESSA semantica `binding_verifier` (`resolved sse verificato`)
//     indipendentemente dal provider (Req 9.3) ricalcolando `resolved` via
//     `mark_resolved_if_verified` sul binding scelto.
//
// CAVEAT DI DESIGN: l'`OnlineBindingsProvider` esistente compone già al proprio
// interno un fallback online→embedded. La superficie vuole l'ordine INVERSO
// (embedded→online): lo ottiene come policy sui due provider INIETTATI per
// riferimento, senza toccarne il contratto. Iniettare i due `IBindingsProvider`
// rende la policy testabile con provider finti (fake).
//
// I provider sono stateful (`load(key)` imposta il set "corrente", `resolve`
// opera su di esso). Per evitare ambiguità sullo stato corrente, l'API espone
// `resolve(pair, symbol)`: carica la coppia esatta sul provider e risolve il
// simbolo, in un'unica operazione. Si consuma il percorso canonico
// `mod-index/bindings/{version}/{platform}.pbind` SOLO attraverso il contratto
// `load` dei provider (Req 9.5), mai ricostruito qui.
#ifndef PULSE_LOADER_SURFACE_SURFACE_RESOLVER_HPP
#define PULSE_LOADER_SURFACE_SURFACE_RESOLVER_HPP

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "bindings/binding_verifier.hpp"
#include "bindings/bindings.hpp"

namespace pulse::loader::surface {

using bindings::BindingKey;
using bindings::BindingSet;
using bindings::FunctionBinding;
using bindings::IBindingsProvider;
using bindings::IPrologueVerifier;
using bindings::kPlaceholderSentinel;

class SurfaceResolver {
public:
    // `embedded` (primario) e `online` (fallback) devono sopravvivere al
    // resolver: sono iniettati per riferimento (Req 9.1, 9.2). Il verificatore
    // di prologo applica la stessa semantica `binding_verifier` a entrambi
    // (Req 9.3); il default accetta la verifica offline del `.pbind`.
    SurfaceResolver(IBindingsProvider& embedded, IBindingsProvider& online);
    SurfaceResolver(IBindingsProvider& embedded, IBindingsProvider& online,
                    const IPrologueVerifier& prologueVerifier,
                    std::uintptr_t sentinel = kPlaceholderSentinel);

    // Risolve `symbol` per la coppia esatta `pair` con policy embedded-first.
    // Restituisce il `FunctionBinding` che la superficie userà, con `resolved`
    // ricalcolato dalla semantica `binding_verifier` (uguale fra provider,
    // Req 9.3):
    //   * se l'embedded fornisce un binding RISOLTO → quello (Req 9.1, 9.4);
    //   * altrimenti, se l'online fornisce un binding RISOLTO → quello (Req 9.2);
    //   * se nessuno è risolto, restituisce il binding non risolto dell'embedded
    //     se presente, altrimenti quello dell'online, altrimenti `nullopt`
    //     (nessuna corrispondenza esatta della coppia/simbolo, nessun fuzzy-match).
    [[nodiscard]] std::optional<FunctionBinding> resolve(const BindingKey& pair,
                                                         std::string_view symbol) const;

    // Costruisce il `BindingSet` EFFETTIVO per `pair` applicando la policy
    // embedded-first a ciascun simbolo di `symbols`. Ogni binding incluso porta
    // `resolved` già conforme a `binding_verifier`. È il set da passare a
    // `classify_surface` (Surface_Runtime_Report) così che la classificazione
    // per coppia rifletta la precedenza dei provider. Riusa esclusivamente
    // `load`/`resolve` dei provider (Req 9.5).
    [[nodiscard]] BindingSet effectiveSet(
        const BindingKey& pair, const std::vector<std::string_view>& symbols) const;

private:
    IBindingsProvider& embedded_;
    IBindingsProvider& online_;
    const IPrologueVerifier& prologueVerifier_;
    std::uintptr_t sentinel_;
};

}  // namespace pulse::loader::surface

#endif  // PULSE_LOADER_SURFACE_SURFACE_RESOLVER_HPP
