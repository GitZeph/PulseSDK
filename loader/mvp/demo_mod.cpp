// loader/mvp/demo_mod.cpp — implementazione del cablaggio della demo mod
// sull'HookEngine reale (task 3.15, Requisiti 9.1, 9.2, 9.3, 9.6).
#include "mvp/demo_mod.hpp"

#include <cstdio>
#include <string>
#include <utility>

#include <pulse/hooks.hpp>

#include "hooking/hook_gate.hpp"  // binding_is_installable (predicato puro riusato)

namespace pulse::loader::mvp {

bool default_demo_fake_trampoline(MenuLayer* self) {
    // Stand-in host dell'originale `MenuLayer::init`: marca l'effetto
    // osservabile e ritorna `true`, così i test possono verificare che il
    // detour abbia invocato l'originale (Req 9.3) dopo aver loggato (Req 9.5).
    if (self != nullptr) {
        self->initialized = true;
    }
    return true;
}

DemoMod::DemoMod(hooking::HookEngine& engine,
                 std::shared_ptr<bindings::IBindingsProvider> provider,
                 std::string_view bindingSymbol,
                 std::string_view registrationSymbol,
                 DiagnosticSink log)
    : engine_(engine),
      provider_(std::move(provider)),
      bindingSymbol_(bindingSymbol),
      registrationSymbol_(registrationSymbol),
      log_(log ? std::move(log) : default_diagnostic_sink()) {}

void DemoMod::set_fake_trampoline(DemoFakeTrampoline trampoline) noexcept {
    // Un trampolino nullo riporterebbe il default per preservare l'invariante
    // "callOriginal è sempre cablato dopo un install riuscito".
    fakeTrampoline_ = trampoline ? trampoline : &default_demo_fake_trampoline;
}

void DemoMod::log(std::string_view message) const {
    if (log_) {
        log_(message);
    }
}

DemoModResult DemoMod::fail(DemoModStatus status, std::string message) const {
    log(std::string{"demo mod: "} + message);
    DemoModResult result;
    result.status = status;
    result.message = std::move(message);
    result.installedHooks = 0;
    return result;
}

DemoModResult DemoMod::install() {
    hooked_address_ = 0;
    log(std::string{"demo mod '"} + std::string{kDemoModId} +
        "': cablaggio del detour dimostrativo su '" + bindingSymbol_ +
        "' tramite l'HookEngine reale");

    // Garantisce che il detour dimostrativo sia registrato presso il registro
    // dello SDK (link della TU del hook anche con loader statico).
    (void)ensure_menulayer_init_hook_registered();

    // 1) Risoluzione dell'indirizzo dai bindings reali, a corrispondenza esatta
    //    del simbolo (Req 20.2). Provider assente o simbolo non risolto =>
    //    fail-open senza hook (Req 9.6).
    std::optional<bindings::FunctionBinding> binding;
    if (provider_) {
        binding = provider_->resolve(bindingSymbol_);
    }
    if (!hooking::binding_is_installable(binding)) {
        // Req 9.6: log della causa, NESSUN hook installato, GD prosegue.
        return fail(DemoModStatus::SymbolUnresolved,
                    std::string{"funzione bersaglio '"} + bindingSymbol_ +
                        "' non risolta dai bindings: nessun hook installato per la "
                        "demo mod, Geometry Dash prosegue senza mod (Req 9.6)");
    }

    // 2) Recupera il detour dichiarato con PULSE_HOOK per il simbolo di
    //    registrazione (Req 5.1).
    const auto* registration = pulse::hooks::find(registrationSymbol_);
    if (registration == nullptr || registration->detour == nullptr) {
        return fail(DemoModStatus::HookNotRegistered,
                    std::string{"nessun detour PULSE_HOOK registrato per '"} +
                        registrationSymbol_ + "'");
    }

    // 3) Installa ESATTAMENTE un hook sulla funzione risolta tramite l'HookEngine
    //    (Req 9.1). L'engine garantisce un solo detour per funzione bersaglio e
    //    applica la politica di retry/rollback atomico (Req 2.5).
    hooking::HookRequest request;
    request.owner = std::string{kDemoModId};
    request.functionName = bindingSymbol_;
    request.target = binding->address;
    request.detour = registration->detour;
    request.priority = hooking::kHookPriorityDefault;
    request.loadOrder = 0;

    const hooking::InstallOutcome outcome = engine_.install(request);
    if (!outcome.installed) {
        const std::string cause =
            outcome.error.has_value() ? outcome.error->cause.message
                                      : std::string{"causa sconosciuta"};
        return fail(DemoModStatus::InstallFailed,
                    std::string{"installazione dell'hook su '"} + bindingSymbol_ +
                        "' fallita: " + cause);
    }

    // 4) Cabla il trampolino nello slot del detour così che `callOriginal`
    //    invochi l'originale preservandone firma, parametri e valore di ritorno
    //    (Req 9.3). Sull'host si usa il trampolino finto (stand-in
    //    dell'originale) per l'osservabilità (Req 9.5); sul processo reale di
    //    GD il trampolino eseguibile è fornito dal backend.
    void* trampoline = reinterpret_cast<void*>(fakeTrampoline_);
    if (!pulse::hooks::bind_trampoline(registrationSymbol_, trampoline)) {
        return fail(DemoModStatus::TrampolineBindFailed,
                    std::string{"impossibile cablare il trampolino per '"} +
                        registrationSymbol_ + "'");
    }

    hooked_address_ = binding->address;

    char addr[2 * sizeof(std::uintptr_t) + 3];
    std::snprintf(addr, sizeof(addr), "0x%llx",
                  static_cast<unsigned long long>(binding->address));
    log(std::string{"demo mod '"} + std::string{kDemoModId} +
        "': installato 1 hook su '" + bindingSymbol_ + "' all'indirizzo " + addr +
        "; il detour eseguira' e poi invochera' l'originale (Req 9.1, 9.2, 9.3)");

    DemoModResult result;
    result.status = DemoModStatus::HookInstalled;
    result.message = "hook MenuLayer::init installato per la demo mod (1 hook)";
    result.hookedAddress = binding->address;
    result.installedHooks = engine_.totalHooks();
    return result;
}

}  // namespace pulse::loader::mvp
