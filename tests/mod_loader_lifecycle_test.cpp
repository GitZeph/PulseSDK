// tests/mod_loader_lifecycle_test.cpp — unit test del ciclo di vita
// enable/disable/re-enable e delle transizioni di stato (task 7.11,
// Requisiti 7.1, 7.2, 7.3, 7.4, 7.5, 7.7).
//
// Verifica il cablaggio `ModManagerWiring` sopra la state machine del
// `ModManager`:
//   * enable: entry point invocato e mod portata a Enabled (Req 7.1);
//   * disable: rimozione byte-esatto di TUTTI gli hook del mod via
//     RollbackStore/backend, ZERO hook del mod mentre Disabled, Mod_Module
//     conservato e NON scaricato (Req 7.2, 7.3, 7.7);
//   * re-enable: re-invoca l'entry point, ri-esegue resolve_all e reinstalla i
//     SOLI hook con binding risolto riusando gli indici della finestra
//     memorizzata, SENZA un nuovo dlopen (Req 7.5);
//   * round-trip enable→disable→enable: gli hook del mod tornano esattamente
//     all'insieme dei suoi hook con binding risolto, mod Enabled;
//   * transizione non ammessa: rifiutata mantenendo stato e hook invariati +
//     diagnostica (Req 7.4).
//
// Host-testabile sui seam: FakeModuleLoader (registro di simboli/byte senza
// dlopen reale, conta i load), FakeBackend (memoria simulata byte-esatta),
// resolver e invocatore dell'entry point iniettati. Le registrazioni PULSE_HOOK
// del Mod_Module sono modellate UNA sola volta per mod nell'invocatore (lo
// static-init al dlopen avviene una volta sola): le re-invocazioni dell'entry
// point al re-enable non re-registrano, coerentemente con il design.

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
using pulse::lifecycle::TransitionStatus;

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
           ("pulse_lifecycle_" + name + ".rbk");
}

// Banco di prova: come il wiring test, ma l'invocatore dell'entry point
// registra i PULSE_HOOK del modulo UNA sola volta per mod (modella lo
// static-init al dlopen, che non si ripete al re-enable). Le re-invocazioni
// successive contano l'invocazione ma non re-registrano hook.
struct Harness {
    FakeModuleLoader moduleLoader;
    FakeBackend backend;
    HookOwnershipLedger ledger;
    CapturingSink sink;
    RollbackStore rollback;

    std::unordered_map<std::string, std::uintptr_t> resolved;
    std::unordered_map<ModId, std::vector<std::string>> hooksByMod;
    std::unordered_map<ModId, int> invocations;
    std::unordered_set<ModId> registeredOnce;  // PULSE_HOOK già registrati
    std::deque<int> detourStorage;
    std::deque<void*> trampSlots;

    explicit Harness(const std::string& name)
        : rollback(tempRollbackPath(name)) {}

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

