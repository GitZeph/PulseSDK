// loader/bindings/online_bindings_provider.hpp — provider online auto-aggiornante (Layer 2, task 25.1).
//
// `OnlineBindingsProvider` scarica i set di bindings dall'indice
// `mod-index/bindings/{version}/{platform}.pbind`, ne verifica la firma,
// li memorizza in una cache locale e — in caso di fallimento del download o
// della verifica — ricade sul set embedded (EmbeddedBindingsProvider).
//
// Semantica chiave (Req 20.1, 20.2):
//   - `load`: corrispondenza ESATTA della coppia (GD_Version, piattaforma).
//             L'URL è costruito esattamente dalla coppia richiesta; il set
//             scaricato deve dichiarare la stessa coppia, altrimenti è
//             scartato (nessun fuzzy-match).
//   - `resolve`: corrispondenza ESATTA del simbolo nel set corrente.
//
// Testabilità host (senza rete): il fetch e la verifica di firma sono dietro
// interfacce iniettabili (`IBindingFetcher`, `ISignatureVerifier`). Il fallback
// è un qualsiasi `IBindingsProvider` (tipicamente un EmbeddedBindingsProvider).
#ifndef PULSE_LOADER_BINDINGS_ONLINE_BINDINGS_PROVIDER_HPP
#define PULSE_LOADER_BINDINGS_ONLINE_BINDINGS_PROVIDER_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bindings.hpp"

namespace pulse::loader::bindings {

// Sorgente dei byte grezzi di un file .pbind. Astrae il trasporto (HTTP, file,
// ecc.) così il provider è testabile host senza accesso di rete.
class IBindingFetcher {
public:
    virtual ~IBindingFetcher() = default;

    // Recupera i byte grezzi del .pbind per l'URL/percorso indicato.
    // Restituisce nullopt in caso di fallimento (rete, 404, ...).
    virtual std::optional<std::vector<std::uint8_t>> fetch(const std::string& url) = 0;
};

// Verificatore della firma del payload di un .pbind. Disaccoppia la crittografia
// dal provider: in test si inietta un verificatore fittizio deterministico.
class ISignatureVerifier {
public:
    virtual ~ISignatureVerifier() = default;

    // Verifica `signature` sul `payload`. Restituisce true se la firma è
    // valida e attendibile, false altrimenti.
    virtual bool verify(const std::vector<std::uint8_t>& payload,
                        const std::string& signature) const = 0;
};

// Risultato del parsing del contenitore .pbind: firma + payload serializzato.
struct SignedPbind {
    std::string signature;                // firma sul payload
    std::vector<std::uint8_t> payload;    // BindingSet serializzato
};

// --- Serializzazione del formato .pbind (testuale, semplice e self-contained).
//
// Layout:
//   PBIND1\n
//   SIG:<firma>\n
//   <payload>                 // righe VERSION/PLATFORM/COUNT/<funzioni...>
// La firma copre esclusivamente i byte del payload.

// Serializza il payload (senza header/firma) di un BindingSet.
std::string serializePbindPayload(const BindingSet& set);

// Serializza un .pbind completo (header + firma + payload) per il set indicato.
std::vector<std::uint8_t> serializePbind(const std::string& signature,
                                         const BindingSet& set);

// Effettua il parsing del contenitore .pbind. nullopt se malformato.
std::optional<SignedPbind> parsePbind(const std::vector<std::uint8_t>& bytes);

// Ricostruisce un BindingSet dal payload serializzato. nullopt se malformato.
std::optional<BindingSet> parsePbindPayload(const std::vector<std::uint8_t>& payload);

// Provider online con cache e fallback embedded.
class OnlineBindingsProvider final : public IBindingsProvider {
public:
    // Template di default dell'indice dei bindings.
    static constexpr std::string_view kDefaultUrlTemplate =
        "mod-index/bindings/{version}/{platform}.pbind";

    // `fetcher` e `verifier` devono sopravvivere al provider (iniettati per
    // riferimento). `fallback` è il provider embedded usato quando il download
    // o la verifica falliscono; anch'esso deve sopravvivere al provider.
    OnlineBindingsProvider(IBindingFetcher& fetcher,
                           ISignatureVerifier& verifier,
                           IBindingsProvider& fallback,
                           std::string urlTemplate = std::string{kDefaultUrlTemplate});

    // Carica il set per la coppia con corrispondenza esatta (Req 20.1, 20.2):
    //   1) se in cache (stessa coppia esatta) lo riusa senza ri-scaricare;
    //   2) altrimenti scarica + verifica la firma; se ok e la coppia del set
    //      coincide esattamente con quella richiesta, lo memorizza e lo carica;
    //   3) in caso di fallimento (fetch/parse/verifica/coppia non esatta) ricade
    //      sul provider di fallback (embedded).
    // Restituisce nullopt se né l'online né il fallback hanno una corrispondenza
    // esatta (nessun fuzzy-match).
    std::optional<BindingSet> load(const BindingKey& key) override;

    // Risolve un simbolo a corrispondenza esatta nel set corrente (Req 20.2).
    std::optional<FunctionBinding> resolve(std::string_view symbol) const override;

    // Costruisce l'URL del .pbind per la coppia indicata sostituendo i
    // segnaposto {version} (es. "2.2074") e {platform} nel template.
    std::string buildUrl(const BindingKey& key) const;

private:
    // Cerca un set in cache a corrispondenza esatta della coppia.
    const BindingSet* findCached(const BindingKey& key) const;

    IBindingFetcher& fetcher_;
    ISignatureVerifier& verifier_;
    IBindingsProvider& fallback_;
    std::string urlTemplate_;

    // Set scaricati e verificati, indicizzati per coppia esatta.
    std::vector<BindingSet> cache_;
    // Set attualmente caricato (online o fallback).
    std::optional<BindingSet> current_;
};

}  // namespace pulse::loader::bindings

#endif  // PULSE_LOADER_BINDINGS_ONLINE_BINDINGS_PROVIDER_HPP
