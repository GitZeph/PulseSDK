// tests/mod_loader_fail_open_test.cpp — unit test dell'isolamento fail-open
// per-mod e della barriera no-throw del Mod_Loader (task 7.7, Requisiti 6.1,
// 6.2, 6.3, 6.4, 6.5, 6.6).
//
// Verifica `ModManagerWiring::runNoThrow` e la barriera no-throw di `onEnable`:
//   * modulo non caricabile → fallimento confinato alla sola mod, 0 hook del
//     mod, le altre proseguono (Req 6.1, 6.6);
//   * entry point che restituisce errore → mod a Disabled, 0 hook del mod,
//     rollback byte-esatto degli eventuali hook già installati (Req 6.2);
//   * entry point che LANCIA un'eccezione → nessuna eccezione propagata, mod a
//     Disabled, gli hook del mod ripristinati byte-esatto, le altre proseguono
//     (Req 6.2, 6.3, 6.4);
//   * eccezione iniettata in uno stadio per-mod (resolver dei simboli) →
//     confinata alla sola mod, nessuna propagazione, le altre proseguono;
//   * più mod fallite nella stessa esecuzione → tutte le restanti valide
//     caricate/abilitate indipendentemente dal numero di fallimenti (Req 6.6);
//   * gli hook delle mod valide restano invariati a fronte di fallimenti
//     isolati (Req 6.5).
//
// Host-testabile sui seam: FakeModuleLoader, FakeBackend, resolver/invoker
// iniettati. Le registrazioni PULSE_HOOK del Mod_Module sono modellate
// nell'invocatore dell'entry point (cadono nella finestra di epoca del mod).

#include "lifecycle/mod_loader.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <stdexcept>
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
using pulse::lifecycle::ModState;
using pulse::lifecycle::ModWiringSpec;
using pulse::lifecycle::OwnedHook;

// Azzera il registro globale dello SDK: singleton di processo condiviso.
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
           ("pulse_failopen_" + name + ".rbk");
}

// Banco di prova condiviso (come il wiring test): programma mod nel
// FakeModuleLoader, modella i loro PULSE_HOOK nell'invocatore dell'entry point.
struct Harness {
    FakeModuleLoader moduleLoader;
    FakeBackend backend;
    HookOwnershipLedger ledger;
    CapturingSink sink;
    RollbackStore rollback;

    std::unordered_map<std::string, std::uintptr_t> resolved;
    std::unordered_map<ModId, std::vector<std::string>> hooksByMod;
    std::unordered_map<ModId, int> invocations;
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

    // Resolver dei simboli; opzionalmente lancia UNA volta su un simbolo
    // specifico per iniettare un'eccezione in uno stadio per-mod (resolve_all).
    // Lancia una sola volta perché `resolve_all` percorre l'intero registro
    // globale: dopo l'isolamento della mod difettosa, le mod successive devono
    // poter risolvere il registro senza ri-lanciare sul simbolo residuo.
    std::string throwOnSymbol;
    bool resolverThrew = false;
    ModManagerWiring::SymbolResolver makeResolver() {
        return [this](std::string_view symbol) -> void* {
            if (!throwOnSymbol.empty() && !resolverThrew &&
                symbol == throwOnSymbol) {
                resolverThrew = true;
                throw std::runtime_error("resolver: eccezione iniettata su '" +
                                         std::string(symbol) + "'");
            }
            auto it = resolved.find(std::string(symbol));
            if (it == resolved.end() || it->second == 0) return nullptr;
            return reinterpret_cast<void*>(it->second);
        };
    }

