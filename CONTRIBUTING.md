# Contributing to Pulse

First off — thanks. Pulse is the kind of project that genuinely lives or dies on people showing up, so the fact that you're reading this already means a lot.

This isn't a corporate CONTRIBUTING.md with seventeen required checkboxes. Here's the real deal on how to help.

## The single most useful thing you can do: bindings

If you take nothing else from this page: **the bindings are the bottleneck, and the bindings are how you help most.**

Quick recap of the situation (the README has the full story):

- We auto-derive bindings from GD's own binaries. ~15k from Android, ~3.8k cross-derived to macOS so far.
- **Virtual methods**: handled (we match them through C++ vtables).
- **Non-virtual functions on macOS/Windows**: *not* handled. No symbols, no vtable slot — we can't auto-place them yet. This is the frontier.

### Ways in, from "I have an afternoon" to "this is my thing now"

**Run the extractor on a platform we're missing.**
You'll need a legally-owned GD binary. We don't ship them and neither should you.

```bash
# build the CLI
cargo build --release --manifest-path cli/Cargo.toml

# Android (the master — gives names + signatures + Android offsets)
./cli/target/release/pulse bindings extract \
  --gd 2.2081 --platform android-arm64 \
  --elf /path/to/libcocos2dcpp.so \
  --catalog-root mod-index/catalog

# macOS (cross-derives virtual offsets from the Android ordering)
./cli/target/release/pulse bindings extract \
  --gd 2.2081 --platform macos-arm64 \
  --elf /path/to/libcocos2dcpp.so \
  --macho /path/to/"Geometry Dash"-arm64-slice \
  --catalog-root mod-index/catalog

# Windows — same idea, with --pe instead of --macho. Nobody's run this on a
# real Windows binary yet. Be the first and tell us what happens.
```

Then open a PR with the new/updated catalog files, or just an issue with what you saw.

**Verify offsets.** We've got thousands flagged "looks like a real function." Confirming them against the actual game (disassemble, check it's the right thing) and flagging the wrong ones is enormously valuable and doesn't require writing any Rust.

**Crack the non-virtual problem.** If you live in IDA/Ghidra/BinDiff, this is the holy grail. The idea: given a function we've already identified on Android (name, signature, behavior), find the *same* function on a symbol-less macOS/Windows binary — via string cross-references, call-graph shape, constants, whatever works. Prototypes, scripts, half-baked ideas, all welcome. This is literally how the broader scene built its datasets; we want a more automated version of it.

**Keep us alive across GD versions.** When RobTop ships an update, every offset moves. We want re-deriving the whole catalog for a new version (major *or* minor) to be near one command. If you hit friction doing that, that friction is a contribution waiting to be filed.

## Code contributions

- Rust for the CLI / extractor (`cli/`), C++20/23 for the loader and SDK (`loader/`, `sdk/`).
- On macOS, builds need `env -u LDFLAGS` in front of `cargo`/`cmake` (a system-wide linker flag otherwise breaks AppleClang — yeah, we know, it's annoying).
- There's a real test suite. Run it before you PR: `env -u LDFLAGS cargo test --manifest-path cli/Cargo.toml`.
- The whole project is built fail-closed: when in doubt, don't emit a wrong binding. A missing binding is a TODO; a wrong binding is a crash in someone's game. Keep that spirit.

## How to actually contribute

1. Open an issue or a discussion first if it's a big thing — saves everyone time.
2. Fork, branch, PR. Describe what you did and how you checked it.
3. If you touched bindings, say which version/platform and how you verified.
4. Be kind in reviews. We're all here on our own time because we like this dumb little cube game.

## Ground rules

- Don't commit GD binaries or any copyrighted RobTop content. Ever. (The `.gitignore` already blocks the obvious ones, but don't fight it.)
- Don't pirate the game to contribute. Own your copy.
- Be decent to each other. That's it.

Got a question? Open a discussion. Welcome aboard. 🟩
