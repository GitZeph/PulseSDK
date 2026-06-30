// bindings.gen.hpp — GENERATO da `pulse surface generate` (Cpp_Generator).
// NON modificare a mano: rigenerato deterministicamente dalla Surface_IR.
// Header-only, parte dell'API SDK C++ di Pulse (Req 3.4, 10.1).
// Specializzazioni `BindingTraits` (firma canonica this-first, derivata
// dal Binding_Catalog, Req 3.1/3.2/5.4) e marcatori d'appartenenza alla
// superficie per il Build_Check (Req 5.3).
#pragma once

#include <pulse/hooks.hpp>

#include "types.gen.hpp"

// --- BindingTraits: firma canonica per token (this-first) ---
template <>
struct pulse::hooks::BindingTraits<pulse::hooks::FixedString("MenuLayer_init")> {
    using Fn = bool(pulse::gd::MenuLayer*);
};

// --- Marcatore d'appartenenza alla superficie (Build_Check, Req 5.3) ---
namespace pulse::gd::detail {
// Template primario: un token NON in superficie è `false` (fail-closed).
template <::pulse::hooks::FixedString Token>
consteval bool in_surface() { return false; }
template <>
consteval bool in_surface<::pulse::hooks::FixedString("MenuLayer_init")>() { return true; }
}  // namespace pulse::gd::detail

// --- Costanti d'appartenenza (una per token in superficie) ---
inline constexpr bool kPulseGdInSurface_MenuLayer_init = true;
