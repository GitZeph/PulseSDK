// loader/bindings/embedded_bindings_provider.hpp — provider embedded del MVP.
//
// `EmbeddedBindingsProvider` incorpora staticamente i bindings necessari al
// MVP. Per il MVP è presente un solo set: (2.2074, "windows-x64") contenente
// l'offset e la firma di `MenuLayer::init` (vedi design → MVP).
//
// Caratteristiche:
//   - `load`: corrispondenza ESATTA della coppia (GD_Version, piattaforma)
//             (Req 4.4/20.2). Nessun fuzzy-match né sostituzione da versione/
//             piattaforma diversa.
//   - `resolve`: corrispondenza ESATTA del simbolo nel set caricato (Req 4.4/20.2).
//   - `resolved sse verificato` (Req 4.2/4.3): ogni binding incorporato o
//     affiancato (`.pbind` reale) è marcato `resolved` SSE il suo indirizzo è
//     verificato (non-zero, != sentinel, prologo conforme alla firma) tramite
//     `binding_verifier`. Le voci non verificate restano non risolte → zero
//     hook a valle (Req 4.5).
#ifndef PULSE_LOADER_BINDINGS_EMBEDDED_BINDINGS_PROVIDER_HPP
#define PULSE_LOADER_BINDINGS_EMBEDDED_BINDINGS_PROVIDER_HPP

#include <optional>
#include <string_view>
#include <vector>

#include "binding_verifier.hpp"
#include "bindings.hpp"

namespace pulse::loader::bindings {

class EmbeddedBindingsProvider final : public IBindingsProvider {
public:
    EmbeddedBindingsProvider();

    // Carica il set per la coppia con corrispondenza esatta (Req 4.4, 20.1, 20.2).
    // Sul successo memorizza il set come "corrente" per le successive `resolve`.
    std::optional<BindingSet> load(const BindingKey& key) override;

    // Risolve un simbolo a corrispondenza esatta nel set corrente (Req 4.4/20.2).
    std::optional<FunctionBinding> resolve(std::string_view symbol) const override;

    // Affianca un binding set proveniente da un Binding_Set_File `.pbind` reale
    // (es. `mod-index/bindings/2.2081/macos-arm64.pbind`), applicando la verifica
    // "resolved sse verificato" (Req 4.2/4.3): ogni binding del set è marcato
    // `resolved` SSE il suo indirizzo è verificato (non-zero, != sentinel,
    // prologo conforme alla firma) e la voce dichiarava `verified`. Se esiste già
    // un set per la STESSA coppia esatta, viene sostituito (nessun fuzzy-match,
    // nessun duplicato di coppia). Usa il verificatore di prologo iniettato; la
    // variante senza verificatore usa `TrustingPrologueVerifier` (host/CI).
    void addVerifiedSet(const BindingSet& set);
    void addVerifiedSet(const BindingSet& set, const IPrologueVerifier& prologueVerifier);

private:
    // Tutti i set disponibili in questa istanza del provider (embedded +
    // eventuali `.pbind` reali affiancati), già verificati.
    std::vector<BindingSet> sets_;
    // Set attualmente caricato (popolato da `load` su corrispondenza esatta).
    std::optional<BindingSet> current_;
};

}  // namespace pulse::loader::bindings

#endif  // PULSE_LOADER_BINDINGS_EMBEDDED_BINDINGS_PROVIDER_HPP
