// tests/mod_loader_wiring_test.cpp — unit test del cablaggio del ModManager
// (task 7.4, Requisiti 5.4, 5.5, 5.6, 5.7, 9.1, 9.4).
//
// Verifica `ModManagerWiring`: per ogni Mod_Id registra nel `ModManager` un
// `EntryPointFn` che apre l'epoca, carica il Mod_Module, risolve+invoca l'entry
// point UNA volta, esegue `resolve_all` e installa i soli hook risolti della
// finestra via `HookGate` (persistendo i byte originali con owner=Mod_Id prima
// dell'install, cablando il trampolino e attribuendo l'hook); e un
// `TerminatorFn` che rimuove i soli hook del Mod_Id. Nessun install su indirizzi
// non risolti, con diagnostica attribuita (Mod_Id + simbolo).
//
// Host-testabile sui seam: FakeModuleLoader (registro di simboli/byte senza
// dlopen reale), FakeBackend (memoria simulata delle funzioni bersaglio,
// readOriginal/install/remove byte-esatti), resolver e invocatore dell'entry
// point iniettati. Le registrazioni PULSE_HOOK del Mod_Module sono modellate
// nell'invocatore dell'entry point (cadono nella finestra di epoca del mod).

#include "lifecycle/mod_loader.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include <pulse/hooks.hpp>

#include "core/runtime_context.hpp"
#include "hooking/fake_backend.hpp"
#include "hooking/rollback_store.hpp"
#include "lifecycle/hook_ownership.hpp"
#include "lifecycle/mod_manager.hpp"
#include "lifecycle/module_loader.hpp"

namespace {

using pulse::hooking::FakeBackend;
using pulse::hooking::RollbackStore;
using pulse::lifecycle::Bytes;
using pulse::lifecycle::EntryPointOutcome;
using pulse::lifecycle::FakeModuleLoader;
using pulse::lifecycle::HookOwnershipLedger;
using pulse::lifecycle::ModId;
using pulse::lifecycle::ModManager;
using pulse::lifecycle::ModManagerWiring;
using pulse::lifecycle::ModWiringSpec;
using pulse::lifecycle::OwnedHook;

// Azzera il registro globale dello SDK: è un singleton di processo condiviso
// tra le unità di traduzione, va riportato a uno stato noto a ogni test.
void resetRegistry() { pulse::hooks::registry().clear(); }

// Cattura le diagnostiche emesse dal cablaggio.
struct CapturingSink {
    std::vector<std::string> messages;
    pulse::loader::DiagnosticSink sink() {
        return [this](std::string_view m) { messages.emplace_back(m); };
    }
    bool anyContains(std::string_view a) const {
        for (const std::string& m : messages)
            if (m.find(a) != std::string::npos) return true;
        return false;
    }
    bool anyContains(std::string_view a, std::string_view b) const {
        for (const std::string& m : messages)
            if (m.find(a) != std::string::npos && m.find(b) != std::string::npos)
                return true;
        return false;
    }
};

// Runtime_Context fittizio (macOS arm64, GD 2.2081).
pulse::loader::RuntimeContext makeCtx() {
    pulse::loader::RuntimeContext ctx;
    ctx.gdVersion = pulse::loader::GdVersion{2, 2081};
    ctx.platform = pulse::loader::Platform::MacOS;
    ctx.platformId = "macos-arm64";
    return ctx;
}

// Percorso temporaneo unico per il RollbackStore di un test.
std::filesystem::path tempRollbackPath(const std::string& name) {
    return std::filesystem::path(::testing::TempDir()) /
           ("pulse_wiring_" + name + ".rbk");
}

// Banco di prova condiviso: registro di hook simulati per ogni mod (modello
// delle registrazioni PULSE_HOOK del Mod_Module), resolver dei simboli e
// invocatore dell'entry point che le inserisce nella finestra di epoca.
struct Harness {
    FakeModuleLoader moduleLoader;
    FakeBackend backend;
    HookOwnershipLedger ledger;
    CapturingSink sink;
    RollbackStore rollback;

    // simbolo → indirizzo risolto (nullptr = non risolvibile).
    std::unordered_map<std::string, std::uintptr_t> resolved;
    // modId → simboli che il modulo "registra" all'enable (PULSE_HOOK).
    std::unordered_map<ModId, std::vector<std::string>> hooksByMod;
    // numero di invocazioni dell'entry point per mod (oracolo indipendente).
    std::unordered_map<ModId, int> invocations;
    // slot dei trampolini, indirizzi stabili per bind_trampoline.
    std::deque<void*> trampSlots;
    std::deque<int> detourStorage;

    explicit Harness(const std::string& name)
        : rollback(tempRollbackPath(name)) {}