    // Invocatore: conta SEMPRE l'invocazione (oracolo di Req 7.1/7.5), ma
    // registra i PULSE_HOOK del modulo SOLO alla prima invocazione per mod
    // (static-init al dlopen, non ripetuto al re-enable). `fail` forza un
    // fallimento dell'entry point.
    ModManagerWiring::EntryPointInvoker makeInvoker(bool fail = false) {
        return [this, fail](const ModId& modId, void* entry) -> EntryPointOutcome {
            (void)entry;
            invocations[modId] += 1;
            if (fail) {
                return EntryPointOutcome::failure("entry point fallito (test)");
            }
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

// Insieme dei target attualmente attribuiti al mod (per confronto round-trip).
std::vector<std::uintptr_t> ownedTargets(const HookOwnershipLedger& ledger,
                                         const ModId& mod) {
    std::vector<std::uintptr_t> out;
    for (const OwnedHook& h : ledger.hooksOf(mod)) out.push_back(h.target);
    std::sort(out.begin(), out.end());
    return out;
}

// --- Req 7.2 / 7.3 / 7.7: disable rimuove tutti gli hook, modulo conservato ---

TEST(ModLoaderLifecycle, DisableRemovesAllHooksKeepsModuleLoaded) {
    resetRegistry();
    Harness h("disable_removes_hooks");

    const ModId mod = "mod.a";
    h.programMod(mod, "a_init", Bytes{0x01}, /*entryResolvable=*/true,
                 {{"A::f", true}, {"A::g", true}});

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), h.makeInvoker(),
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{mod, Bytes{0x01}, "a_init"});

    // enable → Enabled con 2 hook installati e attribuiti (Req 7.1).
    ASSERT_TRUE(wiring.enable(manager, mod).applied());
    EXPECT_EQ(manager.stateOf(mod), ModState::Enabled);
    ASSERT_EQ(h.ledger.hooksOf(mod).size(), 2u);
    EXPECT_EQ(h.backend.installedCount(), 2u);
    EXPECT_EQ(h.moduleLoader.loadCount(), 1u);

    // disable → Disabled: ZERO hook del mod (Req 7.2/7.3), modulo NON scaricato
    // e Mod_Package conservato (Req 7.7).
    const auto r = wiring.disable(manager, mod);
    EXPECT_TRUE(r.applied());
    EXPECT_EQ(manager.stateOf(mod), ModState::Disabled);
    EXPECT_TRUE(h.ledger.hooksOf(mod).empty());           // zero hook (Req 7.3)
    EXPECT_EQ(h.backend.installedCount(), 0u);            // rimossi byte-esatto
    EXPECT_TRUE(wiring.moduleLoaded(mod));                // modulo conservato (Req 7.7)
    EXPECT_EQ(h.moduleLoader.unloadCount(), 0u);          // nessun dlclose al disable
}

// --- Req 7.5: re-enable senza nuovo dlopen, reinstalla la finestra memorizzata-

TEST(ModLoaderLifecycle, ReEnableReusesWindowWithoutNewDlopen) {
    resetRegistry();
    Harness h("reenable_reuses_window");

    const ModId mod = "mod.b";
    h.programMod(mod, "b_init", Bytes{0x02}, true,
                 {{"B::f", true}, {"B::g", true}});

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), h.makeInvoker(),
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{mod, Bytes{0x02}, "b_init"});

    ASSERT_TRUE(wiring.enable(manager, mod).applied());
    const auto [start0, end0] = wiring.epochWindow(mod);
    EXPECT_EQ(start0, 0u);
    EXPECT_EQ(end0, 2u);
    EXPECT_EQ(wiring.entryPointInvocations(mod), 1);
    EXPECT_EQ(h.moduleLoader.loadCount(), 1u);

    ASSERT_TRUE(wiring.disable(manager, mod).applied());

    // re-enable: NESSUN nuovo dlopen, entry point re-invocato, finestra riusata.
    ASSERT_TRUE(wiring.enable(manager, mod).applied());
    EXPECT_EQ(manager.stateOf(mod), ModState::Enabled);
    EXPECT_EQ(h.moduleLoader.loadCount(), 1u);            // nessun nuovo dlopen (Req 7.5)
    EXPECT_EQ(wiring.entryPointInvocations(mod), 2);      // entry point re-invocato
    const auto [start1, end1] = wiring.epochWindow(mod);
    EXPECT_EQ(start1, start0);                            // finestra memorizzata riusata
    EXPECT_EQ(end1, end0);
    // Hook reinstallati e ri-attribuiti (Req 7.5).
    EXPECT_EQ(h.ledger.hooksOf(mod).size(), 2u);
    EXPECT_EQ(h.backend.installedCount(), 2u);
    EXPECT_TRUE(h.sink.anyContains(mod, "re-enable"));
}

// --- round-trip enable→disable→enable: stesso insieme di hook risolti ---------

TEST(ModLoaderLifecycle, EnableDisableEnableRoundTripRestoresResolvedHooks) {
    resetRegistry();
    Harness h("roundtrip");

    const ModId mod = "mod.rt";
    // Mix di hook risolti e non risolti: solo i risolti devono essere installati.
    h.programMod(mod, "rt_init", Bytes{0x03}, true,
                 {{"R::resolved1", true},
                  {"R::missing", false},
                  {"R::resolved2", true}});

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), h.makeInvoker(),
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{mod, Bytes{0x03}, "rt_init"});

    ASSERT_TRUE(wiring.enable(manager, mod).applied());
    const std::vector<std::uintptr_t> before = ownedTargets(h.ledger, mod);
    ASSERT_EQ(before.size(), 2u);  // solo i due hook risolti
    EXPECT_EQ(h.backend.installedCount(), 2u);

    ASSERT_TRUE(wiring.disable(manager, mod).applied());
    EXPECT_TRUE(h.ledger.hooksOf(mod).empty());
    EXPECT_EQ(h.backend.installedCount(), 0u);

    ASSERT_TRUE(wiring.enable(manager, mod).applied());
    const std::vector<std::uintptr_t> after = ownedTargets(h.ledger, mod);

    // L'insieme degli hook del mod torna ESATTAMENTE a quello dei suoi hook con
    // binding risolto, e la mod è Enabled.
    EXPECT_EQ(after, before);
    EXPECT_EQ(h.backend.installedCount(), 2u);
    EXPECT_EQ(manager.stateOf(mod), ModState::Enabled);
    // Nessun install sull'indirizzo non risolto, nemmeno al re-enable.
    EXPECT_TRUE(h.sink.anyContains(mod, "R::missing"));
}

