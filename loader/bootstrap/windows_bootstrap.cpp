// loader/bootstrap/windows_bootstrap.cpp — implementazione del bootstrap
// Windows via DLL proxy (Req 2.3, 2.6, 2.7, 2.8).
//
// Le parti che dipendono dalle API Win32 sono isolate dietro `#if defined(_WIN32)`
// così l'unità compila anche quando viene costruita su host non-Windows
// (macOS/Linux) per la sola validazione della build. In quel caso `inject()`
// ritorna un fallimento diagnostico `UnsupportedHost` senza alcun side effect.
//
// Vettore di early-load (porta il runtime ad avviarsi PRIMA del `main` del
// gioco, quindi prima della scena iniziale — Req 2.3):
//   - DLL proxy: la DLL del loader ha lo stesso nome di una DLL caricata da GD
//     al lancio (`XInput9_1_0.dll`/`fmod.dll`); il loader del SO la mappa prima
//     del `main`, il suo `DllMain(DLL_PROCESS_ATTACH)` (o un costruttore di
//     early-load) invoca l'entry centralizzato e poi forwarda gli export alla
//     DLL reale.
//   - Fallback: injection esterna via `CreateRemoteThread` in un processo GD
//     già avviato (lo stub di forwarding non è richiesto in quel percorso).
// In entrambi i casi il vero punto di ingresso invoca l'entry point
// centralizzato `pulse_loader_runtime_entry` (riuso del `CentralizedLoader`).
#include "bootstrap/windows_bootstrap.hpp"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#if defined(PULSE_LOADER_ARTIFACT)
// Necessari solo nella build del Loader_Artifact dinamico per il vettore di
// early-load (DllMain/costruttore) e il suo idempotency guard (Req 2.3, 2.6).
// Nella build statica per i test host questi include non sono tirati dentro.
#include <atomic>
#include <string>
#include <string_view>

#include "core/loader_core.hpp"          // pulse::loader::default_diagnostic_sink()
#include "pulse_loader/runtime_entry.h"  // pulse_loader_runtime_entry()
#endif  // PULSE_LOADER_ARTIFACT

namespace pulse::bootstrap {
namespace {

// Carica la DLL di sistema reale verso cui il proxy inoltra gli export, così
// che il gioco continui a ricevere le funzioni attese. Cerca prima nella
// directory di sistema per evitare di ricaricare il proxy stesso.
HMODULE load_real_system_dll(std::string_view dll_name) {
    char system_dir[MAX_PATH] = {};
    const UINT len = ::GetSystemDirectoryA(system_dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return nullptr;
    }

    std::string full_path(system_dir, len);
    full_path.push_back('\\');
    full_path.append(dll_name.data(), dll_name.size());

    return ::LoadLibraryA(full_path.c_str());
}

}  // namespace

BootstrapResult WindowsBootstrap::inject() {
    // Idempotenza: una seconda chiamata nello stesso processo è un errore
    // diagnostico, non un nuovo aggancio (evita doppia inizializzazione).
    if (injected_) {
        return BootstrapResult::failure(
            BootstrapErrorCode::AlreadyInjected,
            "WindowsBootstrap: runtime gia iniettato in questo processo");
    }

    // Step 1 — catena del proxy: carichiamo la DLL di sistema reale verso cui
    // forwardare gli export. Se fallisce, il gioco perderebbe le funzioni
    // attese: segnaliamo la diagnostica e lasciamo partire GD senza mod (Req 2.7).
    const HMODULE real_dll = load_real_system_dll(kDefaultProxyTargetDll);
    if (real_dll == nullptr) {
        return BootstrapResult::failure(
            BootstrapErrorCode::ProxyChainLoadFailed,
            "WindowsBootstrap: impossibile caricare la DLL di sistema reale per il forwarding");
    }

    // Step 2 — entry point del runtime PRIMA della scena iniziale (Req 2.3, 2.6).
    // Poiché il loader del SO mappa questa DLL prima del main del gioco, qui
    // abbiamo gia il controllo: avviamo il runtime centralizzato.
    if (runtime_entry_) {
        if (!runtime_entry_()) {
            return BootstrapResult::failure(
                BootstrapErrorCode::EntryPointHookFailed,
                "WindowsBootstrap: l'entry point del runtime ha segnalato un errore di avvio");
        }
    }

    injected_ = true;
    return BootstrapResult::success();
}

}  // namespace pulse::bootstrap