    // Programma una mod nel FakeModuleLoader con un entry point risolvibile e
    // i byte del modulo distinti; registra i suoi hook (simboli) e quali sono
    // risolvibili.
    void programMod(const ModId& modId, const std::string& entrySymbol,
                    const Bytes& image, bool entryResolvable,
                    const std::vector<std::pair<std::string, bool>>& hooks) {
        FakeModuleLoader::ModuleSpec spec;
        if (entryResolvable) {
            spec.exports.push_back({entrySymbol, {0x90, 0x90}});
        }
        moduleLoader.program(image, spec);

        for (const auto& [sym, resolvable] : hooks) {
            hooksByMod[modId].push_back(sym);
            // indirizzo fittizio stabile e non nullo per i simboli risolvibili.
            resolved[sym] = resolvable
                                ? (0x4000 + static_cast<std::uintptr_t>(
                                               std::hash<std::string>{}(sym) &
                                               0xFFFF))
                                : 0;
        }
    }

    ModManagerWiring::SymbolResolver makeResolver() {
        return [this](std::string_view symbol) -> void* {
            auto it = resolved.find(std::string(symbol));
            if (it == resolved.end() || it->second == 0) return nullptr;
            return reinterpret_cast<void*>(it->second);
        };
    }

    // Invocatore dell'entry point: conta l'invocazione e registra i PULSE_HOOK
    // del modulo nel registro globale (cadono nella finestra di epoca del mod).
    ModManagerWiring::EntryPointInvoker makeInvoker(bool fail = false) {
        return [this, fail](const ModId& modId, void* entry) -> EntryPointOutcome {
            (void)entry;
            invocations[modId] += 1;
            if (fail) {
                return EntryPointOutcome::failure("entry point fallito (test)");
            }
            for (const std::string& sym : hooksByMod[modId]) {
                detourStorage.push_back(0);
                trampSlots.push_back(nullptr);
                void* detour = static_cast<void*>(&detourStorage.back());
                void** tramp = &trampSlots.back();
                pulse::hooks::register_hook(sym, detour, tramp);
            }
            return EntryPointOutcome::success();
        };
    }
};

// --- Req 5.4 / 5.6 / 5.8: enable installa e attribuisce gli hook risolti ------

TEST(ModLoaderWiring, EnableInstallsAttributedResolvedHooks) {
    resetRegistry();
    Harness h("install_attributed");

    const ModId mod = "mod.a";
    h.programMod(mod, "mod_init", Bytes{0x01}, /*entryResolvable=*/true,
                 {{"A::f", true}, {"A::g", true}});

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), h.makeInvoker(),
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{mod, Bytes{0x01}, "mod_init"});

    auto result = manager.enableAll({mod});
    ASSERT_EQ(result.enabled.size(), 1u);
    EXPECT_EQ(manager.stateOf(mod), pulse::lifecycle::ModState::Enabled);

    // Entry point invocato esattamente una volta (Req 5.4).
    EXPECT_EQ(wiring.entryPointInvocations(mod), 1);
    EXPECT_EQ(h.invocations[mod], 1);

    // Due hook risolti installati e attribuiti al Mod_Id (Req 5.6, 5.8).
    const std::vector<OwnedHook> owned = h.ledger.hooksOf(mod);
    ASSERT_EQ(owned.size(), 2u);
    EXPECT_EQ(h.backend.installedCount(), 2u);
    for (const OwnedHook& hook : owned) {
        EXPECT_EQ(hook.owner, mod);
        EXPECT_TRUE(h.backend.isInstalled(hook.target));
    }

    // Byte originali persistiti nel RollbackStore con owner=Mod_Id PRIMA
    // dell'install (Req 8.3, 9.5).
    ASSERT_EQ(h.rollback.records().size(), 2u);
    for (const auto& rec : h.rollback.records()) {
        EXPECT_EQ(rec.owner, mod);
        EXPECT_FALSE(rec.originalBytes.empty());
        EXPECT_EQ(rec.platformId, "macos-arm64");
    }
}

// --- Req 5.7 / 9.1 / 9.4: nessun install su indirizzi non risolti -------------

TEST(ModLoaderWiring, UnresolvedAddressesAreNotInstalledWithDiagnostic) {
    resetRegistry();
    Harness h("unresolved_no_install");

    const ModId mod = "mod.mix";
    h.programMod(mod, "mod_init", Bytes{0x02}, /*entryResolvable=*/true,
                 {{"M::resolved", true}, {"M::missing", false}});

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), h.makeInvoker(),
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{mod, Bytes{0x02}, "mod_init"});
    manager.enableAll({mod});

    // Solo l'hook risolto è installato/attribuito; l'indirizzo non risolto no.
    const std::vector<OwnedHook> owned = h.ledger.hooksOf(mod);
    ASSERT_EQ(owned.size(), 1u);
    EXPECT_EQ(owned[0].symbol, "M::resolved");
    EXPECT_EQ(h.backend.installedCount(), 1u);

    // Diagnostica attribuita con Mod_Id + simbolo per l'indirizzo non risolto.
    EXPECT_TRUE(h.sink.anyContains(mod, "M::missing"));
    // Nessun record di rollback per l'hook non installato.
    ASSERT_EQ(h.rollback.records().size(), 1u);
    EXPECT_EQ(h.rollback.records()[0].symbol, "M::resolved");
}

