// tests/mod_loader_teardown_test.cpp — unit test del teardown pulito alla
// chiusura del gioco (task 7.17, Requisiti 8.1, 8.2, 8.3, 8.4, 8.5, 8.6, 8.7,
// 8.8).
//
// Verifica `ModManagerWiring::teardown(manager, order)` costruito sopra
// `ModManager::shutdown`:
//   * invoca il terminator di ogni mod Enabled ESATTAMENTE una volta
//     nell'ordine ESATTAMENTE inverso rispetto a `order` (Req 8.1);
//   * rimuove TUTTI gli hook di ogni Mod_Id byte-esatto via RollbackStore/backend
//     (Req 8.2, 8.3, 8.4);
//   * isola il fallimento di rimozione di un hook (terminator che "fallisce"),
//     rimuove comunque gli hook del Mod_Id e prosegue con le restanti (Req 8.5);
//   * a hook rimossi scarica il Mod_Module (Req 8.7) e porta la mod a Removed
//     (Req 8.8);
//   * al termine ZERO hook attribuiti ad alcuna mod restano installati (Req 8.6).
//
// Host-testabile sui seam: FakeModuleLoader (conta gli unload, nessun dlclose
// reale), FakeBackend (memoria simulata byte-esatta + iniezione di fallimenti),
// resolver e invocatore dell'entry point iniettati. Le registrazioni PULSE_HOOK
// del Mod_Module sono modellate UNA sola volta per mod nell'invocatore.

#include "lifecycle/mod_loader.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
using pulse::lifecycle::ModState;
using pulse::lifecycle::ModWiringSpec;
using pulse::lifecycle::OwnedHook;

// Azzera il registro globale dello SDK (singleton di processo condiviso).
void resetRegistry() { pulse::hooks::registry().clear(); }

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

pulse::loader::RuntimeContext makeCtx() {
    pulse::loader::RuntimeContext ctx;
    ctx.gdVersion = pulse::loader::GdVersion{2, 2081};
    ctx.platform = pulse::loader::Platform::MacOS;
    ctx.platformId = "macos-arm64";
    return ctx;
}

std::filesystem::path tempRollbackPath(const std::string& name) {
    return std::filesystem::path(::testing::TempDir()) /
           ("pulse_teardown_" + name + ".rbk");
}

// Banco di prova: l'invocatore dell'entry point registra i PULSE_HOOK del
// modulo UNA sola volta per mod (modella lo static-init al dlopen). Tutti gli
// indirizzi degli hook sono risolvibili.
struct Harness {
    FakeModuleLoader moduleLoader;
    FakeBackend backend;
    HookOwnershipLedger ledger;
    CapturingSink sink;
    RollbackStore rollback;

    std::unordered_map<std::string, std::uintptr_t> resolved;
    std::unordered_map<ModId, std::vector<std::string>> hooksByMod;
    std::unordered_set<ModId> registeredOnce;
    std::deque<int> detourStorage;
    std::deque<void*> trampSlots;

    explicit Harness(const std::string& name)
        : rollback(tempRollbackPath(name)) {}

    void programMod(const ModId& modId, const std::string& entrySymbol,
                    const Bytes& image,
                    const std::vector<std::string>& hooks) {
        FakeModuleLoader::ModuleSpec spec;
        spec.exports.push_back({entrySymbol, {0x90, 0x90}});
        moduleLoader.program(image, spec);

        for (const std::string& sym : hooks) {
            hooksByMod[modId].push_back(sym);
            resolved[sym] = 0x4000 + static_cast<std::uintptr_t>(
                                         std::hash<std::string>{}(sym) & 0xFFFF);
        }
    }

    [[nodiscard]] std::uintptr_t targetOf(const std::string& symbol) const {
        const auto it = resolved.find(symbol);
        return it == resolved.end() ? 0u : it->second;
    }

    ModManagerWiring::SymbolResolver makeResolver() {
        return [this](std::string_view symbol) -> void* {
            auto it = resolved.find(std::string(symbol));
            if (it == resolved.end() || it->second == 0) return nullptr;
            return reinterpret_cast<void*>(it->second);
        };
    }

