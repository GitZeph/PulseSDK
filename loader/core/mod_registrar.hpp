// loader/core/mod_registrar.hpp — registrazione di hook esterni nel registro
// del Loader (external-mod-loading).
//
// `pulse::hooks::HookRegistration::symbol` è una `std::string_view` NON
// proprietaria: la registrazione non possiede la stringa del simbolo. Quando un
// hook viene registrato a partire da una stringa temporanea (es. il `const
// char*` passato attraverso il confine C-ABI da una mod `dlopen`'d), la view
// resterebbe pendente non appena il chiamante libera la stringa.
//
// `register_external_hook` registra l'hook nel registro `pulse::hooks` di QUESTA
// immagine (il Loader) facendo possedere al Loader una COPIA stabile della
// stringa del simbolo per l'intera vita del processo, così la
// `std::string_view` memorizzata resta valida.
#ifndef PULSE_LOADER_CORE_MOD_REGISTRAR_HPP
#define PULSE_LOADER_CORE_MOD_REGISTRAR_HPP

#include <string_view>

#include <pulse/hooks.hpp>

namespace pulse::loader {

// Registra un hook nel registro `pulse::hooks` di QUESTA immagine, possedendo la
// stringa del simbolo per l'intera vita del processo (HookRegistration.symbol è
// una view non proprietaria).
pulse::hooks::HookRegistration register_external_hook(
    std::string_view symbol, void* detour, void** trampoline, int priority);

}  // namespace pulse::loader

#endif  // PULSE_LOADER_CORE_MOD_REGISTRAR_HPP
