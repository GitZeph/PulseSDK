// loader/bindings/binding_verifier.hpp — verifica "resolved sse verificato"
// dei bindings (task 3.9, Req 4.2, 4.3).
//
// Questo modulo realizza l'invariante centrale del Bindings System reale:
//
//   un FunctionBinding è marcato `resolved` SE E SOLO SE il suo indirizzo è
//   stato verificato contro il binario reale di Geometry Dash.
//
// Un indirizzo è **verificato e non-placeholder** (Req 4.2) sse, simultaneamente:
//   1) l'offset è NON-ZERO;
//   2) l'offset è DIVERSO dal sentinel/placeholder del set embedded;
//   3) il PROLOGO all'indirizzo risolto è CONFORME alla firma registrata.
//
// La verifica del prologo (3) richiede l'immagine reale di GD caricata in
// memoria (Fase E, manuale): per renderla host-testabile è dietro
// l'interfaccia iniettabile `IPrologueVerifier`. Sull'host (CI) il binario non
// è disponibile, quindi il verificatore di default (`TrustingPrologueVerifier`)
// accetta la verifica eseguita offline e registrata nel campo `verified` del
// `.pbind`; i controlli strutturali (1) e (2) restano comunque applicati,
// così un'eventuale voce che dichiara `verified` ma porta un offset
// nullo/sentinel non viene mai marcata `resolved` (difesa in profondità).
//
// Nessun fuzzy-match e nessuna sostituzione: la verifica opera sul singolo
// binding già selezionato dalla coppia esatta (vedi `bindings.hpp`).
#ifndef PULSE_LOADER_BINDINGS_BINDING_VERIFIER_HPP
#define PULSE_LOADER_BINDINGS_BINDING_VERIFIER_HPP

#include <cstdint>

#include "bindings.hpp"

namespace pulse::loader::bindings {

// Offset sentinel/placeholder: un binding che porta questo indirizzo NON è
// stato verificato contro il binario reale e DEVE restare non risolto (Req 4.2).
// Valore riconoscibile e improbabile come offset reale (tutti bit a 1).
inline constexpr std::uintptr_t kPlaceholderSentinel =
    ~static_cast<std::uintptr_t>(0);

// Verificatore del prologo: stabilisce se i byte del prologo all'indirizzo
// risolto sono conformi alla firma registrata (Req 4.2/4.3). Iniettabile perché
// la verifica reale richiede l'immagine Mach-O di GD caricata (Fase E).
class IPrologueVerifier {
public:
    virtual ~IPrologueVerifier() = default;

    // Restituisce true sse il prologo della funzione `binding` combacia con la
    // firma attesa contro il binario reale.
    virtual bool prologueMatchesSignature(const FunctionBinding& binding) const = 0;
};

// Verificatore di default per l'host (CI): il prologo non è disassemblabile
// senza il binario reale, quindi accetta la verifica eseguita offline (campo
// `verified` del `.pbind`). I controlli strutturali (non-zero, != sentinel)
// vengono comunque applicati a monte da `address_is_verified`.
class TrustingPrologueVerifier final : public IPrologueVerifier {
public:
    bool prologueMatchesSignature(const FunctionBinding& /*binding*/) const override {
        return true;
    }
};

// Predicato puro: l'indirizzo del `binding` è verificato e non-placeholder
// (Req 4.2) sse non-zero, diverso dal `sentinel`, e con prologo conforme alla
// firma secondo `prologueVerifier`.
[[nodiscard]] bool address_is_verified(
    const FunctionBinding& binding,
    const IPrologueVerifier& prologueVerifier,
    std::uintptr_t sentinel = kPlaceholderSentinel);

// Restituisce una copia di `binding` con `resolved` impostato SSE l'indirizzo è
// verificato (Req 4.3) E la voce dichiarava la verifica offline (il `resolved`
// in ingresso, popolato dal campo `verified` del `.pbind`). In tutti gli altri
// casi `resolved` è false (binding non risolto → zero hook a valle, Req 4.5).
[[nodiscard]] FunctionBinding mark_resolved_if_verified(
    FunctionBinding binding,
    const IPrologueVerifier& prologueVerifier,
    std::uintptr_t sentinel = kPlaceholderSentinel);

// Applica `mark_resolved_if_verified` a ogni funzione di `set`, restituendo un
// nuovo `BindingSet` con la stessa chiave e gli stessi binding ma con il campo
// `resolved` ricalcolato secondo la verifica (Req 4.3).
[[nodiscard]] BindingSet verify_binding_set(
    const BindingSet& set,
    const IPrologueVerifier& prologueVerifier,
    std::uintptr_t sentinel = kPlaceholderSentinel);

}  // namespace pulse::loader::bindings

#endif  // PULSE_LOADER_BINDINGS_BINDING_VERIFIER_HPP