    ModManagerWiring::EntryPointInvoker makeInvoker() {
        return [this](const ModId& modId, void* entry) -> EntryPointOutcome {
            (void)entry;
            if (registeredOnce.insert(modId).second) {
                for (const std::string& sym : hooksByMod[modId]) {
                    detourStorage.push_back(0);
                    trampSlots.push_back(nullptr);
                    void* detour = static_cast<void*>(&detourStorage.back());
                    void** tramp = &trampSlots.back();
                    pulse::hooks::register_hook(sym, detour, tramp);
                }
            }
            return EntryPointOutcome::success();
        };
    }
};

// --- Req 8.1/8.7/8.8/8.6: teardown in ordine inverso, unload, Removed, zero ---

TEST(ModLoaderTeardown, ReverseOrderUnloadRemovedZeroHooks) {
    resetRegistry();
    Harness h("reverse_order");

    const std::vector<ModId> order = {"mod.a", "mod.b", "mod.c"};
    h.programMod("mod.a", "a_init", Bytes{0x01}, {"A::f", "A::g"});
    h.programMod("mod.b", "b_init", Bytes{0x02}, {"B::f"});
    h.programMod("mod.c", "c_init", Bytes{0x03}, {"C::f", "C::g"});

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), h.makeInvoker(),
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{"mod.a", Bytes{0x01}, "a_init"});
    wiring.registerMod(manager, ModWiringSpec{"mod.b", Bytes{0x02}, "b_init"});
    wiring.registerMod(manager, ModWiringSpec{"mod.c", Bytes{0x03}, "c_init"});

    for (const ModId& id : order) ASSERT_TRUE(wiring.enable(manager, id).applied());
    ASSERT_EQ(h.ledger.installedCount(), 5u);  // 2 + 1 + 2 hook attribuiti
    ASSERT_EQ(h.backend.installedCount(), 5u);

    const std::vector<ModId> torn = wiring.teardown(manager, order);

    // (Req 8.1) terminator invocati ESATTAMENTE una volta in ordine INVERSO.
    EXPECT_EQ(torn, (std::vector<ModId>{"mod.c", "mod.b", "mod.a"}));

    // (Req 8.6) zero hook attribuiti ad alcuna mod restano installati.
    EXPECT_EQ(h.ledger.installedCount(), 0u);
    // (Req 8.2/8.3/8.4) hook rimossi byte-esatto dal backend.
    EXPECT_EQ(h.backend.installedCount(), 0u);

    // (Req 8.7) ogni Mod_Module scaricato; (Req 8.8) ogni mod a Removed.
    EXPECT_EQ(h.moduleLoader.unloadCount(), 3u);
    for (const ModId& id : order) {
        EXPECT_EQ(manager.stateOf(id), ModState::Removed) << id;
        EXPECT_FALSE(wiring.moduleLoaded(id)) << id;
        EXPECT_TRUE(h.ledger.hooksOf(id).empty()) << id;
    }
}

// --- Req 8.3/8.4: ripristino byte-esatto (round-trip) al teardown -------------

TEST(ModLoaderTeardown, ByteExactRestoreRoundTrip) {
    resetRegistry();
    Harness h("byte_exact");

    const std::vector<ModId> order = {"mod.x"};
    h.programMod("mod.x", "x_init", Bytes{0x07}, {"X::f", "X::g"});

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), h.makeInvoker(),
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{"mod.x", Bytes{0x07}, "x_init"});

    // Snapshot dei byte originali pre-installazione delle funzioni bersaglio.
    const std::uintptr_t tf = h.targetOf("X::f");
    const std::uintptr_t tg = h.targetOf("X::g");
    const auto origF = h.backend.readOriginal(tf, 16).value().bytes();
    const auto origG = h.backend.readOriginal(tg, 16).value().bytes();

    ASSERT_TRUE(wiring.enable(manager, "mod.x").applied());
    // Installati: i byte live differiscono dagli originali.
    ASSERT_TRUE(h.backend.isInstalled(tf));
    ASSERT_NE(h.backend.liveBytes(tf).value(), origF);

    wiring.teardown(manager, order);

    // (Req 8.3/8.4) i byte tornano IDENTICI al pre-installazione (round-trip).
    EXPECT_FALSE(h.backend.isInstalled(tf));
    EXPECT_FALSE(h.backend.isInstalled(tg));
    EXPECT_EQ(h.backend.liveBytes(tf).value(), origF);
    EXPECT_EQ(h.backend.liveBytes(tg).value(), origG);
    EXPECT_EQ(manager.stateOf("mod.x"), ModState::Removed);
}

