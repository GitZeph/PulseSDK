# Phase E — Live Geometry Dash verification (manual procedure)

This document records the exact, reproducible procedure used to verify the Pulse
loader against **real** Geometry Dash on macOS, and the fixes that were required
to make a live hook fire. It is integration/manual work — **not** auto-verifiable
in CI.

Verified on **2026-06-29**: GD **2.2081** (CFBundleShortVersionString `2.208`,
bundle id `com.robtop.geometrydashmac`), macOS **26.2 (25C56)**, Apple Silicon
(arm64e).

> ⚠️ Always operate on a **throwaway copy** of the bundle on a **local** volume
> (e.g. `/private/tmp`). Never modify the live Steam install. iCloud-synced
> folders (Desktop/Documents) cause codesign "detritus" failures — avoid them.

---

## 0. Prerequisites / environment

- macOS builds/links **must** run under `env -u LDFLAGS` (a global
  `-fuse-ld=mold` breaks AppleClang linking — Req 1.5).
- Dobby's `.asm` build **fails under ccache**. Build the Dobby-ON artifact with
  the compilers pinned to `/usr/bin/clang` to bypass ccache.
- The copied bundle can't be launched from the Steam UI, so `SteamAPI_Init`
  needs: the **Steam client running and logged in** (account owns GD) **plus** a
  `steam_appid.txt` containing `322170` in the launch working directory.

Suggested shell variables:

```bash
GD_SRC="$HOME/Library/Application Support/Steam/steamapps/common/Geometry Dash/Geometry Dash.app"
GD_COPY="/private/tmp/GD-pulse-test/Geometry Dash.app"
ARTIFACT="$PWD/build-artifact-dobby/lib/libpulse_loader.dylib"
PULSE="$PWD/cli/target/release/pulse"
```

## 1. Build the Dobby-ON loader artifact (bypassing ccache)

```bash
env -u LDFLAGS cmake -S . -B build-artifact-dobby \
  -DPULSE_BUILD_LOADER_ARTIFACT=ON \
  -DPULSE_ENABLE_DOBBY=ON \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_ASM_COMPILER=/usr/bin/clang
env -u LDFLAGS cmake --build build-artifact-dobby --target pulse_loader
```

Sanity-check the artifact is a self-contained arm64 dylib with the right install
name, the exported entry symbol, and **no** `@rpath/libdobby.dylib` dependency
(Dobby is linked statically via the `dobby_static` target):

```bash
file        build-artifact-dobby/lib/libpulse_loader.dylib   # → shared library arm64
otool -D    build-artifact-dobby/lib/libpulse_loader.dylib   # → @loader_path/libpulse_loader.dylib
nm -gU      build-artifact-dobby/lib/libpulse_loader.dylib | grep runtime_entry  # → _pulse_loader_runtime_entry
otool -L    build-artifact-dobby/lib/libpulse_loader.dylib | grep -i dobby       # → (no output)
```

## 2. Build the CLI

```bash
env -u LDFLAGS cargo build --release --manifest-path cli/Cargo.toml
```

## 3. Make a disposable copy and strip Geode

Geode injects by patching `libfmod.dylib` to also load `@rpath/Geode.dylib`.
Two inline-hooking engines on the same prologue conflict, so remove Geode from
the copy by restoring the pristine fmod (`restore_fmod.dylib`) and deleting the
Geode artifacts.

```bash
rm -rf /private/tmp/GD-pulse-test && mkdir -p /private/tmp/GD-pulse-test
cp -R "$GD_SRC" "$GD_COPY"
xattr -cr "$GD_COPY"
cp "$GD_COPY/Contents/Frameworks/restore_fmod.dylib" "$GD_COPY/Contents/Frameworks/libfmod.dylib"
rm -f "$GD_COPY/Contents/Frameworks/Geode.dylib" \
      "$GD_COPY/Contents/Frameworks/GeodeBootstrapper.dylib" \
      "$GD_COPY/Contents/Frameworks/restore_fmod.dylib"
rm -rf "$GD_COPY/Contents/geode"
otool -L "$GD_COPY/Contents/Frameworks/libfmod.dylib" | grep -i geode || echo "OK: no Geode in libfmod"
```

## 4. Install Pulse (native path) and launch

```bash
"$PULSE" install --gd "$GD_COPY" --artifact "$ARTIFACT" --native
echo "322170" > "$GD_COPY/Contents/MacOS/steam_appid.txt"
# Steam client must be running and logged in here.
cd "$GD_COPY/Contents/MacOS"
"./Geometry Dash" > /tmp/gd_pulse.log 2>&1 &
GD_PID=$!
sleep 35      # let it reach the main menu
kill "$GD_PID" 2>/dev/null
cd - >/dev/null
grep -i -E 'pulse\.demo|detour|originale|installato' /tmp/gd_pulse.log
```