    // Invocatore dell'entry point: conta l'invocazione, registra i PULSE_HOOK
    // del modulo (finestra di epoca) e opzionalmente fallisce/lancia.
    enum class EntryBehavior { Ok, Fail, Throw };
    ModManagerWiring::EntryPointInvoker makeInvoker(EntryBehavior behavior) {
        return [this, behavior](const ModId& modId,
                                void* entry) -> EntryPointOutcome {
            (void)entry;
            invocations[modId] += 1;
            if (behavior == EntryBehavior::Throw) {
                throw std::runtime_error("entry point: eccezione iniettata per '" +
                                         modId + "'");
            }
            if (behavior == EntryBehavior::Fail) {
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

// --- Req 6.1 / 6.6: modulo non caricabile isolato, le altre proseguono -------

TEST(ModLoaderFailOpen, ModuleNotLoadableIsolatedOthersLoad) {
    resetRegistry();
    Harness h("module_not_loadable");

    const ModId good = "mod.good";
    const ModId bad = "mod.bad";
    h.programMod(good, "g_init", Bytes{0x01}, true, {{"G::f", true}});

    // mod.bad: load simulata fallita.
    FakeModuleLoader::ModuleSpec badSpec;
    badSpec.failLoad = true;
    h.moduleLoader.program(Bytes{0x02}, badSpec);

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(),
                            h.makeInvoker(Harness::EntryBehavior::Ok),
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{bad, Bytes{0x02}, "b_init"});
    wiring.registerMod(manager, ModWiringSpec{good, Bytes{0x01}, "g_init"});

    const auto result = wiring.runNoThrow(manager, {bad, good});

    // La mod difettosa è isolata (0 hook), la valida è abilitata (Req 6.1, 6.6).
    EXPECT_EQ(manager.stateOf(bad), ModState::Disabled);
    EXPECT_EQ(manager.stateOf(good), ModState::Enabled);
    EXPECT_TRUE(h.ledger.hooksOf(bad).empty());
    ASSERT_EQ(h.ledger.hooksOf(good).size(), 1u);
    EXPECT_EQ(result.enabled, std::vector<ModId>{good});
    ASSERT_EQ(result.failed.size(), 1u);
    EXPECT_EQ(result.failed[0].mod, bad);
    EXPECT_TRUE(h.sink.anyContains(bad));
}

// --- Req 6.2: entry point in errore → Disabled, 0 hook del mod ----------------

TEST(ModLoaderFailOpen, EntryPointErrorDisablesNoHooks) {
    resetRegistry();
    Harness h("entry_error");

    const ModId mod = "mod.err";
    h.programMod(mod, "e_init", Bytes{0x03}, true, {{"E::f", true}});

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(),
                            h.makeInvoker(Harness::EntryBehavior::Fail),
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{mod, Bytes{0x03}, "e_init"});

    const auto result = wiring.runNoThrow(manager, {mod});

    EXPECT_EQ(manager.stateOf(mod), ModState::Disabled);
    EXPECT_TRUE(h.ledger.hooksOf(mod).empty());
    EXPECT_EQ(h.backend.installedCount(), 0u);
    ASSERT_EQ(result.failed.size(), 1u);
    EXPECT_FALSE(result.failed[0].threw);
}

// --- Req 6.3 / 6.4: entry point che LANCIA → nessuna eccezione propagata ------

TEST(ModLoaderFailOpen, EntryPointThrowIsConfinedNoPropagation) {
    resetRegistry();
    Harness h("entry_throw");

    const ModId thrower = "mod.throw";
    const ModId good = "mod.ok";
    h.programMod(thrower, "t_init", Bytes{0x04}, true, {{"T::f", true}});
    h.programMod(good, "o_init", Bytes{0x05}, true, {{"O::g", true}});

    // L'invocatore lancia per OGNI mod; ma il good usa un secondo wiring/invoker.
    // Per semplicità: un invocatore che lancia solo per la mod 'thrower'.
    auto selectiveInvoker =
        [&h, thrower](const ModId& modId, void* entry) -> EntryPointOutcome {
        (void)entry;
        h.invocations[modId] += 1;
        if (modId == thrower) {
            throw std::runtime_error("entry point: boom");
        }
        for (const std::string& sym : h.hooksByMod[modId]) {
            h.detourStorage.push_back(0);
            h.trampSlots.push_back(nullptr);
            void* detour = static_cast<void*>(&h.detourStorage.back());
            void** tramp = &h.trampSlots.back();
            pulse::hooks::register_hook(sym, detour, tramp);
        }
        return EntryPointOutcome::success();
    };

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), selectiveInvoker,
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{thrower, Bytes{0x04}, "t_init"});
    wiring.registerMod(manager, ModWiringSpec{good, Bytes{0x05}, "o_init"});

    // runNoThrow è noexcept: nessuna eccezione deve propagarsi.
    const auto result = wiring.runNoThrow(manager, {thrower, good});

    // La mod che lancia è isolata (Disabled, 0 hook), la valida prosegue.
    EXPECT_EQ(manager.stateOf(thrower), ModState::Disabled);
    EXPECT_EQ(manager.stateOf(good), ModState::Enabled);
    EXPECT_TRUE(h.ledger.hooksOf(thrower).empty());
    ASSERT_EQ(h.ledger.hooksOf(good).size(), 1u);
    EXPECT_EQ(result.enabled, std::vector<ModId>{good});
    ASSERT_EQ(result.failed.size(), 1u);
    EXPECT_EQ(result.failed[0].mod, thrower);
}

// --- Req 6.3 / 6.4: eccezione in uno stadio (resolver) confinata --------------

