// tests/hook_gate_availability_property_test.cpp
// Feature: pulse-gd-integration, Property 3 — Gate sul backend non disponibile.
// Validates: Requirements 3.8, 10.3 (Requisiti 3.8, 10.3)
//
// Property 3 (design.md / tasks.md, task 3.4): "Con un backend la cui
// `available()==false` e qualunque insieme di richieste di install
// (binding risolti / non risolti / assenti) → 0 hook installati + una
// diagnostica che NOMINA il backend (via `name()`)."
//
// `HookGate` (loader/hooking/hook_gate.hpp) consulta `IHookBackend::available()`
// PRIMA di ogni `install`: se il backend riporta `false` al runtime, il gate
// blocca OGNI install (anche per binding perfettamente risolti), non sfiora mai
// il backend (0 tentativi → 0 hook) e registra una diagnostica che identifica
// il backend per nome (Req 3.8). È la stessa policy fail-open invocata quando
// il backend selezionato è non disponibile a runtime (Req 10.3): zero hook,
// diagnostica con la causa.
//
// Su input randomizzati con RapidCheck (≥100 iterazioni per default) generiamo
// un insieme arbitrario di richieste, ciascuna marcata come:
//   * assente               — binding optional vuoto (nessuna corrispondenza),
//   * presente non risolto  — FunctionBinding con `resolved == false`,
//   * presente risolto      — FunctionBinding con `resolved == true`,
// e verifichiamo che, con `available()==false`, l'esito sia SEMPRE
// `BackendUnavailable`, con 0 hook e una diagnostica che nomina il backend.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bindings/bindings.hpp"
#include "hooking/hook_backend.hpp"
#include "hooking/hook_gate.hpp"

namespace {

using pulse::hooking::ByteSpan;
using pulse::hooking::GateOutcome;
using pulse::hooking::HookError;
using pulse::hooking::HookErrorCode;
using pulse::hooking::HookGate;
using pulse::hooking::IHookBackend;
using pulse::hooking::Result;
using pulse::hooking::Trampoline;
using pulse::loader::bindings::FunctionBinding;
using pulse::loader::bindings::Signature;

int g_detour = 0;
void* const kDetour = &g_detour;

// Backend di test la cui disponibilità è SEMPRE false: modella un backend
// selezionato ma non operativo a runtime (es. Dobby non abilitato sul target,
// Req 3.8/10.3). `FakeBackend` è `final` e riporta `available()==true`, quindi
// per esercitare il ramo "non disponibile" del gate serve un backend dedicato
// con `available()==false` e un `name()` personalizzato per verificare la
// diagnostica. Traccia ogni tentativo di install per provare che il gate non
// raggiunga MAI il backend quando `available()==false`.
class UnavailableBackend final : public IHookBackend {
public:
    explicit UnavailableBackend(std::string name) : name_(std::move(name)) {}

    [[nodiscard]] std::size_t installAttempts() const noexcept {
        return installAttempts_;
    }

    Result<Trampoline> install(std::uintptr_t target, void* /*detour*/) override {
        // Non dovrebbe MAI essere invocato dal gate quando available()==false.
        ++installAttempts_;
        return Result<Trampoline>::ok(
            Trampoline{reinterpret_cast<void*>(target ^ 0xA5A5A5A5ULL)});
    }
    Result<void> remove(std::uintptr_t /*target*/) override {
        return Result<void>::ok();
    }
    Result<ByteSpan> readOriginal(std::uintptr_t /*target*/,
                                  std::size_t len) override {
        return Result<ByteSpan>::ok(
            ByteSpan{std::vector<std::uint8_t>(len, 0u)});
    }
    [[nodiscard]] std::string_view name() const noexcept override {
        return name_;
    }
    [[nodiscard]] bool available() const noexcept override { return false; }

private:
    std::string name_;
    std::size_t installAttempts_{0};
};

// Codifica della tipologia di richiesta generata.
enum class RequestKind { Absent = 0, PresentUnresolved = 1, PresentResolved = 2 };

}  // namespace