### Expected (success)

```
[pulse-loader] runtime: 'MenuLayer::init' RVA 0x316688 ribasato su imageBase 0x… => indirizzo assoluto 0x…
[pulse-loader] runtime: demo mod 'pulse.demo' attiva — hook su MenuLayer::init installato … con trampolino reale cablato …
[pulse-loader] [pulse.demo] MenuLayer::init detour: esecuzione del detour (prima dell'originale)
[pulse-loader] [pulse.demo] MenuLayer::init: originale eseguito, valore di ritorno=true
```

The detour runs **before** the game's original `MenuLayer::init`, then calls the
original through the real Dobby trampoline.

## 5. Verify byte-exact uninstall

```bash
"$PULSE" uninstall --gd "$GD_COPY"
shasum -a 256 "$GD_COPY/Contents/MacOS/Geometry Dash"
# → 76097a7e8dad567711b776d08141f00e176b81512a3ee3073d598a30fb5f1b91  (== pristine original)
otool -L "$GD_COPY/Contents/MacOS/Geometry Dash" | grep -i pulse || echo "OK: no pulse load command"
ls "$GD_COPY/Contents/MacOS/libpulse_loader.dylib" 2>/dev/null || echo "OK: dylib removed"
```

---

## The MenuLayer::init binding (GD 2.2081 / macos-arm64)

- RVA `0x316688` (image base `0x100000000` → preferred VA `0x100316688`).
- Cross-checked **observationally** against the Geode 2.2081 community binding
  `m1` field (numeric address only, no code reuse), then confirmed against the
  real binary by disassembling the prologue with `otool -arch arm64 -tv`.
- Stored in `mod-index/bindings/2.2081/macos-arm64.pbind` with `verified = true`.
- The loader rebases this RVA to the live address at runtime:
  `_dyld_get_image_header(0) + offset` (see `loader/core/runtime_entry.cpp`).

## Fixes that were required for a live hook (durable, in-repo)

These bugs were latent and never exercised by host tests; they surfaced only on
the real runtime path:

1. **RVA→absolute rebase** — `loader/core/runtime_entry.cpp` now adds the live
   main-image base to the `.pbind` RVA before handing it to the hook backend.
   Previously the raw RVA was passed straight to Dobby.
2. **Dobby API** — `loader/hooking/dobby_backend.cpp` calls
   `DobbyHook(void*, void*, void**)` (the current API has no
   `dobby_dummy_func_t`).
