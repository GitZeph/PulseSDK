// loader/bootstrap/macos_bootstrap.cpp — implementazione del bootstrap macOS
// via dylib early-load (Req 1.1, 1.2, 1.3, 1.4).
//
// Le parti che dipendono dalle API specifiche di macOS sono isolate dietro
// `#if defined(__APPLE__)` così l'unità compila anche quando viene costruita su
// host non-Apple (Windows/Linux) per la sola validazione della build. In quel
// caso `inject()` ritorna un fallimento diagnostico `UnsupportedHost` senza
// alcun side effect.
//
// Meccanismo di early-load (entrambi portano il costruttore della dylib a
// essere eseguito PRIMA del `main` del gioco, quindi prima della scena
// iniziale — Req 1.2):
//   - `DYLD_INSERT_LIBRARIES`: dyld precarica la dylib del loader prima delle
//     dipendenze del processo di Geometry Dash.
//   - Patch del Mach-O load command (`LC_LOAD_DYLIB`): la dylib del loader è
//     dichiarata come dipendenza dell'eseguibile di GD e dyld la carica al
//     lancio.
// In produzione il vero punto di ingresso è una funzione marcata
// `__attribute__((constructor))` che chiama `MacOSBootstrap::inject()`.
#include "bootstrap/macos_bootstrap.hpp"

#if defined(__APPLE__)

#include <mach-o/dyld.h>

#if defined(PULSE_LOADER_ARTIFACT)
// Necessari solo nella build del Loader_Artifact dinamico per il costruttore di
// early-load e il suo idempotency guard (Req 1.2, 2.6). Nella build statica per
// i test host questi include non sono tirati dentro.
#include <atomic>
#include <string>
#include <string_view>

#include "core/loader_core.hpp"       // pulse::loader::default_diagnostic_sink()
#include "pulse_loader/runtime_entry.h"  // pulse_loader_runtime_entry()
#endif  // PULSE_LOADER_ARTIFACT

namespace pulse::bootstrap {
namespace {

// Verifica euristica di trovarsi nel contesto di early-load: l'eseguibile
// principale (immagine 0) deve essere mappato. Durante l'esecuzione del
// costruttore della dylib le immagini caricate da dyld sono già enumerabili,
// quindi questa condizione è soddisfatta prima del `main` del gioco.
bool images_are_mapped() {
    return ::_dyld_image_count() > 0;
}

}  // namespace

BootstrapResult MacOSBootstrap::inject() {
    // Idempotenza: una seconda chiamata nello stesso processo è un errore
    // diagnostico, non un nuovo aggancio (evita doppia inizializzazione).
    if (injected_) {
        return BootstrapResult::failure(
            BootstrapErrorCode::AlreadyInjected,
            "MacOSBootstrap: runtime gia iniettato in questo processo");
    }

    // Step 1 — conferma del contesto di early-load. Il costruttore della dylib
    // (via DYLD_INSERT_LIBRARIES o patch del load command) viene eseguito da
    // dyld prima del main del gioco: a questo punto le immagini sono mappate e
    // abbiamo il controllo prima della scena iniziale (Req 1.2).
    if (!images_are_mapped()) {
        return BootstrapResult::failure(
            BootstrapErrorCode::EntryPointHookFailed,
            "MacOSBootstrap: contesto di early-load non disponibile (immagini non mappate)");
    }

    // Step 2 — entry point del runtime PRIMA della scena iniziale (Req 1.2, 1.4).
    // Avendo gia il controllo nel costruttore della dylib, avviamo il runtime
    // centralizzato. Se segnala un errore di avvio, lo riportiamo come
    // diagnostica e lasciamo partire GD senza mod (Req 1.3).
    if (runtime_entry_) {
        if (!runtime_entry_()) {
            return BootstrapResult::failure(
                BootstrapErrorCode::EntryPointHookFailed,
                "MacOSBootstrap: l'entry point del runtime ha segnalato un errore di avvio");
        }
    }

    injected_ = true;
    return BootstrapResult::success();
}

}  // namespace pulse::bootstrap