// ===========================================================================
// Property 3 — con backend `available()==false`, qualunque insieme di richieste
// (assenti / non risolte / risolte) produce SEMPRE: 0 hook installati, 0
// tentativi sul backend, e per ogni richiesta una diagnostica che nomina il
// backend via `name()`.
// Feature: pulse-gd-integration, Property 3. Validates: Requirements 3.8, 10.3.
// ===========================================================================
RC_GTEST_PROP(Property3BackendUnavailableGate,
              ZeroHooksAndNamedDiagnosticWhenBackendUnavailable,
              ()) {
    // Nome del backend non vuoto e variabile, per verificare che la diagnostica
    // riporti SEMPRE il nome corrente (non un valore hard-coded).
    const auto backendName =
        *rc::gen::nonEmpty(
             rc::gen::container<std::string>(rc::gen::inRange<char>('a', 'z')))
             .as("nome backend");

    // Insieme arbitrario (anche vuoto) di richieste di install, ciascuna di
    // tipo Absent / PresentUnresolved / PresentResolved.
    const auto kinds =
        *rc::gen::container<std::vector<RequestKind>>(
             rc::gen::element(RequestKind::Absent,
                              RequestKind::PresentUnresolved,
                              RequestKind::PresentResolved))
             .as("tipi di richiesta");

    UnavailableBackend backend{backendName};

    // Sink diagnostico: raccoglie ogni messaggio "registrato" dal gate.
    std::vector<std::string> diagnostics;
    HookGate gate{backend, [&diagnostics](std::string_view m) {
                      diagnostics.emplace_back(m);
                  }};

    std::uintptr_t addr = 0x1000;  // non nullo, distinto per indice
    for (const auto kind : kinds) {
        const std::uintptr_t target = addr;
        addr += 0x40;

        switch (kind) {
            case RequestKind::Absent: {
                const auto result =
                    gate.install(std::optional<FunctionBinding>{}, kDetour);
                RC_ASSERT(result.outcome == GateOutcome::BackendUnavailable);
                RC_ASSERT(result.backendUnavailable());
                RC_ASSERT(!result.installed());
                // Diagnostica che NOMINA il backend (Req 3.8).
                RC_ASSERT(result.error.message.find(backendName) !=
                          std::string::npos);
                break;
            }
            case RequestKind::PresentUnresolved: {
                FunctionBinding fn;
                fn.symbol = "Sym::f";
                fn.address = target;
                fn.signature = Signature{"void", {}};
                fn.resolved = false;
                const auto result =
                    gate.install(std::optional<FunctionBinding>{fn}, kDetour);
                RC_ASSERT(result.outcome == GateOutcome::BackendUnavailable);
                RC_ASSERT(result.error.message.find(backendName) !=
                          std::string::npos);
                break;
            }
            case RequestKind::PresentResolved: {
                // Binding perfettamente risolto: l'UNICA causa del blocco è la
                // non disponibilità del backend (il gate di disponibilità
                // precede il controllo di risoluzione).
                FunctionBinding fn;
                fn.symbol = "MenuLayer::init";
                fn.address = target;
                fn.signature = Signature{"bool", {"MenuLayer*"}};
                fn.resolved = true;
                const auto result =
                    gate.install(std::optional<FunctionBinding>{fn}, kDetour);
                RC_ASSERT(result.outcome == GateOutcome::BackendUnavailable);
                RC_ASSERT(result.error.message.find(backendName) !=
                          std::string::npos);
                break;
            }
        }
    }

    // INVARIANTE (Req 3.8, 10.3): nessun hook installato e backend mai sfiorato.
    RC_ASSERT(backend.installAttempts() == 0u);
    RC_ASSERT(gate.installedCount() == 0u);
    RC_ASSERT(gate.hooksOnUnresolvedAddresses() == 0u);
    RC_ASSERT(gate.blockedUnavailableCount() == kinds.size());

    // Ogni richiesta ha prodotto esattamente una diagnostica che nomina il
    // backend (via `name()`); con insieme vuoto, nessuna diagnostica.
    RC_ASSERT(diagnostics.size() == kinds.size());
    for (const auto& msg : diagnostics) {
        RC_ASSERT(msg.find(backendName) != std::string::npos);
    }
}
