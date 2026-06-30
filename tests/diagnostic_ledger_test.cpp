// tests/diagnostic_ledger_test.cpp — unit test del DiagnosticLedger
// (task 7.19, Requisiti 10.1, 10.2, 10.3, 10.4).
//
// Verifica:
//   * Partizione completa a tre vie {Loaded, Excluded, Isolated} (Req 10.4):
//     ogni mod individuata in ESATTAMENTE uno; un Mod_Id non può comparire in
//     due esiti (il secondo record è rifiutato), e ogni Mod_Id caricato compare
//     una sola volta (Req 10.1).
//   * Ogni esito Excluded/Isolated porta ESATTAMENTE una CauseCategory
//     dell'insieme chiuso (Req 10.2); Loaded non ha causa.
//   * partitionComplete()/missingFrom() rispetto al set individuato (Req 10.4).
//   * Gli eventi hook install/remove identificano Mod_Id, simbolo e tipo
//     operazione (Req 10.3), sia direttamente sul ledger sia cablati nei
//     percorsi install/remove di ModManagerWiring (FakeBackend/FakeModuleLoader).

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
using pulse::lifecycle::CauseCategory;
using pulse::lifecycle::DiagnosticEntry;
using pulse::lifecycle::DiagnosticLedger;
using pulse::lifecycle::EntryPointOutcome;
using pulse::lifecycle::FakeModuleLoader;
using pulse::lifecycle::HookEvent;
using pulse::lifecycle::HookOp;
using pulse::lifecycle::HookOwnershipLedger;
using pulse::lifecycle::ModId;
using pulse::lifecycle::ModManager;
using pulse::lifecycle::ModManagerWiring;
using pulse::lifecycle::ModOutcome;
using pulse::lifecycle::ModWiringSpec;

// =============================================================================
// Esiti / partizione (Req 10.1, 10.2, 10.4)
// =============================================================================

TEST(DiagnosticLedger, RecordsEachOutcomeOnceAndQueryable) {
    DiagnosticLedger ledger;

    EXPECT_TRUE(ledger.recordLoaded("mod.a"));
    EXPECT_TRUE(ledger.recordExcluded("mod.b", CauseCategory::DependencyCycle));
    EXPECT_TRUE(
        ledger.recordIsolated("mod.c", CauseCategory::EntryPointFailed));

    EXPECT_EQ(ledger.loaded(), (std::vector<ModId>{"mod.a"}));

    const auto excluded = ledger.excluded();
    ASSERT_EQ(excluded.size(), 1u);
    EXPECT_EQ(excluded[0].modId, "mod.b");
    EXPECT_EQ(excluded[0].outcome, ModOutcome::Excluded);
    ASSERT_TRUE(excluded[0].cause.has_value());
    EXPECT_EQ(*excluded[0].cause, CauseCategory::DependencyCycle);

    const auto isolated = ledger.isolated();
    ASSERT_EQ(isolated.size(), 1u);
    EXPECT_EQ(isolated[0].modId, "mod.c");
    EXPECT_EQ(isolated[0].outcome, ModOutcome::Isolated);
    ASSERT_TRUE(isolated[0].cause.has_value());
    EXPECT_EQ(*isolated[0].cause, CauseCategory::EntryPointFailed);
}

TEST(DiagnosticLedger, LoadedEntryHasNoCause) {
    DiagnosticLedger ledger;
    ASSERT_TRUE(ledger.recordLoaded("mod.a", "ok"));

    const auto& entries = ledger.entries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].outcome, ModOutcome::Loaded);
    EXPECT_FALSE(entries[0].cause.has_value());  // Loaded → nessuna causa (Req 10.1)
}

TEST(DiagnosticLedger, LoadedModIdAppearsExactlyOnce) {
    DiagnosticLedger ledger;
    EXPECT_TRUE(ledger.recordLoaded("mod.a"));
    // Secondo tentativo per lo stesso Mod_Id caricato → rifiutato (Req 10.1).
    EXPECT_FALSE(ledger.recordLoaded("mod.a"));

    EXPECT_EQ(ledger.loaded().size(), 1u);
    EXPECT_EQ(ledger.entries().size(), 1u);
}