// --- Req 5.3: entry point non risolvibile → nessuna invocazione, isolato ------

TEST(ModLoaderWiring, UnresolvableEntryPointDisablesWithoutInstall) {
    resetRegistry();
    Harness h("entry_unresolvable");

    const ModId mod = "mod.noentry";
    // entry point dichiarato NON esportato dal modulo (entryResolvable=false).
    h.programMod(mod, "missing_init", Bytes{0x03}, /*entryResolvable=*/false,
                 {{"X::f", true}});

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), h.makeInvoker(),
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{mod, Bytes{0x03}, "missing_init"});
    auto result = manager.enableAll({mod});

    // Fallimento isolato: la mod va a Disabled, nessuna invocazione, 0 hook.
    EXPECT_EQ(manager.stateOf(mod), pulse::lifecycle::ModState::Disabled);
    ASSERT_EQ(result.failed.size(), 1u);
    EXPECT_EQ(wiring.entryPointInvocations(mod), 0);
    EXPECT_TRUE(h.ledger.hooksOf(mod).empty());
    EXPECT_EQ(h.backend.installedCount(), 0u);
    EXPECT_TRUE(h.sink.anyContains(mod, "missing_init"));
}

// --- Req: modulo non caricabile → fallimento isolato, 0 hook ------------------

TEST(ModLoaderWiring, ModuleNotLoadableIsolatedNoHooks) {
    resetRegistry();
    Harness h("module_not_loadable");

    const ModId mod = "mod.badmodule";
    const Bytes image{0x04};
    FakeModuleLoader::ModuleSpec spec;
    spec.failLoad = true;  // load simulata fallita
    h.moduleLoader.program(image, spec);

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), h.makeInvoker(),
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{mod, image, "mod_init"});
    auto result = manager.enableAll({mod});

    EXPECT_EQ(manager.stateOf(mod), pulse::lifecycle::ModState::Disabled);
    ASSERT_EQ(result.failed.size(), 1u);
    EXPECT_EQ(wiring.entryPointInvocations(mod), 0);
    EXPECT_FALSE(wiring.moduleLoaded(mod));
    EXPECT_EQ(h.backend.installedCount(), 0u);
    EXPECT_TRUE(h.sink.anyContains(mod));
}

// --- Req 7.3: il terminator rimuove i SOLI hook del Mod_Id --------------------

TEST(ModLoaderWiring, TerminatorRemovesOnlyOwnersHooks) {
    resetRegistry();
    Harness h("terminator_isolation");

    const ModId a = "mod.a";
    const ModId b = "mod.b";
    h.programMod(a, "a_init", Bytes{0x10}, true, {{"A::f", true}});
    h.programMod(b, "b_init", Bytes{0x11}, true, {{"B::g", true}, {"B::h", true}});

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), h.makeInvoker(),
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{a, Bytes{0x10}, "a_init"});
    wiring.registerMod(manager, ModWiringSpec{b, Bytes{0x11}, "b_init"});
    manager.enableAll({a, b});

    ASSERT_EQ(h.ledger.hooksOf(a).size(), 1u);
    ASSERT_EQ(h.ledger.hooksOf(b).size(), 2u);
    EXPECT_EQ(h.backend.installedCount(), 3u);

    // shutdown invoca i terminator (ordine inverso): rimuove gli hook di a e b.
    manager.shutdown({a, b});

    // Tutti gli hook di entrambe le mod rimossi (byte-esatto) e rilasciati.
    EXPECT_TRUE(h.ledger.hooksOf(a).empty());
    EXPECT_TRUE(h.ledger.hooksOf(b).empty());
    EXPECT_EQ(h.backend.installedCount(), 0u);
}

// --- finestra di epoca registrata e modulo caricato ---------------------------

TEST(ModLoaderWiring, EpochWindowRecordedAndModuleLoaded) {
    resetRegistry();
    Harness h("epoch_window");

    const ModId mod = "mod.win";
    h.programMod(mod, "w_init", Bytes{0x20}, true,
                 {{"W::a", true}, {"W::b", true}, {"W::c", true}});

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), h.makeInvoker(),
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{mod, Bytes{0x20}, "w_init"});
    manager.enableAll({mod});

    EXPECT_TRUE(wiring.moduleLoaded(mod));
    const auto [start, end] = wiring.epochWindow(mod);
    EXPECT_EQ(start, 0u);
    EXPECT_EQ(end, 3u);  // tre PULSE_HOOK registrati nella finestra del mod
    // Tutti attribuiti al Mod_Id (Req 5.8, 9.6).
    EXPECT_EQ(h.ledger.hooksOf(mod).size(), 3u);
    EXPECT_EQ(h.ledger.allInstalled().size(), 3u);
}

}  // namespace
