// examples/surfacemod/src/mod.cpp — EXTERNAL Pulse mod built against the
// GENERATED GD API Surface SDK (`pulse surface generate`).
//
// Unlike examples/allhooks-mod (which hand-writes `register_hook`), this mod
// declares its hook through the generated ergonomic macro:
//
//     PULSE_GD_HOOK(MenuLayer, init, bool, (pulse::gd::MenuLayer* self))
//
// which expands (in hooks.gen.hpp) to the compile-time Build_Check —
//   static_assert(in_surface<FixedString("MenuLayer_init")>())   // Req 5.3
//   static_assert(SignatureMatches<FixedString("MenuLayer_init"), Fn>)  // Req 5.1/5.2
// — plus the PULSE_HOOK-style detour/trampoline machinery and the canonical
// register_hook("MenuLayer::init", detour, trampoline, priority) call (the same
// explicit path allhooks uses), so the signature is verified at compile time on
// the C++-safe token while runtime resolution happens on the canonical symbol.
//
// The only verified binding for (GD 2.2081, macos-arm64) is `MenuLayer::init`
// (RVA 0x316688). The detour logs before calling the original, calls the
// original through the trampoline via `callOriginal(self)`, logs the preserved
// return value, and returns it unchanged (transparent call-original, Req 4.3).
//
// `pulse::gd::MenuLayer` is an OPAQUE forward-declared type (types.gen.hpp); the
// mod never needs the game's class layout — it just forwards the opaque pointer.
//
// Logs go to stderr (captured in the GD launch log), prefixed "[surfacemod]".

#include <pulse/gd/types.gen.hpp>     // forward-declared opaque GD types
#include <pulse/gd/bindings.gen.hpp>  // BindingTraits + in_surface<> markers
#include <pulse/gd/hooks.gen.hpp>     // PULSE_GD_HOOK macro

#include <cstdio>

// Detour for MenuLayer::init, declared through the generated Surface macro.
// PULSE_GD_HOOK provides, in a dedicated namespace, the trampoline slot
// (`pulse_original`), the canonical register_hook(...) registration, and the
// `callOriginal(...)` helper that preserves the Hook_Point signature.
PULSE_GD_HOOK(MenuLayer, init, bool, (pulse::gd::MenuLayer* self)) {
    std::fprintf(stderr,
                 "[surfacemod] MenuLayer::init detour: running BEFORE original "
                 "(self=%p)\n",
                 static_cast<void*>(self));
    std::fflush(stderr);

    // Call the game's original through the real trampoline slot wired by the
    // loader after install, preserving parameters and the return value.
    const bool result = callOriginal(self);

    std::fprintf(stderr,
                 "[surfacemod] MenuLayer::init detour: original returned %s "
                 "(value preserved)\n",
                 result ? "true" : "false");
    std::fflush(stderr);
    return result;
}

// Entry point declared in pulse.toml ([[entry_points]].symbol = "pulse_mod_init").
// Invoked exactly once by the Mod_Loader on the transition to Enabled. The hook
// is already auto-registered by PULSE_GD_HOOK's static initializer (marked
// `used` so it survives dead-strip), so this body only logs a confirmation.
extern "C" void pulse_mod_init() {
    std::fprintf(stderr,
                 "[surfacemod] pulse_mod_init: PULSE_GD_HOOK auto-registered the "
                 "MenuLayer::init detour (canonical \"MenuLayer::init\")\n");
    std::fflush(stderr);
}
