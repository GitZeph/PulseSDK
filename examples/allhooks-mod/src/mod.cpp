// examples/allhooks-mod/src/mod.cpp — minimal EXTERNAL Pulse mod that exercises
// every GD hook currently shipped in the verified bindings (.pbind).
//
// At present the only verified binding for (GD 2.2081, macos-arm64) is
// `MenuLayer::init` (RVA 0x316688). This mod hooks it: the detour runs BEFORE
// the game's original, logs, then calls the original through the trampoline and
// returns its value unchanged (transparent call-original).
//
// How this reaches the loader's registry: the SDK's `register_hook` (header
// `<pulse/hooks.hpp>`) auto-delegates, at runtime, to the loader artifact's
// exported `pulse_loader_register_hook` (resolved via dlsym(RTLD_DEFAULT,...)).
// So a `dlopen`'d external mod registers into the SAME registry the loader
// resolves/installs from. The loader writes the real trampoline address into
// `g_orig_menulayer_init` via `bind_trampoline` after installing the detour.
//
// Logs go to stderr (captured in the GD launch log), prefixed "[allhooks]".

#include <pulse/hooks.hpp>

#include <cstdio>

namespace {

// Trampoline slot to the original MenuLayer::init. The loader fills this in
// after installing the hook; until then it is null (guarded below).
// Signature of MenuLayer::init is `bool(MenuLayer*)`; we use `void*` for `self`
// since the mod does not have the game's MenuLayer type.
bool (*g_orig_menulayer_init)(void* self) = nullptr;

}  // namespace

// Detour for MenuLayer::init. Runs before the original, then forwards to it.
extern "C" bool pulse_allhooks_menulayer_init(void* self) {
    std::fprintf(stderr,
                 "[allhooks] MenuLayer::init detour: running BEFORE original "
                 "(self=%p)\n",
                 self);
    std::fflush(stderr);

    const bool result =
        g_orig_menulayer_init != nullptr ? g_orig_menulayer_init(self) : true;

    std::fprintf(stderr,
                 "[allhooks] MenuLayer::init detour: original returned %s "
                 "(value preserved)\n",
                 result ? "true" : "false");
    std::fflush(stderr);
    return result;
}

// Entry point declared in pulse.toml ([[entry_points]].symbol = "pulse_mod_init").
// Invoked exactly once by the Mod_Loader on the transition to Enabled. Registers
// the mod's hook for every GD binding currently shipped in the .pbind.
extern "C" void pulse_mod_init() {
    std::fprintf(stderr,
                 "[allhooks] pulse_mod_init: registering hook on MenuLayer::init\n");
    std::fflush(stderr);

    pulse::hooks::register_hook(
        "MenuLayer::init",
        reinterpret_cast<void*>(&pulse_allhooks_menulayer_init),
        reinterpret_cast<void**>(&g_orig_menulayer_init));
}