TEST(DiagnosticLedger, ModCannotBeInTwoOutcomes) {
    DiagnosticLedger ledger;
    EXPECT_TRUE(ledger.recordLoaded("mod.a"));

    // Tentare un altro esito per lo stesso Mod_Id è rifiutato: la partizione
    // resta disgiunta (Req 10.4), la voce esistente è preservata.
    EXPECT_FALSE(
        ledger.recordExcluded("mod.a", CauseCategory::DependencyUnsatisfied));
    EXPECT_FALSE(
        ledger.recordIsolated("mod.a", CauseCategory::ModuleNotLoadable));

    ASSERT_TRUE(ledger.outcomeOf("mod.a").has_value());
    EXPECT_EQ(*ledger.outcomeOf("mod.a"), ModOutcome::Loaded);
    EXPECT_TRUE(ledger.excluded().empty());
    EXPECT_TRUE(ledger.isolated().empty());
    EXPECT_EQ(ledger.entries().size(), 1u);
}

TEST(DiagnosticLedger, PartitionCompleteWhenEveryDiscoveredHasExactlyOneOutcome) {
    DiagnosticLedger ledger;
    const std::vector<ModId> discovered{"mod.a", "mod.b", "mod.c"};

    ledger.recordLoaded("mod.a");
    ledger.recordExcluded("mod.b", CauseCategory::InvalidPackage);
    ledger.recordIsolated("mod.c", CauseCategory::SymbolUnresolved);

    EXPECT_TRUE(ledger.partitionComplete(discovered));
    EXPECT_TRUE(ledger.missingFrom(discovered).empty());
}

TEST(DiagnosticLedger, PartitionIncompleteWhenAModIsMissing) {
    DiagnosticLedger ledger;
    const std::vector<ModId> discovered{"mod.a", "mod.b", "mod.c"};

    ledger.recordLoaded("mod.a");
    ledger.recordExcluded("mod.b", CauseCategory::InvalidPackage);
    // mod.c non registrata → partizione incompleta.

    EXPECT_FALSE(ledger.partitionComplete(discovered));
    EXPECT_EQ(ledger.missingFrom(discovered), (std::vector<ModId>{"mod.c"}));
}

TEST(DiagnosticLedger, PartitionRejectsOutcomeForUndiscoveredMod) {
    DiagnosticLedger ledger;
    const std::vector<ModId> discovered{"mod.a"};

    ledger.recordLoaded("mod.a");
    ledger.recordExcluded("mod.ghost", CauseCategory::InvalidPackage);

    // Un esito riguarda una mod NON individuata → unione ≠ individuate.
    EXPECT_FALSE(ledger.partitionComplete(discovered));
}

TEST(DiagnosticLedger, RecordEntryNormalizesLoadedCause) {
    DiagnosticLedger ledger;

    DiagnosticEntry loaded;
    loaded.modId = "mod.a";
    loaded.outcome = ModOutcome::Loaded;
    loaded.cause = CauseCategory::InvalidPackage;  // spurio: deve essere azzerato
    EXPECT_TRUE(ledger.record(std::move(loaded)));

    DiagnosticEntry excluded;
    excluded.modId = "mod.b";
    excluded.outcome = ModOutcome::Excluded;
    excluded.cause = CauseCategory::DependencyCycle;
    EXPECT_TRUE(ledger.record(std::move(excluded)));

    const auto& entries = ledger.entries();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].outcome, ModOutcome::Loaded);
    EXPECT_FALSE(entries[0].cause.has_value());  // normalizzata
    EXPECT_EQ(entries[1].outcome, ModOutcome::Excluded);
    ASSERT_TRUE(entries[1].cause.has_value());
    EXPECT_EQ(*entries[1].cause, CauseCategory::DependencyCycle);
}

// =============================================================================
// Eventi hook direttamente sul ledger (Req 10.3)
// =============================================================================

TEST(DiagnosticLedger, HookEventsIdentifyModSymbolAndOperation) {
    DiagnosticLedger ledger;

    ledger.recordHookInstalled("mod.a", "A::f");
    ledger.recordHookInstalled("mod.a", "A::g");
    ledger.recordHookRemoved("mod.a", "A::f");

    const auto& events = ledger.hookEvents();
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0], (HookEvent{"mod.a", "A::f", HookOp::Install}));
    EXPECT_EQ(events[1], (HookEvent{"mod.a", "A::g", HookOp::Install}));
    EXPECT_EQ(events[2], (HookEvent{"mod.a", "A::f", HookOp::Remove}));
}