TEST(ModLoaderFailOpen, ExceptionInResolverIsConfined) {
    resetRegistry();
    Harness h("resolver_throw");

    const ModId mod = "mod.resolver";
    const ModId good = "mod.good";
    h.programMod(mod, "r_init", Bytes{0x06}, true, {{"R::boom", true}});
    h.programMod(good, "g_init", Bytes{0x07}, true, {{"G::ok", true}});
    h.throwOnSymbol = "R::boom";  // resolve_all lancerà su questo simbolo

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(),
                            h.makeInvoker(Harness::EntryBehavior::Ok),
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{mod, Bytes{0x06}, "r_init"});
    wiring.registerMod(manager, ModWiringSpec{good, Bytes{0x07}, "g_init"});

    const auto result = wiring.runNoThrow(manager, {mod, good});

    // La mod il cui stadio lancia è isolata; la valida prosegue (Req 6.3/6.4/6.6).
    EXPECT_EQ(manager.stateOf(mod), ModState::Disabled);
    EXPECT_EQ(manager.stateOf(good), ModState::Enabled);
    EXPECT_TRUE(h.ledger.hooksOf(mod).empty());
    ASSERT_EQ(h.ledger.hooksOf(good).size(), 1u);
    ASSERT_EQ(result.failed.size(), 1u);
    EXPECT_EQ(result.failed[0].mod, mod);
}

// --- Req 6.5 / 6.6: più fallimenti, hook delle mod valide invariati -----------

TEST(ModLoaderFailOpen, ValidModsHooksUnchangedAcrossMultipleFailures) {
    resetRegistry();
    Harness h("multi_failure");

    const ModId v1 = "mod.v1";
    const ModId v2 = "mod.v2";
    const ModId f1 = "mod.f1";  // modulo non caricabile
    const ModId f2 = "mod.f2";  // entry point in errore

    h.programMod(v1, "v1_init", Bytes{0x10}, true, {{"V1::a", true}});
    h.programMod(v2, "v2_init", Bytes{0x11}, true,
                 {{"V2::a", true}, {"V2::b", true}});

    FakeModuleLoader::ModuleSpec f1Spec;
    f1Spec.failLoad = true;
    h.moduleLoader.program(Bytes{0x12}, f1Spec);  // f1 non caricabile
    h.programMod(f2, "f2_init", Bytes{0x13}, true, {{"F2::a", true}});

    // Invocatore: ok per v1/v2, fallisce per f2.
    auto invoker =
        [&h, f2](const ModId& modId, void* entry) -> EntryPointOutcome {
        (void)entry;
        h.invocations[modId] += 1;
        if (modId == f2) {
            return EntryPointOutcome::failure("init fallito");
        }
        for (const std::string& sym : h.hooksByMod[modId]) {
            h.detourStorage.push_back(0);
            h.trampSlots.push_back(nullptr);
            void* detour = static_cast<void*>(&h.detourStorage.back());
            void** tramp = &h.trampSlots.back();
            pulse::hooks::register_hook(sym, detour, tramp);
        }
        return EntryPointOutcome::success();
    };

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), invoker, h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{f1, Bytes{0x12}, "f1_init"});
    wiring.registerMod(manager, ModWiringSpec{v1, Bytes{0x10}, "v1_init"});
    wiring.registerMod(manager, ModWiringSpec{f2, Bytes{0x13}, "f2_init"});
    wiring.registerMod(manager, ModWiringSpec{v2, Bytes{0x11}, "v2_init"});

    const auto result = wiring.runNoThrow(manager, {f1, v1, f2, v2});

    // Tutte le mod valide caricate/abilitate, indipendentemente dai 2 fallimenti
    // (Req 6.6); i loro hook sono installati e invariati (Req 6.5).
    EXPECT_EQ(manager.stateOf(v1), ModState::Enabled);
    EXPECT_EQ(manager.stateOf(v2), ModState::Enabled);
    EXPECT_EQ(manager.stateOf(f1), ModState::Disabled);
    EXPECT_EQ(manager.stateOf(f2), ModState::Disabled);

    EXPECT_EQ(h.ledger.hooksOf(v1).size(), 1u);
    EXPECT_EQ(h.ledger.hooksOf(v2).size(), 2u);
    EXPECT_TRUE(h.ledger.hooksOf(f1).empty());
    EXPECT_TRUE(h.ledger.hooksOf(f2).empty());

    // Solo gli hook delle mod valide (1 + 2 = 3) sono installati nel backend.
    EXPECT_EQ(h.backend.installedCount(), 3u);

    EXPECT_EQ(result.enabled.size(), 2u);
    EXPECT_EQ(result.failed.size(), 2u);
}

// --- Req 6.6: mod non registrata in `order` è ignorata senza effetti ----------

TEST(ModLoaderFailOpen, UnregisteredModInOrderIsIgnored) {
    resetRegistry();
    Harness h("unregistered");

    const ModId good = "mod.good";
    const ModId ghost = "mod.ghost";  // mai registrata
    h.programMod(good, "g_init", Bytes{0x20}, true, {{"G::f", true}});

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(),
                            h.makeInvoker(Harness::EntryBehavior::Ok),
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{good, Bytes{0x20}, "g_init"});

    const auto result = wiring.runNoThrow(manager, {ghost, good});

    EXPECT_EQ(manager.stateOf(good), ModState::Enabled);
    EXPECT_EQ(result.enabled, std::vector<ModId>{good});
    EXPECT_TRUE(result.failed.empty());  // ghost ignorata, non un fallimento
}

}  // namespace
