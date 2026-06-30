// loader/surface/surface_report.hpp — Surface_Runtime_Report della GD_API_Surface
// (task 13.1, 13.2, 13.4; Req 6.1-6.4, 7.1-7.4, 8.1-8.4).
//
// La superficie a runtime NON risolve né verifica da sé: RIUSA `binding_verifier`
// (`address_is_verified` / `mark_resolved_if_verified`) e la stessa logica di
// *partial resolution* di `resolve_all` (gli elementi risolti restano
// installabili, gli irrisolti sono elencati separatamente senza interromperli).
// Questo modulo aggiunge SOLTANTO un sottile reporting che classifica ciascun
// `API_Element` come risolvibile / non risolvibile per la coppia del
// `RuntimeContext` e ne espone la provenienza per riferimento.
//
// INVARIANTI (riuso, nessuna re-implementazione):
//   * resolved sse verificato (Req 6.1): un `API_Element` è risolvibile SSE il
//     binding sottostante è risolto da `binding_verifier`, ossia `verified` (il
//     flag `resolved` del binding, popolato dal `.pbind`) è vero E l'offset è
//     non-zero, diverso dal `Sentinel_Value` e con prologo conforme. Si riusa
//     `mark_resolved_if_verified`, che è esattamente questo predicato.
//   * partial resolution (Req 6.2, 6.3, 7.4): gli hook su binding non risolti
//     restano NON installati e sono elencati tra gli `unresolvable`, SENZA
//     impedire l'installazione dei risolti.
//   * install solo dopo verifica (Req 6.4): solo gli elementi in `resolvable`
//     guidano l'installazione del detour (l'install effettiva è il percorso
//     esistente HookGate/backend; qui si produce la classificazione che
//     l'installer consuma).
//   * per coppia, nessuna cross-coppia (Req 7.1, 7.2, 7.3): la classificazione
//     usa ESCLUSIVAMENTE il `BindingSet` della coppia del `RuntimeContext`. Lo
//     stesso insieme di simboli (`API_Element`) è classificato su tutte le
//     coppie; cambia solo il flag `resolvable`.
//   * gate di provenienza (Req 8.1, 8.2, 8.3): un elemento risolvibile espone un
//     `ProvenanceRef` (per riferimento, senza ricomputazione); un elemento
//     `verified = true` ma con provenienza INCOMPLETA è DECLASSATO a non
//     risolvibile e segnalato con simbolo+coppia.
#ifndef PULSE_LOADER_SURFACE_SURFACE_REPORT_HPP
#define PULSE_LOADER_SURFACE_SURFACE_REPORT_HPP

#include <deque>
#include <string_view>
#include <vector>

#include "bindings/binding_verifier.hpp"
#include "bindings/bindings.hpp"
#include "surface/surface_provenance.hpp"

namespace pulse::loader::surface {

using bindings::BindingKey;
using bindings::BindingSet;
using bindings::IPrologueVerifier;
using bindings::kPlaceholderSentinel;

// Verificatore di prologo predefinito per l'host/CI (accetta la verifica
// offline registrata nel `.pbind`). I controlli strutturali (non-zero,
// != sentinel) restano sempre applicati da `address_is_verified`.
[[nodiscard]] const IPrologueVerifier& default_prologue_verifier();

// Stato di un singolo `API_Element` nella superficie runtime.
struct SurfaceElementStatus {
    std::string_view symbol;             // simbolo canonico (es. "MenuLayer::init")
    bool resolvable{false};              // resolved sse verificato (Req 6.1)
    const ProvenanceRef* provenance{nullptr};  // non-null SSE risolvibile (Req 8.1)
};

// Errore di auditabilità: elemento `verified = true` ma con provenienza
// incompleta per la coppia (Req 8.2). Identifica simbolo + coppia.
struct SurfaceAuditError {
    std::string_view symbol;
    BindingKey pair;
};

// Report runtime della superficie per una coppia (GD_Version, piattaforma).
//
// `resolvable`   elementi installabili (Req 6.4): il loader installa il detour
//                solo per questi.
// `unresolvable` elementi NON installati e segnalati separatamente (Req 6.2,
//                6.3, 7.4): binding non risolto OPPURE provenienza incompleta.
// `auditErrors`  sottoinsieme degli `unresolvable` declassati per provenienza
//                incompleta (Req 8.2), con simbolo + coppia.
//
// Il report POSSIEDE lo storage dei `ProvenanceRef` puntati dagli stati
// risolvibili (`provenanceStorage`): è un `std::deque` così i puntatori restano
// stabili man mano che gli elementi vengono aggiunti e attraverso lo spostamento
// del report. Il report è quindi spostabile ma non viene copiato.
struct SurfaceRuntimeReport {
    std::vector<SurfaceElementStatus> resolvable;
    std::vector<SurfaceElementStatus> unresolvable;
    std::vector<SurfaceAuditError> auditErrors;
    std::deque<ProvenanceRef> provenanceStorage;

    [[nodiscard]] std::size_t resolvableCount() const { return resolvable.size(); }
    [[nodiscard]] std::size_t unresolvableCount() const { return unresolvable.size(); }
};

// Classifica l'insieme di `API_Element` (`symbols`) per la coppia del
// `RuntimeContext`, usando ESCLUSIVAMENTE il `BindingSet` di quella coppia
// (`setForPair`) — nessuna derivazione cross-coppia (Req 7.2).
//
// Per ogni simbolo:
//   1) risolve il binding nel `setForPair` (corrispondenza esatta del simbolo);
//   2) RIUSA `binding_verifier` (`mark_resolved_if_verified`) per stabilire se è
//      risolto (Req 6.1): `verified` + offset valido + prologo conforme;
//   3) se NON risolto → `unresolvable` (Req 6.2);
//   4) se risolto, applica il gate di provenienza (Req 8.1, 8.2): se è fornito
//      uno `SurfaceProvenanceStore` con un record per (simbolo, coppia) e quel
//      record è INCOMPLETO, l'elemento è DECLASSATO a `unresolvable` con un
//      errore di auditabilità (simbolo + coppia); altrimenti è `resolvable` con
//      un `ProvenanceRef` (per riferimento, senza ricomputazione, Req 8.3).
//
// Quando `provenance == nullptr`, la completezza è quella implicata dal flag
// `verified` del `.pbind` + gate build-time già applicato: gli elementi risolti
// sono risolvibili con un `ProvenanceRef{complete = true, record = nullptr}`.
//
// I simboli sono passati come `string_view`: devono sopravvivere all'uso del
// report. La coppia è `setForPair.key()`.
[[nodiscard]] SurfaceRuntimeReport classify_surface(
    const std::vector<std::string_view>& symbols,
    const BindingSet& setForPair,
    const IPrologueVerifier& prologueVerifier = default_prologue_verifier(),
    const SurfaceProvenanceStore* provenance = nullptr,
    std::uintptr_t sentinel = kPlaceholderSentinel);

}  // namespace pulse::loader::surface

#endif  // PULSE_LOADER_SURFACE_SURFACE_REPORT_HPP
