// loader/bindings/bindings.hpp — modello dati del Bindings System (Layer 2).
//
// Definisce le strutture che indicizzano, per la coppia univoca
// (GD_Version, piattaforma), gli indirizzi e le firme delle funzioni del
// gioco, più l'interfaccia `IBindingsProvider` per la loro risoluzione.
//
// Requisiti coperti dal modello:
//   - 20.1: indirizzi e firme indicizzati univocamente per (GD_Version, piattaforma).
//   - 20.2: corrispondenza ESATTA della coppia; nessun fuzzy-match.
//
// Nota di integrazione: il tipo `GdVersion` è dichiarato qui nello spazio dei
// nomi `pulse::loader::bindings` per mantenere il sottosistema compilabile in
// isolamento mentre il Loader Core (task 2.x) definisce il proprio contesto.
// La riconciliazione verso un unico `GdVersion` condiviso avverrà al cablaggio
// (bootstrap → core → bindings) senza modificare l'interfaccia pubblica.
#ifndef PULSE_LOADER_BINDINGS_BINDINGS_HPP
#define PULSE_LOADER_BINDINGS_BINDINGS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::loader::bindings {

// Versione specifica del binario di Geometry Dash (es. {2, 2074}).
// `major`/`minor` identificano la release; l'uguaglianza è esatta campo-per-campo.
struct GdVersion {
    int major{};
    int minor{};

    friend constexpr bool operator==(const GdVersion&, const GdVersion&) = default;
};

// Chiave di indicizzazione univoca dei bindings (Req 20.1).
// Una coppia (versione, piattaforma) identifica esattamente un `BindingSet`.
struct BindingKey {
    GdVersion version{};
    std::string platformId;  // es. "windows-x64", "android-arm64"

    friend bool operator==(const BindingKey&, const BindingKey&) = default;
};

// Firma di una funzione del gioco: tipo di ritorno + tipi dei parametri.
// Serve alla verifica di compatibilità della firma a compile-time lato SDK
// (Req 5.2) e a documentare il contratto dell'indirizzo risolto.
//
// Convenzione: per i metodi membro il puntatore `this` è il primo parametro
// (es. `MenuLayer::init` → return "bool", parameters {"MenuLayer*"}).
struct Signature {
    std::string returnType;
    std::vector<std::string> parameterTypes;

    friend bool operator==(const Signature&, const Signature&) = default;
};

// Binding di una singola funzione del gioco.
struct FunctionBinding {
    std::string symbol;          // es. "MenuLayer::init"
    std::uintptr_t address{};    // offset relativo alla base dell'immagine
    Signature signature;         // firma per il type-check
    bool resolved{false};        // true se `address` è risolto e usabile (gate Req 20.4)

    friend bool operator==(const FunctionBinding&, const FunctionBinding&) = default;
};

// Insieme dei bindings per una specifica coppia (GD_Version, piattaforma).
class BindingSet {
public:
    BindingSet() = default;
    explicit BindingSet(BindingKey key) : key_(std::move(key)) {}

    const BindingKey& key() const noexcept { return key_; }
    const std::vector<FunctionBinding>& functions() const noexcept { return functions_; }

    // Aggiunge un binding all'insieme.
    void add(FunctionBinding binding) { functions_.push_back(std::move(binding)); }

    // Risoluzione a corrispondenza ESATTA del simbolo (Req 20.2).
    // Nessun fuzzy-match: il confronto è un'uguaglianza esatta della stringa.
    // Restituisce nullopt se nessun simbolo corrisponde esattamente.
    std::optional<FunctionBinding> resolve(std::string_view symbol) const {
        for (const auto& fn : functions_) {
            if (fn.symbol == symbol) {  // uguaglianza esatta, case-sensitive
                return fn;
            }
        }
        return std::nullopt;
    }

private:
    BindingKey key_{};
    std::vector<FunctionBinding> functions_;
};

// Interfaccia del provider di bindings (Req 20.1, 20.2).
// `load` restituisce il set solo su corrispondenza ESATTA della coppia chiave;
// `resolve` risolve un simbolo nel set attualmente caricato a corrispondenza
// esatta. Nessuna implementazione deve eseguire fuzzy-match.
class IBindingsProvider {
public:
    virtual ~IBindingsProvider() = default;

    // Carica il set per la coppia (GD_Version, piattaforma) con corrispondenza
    // esatta. Restituisce nullopt se non esiste alcun set per quella coppia.
    virtual std::optional<BindingSet> load(const BindingKey& key) = 0;

    // Risolve un simbolo a corrispondenza esatta nel set caricato.
    // Restituisce nullopt se il simbolo non è presente o nessun set è caricato.
    virtual std::optional<FunctionBinding> resolve(std::string_view symbol) const = 0;
};

}  // namespace pulse::loader::bindings

#endif  // PULSE_LOADER_BINDINGS_BINDINGS_HPP