// --- Req 8.5: fallimento del terminator isolato, hook comunque rimossi --------

TEST(ModLoaderTeardown, TerminatorFailureIsolatedHooksStillRemoved) {
    resetRegistry();
    Harness h("terminator_failure");

    const std::vector<ModId> order = {"mod.a", "mod.b", "mod.c"};
    h.programMod("mod.a", "a_init", Bytes{0x01}, {"A::f"});
    h.programMod("mod.b", "b_init", Bytes{0x02}, {"B::f", "B::g"});
    h.programMod("mod.c", "c_init", Bytes{0x03}, {"C::f"});

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), h.makeInvoker(),
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{"mod.a", Bytes{0x01}, "a_init"});
    wiring.registerMod(manager, ModWiringSpec{"mod.b", Bytes{0x02}, "b_init"});
    wiring.registerMod(manager, ModWiringSpec{"mod.c", Bytes{0x03}, "c_init"});

    for (const ModId& id : order) ASSERT_TRUE(wiring.enable(manager, id).applied());
    ASSERT_EQ(h.ledger.installedCount(), 4u);

    // Inietta il fallimento di rimozione di un hook di mod.b (terminator che
    // "fallisce" durante il teardown): il backend rifiuta il remove di B::f.
    h.backend.failRemoveAt(h.targetOf("B::f"));

    const std::vector<ModId> torn = wiring.teardown(manager, order);

    // (Req 8.1) ordine inverso comunque rispettato, terminator invocati una volta.
    EXPECT_EQ(torn, (std::vector<ModId>{"mod.c", "mod.b", "mod.a"}));

    // (Req 8.5) il fallimento è isolato e registrato: l'attribuzione di TUTTI
    // gli hook del Mod_Id è comunque rilasciata (zero hook attribuiti restano),
    // e le altre mod sono comunque scaricate fino a Removed (proseguimento).
    EXPECT_EQ(h.ledger.installedCount(), 0u);  // (Req 8.6)
    for (const ModId& id : order) {
        EXPECT_TRUE(h.ledger.hooksOf(id).empty()) << id;
        EXPECT_EQ(manager.stateOf(id), ModState::Removed) << id;
    }
    EXPECT_EQ(h.moduleLoader.unloadCount(), 3u);  // tutte scaricate (Req 8.7)
    EXPECT_TRUE(h.sink.anyContains("B::f"));       // causa registrata
}

// --- Req 8.1: solo le mod Enabled, terminator non re-invocato dopo Removed ----

TEST(ModLoaderTeardown, OnlyEnabledModsTornDownDisabledSkipped) {
    resetRegistry();
    Harness h("only_enabled");

    const std::vector<ModId> order = {"mod.a", "mod.b"};
    h.programMod("mod.a", "a_init", Bytes{0x01}, {"A::f"});
    h.programMod("mod.b", "b_init", Bytes{0x02}, {"B::f"});

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), h.makeInvoker(),
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{"mod.a", Bytes{0x01}, "a_init"});
    wiring.registerMod(manager, ModWiringSpec{"mod.b", Bytes{0x02}, "b_init"});

    ASSERT_TRUE(wiring.enable(manager, "mod.a").applied());
    ASSERT_TRUE(wiring.enable(manager, "mod.b").applied());
    // mod.b disabilitata prima del teardown: non è Enabled, niente terminator.
    ASSERT_TRUE(wiring.disable(manager, "mod.b").applied());
    ASSERT_EQ(manager.stateOf("mod.b"), ModState::Disabled);

    const std::vector<ModId> torn = wiring.teardown(manager, order);

    // Solo mod.a (Enabled) è oggetto del terminator nel teardown (Req 8.1).
    EXPECT_EQ(torn, (std::vector<ModId>{"mod.a"}));
    EXPECT_EQ(manager.stateOf("mod.a"), ModState::Removed);
    // mod.b resta Disabled (non processata dal teardown delle Enabled).
    EXPECT_EQ(manager.stateOf("mod.b"), ModState::Disabled);
    // Nessun hook attribuito resta installato in ogni caso (Req 8.6).
    EXPECT_EQ(h.ledger.installedCount(), 0u);
    // Solo il Mod_Module di mod.a è stato scaricato dal teardown (Req 8.7).
    EXPECT_EQ(h.moduleLoader.unloadCount(), 1u);
}

}  // namespace
