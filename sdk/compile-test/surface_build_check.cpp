// surface_build_check.cpp — snippet "must-compile" del Build_Check header-only
// della GD_API_Surface (task 15.1, Req 3.4, 10.1).
//
// Questo file NON è compilato dalla build di default: vive dietro l'opzione
// CMake `PULSE_BUILD_SURFACE_COMPILE_TEST` (default OFF). Quando l'opzione è ON
// il target `pulse_surface_compile_test` lo compila come verifica che gli header
// generati da `pulse surface generate` (header-only, consegnati come API SDK
// C++) formino un Build_Check coerente:
//
//   - include i tre header generati dal manifest seed
//     (`sdk/include/pulse/gd/{types,bindings,hooks}.gen.hpp`);
//   - dichiara un hook con `PULSE_GD_HOOK` su `MenuLayer::init`, l'unico
//     Typed_Binding verificato (firma `bool` / `["MenuLayer*"]`).
//
// La firma dichiarata COINCIDE con il Typed_Binding, quindi gli `static_assert`
// generati (appartenenza alla superficie + `SignatureMatches`) sono soddisfatti
// e la traduzione compila: ciò prova che l'infrastruttura header-only del
// Build_Check funziona end-to-end (Req 3.4) e che l'API C++ è consegnata
// esclusivamente tramite gli header generati (Req 10.1).
#include <pulse/gd/types.gen.hpp>
#include <pulse/gd/bindings.gen.hpp>
#include <pulse/gd/hooks.gen.hpp>

// must-compile: la firma `bool(pulse::gd::MenuLayer*)` corrisponde esattamente
// al `BindingTraits<FixedString("MenuLayer_init")>::Fn` derivato dal catalogo.
PULSE_GD_HOOK(MenuLayer, init, bool, (pulse::gd::MenuLayer* self)) {
    // `callOriginal` preserva firma e valore di ritorno dell'Hook_Point.
    return callOriginal(self);
}
