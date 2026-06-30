// loader/mvp/demo_mod.hpp — cablaggio della demo mod sull'HookEngine reale
// (task 3.15, Requisiti 9.1, 9.2, 9.3, 9.6).
//
// Questo modulo riusa il detour dimostrativo MVP (`mvp/menulayer_init_hook.*`)
// ma, a differenza di `MvpLoader` (che passa per `HookGate`), lo cabla sul
// COORDINATORE reale degli hook — `pulse::hooking::HookEngine` — e sui bindings
// reali risolti dall'`IBindingsProvider`. Realizza la prova end-to-end
// host-testabile descritta nel design (sezione 8 "Prova end-to-end"):
//
//   1. Req 9.1 — risolve `MenuLayer::init` dai bindings e installa
//      *esattamente un* hook su quella funzione attraverso l'HookEngine, prima
//      della prima invocazione (l'engine garantisce un solo detour per
//      funzione bersaglio).
//   2. Req 9.2 — il detour emette un log via la facility diagnostica che
//      identifica la demo mod (`pulse.demo`) e l'hook come sorgente.
//   3. Req 9.3 — il detour invoca l'originale tramite trampolino e ne ritorna
//      il valore non modificato.
//   4. Req 9.6 — se `MenuLayer::init` è non risolto: log della causa, NESSUN
//      hook installato, Geometry Dash prosegue (nessuna terminazione).
//
// Osservabilità host: il codice reale di `MenuLayer::init` e un trampolino
// eseguibile esistono solo nel processo reale di Geometry Dash (Fase E,
// manuale). Sull'host di test si mantiene il `DemoLogSink` (in
// `menulayer_init_hook.*`) e un **trampolino finto** iniettabile — stand-in
// dell'originale — così l'ordine "detour prima dell'originale" (Req 9.5) e il
// valore di ritorno preservato (Req 9.3) sono verificabili in CI senza GD.
//
// Stack: C++20/23. Il modulo non dipende da un backend di piattaforma: i test
// usano il `FakeBackend` in-memory dietro l'HookEngine.
#ifndef PULSE_LOADER_MVP_DEMO_MOD_HPP
#define PULSE_LOADER_MVP_DEMO_MOD_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "bindings/bindings.hpp"
#include "core/loader_core.hpp"  // DiagnosticSink, default_diagnostic_sink()
#include "hooking/hook_engine.hpp"
#include "mvp/menulayer_init_hook.hpp"

namespace pulse::loader::mvp {

// Esito del cablaggio della demo mod sull'HookEngine reale.
enum class DemoModStatus {
    HookInstalled,         // esattamente un hook installato su MenuLayer::init (Req 9.1)
    SymbolUnresolved,      // MenuLayer::init non risolto: 0 hook, GD prosegue (Req 9.6)
    HookNotRegistered,     // nessun detour PULSE_HOOK registrato per il simbolo
    InstallFailed,         // l'HookEngine/backend ha fallito l'installazione
    TrampolineBindFailed,  // impossibile cablare il trampolino nello slot del detour
};

// Risultato dettagliato del cablaggio, ispezionabile senza eccezioni.
struct DemoModResult {
    DemoModStatus status{DemoModStatus::SymbolUnresolved};
    std::string message{};
    std::uintptr_t hookedAddress{0};
    std::size_t installedHooks{0};  // hook installati (1 su successo, 0 su fail-open)

    [[nodiscard]] bool installed() const noexcept {
        return status == DemoModStatus::HookInstalled;
    }
};

// Trampolino finto: stand-in host della funzione originale `MenuLayer::init`.
// Il detour lo invoca via `callOriginal` per l'osservabilità host (Req 9.3,
// 9.5). La firma rispecchia `bool MenuLayer::init()` con `self` esplicito.
using DemoFakeTrampoline = bool (*)(MenuLayer*);

// Trampolino finto di default: modella l'effetto osservabile dell'originale
// marcando `self->initialized` e ritornando `true`.
bool default_demo_fake_trampoline(MenuLayer* self);

// DemoMod — cabla il detour dimostrativo (`menulayer_init_hook`) sull'HookEngine
// reale e sui bindings reali (Req 9.1, 9.2, 9.3, 9.6).
//
// Collaboratori iniettabili (testabilità host):
//   * `engine`  : coordinatore reale degli hook (test: HookEngine + FakeBackend);
//   * `provider`: provider dei bindings reale, già caricato con il set della
//                 coppia (GD_Version, piattaforma) rilevata;
//   * `log`     : sink diagnostico del cablaggio (default: stderr).
class DemoMod {
public:
    DemoMod(hooking::HookEngine& engine,
            std::shared_ptr<bindings::IBindingsProvider> provider,
            std::string_view bindingSymbol = kMenuLayerInitBindingSymbol,
            std::string_view registrationSymbol = kMenuLayerInitRegistrationSymbol,
            DiagnosticSink log = nullptr);

    // Risolve `MenuLayer::init` dai bindings e installa *esattamente un* hook
    // su quella funzione via l'HookEngine, cablando il trampolino (finto
    // sull'host) nello slot del detour così che `callOriginal` invochi
    // l'originale (Req 9.1, 9.2, 9.3). Se il simbolo è non risolto: log della
    // causa, nessun hook, GD prosegue (Req 9.6).
    DemoModResult install();

    // Imposta il trampolino finto usato per l'osservabilità host (Req 9.3,
    // 9.5). Sul processo reale di GD il trampolino eseguibile è quello del
    // backend; sull'host si usa questo stand-in.
    void set_fake_trampoline(DemoFakeTrampoline trampoline) noexcept;

    // Indirizzo agganciato dall'ultimo `install()` riuscito (0 se nessuno).
    [[nodiscard]] std::uintptr_t hooked_address() const noexcept {
        return hooked_address_;
    }

private:
    void log(std::string_view message) const;
    DemoModResult fail(DemoModStatus status, std::string message) const;

    hooking::HookEngine& engine_;
    std::shared_ptr<bindings::IBindingsProvider> provider_;
    std::string bindingSymbol_;
    std::string registrationSymbol_;
    DiagnosticSink log_;
    DemoFakeTrampoline fakeTrampoline_{&default_demo_fake_trampoline};
    std::uintptr_t hooked_address_{0};
};

}  // namespace pulse::loader::mvp

#endif  // PULSE_LOADER_MVP_DEMO_MOD_HPP
