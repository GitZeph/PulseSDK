// loader/bootstrap/platform_bootstrap.hpp — Layer 0 (Bootstrap/Injection).
//
// Definisce l'astrazione di bootstrap per-piattaforma del Pulse Loader
// (Requisito 1). L'obiettivo comune di ogni implementazione è ottenere il
// controllo del processo di Geometry Dash PRIMA dell'esecuzione del codice
// della scena iniziale del gioco (Req 1.2) e invocare un unico entry point
// centralizzato del runtime (Req 1.4).
//
// Questo header è completamente PLATFORM-AGNOSTIC: l'interfaccia
// `IPlatformBootstrap`, l'enum `Platform`, `DiagnosticError` e
// `BootstrapResult` non dipendono da API specifiche del SO e compilano su
// qualsiasi host (Windows, macOS, Linux). Le implementazioni concrete
// (es. `WindowsBootstrap`) isolano il codice specifico di piattaforma dietro
// guardie del preprocessore.
#ifndef PULSE_LOADER_BOOTSTRAP_PLATFORM_BOOTSTRAP_HPP
#define PULSE_LOADER_BOOTSTRAP_PLATFORM_BOOTSTRAP_HPP

#include <optional>
#include <string>
#include <string_view>

namespace pulse::bootstrap {

// Piattaforme supportate dal bootstrap (Req 1.1). L'enum elenca anche le
// varianti ABI di Android, distinte perché richiedono artefatti separati.
enum class Platform {
    WindowsX64,
    MacOS,
    AndroidArm64,
    AndroidArmV7,
    IOSArm64,
};

// Identificatore stabile e leggibile della piattaforma, utile per logging e
// per la chiave dei bindings ("windows-x64", "android-arm64", ...).
constexpr std::string_view platform_id(Platform platform) noexcept {
    switch (platform) {
        case Platform::WindowsX64:   return "windows-x64";
        case Platform::MacOS:        return "macos";
        case Platform::AndroidArm64: return "android-arm64";
        case Platform::AndroidArmV7: return "android-armv7";
        case Platform::IOSArm64:     return "ios-arm64";
    }
    return "unknown";
}

// Categorie della causa di un fallimento di bootstrap. Servono a produrre una
// diagnostica che identifichi la causa (Req 1.3) in modo programmaticamente
// distinguibile dal Loader Core.
enum class BootstrapErrorCode {
    None,
    ProxyChainLoadFailed,   // impossibile caricare/forwardare la DLL reale di sistema
    EntryPointHookFailed,   // impossibile agganciare il punto pre-scena (AppDelegate)
    AlreadyInjected,        // runtime già iniettato in questo processo
    UnsupportedHost,        // implementazione non operativa sull'host corrente
    Unknown,
};

// Errore diagnostico restituito quando l'iniezione fallisce (Req 1.3).
// Mantiene un codice categorizzato + un messaggio leggibile per il log.
struct DiagnosticError {
    BootstrapErrorCode code{BootstrapErrorCode::Unknown};
    std::string        message;

    DiagnosticError() = default;
    DiagnosticError(BootstrapErrorCode c, std::string msg)
        : code(c), message(std::move(msg)) {}
};

// Esito di un tentativo di bootstrap/iniezione.
// - `injected == true`  -> il runtime ha ottenuto il controllo prima della
//                          scena iniziale; `error` è vuoto.
// - `injected == false` -> l'iniezione è fallita; `error` è popolato con la
//                          causa diagnostica (Req 1.3), così il loader può
//                          loggare e lasciar partire GD senza mod.
struct BootstrapResult {
    bool                           injected{false};
    std::optional<DiagnosticError> error;

    // Costruisce un esito di successo.
    static BootstrapResult success() {
        return BootstrapResult{true, std::nullopt};
    }

    // Costruisce un esito di fallimento con la diagnostica associata.
    static BootstrapResult failure(BootstrapErrorCode code, std::string message) {
        return BootstrapResult{false, DiagnosticError{code, std::move(message)}};
    }
};

// Interfaccia di bootstrap per-piattaforma, selezionata a compile-time per il
// target di build. Ogni piattaforma fornisce una sola implementazione.
class IPlatformBootstrap {
public:
    virtual ~IPlatformBootstrap() = default;

    // Ottiene il controllo del processo di Geometry Dash e avvia il runtime.
    // DEVE ritornare prima dell'esecuzione del codice della scena iniziale del
    // gioco (Req 1.2). In caso di fallimento ritorna un `BootstrapResult` con
    // `injected == false` e una `DiagnosticError` (Req 1.3); non deve impedire
    // l'avvio del gioco senza mod.
    virtual BootstrapResult inject() = 0;

    // Piattaforma servita da questa implementazione (Req 1.1).
    virtual Platform platform() const = 0;
};

}  // namespace pulse::bootstrap

#endif  // PULSE_LOADER_BOOTSTRAP_PLATFORM_BOOTSTRAP_HPP
