// loader/hooking/hook_gate.hpp — gating dell'installazione degli hook sulla
// risoluzione dei bindings (Layer 3 — Hooking Engine, Requisiti 20.3, 20.4).
//
// Questo componente realizza la *politica di gating* descritta nel design
// (sezione "Layer 2 — Bindings System"):
//
//   «se `load` non trova corrispondenza esatta (GD_Version, piattaforma), il
//    loader mostra errore di incompatibilità e RIFIUTA ogni `install` su
//    simboli non risolti; l'invariante numeroHookSuIndirizziNonRisolti == 0 è
//    mantenuta dall'Hooking Engine che verifica `binding.resolved` PRIMA di
//    chiamare il backend.»
//
// Responsabilità (single responsibility, riusabile):
//   * consultare `IHookBackend::available()` PRIMA di ogni `install`; se il
//     backend non è disponibile a runtime, NON installare alcun hook (0 hook),
//     mantenendo l'invariante, e registrare una diagnostica che nomina il
//     backend via `name()` (Requisiti 3.8, 10.3 della feature
//     `pulse-gd-integration`);
//   * verificare `FunctionBinding::resolved` (e la presenza del binding stesso)
//     PRIMA di invocare `IHookBackend::install`;
//   * NON installare alcun hook quando il binding è assente o non risolto,
//     così il numero di hook su indirizzi non risolti resta sempre 0 (Req 20.4);
//   * fare emergere un errore di incompatibilità leggibile (Req 20.3).
//
// È header-only e non ha dipendenze esterne oltre all'interfaccia canonica
// `pulse::hooking::IHookBackend` (hooking/hook_backend.hpp) e al modello dati
// del Bindings System (`pulse::loader::bindings::FunctionBinding`). Può quindi
// essere riusato sia dal cablaggio MVP sia dal futuro motore di catena.
//
// Stack: C++20/23 (Requisito 26.1).
#ifndef PULSE_LOADER_HOOKING_HOOK_GATE_HPP
#define PULSE_LOADER_HOOKING_HOOK_GATE_HPP

#include "bindings/bindings.hpp"
#include "hooking/hook_backend.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace pulse::hooking {

// Alias locale verso il modello dati del Bindings System (Layer 2), così i
// riferimenti `bindings::...` in questo header risolvono correttamente anche
// all'interno del namespace `pulse::hooking`.
namespace bindings = ::pulse::loader::bindings;

// Sink diagnostico leggero del gate: callback invocato per "registrare" una
// diagnostica (es. backend non disponibile, Req 3.8/10.3). Coerente con il
// DiagnosticSink del Loader Core e con l'HookEventSink della catena; se nullo,
// la diagnostica resta comunque disponibile in `GateResult::error`.
using GateDiagnosticSink = std::function<void(std::string_view)>;

// Esito del gating di un singolo install.
enum class GateOutcome {
    // Binding presente e risolto: il backend ha installato il detour.
    Installed,
    // Binding assente (nessuna corrispondenza esatta) oppure presente ma con
    // `resolved == false`: NESSUN install è stato tentato (0 hook), e viene
    // riportato un errore di incompatibilità (Req 20.3, 20.4).
    IncompatibleUnresolved,
    // Il backend di hooking ha riportato `available() == false` al runtime:
    // NESSUN install è stato tentato (0 hook) e viene registrata una
    // diagnostica che nomina il backend (Req 3.8, 10.3).
    BackendUnavailable,
    // Gate superato (binding risolto) ma il backend ha fallito l'install: la
    // diagnostica del backend è propagata in `error` (Req 2.5).
    InstallFailed,
};

// Risultato del gating: esito, trampolino (solo se installato) ed eventuale
// errore. Pensato per essere ispezionato dal chiamante senza eccezioni.
struct GateResult {
    GateOutcome outcome{GateOutcome::IncompatibleUnresolved};
    Trampoline trampoline{};
    HookError error{};

    // true sse il detour è stato effettivamente installato sul backend.
    [[nodiscard]] bool installed() const noexcept {
        return outcome == GateOutcome::Installed;
    }

    // true sse l'install è stato bloccato per binding assente o non risolto.
    // In questo caso il backend NON è stato invocato (Req 20.4).
    [[nodiscard]] bool incompatible() const noexcept {
        return outcome == GateOutcome::IncompatibleUnresolved;
    }

