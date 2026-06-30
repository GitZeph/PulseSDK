// loader/mvp/mvp_loader.cpp — implementazione del cablaggio MVP (task 4.4).
#include "mvp/mvp_loader.hpp"

#include <cstdio>
#include <string>
#include <utility>

#include <pulse/hooks.hpp>

#include "bootstrap/windows_bootstrap.hpp"
#include "hooking/hook_gate.hpp"

namespace pulse::loader::mvp {

bindings::GdVersion to_bindings_version(const GdVersion& version) noexcept {
    // Riconciliazione dei due `GdVersion`: il Loader Core usa campi
    // `std::uint32_t`, il Bindings System usa `int`. La conversione è esplicita
    // e localizzata qui al cablaggio, senza toccare le interfacce pubbliche.
    return bindings::GdVersion{
        static_cast<int>(version.major),
        static_cast<int>(version.minor),
    };
}

bindings::BindingKey make_binding_key(const RuntimeContext& context,
                                      std::string_view platformIdOverride) {
    const std::string platformId = platformIdOverride.empty()
                                       ? context.platformId
                                       : std::string{platformIdOverride};
    return bindings::BindingKey{to_bindings_version(context.gdVersion), platformId};
}

MvpLoader::MvpLoader(std::shared_ptr<IVersionDetector> detector,
                     std::unique_ptr<hooking::IHookBackend> backend,
                     std::shared_ptr<bindings::IBindingsProvider> provider,
                     MvpConfig config,
                     DiagnosticSink log)
    : core_(std::move(detector), log),
      backend_(std::move(backend)),
      provider_(std::move(provider)),
      config_(std::move(config)),
      log_(log ? std::move(log) : default_diagnostic_sink()) {}

void MvpLoader::log(std::string_view message) const {
    if (log_) {
        log_(message);
    }
}

MvpResult MvpLoader::fail(MvpStatus status, std::string message) const {
    log(std::string{"MVP: "} + message);
    MvpResult result;
    result.status = status;
    result.message = std::move(message);
    result.injected = false;
    return result;
}

MvpResult MvpLoader::run() {
    hooked_address_ = 0;
    log("MVP: avvio del flusso di caricamento centralizzato (bootstrap → core → "
        "bindings → backend → PULSE_HOOK)");

    // Garantisce che il detour dimostrativo sia registrato (link della TU del
    // hook anche quando il loader è una libreria statica).
    (void)ensure_menulayer_init_hook_registered();

    // 1) Loader Core: rilevamento piattaforma + GD_Version (Req 1.5, 1.7).
    const InitOutcome outcome = core_.initialize();
    if (outcome != InitOutcome::Success) {
        return fail(MvpStatus::VersionDetectionFailed,
                    "rilevamento di GD_Version fallito: caricamento abortito");
    }
    const RuntimeContext& ctx = core_.context();
    log(std::string{"MVP: rilevati GD "} + std::to_string(ctx.gdVersion.major) + "." +
        std::to_string(ctx.gdVersion.minor) + " su piattaforma '" + ctx.platformId +
        "'");

    // 2) Bindings System: carica il set ESATTO (GD_Version, piattaforma) (Req 20.2).
    const bindings::BindingKey key = make_binding_key(ctx, config_.platformIdOverride);
    log(std::string{"MVP: richiesta bindings per ("} + std::to_string(key.version.major) +
        "." + std::to_string(key.version.minor) + ", " + key.platformId + ")");
    if (!provider_->load(key)) {
        return fail(MvpStatus::BindingsNotFound,
                    std::string{"nessun set di bindings per la coppia esatta ("} +
                        std::to_string(key.version.major) + "." +
                        std::to_string(key.version.minor) + ", " + key.platformId +
                        "): hook non installati");
    }

    // 3) Risoluzione dell'indirizzo di MenuLayer::init (corrispondenza esatta).
    //    Gate Req 20.4: si verifica `binding.resolved` PRIMA di toccare il
    //    backend; un binding assente o non risolto blocca l'hook (0 install).
    const auto binding = provider_->resolve(config_.target.bindingSymbol);
    if (!hooking::binding_is_installable(binding)) {
        return fail(MvpStatus::SymbolUnresolved,
                    std::string{"simbolo '"} + std::string{config_.target.bindingSymbol} +
                        "' non risolto: hook annullato (versione/piattaforma "
                        "incompatibile, 0 hook su indirizzi non risolti)");
    }
    log(std::string{"MVP: '"} + binding->symbol + "' risolto all'indirizzo 0x" +
        [&] {
            char buf[2 * sizeof(std::uintptr_t) + 1];
            std::snprintf(buf, sizeof(buf), "%llx",
                          static_cast<unsigned long long>(binding->address));
            return std::string{buf};
        }());

    // 4) Recupera il detour dichiarato con PULSE_HOOK (Req 5.1).
    const auto* registration = pulse::hooks::find(config_.target.registrationSymbol);
    if (registration == nullptr || registration->detour == nullptr) {
        return fail(MvpStatus::HookNotRegistered,
                    std::string{"nessun detour PULSE_HOOK registrato per '"} +
                        std::string{config_.target.registrationSymbol} + "'");
    }

    // 5) Installa il detour sul backend all'indirizzo risolto (Req 2.2),
    //    passando per il gate riusabile che ribadisce binding.resolved prima
    //    di delegare al backend (Req 20.4: 0 hook su indirizzi non risolti).
    if (!backend_->available()) {
        log(std::string{"MVP: attenzione, backend di hooking '"} +
            std::string{backend_->name()} +
            "' non operativo su questo host; il flusso prosegue per la "
            "validazione del cablaggio");
    }
    hooking::HookGate gate{*backend_};
    auto gated = gate.install(*binding, registration->detour);
    if (gated.incompatible()) {
        return fail(MvpStatus::SymbolUnresolved,
                    std::string{"hook bloccato dal gate: "} + gated.error.message);
    }
    if (!gated.installed()) {
        return fail(MvpStatus::InstallFailed,
                    std::string{"installazione dell'hook fallita: "} +
                        gated.error.message);
    }

    // 6) Cabla il trampolino nello slot del detour: dopo questo, invocando il
    //    detour, `callOriginal` invoca l'originale del gioco (Req 2.2, 5.3).
    void* trampoline = gated.trampoline.address();
    if (!pulse::hooks::bind_trampoline(config_.target.registrationSymbol, trampoline)) {
        return fail(MvpStatus::TrampolineBindFailed,
                    std::string{"impossibile cablare il trampolino per '"} +
                        std::string{config_.target.registrationSymbol} + "'");
    }

    hooked_address_ = binding->address;
    log("MVP: hook su MenuLayer::init installato e trampolino cablato; il detour "
        "eseguira' e poi invochera' l'originale");

    MvpResult result;
    result.status = MvpStatus::Success;
    result.message = "hook MenuLayer::init installato con successo";
    result.hookedAddress = binding->address;
    result.injected = true;
    return result;
}

bootstrap::BootstrapResult MvpLoader::bootstrap_and_run() {
    // Cablaggio bootstrap → runtime: l'entry point del runtime è `run()`. Su
    // Windows il bootstrap via DLL proxy ottiene il controllo prima della scena
    // iniziale e invoca l'entry point (Req 1.2, 1.4); su host non-Windows
    // ritorna `UnsupportedHost` senza eseguire il flusso (Req 1.3).
    bootstrap::WindowsBootstrap boot([this]() { return this->run().injected; });
    return boot.inject();
}

const RuntimeContext& MvpLoader::context() const noexcept { return core_.context(); }

}  // namespace pulse::loader::mvp
