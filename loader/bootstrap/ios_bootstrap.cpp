// loader/bootstrap/ios_bootstrap.cpp — implementazione del bootstrap iOS via
// constructor early-load (Req 2.5, 2.6, 2.7, 2.8).
//
// Deliverable secondario ad alto livello: iOS è coperto dietro lo stesso seam
// `IPlatformBootstrap` di macOS, con lo stesso idempotency guard di processo e
// la stessa policy safe-fail dell'attività 3.1 (`MacOSBootstrap`). Il backend di
// hooking Dobby è condiviso con macOS (abilitato su target Apple via
// `PULSE_ENABLE_DOBBY`); a livello di bootstrap ciò si riflette nell'invocare lo
// stesso entry point centralizzato `pulse_loader_runtime_entry`.
//
// La parte specifica iOS è isolata dietro la guardia di TargetConditionals
// (`TARGET_OS_IPHONE`). ATTENZIONE: anche macOS desktop definisce `__APPLE__`,
// perciò NON è sufficiente discriminare su `__APPLE__`: occorre `<TargetConditionals.h>`
// e il flag `TARGET_OS_IPHONE` per attivare questo path SOLO su iOS (e non su
// macOS desktop, gestito da `MacOSBootstrap`). Su qualunque host che non sia
// iOS — inclusi macOS desktop e Linux — `inject()` ritorna un fallimento
// diagnostico `UnsupportedHost` senza side effect, così l'unità compila sull'host
// di sviluppo per la sola validazione della build (Req 2.8).
#include "bootstrap/ios_bootstrap.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// Attiva il path iOS reale solo su device/simulatore iOS, mai su macOS desktop.
#if defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE

#if defined(PULSE_LOADER_ARTIFACT)
// Necessari solo nella build del Loader_Artifact dinamico per il costruttore di
// early-load e il suo idempotency guard (Req 2.6). Nella build statica per i
// test host questi include non sono tirati dentro.
#include <atomic>
#include <string>
#include <string_view>

#include "core/loader_core.hpp"          // pulse::loader::default_diagnostic_sink()
#include "pulse_loader/runtime_entry.h"  // pulse_loader_runtime_entry()
#endif  // PULSE_LOADER_ARTIFACT

namespace pulse::bootstrap {

BootstrapResult IOSBootstrap::inject() {
    // Idempotenza: una seconda chiamata nello stesso processo è un errore
    // diagnostico, non un nuovo aggancio (evita doppia inizializzazione).
    if (injected_) {
        return BootstrapResult::failure(
            BootstrapErrorCode::AlreadyInjected,
            "IOSBootstrap: runtime gia iniettato in questo processo");
    }

    // Entry point del runtime PRIMA della scena iniziale (Req 2.5).
    // Poiché la dylib è caricata da Substrate/ElleKit o da dyld (repacking)
    // prima del main del gioco, qui abbiamo gia il controllo: avviamo il
    // runtime centralizzato. Se segnala un errore di avvio, lo riportiamo come
    // diagnostica e lasciamo partire GD senza mod (Req 2.7, 2.8).
    if (runtime_entry_) {
        if (!runtime_entry_()) {
            return BootstrapResult::failure(
                BootstrapErrorCode::EntryPointHookFailed,
                "IOSBootstrap: l'entry point del runtime ha segnalato un errore di avvio");
        }
    }

    injected_ = true;
    return BootstrapResult::success();
}

}  // namespace pulse::bootstrap

#if defined(PULSE_LOADER_ARTIFACT)
// ---------------------------------------------------------------------------
// Entry point centralizzato + idempotency guard (Req 2.5, 2.6).
//
// Compilato SOLO nella build del Loader_Artifact dinamico per iOS
// (`PULSE_BUILD_LOADER_ARTIFACT=ON` → macro `PULSE_LOADER_ARTIFACT=1`). Nella
// build statica per i test host questo blocco non esiste, così l'output statico
// resta invariato.
//
// Il costruttore `__attribute__((constructor))` è eseguito da dyld PRIMA del
// `main` del gioco (quindi prima della scena iniziale — Req 2.5). I due vettori
// di distribuzione iOS — Substrate/ElleKit (jailbreak) e patch `LC_LOAD_DYLIB`
// dell'app ripacchettizzata (sideload) — possono essere entrambi presenti e far
// eseguire il costruttore più volte: un guard di processo basato su
// `std::atomic_flag` garantisce che l'entry centralizzato e condiviso con macOS
// `pulse_loader_runtime_entry` sia invocato UNA SOLA VOLTA per processo
// (Req 2.6). Il percorso è completamente safe-fail: non propaga eccezioni e non
// aborta (Req 2.8).
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
        // Unico entry point centralizzato del runtime, condiviso con macOS
        // (backend Dobby comune). Il bootstrap ottiene il controllo nel
        // costruttore della dylib e lo invoca.
        IOSBootstrap boot{[]() -> bool { return ::pulse_loader_runtime_entry(); }};
        const BootstrapResult result = boot.inject();
        if (!result.injected) {
            // Safe-fail (Req 2.7, 2.8): logga la causa diagnostica e lascia
            // partire GD senza mod.
            const std::string_view message =
                result.error
                    ? std::string_view{result.error->message}
                    : std::string_view{"IOSBootstrap: iniezione fallita senza diagnostica"};
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
// immagini, prima del `main` del gioco (Req 2.5). Invoca l'entry centralizzato
// una sola volta tramite il guard di processo (Req 2.6).
__attribute__((constructor))
static void pulse_ios_early_load() {
    pulse::bootstrap::run_pulse_early_load_once();
}
#endif  // PULSE_LOADER_ARTIFACT

#else  // host non-iOS (macOS desktop, Linux, Windows) — build cross-platform.

namespace pulse::bootstrap {

BootstrapResult IOSBootstrap::inject() {
    // L'iniezione via constructor early-load richiede un processo iOS con la
    // dylib caricata da Substrate/ElleKit o da dyld (repacking). Sull'host non
    // iOS — incluso macOS desktop, dove il path corretto è MacOSBootstrap —
    // l'implementazione non è operativa: ritorniamo una diagnostica esplicita
    // senza side effect, mantenendo la build compilabile e testabile.
    if (injected_) {
        return BootstrapResult::failure(
            BootstrapErrorCode::AlreadyInjected,
            "IOSBootstrap: runtime gia iniettato in questo processo");
    }
    // Esercita comunque l'entry point se fornito, per consentirne il test su
    // host non-iOS, ma senza marcare l'iniezione come riuscita.
    if (runtime_entry_) {
        (void)runtime_entry_;
    }
    return BootstrapResult::failure(
        BootstrapErrorCode::UnsupportedHost,
        "IOSBootstrap: bootstrap via constructor early-load disponibile solo su iOS");
}

}  // namespace pulse::bootstrap

#endif  // TARGET_OS_IPHONE