3. **Self-contained artifact** — `loader/CMakeLists.txt` links `dobby_static`,
   so the loader embeds Dobby instead of depending on `@rpath/libdobby.dylib`
   (which dyld can't resolve once injected).
4. **Registration survives dead-strip** — `sdk/include/pulse/hooks.hpp` marks
   the `PULSE_HOOK` registration object `__attribute__((used))`.
5. **Static-init order** — `runtime_entry` runs from an early-load constructor
   that can execute before other TUs' global initializers. So
   `loader/mvp/menulayer_init_hook.cpp` registers the detour **explicitly and
   idempotently** in `ensure_menulayer_init_hook_registered()`, and the demo log
   sink uses an "init on first use" function-local static instead of a global.
6. **Entitlements** — `cli/assets/pulse.entitlements` adds
   `com.apple.security.cs.disable-executable-page-protection`,
   `…allow-jit`, and `…allow-unsigned-executable-memory`. Without these, Dobby's
   in-place patch of the signed `__TEXT` did not take effect under hardened
   runtime (the hook "installed" but never fired).

## Caveats

- **Security tradeoff**: the added entitlements materially loosen the process's
  hardening. They are inherent to inline hooking of a signed binary.
- **Geode coexistence is unsolved**: this was verified with Geode removed from
  the copy. Running alongside Geode (another inline-hooking engine on the same
  function) needs a deliberate strategy.
- **Not CI-verifiable**: requires the real GD binary, the `codesign` toolchain,
  a running Steam client, and manual game launch.

---

# External mods (external-mod-loading Phase E)

This section extends the manual procedure above to a **real external `.pulse`
mod** discovered from a Mods_Directory at runtime (external-mod-loading tasks
9.1–9.3), instead of the built-in demo. It assumes the build/inject/launch
mechanics from §1–§5 above.

## Enablement layer landed in-repo (host-tested)

These were latent gaps that blocked loading a real on-disk `.pulse`; all are now
implemented and covered by host tests (593/593 green):

- **Loader-side `.pulse` reader** — `loader/package/pulse_zip_reader.{hpp,cpp}`:
  a self-contained, central-directory-based ZIP parser that turns a `.pulse`
  file on disk into a `PackageArchive` and runs it through `PulsePackage::open`.
  Supports **STORED** entries (method 0); compressed entries (method 8) are
  rejected with a fail-open diagnostic. No FetchContent / network dependency.
- **CLI writes STORED** — `cli/src/builder.rs` `write_zip` now uses
  `CompressionMethod::Stored`, so Pulse-produced `.pulse` files are readable by
  the loader reader above.
- **Native-module name reconciliation** — the CLI emits the native module as
  `code/<platform>.<ext>` (e.g. `code/macos-arm64.dylib`). The validator
  (`mod_manifest_validator.cpp`) and `ModLoader::run` now accept
  `code/module.pulsebin` **or** a platform-named `code/*.{dylib,so,dll}`,
  selecting the entry matching `RuntimeContext.platformId`.
- **Runtime opener wired** — `loader/core/runtime_entry.cpp` now calls
  `modLoader.setPackageOpener(... open_pulse_file ...)`, so the runtime actually
  discovers on-disk mods (previously it logged "nessun PackageOpener
  configurato; zero mod scoperte").

## ✅ RESOLVED — external hooks now land in the loader's registry

`pulse::hooks::registry()` (`sdk/include/pulse/hooks.hpp`) is a header-only
`inline` function with a function-local `static`. Within one linked image that
is a single instance, but a `dlopen`'d external mod `.dylib` (built standalone,
loaded `RTLD_LOCAL`, macOS two-level namespace) gets its **own** registry. So a
native external mod's `PULSE_HOOK`/`register_hook` calls would populate the
*mod's* registry, which the loader's `resolve_all`/`installWindow` never see.

What shipped: the Loader_Artifact now exports a C-ABI registrar
`pulse_loader_register_hook(symbol, detour, trampoline, priority)` with **default
visibility** (`loader/core/mod_registrar.cpp`, guarded by
`PULSE_LOADER_ARTIFACT`). It registers into the loader's `pulse::hooks` registry
and **owns a process-lifetime copy** of the symbol string (because
`HookRegistration.symbol` is a non-owning `std::string_view`). The SDK's
`register_hook` (`sdk/include/pulse/hooks.hpp`) now **auto-delegates** to that
entry point via `dlsym(RTLD_DEFAULT, "pulse_loader_register_hook")` when running
inside the injected process, so a `dlopen`'d mod's `register_hook`/`PULSE_HOOK`
calls land in the **loader's** registry (shared across the module boundary). If
no loader is present (host tests, standalone), it falls back to this image's
local registry — host behavior is unchanged. Inside the artifact itself
`register_hook` always uses the local registry, so the exported registrar is
recursion-free.

Consequence: with the enablement layer above plus this fix, the runtime will
**discover → validate → check compat → resolve deps → dlopen → resolve & invoke
the entry point** for a real external `.pulse`, and the mod's **hooks now
register into the loader's registry** so `resolve_all`/`installWindow` can
install them. This is covered by host tests (`pulse_mod_registrar_test`,
`pulse_sdk_hook_delegation_test`); the end-to-end install/fire is still only
verifiable on real hardware (§9.2 below).

## Build the test mod and package it (task 9.1)

```bash
"$PULSE" new com.pulse.testmod         # scaffolds a project
```

Edit `com.pulse.testmod/pulse.toml` to declare a native mod, an entry point
symbol, and GD compatibility:

```toml
schema_version = 1

[mod]
id = "com.pulse.testmod"
version = "0.1.0"
name = "Pulse Test Mod"
type = "native"

[[entry_points]]
kind = "init"
symbol = "pulse_mod_init"      # MUST be the exported (extern "C") symbol below

[compat]
platform = "macos-arm64"
gd_min = "2.2081.0"
gd_max = "2.2081.0"
```

Replace `src/` with a single C++ source that exports the entry point and
registers a hook on the only verified binding (`MenuLayer::init`). Because the
`PULSE_HOOK` macro needs a C++ identifier (no `::`), register the binding symbol
explicitly via `register_hook`:

```cpp
#include <pulse/hooks.hpp>
static bool (*orig_init)(void*) = nullptr;
extern "C" bool pulse_testmod_menulayer(void* self) {
    // external-mod detour: runs before the original, then calls it
    return orig_init ? orig_init(self) : true;
}
extern "C" void pulse_mod_init() {
    pulse::hooks::register_hook(
        "MenuLayer::init",
        reinterpret_cast<void*>(&pulse_testmod_menulayer),
        reinterpret_cast<void**>(&orig_init));
}
```

Build the package (real toolchain; produces `code/macos-arm64.dylib`):

```bash
env -u LDFLAGS "$PULSE" build com.pulse.testmod
ls com.pulse.testmod/com.pulse.testmod.pulse     # the .pulse container
```

Smoke-check the seam (9.1): the loader's `MacOsModuleLoader` extracts the
verified bytes to `~/Library/Caches/Pulse/<session>/<modid>.dylib` (0600),
`dlopen`s it, resolves `pulse_mod_init` via `dlsym`, then `dlclose` + removes
the temp file on unload. (Observable in the loader log during the run below.)

## Drop it in the Mods_Directory and run (task 9.2)

The runtime resolves the Mods_Directory as `mods/` next to the GD executable
(`resolve_mods_directory()` → `<exe dir>/mods`).

```bash
# §1, §3 above: build the Dobby-ON artifact, copy the bundle, strip Geode.
mkdir -p "$GD_COPY/Contents/MacOS/mods"
cp com.pulse.testmod/com.pulse.testmod.pulse "$GD_COPY/Contents/MacOS/mods/"

"$PULSE" install --gd "$GD_COPY" --artifact "$ARTIFACT" --native
echo "322170" > "$GD_COPY/Contents/MacOS/steam_appid.txt"
cd "$GD_COPY/Contents/MacOS"
"./Geometry Dash" > /tmp/gd_pulse_ext.log 2>&1 &
GD_PID=$!; sleep 35; kill "$GD_PID" 2>/dev/null; cd - >/dev/null
grep -i -E "Mod_Loader|com.pulse.testmod|detour|module" /tmp/gd_pulse_ext.log
```

Expected **with the current code** (discovery works; hooks now share the
loader's registry): log lines showing the Mods_Directory scanned,
`com.pulse.testmod` discovered, validated, compatible, its module loaded, and
`pulse_mod_init` invoked once — plus the built-in `[pulse.demo]` lines (demo
preserved). Because the registry is now shared across the `dlopen` boundary (the
mod delegates to the loader's exported `pulse_loader_register_hook`), the
external mod's detour lines **are** now expected to appear alongside the demo's.
This end-to-end install/fire is verified only on real hardware.

## Disable / teardown / byte-exact uninstall (task 9.3)

On GD shutdown, the loader teardown removes the mod's hooks byte-exact (reverse
order), `dlclose`s the module, and brings the mod to Removed. Then:

```bash
"$PULSE" uninstall --gd "$GD_COPY"
shasum -a 256 "$GD_COPY/Contents/MacOS/Geometry Dash"   # == pristine original
```

(The executable restore is independent of the mod; the mod lives under
`Contents/MacOS/mods/` and is removed by deleting that directory.)

---

## ✅ VERIFIED on real GD — 2026-06-30 (tasks 9.1, 9.2, 9.3)

A real external `.pulse` mod (`examples/allhooks-mod`, id `com.pulse.allhooks`,
hooking the only verified binding `MenuLayer::init`) was discovered from the
Mods_Directory, loaded, and fired end-to-end on **real GD 2.2081** (Apple
Silicon, Steam).

> **Note (Hook_Chaining, task 7.6).** The `PULSE_DISABLE_BUILTIN_DEMO`
> palliative has been **removed** from `loader/core/runtime_entry.cpp`. The
> launch below originally required `PULSE_DISABLE_BUILTIN_DEMO=1` to free
> `MenuLayer::init` for the external mod; that env var is **no longer needed**.
> The demo↔external-mod conflict is now resolved by the **HookChainRegistry**:
> the built-in demo (`pulse.demo`) and the external mod coexist as ordered
> Hook_Links on the **single** Underlying_Installation of `MenuLayer::init`.

### The demo↔external-mod conflict and its resolution

The first attempt (before Hook_Chaining) failed with `DobbyHook ... codice -1`:
the **built-in demo** and the external mod both hooked the **same** address
(`MenuLayer::init`, the only verified binding). Two inline hooks on one prologue
conflict — Dobby rejects the second. The external mod was correctly
isolated/rolled back and GD still reached the menu, but its detour never fired.

Resolution (durable, in-repo): the conflict is fixed by **Hook_Chaining**, not
by disabling the demo. `loader/core/runtime_entry.cpp` groups the entire SDK
registry by resolved Target_Address and feeds each group into
`HookChainRegistry::insertLink`. The built-in demo (`MenuLayer_init`, reconciled
to the `MenuLayer::init` binding, attributed to the reserved Mod_Id `pulse.demo`)
is just another Hook_Link in the chain: the first link creates the **single**
DobbyHook toward the stable Head_Thunk, and subsequent links (the external mod)
only relink neighbour Trampoline_Slots — no second DobbyHook, no
"address already hooked" error. Both detours execute in Chain_Order and the last
link calls the real original through the trampoline. The earlier
`PULSE_DISABLE_BUILTIN_DEMO` environment variable has been **eliminated** as part
of task 7.6.

### Launch (no env var — demo and external mod coexist via Hook_Chaining)

```bash
mkdir -p "$GD_COPY/Contents/MacOS/mods"
cp examples/allhooks-mod/com.pulse.allhooks.pulse "$GD_COPY/Contents/MacOS/mods/"
"$PULSE" install --gd "$GD_COPY" --artifact "$ARTIFACT" --native
echo "322170" > "$GD_COPY/Contents/MacOS/steam_appid.txt"
cd "$GD_COPY/Contents/MacOS"
"./Geometry Dash" > /tmp/gd_pulse_ext.log 2>&1 &
GD_PID=$!; sleep 35; kill "$GD_PID" 2>/dev/null; cd - >/dev/null
```

### Observed (success) — `/tmp/gd_pulse_ext.log`

```
[pulse-loader] Mod_Loader: demo interna attribuita al Mod_Id riservato 'pulse.demo' (Req 9.6).
[allhooks] pulse_mod_init: registering hook on MenuLayer::init
[pulse-loader] mod 'com.pulse.allhooks': hook installato sul simbolo 'MenuLayer::init' (Req 5.6, 5.8).
[pulse-loader] Mod_Loader: pipeline completata — 1 caricate, 0 escluse, 0 isolate; 1 hook installati al termine (Req 9.6).
[pulse-loader] runtime: caricamento centralizzato — Mod_Loader: init step — 1 mod caricate, 1 hook installati.
[allhooks] MenuLayer::init detour: running BEFORE original (self=0xbdad3fe00)
[allhooks] MenuLayer::init detour: original returned true (value preserved)
```

This proves, on the real binary:
- **9.1** — `MacOsModuleLoader` extracted the verified bytes to a per-session
  temp dylib, `dlopen`'d it, and resolved `pulse_mod_init` via `dlsym` (the
  entry point ran: `[allhooks] pulse_mod_init: registering...`).
- **9.2** — the external mod's `register_hook` landed in the **loader's** registry
  (shared across the `dlopen` boundary via the exported
  `pulse_loader_register_hook`), `resolve_all`/`HookGate` installed the detour via
  Dobby with no conflict, and the detour fired **before** the original and called
  it through the real trampoline with the return value preserved.

### Byte-exact uninstall — verified

```
$ "$PULSE" uninstall --gd "$GD_COPY"
Disinstallazione completata: Pulse_Loader rimosso …
$ shasum -a 256 "$GD_COPY/Contents/MacOS/Geometry Dash"
76097a7e8dad567711b776d08141f00e176b81512a3ee3073d598a30fb5f1b91   (== pristine original)
$ otool -L … | grep -i pulse   # → (no pulse load command)
$ ls … /libpulse_loader.dylib  # → removed
```

- **9.3** — the executable is restored **byte-exact** (SHA-256 identical to the
  pristine original), the `@loader_path/libpulse_loader.dylib` load command is
  gone, and the injected dylib is removed. (Dobby's hooks are in-memory only and
  vanish with the process; the on-disk restore is the install/uninstall path.)

---

## ✅ RE-VERIFIED on real GD — Hook_Chaining tasks 9.1 / 9.2 / 9.3

Verified on real **GD 2.2081** (Apple Silicon, Steam) with the Dobby-ON artifact
built `-DPULSE_ENABLE_HEAD_THUNK_ASM=ON` (real per-arch Head_Thunk).

- **9.1** — The real arm64 Head_Thunk assembled into the artifact
  (`pulse_head_thunk_arm64_tpl`: `ldr x16,<lit>` / `ldr x16,[x16]` / `br x16`).
  On real GD the demo detour fired with a correct `self`, then called the
  original through the trampoline — the signature-agnostic indirect branch to
  `currentHead` preserves the argument registers.
- **9.3** — `uninstall` restores the executable **byte-exact**
  (`shasum -a 256` == pristine `76097a7e…1b91`), no `pulse` load command, dylib
  removed.

### 9.2 — a real defect found and fixed (the external-mod install bypassed the chain)

The first 9.2 attempt **failed** with `DobbyHook … codice -1`: the built-in demo
went through the `HookChainRegistry` (one install on `MenuLayer::init`), but the
**external-mod pipeline** (`ModManagerWiring::installWindow`) still installed its
hook **directly via `HookGate`** — a *second* `DobbyHook` on the same address →
conflict. Task 7.5's wiring had chain-ified only the demo, not the external-mod
path.

Fix (in-repo): `ModManagerWiring` now accepts a shared `HookChainRegistry`
(`setChainRegistry`), and `installWindow` / `rollbackModHooks` route through it
when wired (`insertLink` / `removeOwner`) instead of the direct `HookGate` path;
`runtime_entry` injects the **same** process-lifetime registry used for the demo
into the `ModLoader`. Host coverage: `tests/mod_loader_chain_coexistence_test.cpp`
(external mod becomes a 2nd link on the single install — `installAttempts == 1`,
no `codice -1`; teardown relinks then byte-exact uninstalls). Full host suite:
675/675 green.

### Observed (success, no env var) — `/tmp/gd_pulse_ext.log`

```
[pulse-loader] hook-chaining: aggiunto anello mod 'pulse.demo' @ 0x104caa688 in posizione 0 (Chain_Head)
[allhooks] pulse_mod_init: registering hook on MenuLayer::init
[pulse-loader] hook-chaining: aggiunto anello mod 'com.pulse.allhooks' @ 0x104caa688 in posizione 1
[pulse-loader] mod 'com.pulse.allhooks': hook inserito in catena sul simbolo 'MenuLayer::init' (Hook_Chaining, Req 8; nessuna seconda DobbyHook).
[pulse-loader] [pulse.demo] MenuLayer::init detour: esecuzione del detour (prima dell'originale)
[allhooks] MenuLayer::init detour: running BEFORE original (self=0x75facee00)
[allhooks] MenuLayer::init detour: original returned true (value preserved)
[pulse-loader] [pulse.demo] MenuLayer::init: originale eseguito, valore di ritorno=true
```

Both the demo (`pulse.demo`, Chain_Head) and the external mod
(`com.pulse.allhooks`, position 1) coexist as ordered Hook_Links on the **single**
Underlying_Installation of `MenuLayer::init`: both detours run in Chain_Order
before the original, the last link reaches the Real_Original through the
trampoline, and the return value is preserved — **no `codice -1`** (Req 8.1, 8.2,
8.4 satisfied end-to-end).

---

# GD API Surface — Phase I real-GD verification (task 17.1)

This section records the live verification of the **gd-api-surface** Phase I:
the trampoline forwarding of the original `MenuLayer::init` when a mod is
written against the **generated Surface SDK** (the `PULSE_GD_HOOK` macro emitted
into `sdk/include/pulse/gd/hooks.gen.hpp`), rather than a hand-written
`register_hook`. It is integration/manual work — **not** auto-verifiable in CI.

Verified on **2026-06-30**: real GD **2.2081** (Apple Silicon, Steam), with the
Dobby-ON loader artifact and Geode stripped from the throwaway copy (§0–§3 of
the procedure above).

## Artifacts under test

- **Generated Surface SDK** — produced by `pulse surface generate` from the seed
  manifest `mod-index/surface/surface.toml`:
  `sdk/include/pulse/gd/{types,bindings,hooks}.gen.hpp`. Header-only; the only
  `API_Element` is `MenuLayer::init` (`bool` / `["MenuLayer*"]`, RVA `0x316688`,
  `verified = true` for `(2.2081, macos-arm64)`), reused from the
  `bindings-pipeline` catalog seed (no parallel offset source).
- **Surface example mod** — `examples/surfacemod/` (id `com.pulse.surfacemod`),
  native mod hooking via the generated macro:
  `PULSE_GD_HOOK(MenuLayer, init, bool, (pulse::gd::MenuLayer* self))`. The macro
  expands to the compile-time Build_Check (`static_assert(in_surface<…>())` +
  `SignatureMatches`) + `PULSE_HOOK` + canonical
  `register_hook("MenuLayer::init", …)`. The mod's clean compile is itself the
  host-side evidence that the generated Build_Check `static_assert`s pass.
- **Loader artifact** — `build-artifact-dobby/lib/libpulse_loader.dylib`
  (arm64, exports `_pulse_loader_runtime_entry`, Dobby linked statically).

## Launch

Same as the external-mod procedure above, dropping
`examples/surfacemod/com.pulse.surfacemod.pulse` into
`"$GD_COPY/Contents/MacOS/mods/"`, `pulse install --native`, `steam_appid.txt`
= `322170`, then launching with the Steam client running and logged in. Log
captured to `/tmp/gd_surface.log`.

## Observed (success) — `/tmp/gd_surface.log`

```
[surfacemod] pulse_mod_init: PULSE_GD_HOOK auto-registered the MenuLayer::init detour (canonical "MenuLayer::init")
[pulse-loader] mod 'com.pulse.surfacemod': hook inserito in catena sul simbolo 'MenuLayer::init' (Hook_Chaining, Req 8; nessuna seconda DobbyHook).
[pulse-loader] [pulse.demo] MenuLayer::init detour: esecuzione del detour (prima dell'originale)
[surfacemod] MenuLayer::init detour: running BEFORE original (self=0xc6432ae00)
[surfacemod] MenuLayer::init detour: original returned true (value preserved)
[pulse-loader] [pulse.demo] MenuLayer::init: originale eseguito, valore di ritorno=true
```

This proves, on the real binary (Req 4.3):

- The mod compiled against the **generated Surface SDK** registered its detour
  via the canonical symbol `"MenuLayer::init"` (no address literal), through the
  `PULSE_GD_HOOK` macro — the auto-registration line confirms it.
- The detour was installed on the resolved `MenuLayer::init` binding and fired
  **before** the original.
- `callOriginal(self)` **forwarded the call through the loader-wired trampoline
  slot** and returned the value produced by the original (`true`, preserved).
- The Surface mod and the built-in `pulse.demo` coexist as ordered Hook_Links on
  the **single** Underlying_Installation of `MenuLayer::init` (Hook_Chaining) —
  no second DobbyHook, no `codice -1`.

This is the only manual link of the gd-api-surface spec; all other phases (A–H)
are host-tested in CI. Task 17.1 and the final checkpoint are satisfied
end-to-end.

---

# Binary Binding Extraction — Phase H, part 1: real Android `libcocos2dcpp.so` (task 17.1)

This records the **android-arm64 symbol-table extraction** run against the
**real** GD 2.2081 Android library, performed on 2026-06-30. It is the
fully-automatic, host-runnable half of Phase H (no live GD process needed — the
extractor just reads a binary the user supplied at `public/libcocos2dcpp.so`).
The **macos-arm64 cross-derivation** half still needs the macOS Mach-O and is
recorded separately when run.

## Input

- `public/libcocos2dcpp.so` — ELF 64-bit, ARM aarch64, **stripped** of the
  static `.symtab` but retaining a rich **dynamic** symbol table: `nm -D` reports
  **13,857** defined function symbols (`T _ZN…`), **20,609** mangled `_Z…`
  entries total, and **925** `_ZTV` vtable symbols. BuildID
  `ca621323d54c1d94f5f13a2778ce9c8baa31b9a8`.

## Command

```bash
env -u LDFLAGS cargo build --release --manifest-path cli/Cargo.toml
pulse bindings extract --gd 2.2081 --platform android-arm64 \
  --elf public/libcocos2dcpp.so --catalog-root <catalog>
```

## Result (verified)

- **Exit code 0.** **14,975** `symbol-table` offsets emitted; **14,973**
  Catalog_Entry TOML files written; **57** symbols excluded at demangling
  (undefined/imported `FMOD::…` — expected and correct, Req 2.5/2.6).
- **14,431 `verified = true`** (prologue-sanity plausible) vs **542
  `verified = false`** (fail-closed: zero/sentinel offset or non-plausible
  prologue) — ~96% verified.
- **Ground-truth spot-check:** the extracted `MenuLayer::init` android offset is
  `rva = 0x5C2E7C`; `nm -D` reports `_ZN9MenuLayer4initEv` at `0x5c2e7c` — exact
  match. The entry carries the this-first signature `params = ["MenuLayer*"]`,
  `tier = "symbol-table"`, `prologue_method = "auto-sanity"`,
  `prologue_outcome = "plausible-entry"`. Return type is `"?"` — the documented
  Itanium limitation: non-template member return types are not encoded in the
  mangled name and are NOT fabricated.
- `pulse bindings generate` loads the produced catalog and pivots it to `.pbind`
  with no naming errors.

## A real defect found and fixed (would never appear on synthetic fixtures)

The first real run wrote ~10,700 files then **crashed mid-commit** on
`cocos2d::CCPoint::operator/`: the per-symbol filename stem mapped `::`→`__` but
did not sanitize C++ operator characters, so the `/` in `operator/` produced a
path with a stray directory separator (`.../operator/.toml`) and the atomic
write failed.

Fix (durable, in-repo): `symbol_file_stem` (`cli/src/bindings/contribution.rs`)
now encodes every character outside `[A-Za-z0-9_.-]` as a self-delimiting
`_x<HEX>_` escape (injective; `operator/` → `…operator_x2F_`), preserving the
legacy `::`→`__` mapping byte-for-byte (so the existing `MenuLayer__init.toml`
seed name is unchanged), with a length-guard hash suffix for pathological ids.
The extractor writer (`cli/src/extract/writer.rs`) now also validates stems and
detects path collisions in its **planning phase**, so a bad name fails closed
(`ExtractError::InvalidStem`) **before any file is written** — no more partial
mid-commit writes. Host suite: 325 lib tests green (10 new). The 94
operator-overload files now write correctly (e.g.
`cocos2d__CCPoint__operator_x2F_.toml`, with `symbol = "cocos2d::CCPoint::operator/"`
intact inside).

## What this proves and what remains

- **Proven on real data:** the Android symbol-table path derives ~15k
  first-party bindings (names + this-first signatures + verified offsets +
  provenance) end-to-end, with offsets confirmed against the binary's own symbol
  table. This is the first-party master list the design bet on.
- **Still pending (needs the macOS Mach-O):** the `macos-arm64` cross-derivation
  — using the Phase G2 Android vtable ordering to map each method to a macOS
  vtable slot — has not been run, because the macOS GD binary was not supplied to
  this run. It is the remaining half of task 17.1 and the real test of the
  ELF-relocation handling in `elf_vtable.rs`.

---

# Binary Binding Extraction — Phase H, part 2: macOS cross-derivation (task 17.1)

This records the **macos-arm64 cross-derivation** run against the real GD 2.2081
binaries (Android ELF + macOS Mach-O arm64 slice), performed 2026-07-01. This is
the Phase G2 path — using the Android vtable ordering to map each virtual method
to its macOS vtable slot — and the real test of the ELF-vtable + Mach-O RTTI
reconstruction on actual data.

## Inputs

- `public/libcocos2dcpp.so` — Android ELF (names/signatures + vtable order).
- `public/Geometry Dash` — universal Mach-O; the **arm64 slice** was thinned with
  `lipo -thin arm64 "public/Geometry Dash" -output /tmp/gd-arm64` (the loader
  requires a thin Mach-O). The real arm64 slice has only **580 symbols** and is
  **stripped of GD's `__ZTV`/`__ZTI` RTTI symbols** (only ~6 `__ZTV` / ~9 `__ZTI`
  from libc++/cxxabi survive).