    // true sse l'install è stato bloccato perché il backend non è disponibile
    // a runtime (`available() == false`). In questo caso il backend NON è stato
    // invocato e nessun hook è installato (Req 3.8, 10.3).
    [[nodiscard]] bool backendUnavailable() const noexcept {
        return outcome == GateOutcome::BackendUnavailable;
    }
};

// ---------------------------------------------------------------------------
// Predicati puri del gate (riusabili e privi di effetti collaterali).
// ---------------------------------------------------------------------------

// Un binding è installabile sse è presente E il suo indirizzo è risolto.
[[nodiscard]] inline bool binding_is_installable(
    const bindings::FunctionBinding& binding) noexcept {
    return binding.resolved;
}

// Variante su binding opzionale: assente => non installabile (nessuna
// corrispondenza esatta nel set, Req 20.3).
[[nodiscard]] inline bool binding_is_installable(
    const std::optional<bindings::FunctionBinding>& binding) noexcept {
    return binding.has_value() && binding->resolved;
}

// Costruisce l'errore di incompatibilità leggibile per un binding bloccato.
[[nodiscard]] inline HookError make_incompatibility_error(
    std::string_view symbol) {
    std::string msg{"hook bloccato: binding non risolto per il simbolo '"};
    msg.append(symbol);
    msg.append(
        "'; versione/piattaforma incompatibile, nessun hook installato su "
        "indirizzi non risolti (Req 20.3, 20.4)");
    return HookError{HookErrorCode::InvalidArgument, std::move(msg)};
}

[[nodiscard]] inline HookError make_incompatibility_error() {
    return HookError{
        HookErrorCode::InvalidArgument,
        "hook bloccato: nessuna corrispondenza esatta (GD_Version, "
        "piattaforma); versione incompatibile, nessun hook installato su "
        "indirizzi non risolti (Req 20.3, 20.4)"};
}

// Costruisce l'errore diagnostico per backend non disponibile, NOMINANDO il
// backend via `name()` (Req 3.8, 10.3): l'errore identifica per nome il backend
// che ha riportato `available() == false`.
[[nodiscard]] inline HookError make_backend_unavailable_error(
    std::string_view backend_name) {
    std::string msg{"hook bloccato: backend di hooking '"};
    msg.append(backend_name);
    msg.append(
        "' non disponibile a runtime (available()==false); nessun hook "
        "installato, Geometry Dash prosegue senza mod (Req 3.8, 10.3)");
    return HookError{HookErrorCode::Unsupported, std::move(msg)};
}

// ---------------------------------------------------------------------------
// HookGate — componente riusabile che fa da guardia all'install path.
// ---------------------------------------------------------------------------
//
// Avvolge un `IHookBackend` e garantisce per costruzione l'invariante
// `numeroHookSuIndirizziNonRisolti == 0`: l'unico percorso che raggiunge
// `backend.install(...)` richiede un binding presente e risolto.
class HookGate {
public:
    explicit HookGate(IHookBackend& backend) noexcept : backend_(backend) {}

    // Variante con sink diagnostico: la diagnostica del gate (es. backend non
    // disponibile, Req 3.8/10.3) viene "registrata" invocando `sink` con il
    // messaggio. Se `sink` è nullo, la diagnostica resta in `GateResult::error`.
    HookGate(IHookBackend& backend, GateDiagnosticSink sink) noexcept
        : backend_(backend), sink_(std::move(sink)) {}

    // Gating dell'install per un binding già risolto in un opzionale.
    // - backend non disponibile => BackendUnavailable (nessun install, 0 hook)
    // - opzionale vuoto          => IncompatibleUnresolved (nessuna corrispondenza)
    // - binding.resolved=0       => IncompatibleUnresolved (indirizzo non risolto)
    // - altrimenti               => delega a backend.install e riporta l'esito.
    GateResult install(const std::optional<bindings::FunctionBinding>& binding,
                       void* detour) {
        // GATE DI DISPONIBILITÀ: consulta available() PRIMA di ogni install
        // (Req 3.8, 10.3). Se il backend non è disponibile, nessun install è
        // tentato e l'invariante 0 hook è preservata.
        if (!backend_.available()) {
            return blockUnavailable();
        }
        if (!binding.has_value()) {
            return blockUnresolved(make_incompatibility_error());
        }
        return installResolvedChecked(*binding, detour);
    }

    // Gating dell'install per un binding concreto.
    GateResult install(const bindings::FunctionBinding& binding, void* detour) {
        // GATE DI DISPONIBILITÀ: consulta available() PRIMA di ogni install
        // (Req 3.8, 10.3).
        if (!backend_.available()) {
            return blockUnavailable();
        }
        return installResolvedChecked(binding, detour);
    }

