// loader/bindings/binding_verifier.cpp — implementazione del predicato
// "resolved sse verificato" (task 3.9, Req 4.2, 4.3).
#include "binding_verifier.hpp"

#include <utility>

namespace pulse::loader::bindings {

bool address_is_verified(const FunctionBinding& binding,
                         const IPrologueVerifier& prologueVerifier,
                         std::uintptr_t sentinel) {
    // (1) Offset non-zero (Req 4.2).
    if (binding.address == 0) {
        return false;
    }
    // (2) Offset diverso dal sentinel/placeholder del set embedded (Req 4.2).
    if (binding.address == sentinel) {
        return false;
    }
    // (3) Prologo conforme alla firma registrata (Req 4.2).
    return prologueVerifier.prologueMatchesSignature(binding);
}

FunctionBinding mark_resolved_if_verified(FunctionBinding binding,
                                          const IPrologueVerifier& prologueVerifier,
                                          std::uintptr_t sentinel) {
    // `resolved` in ingresso codifica la verifica eseguita offline e registrata
    // nel campo `verified` del `.pbind` (vedi pbind_format). Il binding è
    // risolto SSE quella verifica è affermativa E l'indirizzo supera i
    // controlli (non-zero, != sentinel, prologo conforme). Difesa in
    // profondità: una voce che dichiara `verified` ma porta un offset
    // nullo/sentinel resta NON risolta.
    const bool claimedVerified = binding.resolved;
    binding.resolved =
        claimedVerified && address_is_verified(binding, prologueVerifier, sentinel);
    return binding;
}

BindingSet verify_binding_set(const BindingSet& set,
                              const IPrologueVerifier& prologueVerifier,
                              std::uintptr_t sentinel) {
    BindingSet verified{set.key()};
    for (const auto& fn : set.functions()) {
        verified.add(mark_resolved_if_verified(fn, prologueVerifier, sentinel));
    }
    return verified;
}

}  // namespace pulse::loader::bindings