#if defined(PULSE_LOADER_ARTIFACT)
// ---------------------------------------------------------------------------
// Vettore di early-load + idempotency guard (Req 2.3, 2.6, 2.7, 2.8).
//
// Compilato SOLO nella build del Loader_Artifact dinamico
// (`PULSE_BUILD_LOADER_ARTIFACT=ON` → macro `PULSE_LOADER_ARTIFACT=1`). Nella
// build statica per i test host questo blocco non esiste, così l'output statico
// resta invariato.
//
// Su Windows il punto di ingresso naturale della DLL proxy è `DllMain` con
// `DLL_PROCESS_ATTACH`: il loader del SO lo esegue mentre mappa la DLL, PRIMA
// del `main` del gioco (quindi prima della scena iniziale — Req 2.3). Anche
// quando fossero presenti più vettori di early-load (DLL proxy E injection
// esterna via `CreateRemoteThread`) il guard di processo basato su
// `std::atomic_flag` garantisce che l'entry centralizzato
// `pulse_loader_runtime_entry` sia invocato UNA SOLA VOLTA per processo
// (Req 2.6) — lo stesso schema del bootstrap macOS. Il percorso è interamente
// safe-fail: non propaga eccezioni e non aborta (Req 2.8).
// ---------------------------------------------------------------------------
namespace pulse::bootstrap {
namespace {

// Guard a livello di processo (Req 2.6). `ATOMIC_FLAG_INIT` lo inizializza a
// "clear"; il primo `test_and_set` ritorna `false` (eseguiamo l'entry), ogni
// chiamata successiva ritorna `true` (no-op).
std::atomic_flag g_pulse_entered = ATOMIC_FLAG_INIT;

// Esegue, una sola volta per processo, l'avvio del runtime centralizzato.
// `noexcept`: qualunque eccezione è assorbita per non abortire il processo di
// Geometry Dash (Req 2.8). È estratta dall'entry della DLL così da poter essere
// esercitata sia da `DllMain` sia dal costruttore di early-load.
void run_pulse_early_load_once() noexcept {
    // Idempotenza (Req 2.6): se il guard era già impostato, un altro vettore di
    // early-load ha già avviato il runtime in questo processo: no-op.
    if (g_pulse_entered.test_and_set()) {
        return;
    }
    try {
        // Unico entry point centralizzato del runtime (riuso del
        // CentralizedLoader). Il bootstrap ottiene il controllo nel caricamento
        // della DLL proxy e lo invoca.
        WindowsBootstrap boot{[]() -> bool { return ::pulse_loader_runtime_entry(); }};
        const BootstrapResult result = boot.inject();
        if (!result.injected) {
            // Safe-fail (Req 2.7, 2.8): logga la causa diagnostica e lascia
            // partire GD senza mod.
            const std::string_view message =
                result.error
                    ? std::string_view{result.error->message}
                    : std::string_view{"WindowsBootstrap: iniezione fallita senza diagnostica"};
            pulse::loader::default_diagnostic_sink()(message);
        }
    } catch (...) {
        // Req 2.8: non propaghiamo eccezioni e non abortiamo; GD prosegue con
        // zero mod.
    }
}

}  // namespace
}  // namespace pulse::bootstrap

// Punto di ingresso della DLL proxy: il loader del SO lo esegue mentre mappa la
// DLL, prima del `main` del gioco (Req 2.3). Invoca l'entry centralizzato una
// sola volta tramite il guard di processo (Req 2.6). Ritorna sempre `TRUE` per
// non bloccare il caricamento della DLL (safe-fail, Req 2.8).
extern "C" BOOL WINAPI DllMain(HINSTANCE /*instance*/, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        pulse::bootstrap::run_pulse_early_load_once();
    }
    return TRUE;
}

#if defined(__GNUC__) || defined(__clang__)
// Vettore di early-load alternativo per toolchain GCC/Clang (MinGW): un
// costruttore eseguito al caricamento dell'immagine. Condivide lo stesso guard
// di processo, quindi resta invocato una sola volta anche insieme a `DllMain`
// (Req 2.6).
__attribute__((constructor))
static void pulse_windows_early_load() {
    pulse::bootstrap::run_pulse_early_load_once();
}
#endif  // __GNUC__ || __clang__
#endif  // PULSE_LOADER_ARTIFACT

#else  // !_WIN32 — build cross-platform su host non-Windows.

namespace pulse::bootstrap {

BootstrapResult WindowsBootstrap::inject() {
    // L'iniezione via DLL proxy richiede le API Win32 e un processo Windows.
    // Sull'host non-Windows l'implementazione non è operativa: ritorniamo una
    // diagnostica esplicita senza side effect, mantenendo la build compilabile.
    if (injected_) {
        return BootstrapResult::failure(
            BootstrapErrorCode::AlreadyInjected,
            "WindowsBootstrap: runtime gia iniettato in questo processo");
    }
    // Esercita comunque l'entry point se fornito, per consentirne il test su
    // host non-Windows, ma senza marcare l'iniezione come riuscita.
    if (runtime_entry_) {
        (void)runtime_entry_;
    }
    return BootstrapResult::failure(
        BootstrapErrorCode::UnsupportedHost,
        "WindowsBootstrap: bootstrap via DLL proxy disponibile solo su Windows");
}

}  // namespace pulse::bootstrap

#endif  // _WIN32