## Command

```bash
pulse bindings extract --gd 2.2081 --platform macos-arm64 \
  --elf public/libcocos2dcpp.so --macho /tmp/gd-arm64 --catalog-root <catalog>
```

## Two real defects found and fixed (neither was the predicted relocation issue)

1. **Android side — demangle-prefix mismatch (0 classes → 925).** The vtable
   class recovery used `class_from_special(name, "vtable for ")`, but
   `cpp_demangle` renders vtable symbols as `{vtable(MenuLayer)}`, **not** the
   c++filt-style `vtable for MenuLayer`. So every `_ZTV` symbol returned `None`
   and **0** Android classes were recovered. Fix: new `class_from_vtable()`
   accepts both `{vtable(X)}` and `vtable for X`, gated on a real `_ZTV`/`__ZTV`
   prefix (excludes VTT/construction-vtables). This alone took Android class
   recovery from **0 → 925**. (The Android `.data.rel.ro` relocations were
   actually fine: `R_AARCH64_RELATIVE`, which `object` already surfaces as
   `Absolute` with the target RVA in the addend — the existing resolver handled
   them once the class was found.)
2. **macOS side — the real Mach-O is stripped of GD RTTI symbols.** The original
   Mach-O reconstruction was 100% symbol-name-driven (`__ZTV`/`__ZTI`), which
   recovers **zero** GD classes on the real (stripped) slice. Fix: a fail-closed
   **structural Itanium RTTI scanner** (`reconstruct_macho_structural`) that
   walks the in-memory RTTI chain — typeinfo **name string** (`9MenuLayer`,
   demangled identically to the Android side) → `type_info` object → **primary
   vtable** (offset-to-top 0, type_info pointer, first slot in `__text`) — and
   reads the function-pointer array. A class with >1 distinct primary-vtable
   candidate is **excluded** (never guessed). Symbol-found classes still take
   precedence, so synthetic/symbolful fixtures are unaffected.

