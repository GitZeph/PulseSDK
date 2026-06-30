// hooks.gen.hpp — GENERATO da `pulse surface generate` (Cpp_Generator).
// NON modificare a mano: rigenerato deterministicamente dalla Surface_IR.
// Header-only, parte dell'API SDK C++ di Pulse (Req 3.4, 10.1).
// Macro ergonomica `PULSE_GD_HOOK` (Req 4.x, 5.x). Ponte fra il
// `cpp_token` (chiave dei BindingTraits, verifica di firma a
// compile-time) e il simbolo canonico `Class::method` (registrazione
// runtime via register_hook, come examples/allhooks-mod).
#pragma once

#include <utility>

#include <pulse/hooks.hpp>

#include "bindings.gen.hpp"

// Variante con priorità di catena esplicita (Req 4.6).
#define PULSE_GD_HOOK_PRIORITY(Class, Method, Priority, Ret, Params)            \
    namespace pulse_gd_hook_##Class##_##Method {                               \
        /* Tipo della funzione bersaglio (i nomi dei parametri sono ignorati). */ \
        using PulseFn = Ret Params;                                            \
        /* Appartenenza alla superficie: fallisce se il token non è in          \
           superficie (Req 5.3). */                                            \
        static_assert(                                                         \
            ::pulse::gd::detail::in_surface<                                   \
                ::pulse::hooks::FixedString(#Class "_" #Method)>(),            \
            "PULSE_GD_HOOK(" #Class "::" #Method "): API_Element assente "      \
            "dalla GD_API_Surface (simbolo non in superficie).");             \
        /* Verifica della firma a compile-time contro il binding (Req 5.1/5.2). */ \
        static_assert(                                                         \
            ::pulse::hooks::SignatureMatches<                                  \
                ::pulse::hooks::FixedString(#Class "_" #Method), PulseFn>,     \
            "PULSE_GD_HOOK(" #Class "::" #Method "): la firma dichiarata è "    \
            "incompatibile con quella del Binding_Catalog.");                 \
        /* Slot del trampolino: cablato dall'Hooking Engine dopo install(). */  \
        inline PulseFn* pulse_original = nullptr;                              \
        /* Dichiarazione anticipata del detour; il corpo segue la macro. */     \
        Ret pulse_detour Params;                                               \
        /* Invoca l'originale preservando parametri e valore di ritorno (Req 4.2). */ \
        template <class... PulseArgs>                                          \
        inline Ret callOriginal(PulseArgs&&... pulse_args) {                   \
            return pulse_original(std::forward<PulseArgs>(pulse_args)...);     \
        }                                                                      \
        /* Registrazione SUL SIMBOLO CANONICO (Req 4.1), come allhooks: nessun  \
           letterale di indirizzo. `used` impedisce il dead-strip (Req 4.4). */ \
        PULSE_HOOK_USED                                                        \
        inline const ::pulse::hooks::HookRegistration pulse_registration =     \
            ::pulse::hooks::register_hook(                                     \
                #Class "::" #Method,                                          \
                reinterpret_cast<void*>(&pulse_detour),                        \
                reinterpret_cast<void**>(&pulse_original), (Priority));        \
    }                                                                          \
    Ret pulse_gd_hook_##Class##_##Method::pulse_detour Params

// Forma minimale: priorità di catena di default (500, Req 4.5).
#define PULSE_GD_HOOK(Class, Method, Ret, Params)                              \
    PULSE_GD_HOOK_PRIORITY(Class, Method, 500, Ret, Params)

// --- Hook_Point disponibili nella superficie (ordinati per SymbolId) ---
// PULSE_GD_HOOK(MenuLayer, …, bool(pulse::gd::MenuLayer*)) -> registra "MenuLayer::init"
