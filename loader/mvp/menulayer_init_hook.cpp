// loader/mvp/menulayer_init_hook.cpp — definizione del detour dimostrativo MVP.
//
// Espande la macro `PULSE_HOOK` per `MenuLayer::init` (registrazione
// "MenuLayer_init") con logging dell'esecuzione del detour e dell'originale
// (Requisiti 2.2, 5.3). Il corpo del detour:
//   1) logga l'ingresso (prima dell'originale);
//   2) invoca `callOriginal(self)` preservando parametri e valore di ritorno;
//   3) logga l'esito dell'originale.
#include "mvp/menulayer_init_hook.hpp"

#include <string>
#include <utility>

#include <pulse/hooks.hpp>

namespace pulse::loader::mvp {
namespace {

// Accessor del sink di log del demo a inizializzazione "on first use" (Meyers
// singleton). MOTIVO: `set_demo_log_sink` viene invocata da `runtime_entry`,
// che gira da un costruttore di early-load (`__attribute__((constructor))`).
// Un `std::function` globale a scope di file potrebbe non essere ancora
// costruito a quel punto; il suo costruttore di default, eseguito DOPO, lo
// resetterebbe a vuoto, perdendo il sink (stesso fiasco dell'ordine di
// inizializzazione statica già visto per il registro degli hook). Uno static
// locale è inizializzato una sola volta, alla prima chiamata, quindi è immune.
DemoLogSink& demo_sink() {
    static DemoLogSink sink{};
    return sink;
}

}  // namespace

void set_demo_log_sink(DemoLogSink sink) { demo_sink() = std::move(sink); }

void demo_log(std::string_view message) {
    if (demo_sink()) {
        demo_sink()(message);
    }
}

}  // namespace pulse::loader::mvp

// ---------------------------------------------------------------------------
// Detour dimostrativo su MenuLayer::init (Requisiti 2.2, 5.3).
//
// La macro genera il detour e lo registra presso il registro globale dello SDK
// (`pulse::hooks`). L'Hooking Engine (MvpLoader) risolve l'indirizzo via i
// bindings, installa questo detour sul backend e cabla il trampolino verso
// l'originale: dopo il cablaggio `callOriginal(self)` invoca l'originale del
// gioco preservandone firma, parametri e valore di ritorno.
// ---------------------------------------------------------------------------
PULSE_HOOK(MenuLayer_init, bool, (pulse::loader::mvp::MenuLayer* self)) {
    // Req 9.2/9.5: il log identifica la demo mod (`pulse.demo`) e l'hook
    // (MenuLayer::init) come sorgente, e registra l'esecuzione del detour PRIMA
    // dell'originale.
    pulse::loader::mvp::demo_log(
        "[pulse.demo] MenuLayer::init detour: esecuzione del detour (prima "
        "dell'originale)");

    // Invoca l'originale del gioco preservando il parametro `self` e il valore
    // di ritorno della firma `bool MenuLayer::init()`.
    const bool ok = callOriginal(self);

    pulse::loader::mvp::demo_log(
        ok ? "[pulse.demo] MenuLayer::init: originale eseguito, valore di "
             "ritorno=true"
           : "[pulse.demo] MenuLayer::init: originale eseguito, valore di "
             "ritorno=false");
    return ok;
}

// ---------------------------------------------------------------------------
// Registrazione ESPLICITA e idempotente del detour (definita DOPO la macro così
// da poter referenziare il detour e lo slot del trampolino generati in
// `pulse_hook_MenuLayer_init`).
//
// MOTIVO: il detour si auto-registra anche tramite l'inizializzatore statico
// `pulse_registration` della macro, ma l'ORDINE di esecuzione degli
// inizializzatori statici tra unità di traduzione NON è garantito. Nel
// Loader_Artifact l'entry point gira da un costruttore di early-load
// (`__attribute__((constructor))` in macos_bootstrap.cpp) che può essere
// eseguito PRIMA dell'inizializzatore di `pulse_registration`: in quel caso il
// registro sarebbe ancora vuoto quando il runtime cerca il detour ("nessun
// detour registrato"). Registrando qui, nel momento in cui il LoaderCore invoca
// `ensure_...`, la registrazione è garantita PRIMA della `find` (Req 2.2).
//
// Idempotente: se il simbolo è già presente (l'inizializzatore statico è già
// girato), non si registra di nuovo.
namespace pulse::loader::mvp {

std::string_view ensure_menulayer_init_hook_registered() {
    if (::pulse::hooks::find(
            std::string_view{kMenuLayerInitRegistrationSymbol}) == nullptr) {
        ::pulse::hooks::register_hook(
            kMenuLayerInitRegistrationSymbol,
            reinterpret_cast<void*>(&::pulse_hook_MenuLayer_init::pulse_detour),
            reinterpret_cast<void**>(
                &::pulse_hook_MenuLayer_init::pulse_original));
    }
    return kMenuLayerInitRegistrationSymbol;
}

}  // namespace pulse::loader::mvp