    // Numero di hook installati con successo su indirizzi RISOLTI.
    [[nodiscard]] std::size_t installedCount() const noexcept {
        return installedResolved_;
    }

    // Numero di install bloccati dal gate per binding assente/non risolto.
    [[nodiscard]] std::size_t blockedUnresolvedCount() const noexcept {
        return blockedUnresolved_;
    }

    // Numero di install bloccati dal gate perché il backend non è disponibile
    // a runtime (`available() == false`, Req 3.8/10.3).
    [[nodiscard]] std::size_t blockedUnavailableCount() const noexcept {
        return blockedUnavailable_;
    }

    // Invariante del gate (Req 20.4): il numero di hook installati su indirizzi
    // NON risolti è sempre 0, perché l'unico percorso verso `install` richiede
    // un binding risolto. Esposto per asserzioni esplicite nei test.
    [[nodiscard]] std::size_t hooksOnUnresolvedAddresses() const noexcept {
        return installedUnresolved_;
    }

private:
    // Percorso comune dopo il superamento del gate di disponibilità: verifica
    // `binding.resolved` PRIMA di toccare il backend (Req 20.4) e, se risolto,
    // delega a `backend_.install` riportandone l'esito.
    GateResult installResolvedChecked(const bindings::FunctionBinding& binding,
                                      void* detour) {
        if (!binding_is_installable(binding)) {
            return blockUnresolved(make_incompatibility_error(binding.symbol));
        }

        // Binding risolto: è lecito installare all'indirizzo risolto (Req 2.2).
        auto result = backend_.install(binding.address, detour);
        if (!result.has_value()) {
            GateResult gated;
            gated.outcome = GateOutcome::InstallFailed;
            gated.error = result.error();
            return gated;
        }

        ++installedResolved_;
        GateResult gated;
        gated.outcome = GateOutcome::Installed;
        gated.trampoline = std::move(result).value();
        return gated;
    }

    GateResult blockUnresolved(HookError error) {
        ++blockedUnresolved_;
        GateResult gated;
        gated.outcome = GateOutcome::IncompatibleUnresolved;
        gated.error = std::move(error);
        // Nota: NON si tocca `installedUnresolved_` né il backend: l'invariante
        // numeroHookSuIndirizziNonRisolti == 0 è preservata per costruzione.
        return gated;
    }

    // Blocca l'install perché il backend non è disponibile a runtime (Req 3.8,
    // 10.3): nessun install è tentato (0 hook) e si registra una diagnostica
    // che NOMINA il backend via `name()`.
    GateResult blockUnavailable() {
        ++blockedUnavailable_;
        GateResult gated;
        gated.outcome = GateOutcome::BackendUnavailable;
        gated.error = make_backend_unavailable_error(backend_.name());
        // "Registra" la diagnostica sul sink se presente (Req 3.8/10.3); il
        // messaggio resta comunque disponibile in `gated.error` per i chiamanti
        // che non forniscono un sink.
        if (sink_) {
            sink_(gated.error.message);
        }
        // Nota: il backend NON è invocato: l'invariante 0 hook è preservata.
        return gated;
    }

    IHookBackend& backend_;
    GateDiagnosticSink sink_{};
    std::size_t installedResolved_{0};
    std::size_t blockedUnresolved_{0};
    std::size_t blockedUnavailable_{0};
    // Sempre 0 per costruzione: nessun percorso installa su indirizzi non
    // risolti. Mantenuto come campo per rendere esplicito l'invariante.
    std::size_t installedUnresolved_{0};
};

// ---------------------------------------------------------------------------
// Funzione libera di convenienza: gating una-tantum senza istanziare HookGate.
// ---------------------------------------------------------------------------

[[nodiscard]] inline GateResult gate_install(
    IHookBackend& backend,
    const std::optional<bindings::FunctionBinding>& binding, void* detour,
    GateDiagnosticSink sink = nullptr) {
    HookGate gate{backend, std::move(sink)};
    return gate.install(binding, detour);
}

[[nodiscard]] inline GateResult gate_install(
    IHookBackend& backend, const bindings::FunctionBinding& binding,
    void* detour, GateDiagnosticSink sink = nullptr) {
    HookGate gate{backend, std::move(sink)};
    return gate.install(binding, detour);
}

}  // namespace pulse::hooking

#endif  // PULSE_LOADER_HOOKING_HOOK_GATE_HPP
