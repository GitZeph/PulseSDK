// loader/core/mod_registrar.cpp — implementazione della registrazione di hook
// esterni e (solo nell'artefatto) entry point C-ABI esportato.
//
// `register_external_hook` mantiene uno store con vita pari al processo che
// possiede una copia stabile di ogni stringa di simbolo, così la
// `std::string_view` memorizzata in `HookRegistration::symbol` (non
// proprietaria) resta valida anche dopo che il chiamante ha liberato la stringa
// originale. Si usa un `std::deque` perché i suoi elementi non vengono mai
// rilocati all'aggiunta in coda (a differenza di `std::vector`), quindi gli
// indirizzi/le view restano stabili.
#include "core/mod_registrar.hpp"

#include <pulse/hooks.hpp>

#include <deque>
#include <mutex>
#include <string>

namespace pulse::loader {

pulse::hooks::HookRegistration register_external_hook(
    std::string_view symbol, void* detour, void** trampoline, int priority) {
    static std::deque<std::string> g_symbols;  // indirizzi stabili (deque non riloca)
    static std::mutex g_mu;
    std::string_view stable;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_symbols.emplace_back(symbol);
        stable = g_symbols.back();
    }
    return pulse::hooks::register_hook(stable, detour, trampoline, priority);
}

}  // namespace pulse::loader

#if defined(PULSE_LOADER_ARTIFACT)
// Solo nell'artefatto dinamico: esporta l'entry point C-ABI a visibilità di
// default (vedi PULSE_LOADER_EXPORT). Una mod esterna `dlopen`'d risolve questo
// simbolo nell'immagine del Loader via dyld e ci registra i propri hook, così
// finiscono nel registro del Loader (condiviso oltre il confine del modulo).
//
// All'interno dell'artefatto `pulse::hooks::register_hook` registra nel registro
// LOCALE (del Loader) — vedi sdk/include/pulse/hooks.hpp — quindi questa
// chiamata NON ricorre su sé stessa e l'hook atterra nel registro del Loader.
#include "pulse_loader/runtime_entry.h"  // PULSE_LOADER_EXPORT

extern "C" PULSE_LOADER_EXPORT void pulse_loader_register_hook(
    const char* symbol, void* detour, void** trampoline, int priority) {
    if (symbol == nullptr) {
        return;
    }
    (void)pulse::loader::register_external_hook(symbol, detour, trampoline,
                                                priority);
}
#endif
