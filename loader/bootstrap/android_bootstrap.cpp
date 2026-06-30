// loader/bootstrap/android_bootstrap.cpp — implementazione del bootstrap
// Android via libreria `.so` precaricata e costruttore `JNI_OnLoad`
// (Req 1.1, 1.2, 1.3, 1.4).
//
// Le parti che dipendono dalle API JNI/NDK sono isolate dietro
// `#if defined(__ANDROID__)` così l'unità compila anche quando viene costruita
// su host non-Android (Windows/macOS/Linux) per la sola validazione della
// build. In quel caso `inject()` ritorna un fallimento diagnostico
// `UnsupportedHost` senza alcun side effect.
//
// Strategia di distribuzione (due varianti ABI: arm64-v8a, armeabi-v7a):
//   * Un wrapper APK ripacchettizzato dichiara la `.so` del loader tra le
//     librerie native dell'app. All'avvio dell'`Application`, il runtime Java
//     la carica via `System.loadLibrary("pulse_loader")`.
//   * Il linker dinamico mappa la `.so` e il runtime Java invoca
//     `JNI_OnLoad`. Questo avviene PRIMA che Cocos2d-x crei l'`AppDelegate` e
//     mostri la scena iniziale: è il punto di aggancio early-load (Req 1.2).
//   * `JNI_OnLoad` instrada verso l'entry point centralizzato del runtime
//     (Req 1.4) tramite l'istanza `AndroidBootstrap` configurata.
#include "bootstrap/android_bootstrap.hpp"

#if defined(__ANDROID__)

#include <jni.h>

#if defined(PULSE_LOADER_ARTIFACT)
// Necessari solo nella build del Loader_Artifact dinamico per il costruttore di
// early-load (`JNI_OnLoad`) e il suo idempotency guard (Req 2.6). Nella build
// statica per i test host questi include non sono tirati dentro.
#include <atomic>
#include <string>
#include <string_view>

#include "core/loader_core.hpp"           // pulse::loader::default_diagnostic_sink()
#include "pulse_loader/runtime_entry.h"   // pulse_loader_runtime_entry()
#endif  // PULSE_LOADER_ARTIFACT

namespace pulse::bootstrap {

BootstrapResult AndroidBootstrap::inject() {
    // Idempotenza: una seconda chiamata nello stesso processo è un errore
    // diagnostico, non un nuovo aggancio (evita doppia inizializzazione).
    if (injected_) {
        return BootstrapResult::failure(
            BootstrapErrorCode::AlreadyInjected,
            "AndroidBootstrap: runtime gia iniettato in questo processo");
    }

    // Su Android il controllo early-load è gia stato ottenuto da `JNI_OnLoad`
    // al caricamento della `.so` (prima della scena iniziale, Req 1.2):
    // avviamo qui il runtime centralizzato (Req 1.4).
    if (runtime_entry_) {
        if (!runtime_entry_()) {
            return BootstrapResult::failure(
                BootstrapErrorCode::EntryPointHookFailed,
                "AndroidBootstrap: l'entry point del runtime ha segnalato un errore di avvio");
        }
    }

    injected_ = true;
    return BootstrapResult::success();
}

}  // namespace pulse::bootstrap

