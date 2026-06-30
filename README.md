# Pulse SDK

**Restarting from the beat. Built for devs, made for players, here for everyone.**

Pulse is an open modding SDK and loader for Geometry Dash. Think of it as a place to write GD mods in modern C++, hook the game's functions, and ship them — without the whole thing being a black box you can't read or fix.

It started Mac-first (Apple Silicon, the platform that's always last to get love) and it's growing outward from there. It's young, it's honest about what it can't do yet, and it's wide open for help.

---

## Wait, isn't Geode already a thing?

Yeah, and Geode is great. We're not here to trash it — half of what we know we learned by staring at how the community solved these problems over the years.

Pulse exists because we wanted a loader where every layer is something you can actually open up and understand: the injector, the hook engine, the hook *chaining* (multiple mods on the same function, in order, no fighting), the package format, and — the part this repo is really about right now — **how the bindings get made**.

If you don't know what "bindings" means, hang on, it's the whole story below.

---

## The bindings problem (a.k.a. why this is hard)

GD ships as a compiled binary with the names stripped out. The game is full of functions like `PlayLayer::update` or `MenuLayer::init`, but on macOS and Windows the binary doesn't tell you *where* any of them are. No symbols, no map, nothing. Just megabytes of machine code.

A "binding" is the bridge: a symbol name + its type signature + the actual address (offset) where that function lives in a specific version of the game, on a specific platform. Without bindings, a mod can't hook anything. With them, you can do basically whatever you want.

Geode has thousands of these, built up over years of community reverse-engineering. We started with **one** (`MenuLayer::init`, hand-verified on real GD). The big question was: how do we get from 1 to thousands without just copying someone else's work?

---

## What we actually built

The answer turned out to be: **GD's own Android build is a goldmine, and the binary tells you the truth if you know how to ask.**

The Android version of GD ships with its symbol table intact — class names, method names, full type signatures, the lot. So we wrote an extractor that:

1. reads the Android library and recovers ~15,000 function names + signatures + their Android offsets, straight from the binary (no Geode, no guessing);
2. rebuilds the C++ vtables from the binary to learn the *order* of each class's virtual methods;
3. matches that ordering against the macOS (and Windows) binary's own vtables to figure out where those same functions live on *those* platforms;
4. sanity-checks every single offset by disassembling the function start, and refuses to mark anything "verified" unless it actually looks like a real function.

Everything is **fail-closed**: if it can't prove an offset, it doesn't write a bad one. No guesses that crash your game.

### Where we are right now (real numbers, on real GD 2.2081)

- **~15,000 Android bindings** — names, signatures, offsets. Done.
- **~3,800 macOS bindings cross-derived, ~3,600 verified** — these are the virtual methods across ~925 classes (MenuLayer, PlayLayer, GameManager, CCNode, all the big ones). Spot-checked by hand against the real binary — e.g. we confirmed `CCNode::visit` lands exactly on its `if (!visible) return;` opening. It's real.
- The loader, the Dobby-based hook engine, hook chaining, and external `.pulse` mod loading are all working and tested on real GD.

So: we went from **1 binding to thousands**, automatically, from first principles. That's the headline.

### Where we are NOT (the honest part)

- **Non-virtual functions on macOS/Windows are the gap.** Virtual methods we can place via vtables. Regular functions (most of the game!) have no vtable slot, so on the symbol-less platforms we can't auto-locate them yet. This is the exact thing that took the wider community *years*, and it's where we need the most help (see below).
- **Windows**: the code path exists, we just need someone to run it against a Windows GD binary.
- **iOS**: not wired up yet, and iOS binaries are encrypted, so it needs a bit more work + a decrypted binary.
- We do **not** ship GD binaries, and you shouldn't either. You bring your own legally-owned copy to run the extractor.

---

## This is where you come in 🫵

This is the ask. Pulse only becomes a real Geode-level thing if the bindings keep growing — and that's bigger than any one person.

**We need help with:**

- **Non-virtual cross-platform bindings.** This is the big one. If you're into reverse engineering (IDA, Ghidra, BinDiff, signature matching), help us figure out how to locate non-virtual functions on macOS/Windows starting from the Android symbols we already have. Even ideas and prototypes are gold.
- **Running the extractor on Windows and iOS** and reporting what comes out.
- **Verifying offsets.** We have thousands marked "looks like a function" — confirming them against the real game, flagging the wrong ones, is hugely valuable.
- **New GD versions.** Every time RobTop ships an update (major *or* minor), the offsets shift. We want the pipeline to make re-deriving bindings for a new version as close to one-command as possible. Help us get there.
- **Mods.** Build something. Break it. Tell us what hurt.

If any of that sounds like your kind of fun, open an issue, start a discussion, or just send a PR. Seriously — no contribution is too small, including "your docs are confusing here."

---

## Quick tour of the repo

- `loader/` — the runtime: injection, the hook engine (Dobby), hook chaining, mod loading.
- `sdk/` — the C++ headers mods are written against (`PULSE_HOOK`, the generated GD API surface, etc).
- `cli/` — the `pulse` command-line tool: scaffold/build/publish mods, and the binding extractor (`pulse bindings extract`).
- `mod-index/catalog/` — the bindings themselves, one file per symbol. This is the dataset we want to grow.
- `examples/` — small example mods.
- `docs/` — deeper write-ups, including the blow-by-blow of how bindings get verified on real hardware.

Start with [`docs/STATUS.md`](docs/STATUS.md) for the plain-English "where's the project at" and [`CONTRIBUTING.md`](CONTRIBUTING.md) for how to jump in.

---

## A note on legality & respect

Pulse derives bindings from GD binaries **you own**. We don't redistribute RobTop's code or game binaries — not the `.so`, not the `.app`, nothing. The catalog holds *offsets and signatures* (facts about the game), the same kind of community data modding scenes have shared for ages. Be cool, own your copy, don't pirate the game. RobTop made something we all clearly love enough to spend our nights modding.

---

## License

See [`LICENSE`](LICENSE). The code is open. The bindings catalog is community data — use it, grow it, share it back.

---

*Pulse is a fan project. Not affiliated with RobTop Games. Geometry Dash is RobTop's.*