// =============================================================================
// Eventi hook cablati in ModManagerWiring install/remove (Req 10.3)
// =============================================================================

void resetRegistry() { pulse::hooks::registry().clear(); }

pulse::loader::RuntimeContext makeCtx() {
    pulse::loader::RuntimeContext ctx;
    ctx.gdVersion = pulse::loader::GdVersion{2, 2081};
    ctx.platform = pulse::loader::Platform::MacOS;
    ctx.platformId = "macos-arm64";
    return ctx;
}

std::filesystem::path tempRollbackPath(const std::string& name) {
    return std::filesystem::path(::testing::TempDir()) /
           ("pulse_diagledger_" + name + ".rbk");
}

// Banco di prova: modella le registrazioni PULSE_HOOK del Mod_Module via
// l'invocatore dell'entry point (cadono nella finestra di epoca del mod).
struct Harness {
    FakeModuleLoader moduleLoader;
    FakeBackend backend;
    HookOwnershipLedger ownership;
    RollbackStore rollback;

    std::unordered_map<std::string, std::uintptr_t> resolved;
    std::unordered_map<ModId, std::vector<std::string>> hooksByMod;
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

TEST(DiagnosticLedger, WiringEmitsInstallEventsOnEnable) {
    resetRegistry();
    Harness h("install_events");
    DiagnosticLedger diagnostics;

    const ModId mod = "mod.a";
    h.programMod(mod, "mod_init", Bytes{0x01}, {"A::f", "A::g"});

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ownership,
                            makeCtx(), h.makeResolver(), h.makeInvoker());
    wiring.setDiagnosticLedger(&diagnostics);

    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{mod, Bytes{0x01}, "mod_init"});
    manager.enableAll({mod});

    const auto& events = diagnostics.hookEvents();
    ASSERT_EQ(events.size(), 2u);
    for (const HookEvent& e : events) {
        EXPECT_EQ(e.modId, mod);
        EXPECT_EQ(e.op, HookOp::Install);
        EXPECT_TRUE(e.symbol == "A::f" || e.symbol == "A::g");
    }
}

TEST(DiagnosticLedger, WiringEmitsRemoveEventsOnTeardown) {
    resetRegistry();
    Harness h("remove_events");
    DiagnosticLedger diagnostics;

    const ModId mod = "mod.a";
    h.programMod(mod, "mod_init", Bytes{0x01}, {"A::f", "A::g"});

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ownership,
                            makeCtx(), h.makeResolver(), h.makeInvoker());
    wiring.setDiagnosticLedger(&diagnostics);

    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{mod, Bytes{0x01}, "mod_init"});
    manager.enableAll({mod});
    ASSERT_EQ(diagnostics.hookEvents().size(), 2u);  // 2 install

    // Teardown rimuove gli hook del mod → eventi di remove (Req 10.3).
    wiring.teardown(manager, {mod});

    const auto& events = diagnostics.hookEvents();
    // 2 install + 2 remove.
    ASSERT_EQ(events.size(), 4u);

    int installs = 0, removes = 0;
    for (const HookEvent& e : events) {
        EXPECT_EQ(e.modId, mod);
        EXPECT_TRUE(e.symbol == "A::f" || e.symbol == "A::g");
        if (e.op == HookOp::Install) ++installs;
        if (e.op == HookOp::Remove) ++removes;
    }
    EXPECT_EQ(installs, 2);
    EXPECT_EQ(removes, 2);
}

TEST(DiagnosticLedger, WiringEmitsRemoveEventsOnDisable) {
    resetRegistry();
    Harness h("disable_events");
    DiagnosticLedger diagnostics;

    const ModId mod = "mod.a";
    h.programMod(mod, "mod_init", Bytes{0x01}, {"A::f"});

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ownership,
                            makeCtx(), h.makeResolver(), h.makeInvoker());
    wiring.setDiagnosticLedger(&diagnostics);

    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{mod, Bytes{0x01}, "mod_init"});
    manager.enableAll({mod});
    ASSERT_EQ(diagnostics.hookEvents().size(), 1u);

    wiring.disable(manager, mod);

    const auto& events = diagnostics.hookEvents();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0], (HookEvent{mod, "A::f", HookOp::Install}));
    EXPECT_EQ(events[1], (HookEvent{mod, "A::f", HookOp::Remove}));
}

}  // namespace