// --- Req 7.4: transizione non ammessa rifiutata mantenendo stato e hook -------

TEST(ModLoaderLifecycle, DisallowedTransitionRejectedKeepsStateAndHooks) {
    resetRegistry();
    Harness h("disallowed_transition");

    const ModId mod = "mod.d";
    h.programMod(mod, "d_init", Bytes{0x04}, true, {{"D::f", true}});

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), h.makeInvoker(),
                            h.sink.sink());
    ModManager manager;
    manager.registerMod(mod);  // re-registrato dal wiring sotto
    wiring.registerMod(manager, ModWiringSpec{mod, Bytes{0x04}, "d_init"});

    // Mod in stato Installed: disable (Installed→Disabled) NON è ammessa.
    const auto rejected = wiring.disable(manager, mod);
    EXPECT_TRUE(rejected.rejected());
    EXPECT_EQ(rejected.status, TransitionStatus::Rejected);
    EXPECT_EQ(manager.stateOf(mod), ModState::Installed);  // stato invariato (Req 7.4)
    EXPECT_EQ(h.backend.installedCount(), 0u);             // nessun hook toccato
    ASSERT_FALSE(manager.rejections().empty());            // diagnostica (Req 7.4)

    // Abilita la mod, poi prova una transizione non ammessa a Enabled→Enabled.
    ASSERT_TRUE(wiring.enable(manager, mod).applied());
    ASSERT_EQ(h.ledger.hooksOf(mod).size(), 1u);
    EXPECT_EQ(h.backend.installedCount(), 1u);

    const auto reEnableRejected = manager.transition(mod, ModState::Enabled);
    EXPECT_TRUE(reEnableRejected.rejected());
    // Stato e hook del mod invariati nonostante la transizione rifiutata.
    EXPECT_EQ(manager.stateOf(mod), ModState::Enabled);
    EXPECT_EQ(h.ledger.hooksOf(mod).size(), 1u);
    EXPECT_EQ(h.backend.installedCount(), 1u);
}

// --- Req 7.6: re-enable fallito → Disabled con rollback byte-esatto -----------

TEST(ModLoaderLifecycle, FailedReEnableReturnsToDisabledByteExactRollback) {
    resetRegistry();
    Harness h("failed_reenable");

    const ModId mod = "mod.f";
    h.programMod(mod, "f_init", Bytes{0x05}, true, {{"F::f", true}});

    // Invocatore controllabile: ok al primo enable, fallisce al re-enable.
    int calls = 0;
    auto flakyInvoker = [&](const ModId& id, void* entry) -> EntryPointOutcome {
        (void)entry;
        h.invocations[id] += 1;
        if (++calls == 1) {
            // primo enable: registra i PULSE_HOOK del modulo.
            h.registeredOnce.insert(id);
            for (const std::string& sym : h.hooksByMod[id]) {
                h.detourStorage.push_back(0);
                h.trampSlots.push_back(nullptr);
                void* detour = static_cast<void*>(&h.detourStorage.back());
                void** tramp = &h.trampSlots.back();
                pulse::hooks::register_hook(sym, detour, tramp);
            }
            return EntryPointOutcome::success();
        }
        return EntryPointOutcome::failure("re-enable fallito (test)");
    };

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), flakyInvoker,
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{mod, Bytes{0x05}, "f_init"});

    ASSERT_TRUE(wiring.enable(manager, mod).applied());
    EXPECT_EQ(h.backend.installedCount(), 1u);
    ASSERT_TRUE(wiring.disable(manager, mod).applied());
    EXPECT_EQ(h.backend.installedCount(), 0u);

    // re-enable fallisce: la mod torna a Disabled, nessun hook installato,
    // nessun nuovo dlopen.
    const auto r = wiring.enable(manager, mod);
    EXPECT_EQ(manager.stateOf(mod), ModState::Disabled);
    EXPECT_TRUE(h.ledger.hooksOf(mod).empty());
    EXPECT_EQ(h.backend.installedCount(), 0u);
    EXPECT_EQ(h.moduleLoader.loadCount(), 1u);  // nessun nuovo dlopen
}

}  // namespace