#if defined(PULSE_LOADER_ARTIFACT)
// ---------------------------------------------------------------------------
// Entry point centralizzato + idempotency guard (Req 1.2, 2.6).
//
// Compilato SOLO nella build del Loader_Artifact dinamico
// (`PULSE_BUILD_LOADER_ARTIFACT=ON` → macro `PULSE_LOADER_ARTIFACT=1`). Nella
// build statica per i test host questo blocco non esiste, così l'output statico
// resta invariato (Req 1.3).
//
// Il costruttore `__attribute__((constructor))` è eseguito da dyld PRIMA del
// `main` del gioco (quindi prima della scena iniziale — Req 2.1). Anche quando
// fossero presenti più vettori di early-load (es. `LC_LOAD_DYLIB` patchato E una
// variabile `DYLD_INSERT_LIBRARIES` residua) il costruttore può essere eseguito
// più volte: un guard di processo basato su `std::atomic_flag` garantisce che
// l'entry centralizzato `pulse_loader_runtime_entry` sia invocato UNA SOLA
// VOLTA per processo (Req 2.6). Il percorso è completamente safe-fail: non
// propaga eccezioni e non aborta (Req 2.8).
// ---------------------------------------------------------------------------
namespace pulse::bootstrap {
namespace {

// Guard a livello di processo (Req 2.6). `ATOMIC_FLAG_INIT` lo inizializza a
// "clear"; il primo `test_and_set` ritorna `false` (eseguiamo l'entry), ogni
// chiamata successiva ritorna `true` (no-op).
std::atomic_flag g_pulse_entered = ATOMIC_FLAG_INIT;

// Esegue, una sola volta per processo, l'avvio del runtime centralizzato.
// `noexcept`: qualunque eccezione è assorbita per non abortire il processo di
// Geometry Dash (Req 2.8). È estratta dal costruttore così da poter essere
// esercitata anche fuori dal contesto di dyld.
void run_pulse_early_load_once() noexcept {
    // Idempotenza (Req 2.6): se il guard era già impostato, un altro vettore di
    // early-load ha già avviato il runtime in questo processo: no-op.
    if (g_pulse_entered.test_and_set()) {
        return;
    }
    try {
        // Unico entry point centralizzato del runtime (Req 1.2). Il bootstrap
        // ottiene il controllo nel costruttore della dylib e lo invoca.
        MacOSBootstrap boot{[]() -> bool { return ::pulse_loader_runtime_entry(); }};
        const BootstrapResult result = boot.inject();
        if (!result.injected) {
            // Safe-fail (Req 2.7, 2.8): logga la causa diagnostica e lascia
            // partire GD senza mod.
            const std::string_view message =
                result.error
                    ? std::string_view{result.error->message}
                    : std::string_view{"MacOSBootstrap: iniezione fallita senza diagnostica"};
            pulse::loader::default_diagnostic_sink()(message);
        }
    } catch (...) {
        // Req 2.8: non propaghiamo eccezioni e non abortiamo; GD prosegue con
        // zero mod.
    }
}

}  // namespace
}  // namespace pulse::bootstrap

// Costruttore di early-load: dyld lo esegue durante il caricamento delle
// immagini, prima del `main` del gioco (Req 2.1). Invoca l'entry centralizzato
// una sola volta tramite il guard di processo (Req 2.6).
__attribute__((constructor))
static void pulse_macos_early_load() {
    pulse::bootstrap::run_pulse_early_load_once();
}
#endif  // PULSE_LOADER_ARTIFACT

#else  // !__APPLE__ — build cross-platform su host non-Apple.

namespace pulse::bootstrap {

BootstrapResult MacOSBootstrap::inject() {
    // L'iniezione via dylib early-load richiede dyld e un processo macOS.
    // Sull'host non-Apple l'implementazione non è operativa: ritorniamo una
    // diagnostica esplicita senza side effect, mantenendo la build compilabile.
    if (injected_) {
        return BootstrapResult::failure(
            BootstrapErrorCode::AlreadyInjected,
            "MacOSBootstrap: runtime gia iniettato in questo processo");
    }
    // Esercita comunque l'entry point se fornito, per consentirne il test su
    // host non-Apple, ma senza marcare l'iniezione come riuscita.
    if (runtime_entry_) {
        (void)runtime_entry_;
    }
    return BootstrapResult::failure(
        BootstrapErrorCode::UnsupportedHost,
        "MacOSBootstrap: bootstrap via dylib early-load disponibile solo su macOS");
}

}  // namespace pulse::bootstrap

#endif  // __APPLE__