#if defined(PULSE_LOADER_ARTIFACT)
// ---------------------------------------------------------------------------
// Entry point centralizzato + idempotency guard (Req 1.2/2.4, 2.6, 2.7, 2.8).
//
// Compilato SOLO nella build del Loader_Artifact dinamico
// (`PULSE_BUILD_LOADER_ARTIFACT=ON` → macro `PULSE_LOADER_ARTIFACT=1`), che su
// Android produce la `.so` caricabile. Nella build statica per i test host
// questo blocco non esiste, così l'output statico resta invariato (Req 1.3) e
// la suite host continua a esercitare il solo `inject()` platform-agnostic.
//
// Il costruttore di early-load su Android è `JNI_OnLoad`: il runtime Java lo
// invoca quando la `.so` del loader viene mappata via `System.loadLibrary`,
// DURANTE l'inizializzazione dell'`Application`/`Activity`, ovvero PRIMA che
// Cocos2d-x costruisca l'`AppDelegate` e mandi in scena la prima schermata
// (Req 2.4). Anche se fossero presenti più vettori di early-load (es. la `.so`
// caricata da più punti) l'entry centralizzato `pulse_loader_runtime_entry`
// deve essere invocato UNA SOLA VOLTA per processo (Req 2.6): si usa un guard
// di processo basato su `std::atomic_flag`, identico a quello del bootstrap
// macOS. Il percorso è completamente safe-fail: non propaga eccezioni e non
// aborta, lasciando partire GD senza mod (Req 2.7, 2.8).
// ---------------------------------------------------------------------------
namespace pulse::bootstrap {
namespace {

// Guard a livello di processo (Req 2.6). `ATOMIC_FLAG_INIT` lo inizializza a
// "clear"; il primo `test_and_set` ritorna `false` (eseguiamo l'entry), ogni
// chiamata successiva ritorna `true` (no-op).
std::atomic_flag g_pulse_entered = ATOMIC_FLAG_INIT;

// Esegue, una sola volta per processo, l'avvio del runtime centralizzato.
// `noexcept`: qualunque eccezione è assorbita per non abortire il processo di
// Geometry Dash (Req 2.8). È estratta da `JNI_OnLoad` così da poter essere
// esercitata anche fuori dal contesto del runtime Java.
void run_pulse_early_load_once() noexcept {
    // Idempotenza (Req 2.6): se il guard era già impostato, un altro vettore di
    // early-load ha già avviato il runtime in questo processo: no-op.
    if (g_pulse_entered.test_and_set()) {
        return;
    }
    try {
        // Unico entry point centralizzato del runtime (Req 1.2). Il bootstrap
        // ottiene il controllo in `JNI_OnLoad` e lo invoca.
        AndroidBootstrap boot{[]() -> bool { return ::pulse_loader_runtime_entry(); }};
        const BootstrapResult result = boot.inject();
        if (!result.injected) {
            // Safe-fail (Req 2.7, 2.8): logga la causa diagnostica e lascia
            // partire GD senza mod.
            const std::string_view message =
                result.error
                    ? std::string_view{result.error->message}
                    : std::string_view{"AndroidBootstrap: iniezione fallita senza diagnostica"};
            pulse::loader::default_diagnostic_sink()(message);
        }
    } catch (...) {
        // Req 2.8: non propaghiamo eccezioni e non abortiamo; GD prosegue con
        // zero mod.
    }
}

}  // namespace
}  // namespace pulse::bootstrap

// Costruttore JNI standard di early-load invocato dal runtime Java quando la
// `.so` del loader viene caricata via `System.loadLibrary`. Avvia l'entry
// centralizzato una sola volta tramite il guard di processo (Req 2.4, 2.6) e
// restituisce la versione JNI richiesta affinché il caricamento prosegua e il
// gioco parta normalmente anche in caso di fallimento (Req 2.8).
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* /*vm*/, void* /*reserved*/) {
    pulse::bootstrap::run_pulse_early_load_once();
    return JNI_VERSION_1_6;
}
#endif  // PULSE_LOADER_ARTIFACT

#else  // !__ANDROID__ — build cross-platform su host non-Android.

namespace pulse::bootstrap {

BootstrapResult AndroidBootstrap::inject() {
    // Il bootstrap early-load via `.so` + `JNI_OnLoad` richiede le API JNI/NDK e
    // un processo Android. Sull'host non-Android l'implementazione non è
    // operativa: ritorniamo una diagnostica esplicita senza side effect,
    // mantenendo la build compilabile e testabile.
    if (injected_) {
        return BootstrapResult::failure(
            BootstrapErrorCode::AlreadyInjected,
            "AndroidBootstrap: runtime gia iniettato in questo processo");
    }
    // Non eseguiamo l'entry point e non marchiamo l'iniezione come riuscita:
    // l'aggancio early-load è disponibile solo su Android.
    (void)runtime_entry_;
    return BootstrapResult::failure(
        BootstrapErrorCode::UnsupportedHost,
        "AndroidBootstrap: bootstrap via .so/JNI_OnLoad disponibile solo su Android");
}

}  // namespace pulse::bootstrap

#endif  // __ANDROID__
