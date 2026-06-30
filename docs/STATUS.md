# Where Pulse is at

Plain-English snapshot of the project, no jargon walls. If you want the deep technical logs, the other files in `docs/` have them — this is the "catch me up" page.

## The one-paragraph version

Pulse is a working Geometry Dash mod loader and SDK. It injects into GD, hooks functions, lets multiple mods share the same hook without fighting, and loads external `.pulse` mods — all verified on real GD 2.2081 on Apple Silicon. The thing we've been hammering on lately is **bindings** (knowing where GD's functions live), and we just went from 1 hand-made binding to several thousand auto-derived ones.

## What works today

- **The loader.** Injects into GD, installs hooks through Dobby, cleans up byte-for-byte on uninstall. Verified live on real GD.
- **Hook chaining.** Two mods hooking the same function don't conflict — they run in order, then the original runs. This was a real "two inline hooks fight over one function" problem and it's solved.
- **External mods.** Drop a `.pulse` file in the mods folder, the loader discovers it, checks compatibility, loads it, and its hooks land in the right place.
- **A typed C++ SDK.** You write hooks against generated, type-checked headers. If your hook's signature is wrong, it fails at *compile* time, not with a mystery crash.
- **The binding extractor.** The new big piece — see below.

## The bindings story (the current focus)

GD doesn't tell you where its functions are on macOS or Windows (the binaries are stripped). Bindings are the map. We built a tool that derives that map from GD's *own* binaries instead of copying anyone else's work:

- **Android** is the key — its build keeps all the function names and signatures. We pull ~15,000 of them straight out, with their Android addresses.
- For **macOS/Windows**, we rebuild the C++ vtables from the binary and match function ordering to figure out where the same functions live there.
- Every offset gets a reality check (we disassemble the function start) and nothing is marked "verified" unless it passes.

**Real result on GD 2.2081:** ~15,000 Android bindings, plus ~3,800 macOS bindings cross-derived (~3,600 verified). That's virtual methods across ~925 classes. We hand-checked samples against the actual game and they're correct.

## What's not done (no spin)

- **Non-virtual functions on macOS/Windows.** Virtual methods we can place; regular functions can't be auto-located on the symbol-less platforms yet. This is most of the game's API and it's the hard, unsolved part. It's where the community could make the biggest dent (see CONTRIBUTING).
- **Windows.** The code path is there; nobody's run it on a real Windows binary yet.
- **iOS.** Not wired up, and iOS binaries are encrypted, so it needs extra work plus a decrypted binary.
- **Coverage ≠ Geode.** Geode has years of community RE behind it. We have a strong automated head start and a clear path, not parity. Yet.

## How you can move the needle

Short version: **help with bindings.** Run the extractor on a platform we're missing, verify offsets, or — if you're a reverse-engineering person — help crack the non-virtual cross-platform problem. Full details in [`CONTRIBUTING.md`](../CONTRIBUTING.md).

## Roadmap, roughly

1. Persist the macOS virtual bindings into the catalog and get example mods using them.
2. Run Windows; add iOS support.
3. Make "re-derive everything for a new GD version" a near one-command operation.
4. Tackle non-virtual cross-platform matching — the real road to broad coverage.

No promises on dates. It's done when the community gets it done.