> Note: this structural scanner is a capability **beyond the original design**,
> which assumed RTTI symbols would be present. It was necessary because real GD
> macOS binaries are stripped. It is conservative/fail-closed (single-inheritance
> primary vtables; ambiguous multiple-inheritance cases skipped).

## Result (verified)

- **925** Android vtable classes recovered (was 0).
- **3,827** `cross-derived` macOS offsets emitted; **3,646 `verified = true`**
  (prologue-confirmed), 181 false. ~11,148 of 14,975 methods correctly
  fail-closed (non-virtual symbols with no vtable slot, holes, or ambiguous) —
  expected, and never guessed.
- **otool semantic spot-check** (thinned slice preferred base `0x100000000`;
  emitted `rva` is the full VA):
  - `cocos2d::CCNode::visit` @ `0x1002161D4` disassembles to
    `ldrb w8,[x0,#0x11e]; cmp w8,#0x1; b.ne …; stp …` — i.e. the
    `if (!m_bVisible) return;` opening that is `CCNode::visit`'s known structure.
    This is a **semantic** confirmation that the ordinal mapping lands on the
    *correct* function, not merely a plausible prologue.
  - The address is also the target of multiple `bl`/`b` call sites, consistent
    with a real virtual function entry.

## Status

The macOS cross-derivation path is **verified working on real data**: ~3.8k
typed macOS bindings derived first-party (Android names/signatures + macOS
offsets via vtable-ordinal matching), spot-confirmed by disassembly. Host suite
remains green (325 tests). The `verified=false` and fail-closed remainder are
honest non-results, not errors.
