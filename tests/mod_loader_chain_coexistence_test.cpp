// tests/mod_loader_chain_coexistence_test.cpp — coesistenza demo + mod esterna
// sullo stesso Target_Address via HookChainRegistry cablata nel ModManagerWiring
// (Hook_Chaining, task 7.5 — fix di integrazione, Requisiti 8.1, 8.2, 8.4).
//
// Questo test copre, lato host, il punto di integrazione che il fallimento di
// Fase E (9.2) ha rivelato: la pipeline delle mod esterne
// (`ModManagerWiring::installWindow`) deve installare gli hook ATTRAVERSO la
// `HookChainRegistry` quando questa è cablata, così una mod esterna che hooka la
// STESSA funzione già hookata dalla demo interna diventa un SECONDO Hook_Link
// della medesima catena sull'UNICA Underlying_Installation — nessuna seconda
// `DobbyHook`, nessun `codice -1` (Req 8). Senza il cablaggio, `installWindow`
// installava un secondo detour diretto via `HookGate` sullo stesso indirizzo,
// che il backend rifiuta (FakeBackend modella lo stesso vincolo: una seconda
// install sullo stesso indirizzo fallisce).
//
// Host-testabile sui seam: FakeModuleLoader (nessun dlopen), FakeBackend
// (memoria simulata + una sola install per indirizzo), RollbackStore,
// HookOwnershipLedger e HookChainRegistry che CONDIVIDONO backend/rollback/ledger
// con il cablaggio. La demo è pre-inserita come primo anello via la Registry
// (modella il passo 6a di `runtime_entry`); la mod esterna registra il proprio
// PULSE_HOOK nell'invocatore dell'entry point (cade nella sua finestra di epoca).

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

#include "bindings/bindings.hpp"
#include "core/runtime_context.hpp"
#include "hooking/fake_backend.hpp"
#include "hooking/hook_chain_registry.hpp"
#include "hooking/rollback_store.hpp"
#include "lifecycle/hook_ownership.hpp"
#include "lifecycle/mod_manager.hpp"
#include "lifecycle/module_loader.hpp"

namespace {

using pulse::hooking::ChainOpOutcome;
using pulse::hooking::FakeBackend;
using pulse::hooking::HookChainRegistry;
using pulse::hooking::LinkSpec;
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
using pulse::loader::bindings::FunctionBinding;

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
           ("pulse_chain_coexist_" + name + ".rbk");
}

FunctionBinding resolvedBinding(std::string symbol, std::uintptr_t address) {
    FunctionBinding b;
    b.symbol = std::move(symbol);
    b.address = address;
    b.resolved = true;
    return b;
}

// L'indirizzo del Real_Trampoline sintetico del FakeBackend per un target.
void* fakeRealTrampoline(std::uintptr_t target) {
    return reinterpret_cast<void*>(target ^ 0xA5A5A5A5ULL);
}

// Il bersaglio condiviso (es. MenuLayer::init) su cui demo e mod esterna
// installano entrambe il proprio anello.
constexpr std::uintptr_t kSharedTarget = 0x4000;
const char* const kSharedSymbol = "MenuLayer::init";

// --- Coesistenza: la mod esterna diventa un 2° anello sull'unica install -----
TEST(ModLoaderChainCoexistence, ExternalModSharesSingleInstallWithDemo) {
    resetRegistry();

    FakeModuleLoader moduleLoader;
    FakeBackend backend;
    HookOwnershipLedger ledger;
    RollbackStore rollback{tempRollbackPath("shared_install")};
    std::vector<std::string> messages;
    auto sink = [&messages](std::string_view m) { messages.emplace_back(m); };

    // La Registry CONDIVIDE backend/rollback/ledger con il cablaggio: è l'unica
    // proprietaria dell'unica Underlying_Installation per Target_Address.
    HookChainRegistry chainRegistry{backend, rollback, ledger, sink};

    // Passo 6a (modella runtime_entry): la demo interna è il PRIMO anello su
    // MenuLayer::init → crea l'unica Underlying_Installation (0→1).
    int demoDetourStorage = 0;
    void* demoSlot = nullptr;
    LinkSpec demoLink;
    demoLink.owner = "pulse.demo";
    demoLink.symbol = kSharedSymbol;
    demoLink.priority = 500;
    demoLink.loadOrder = 0;
    demoLink.detour = static_cast<void*>(&demoDetourStorage);
    demoLink.slot = &demoSlot;
    const auto demoResult = chainRegistry.insertLink(
        kSharedTarget, resolvedBinding(kSharedSymbol, kSharedTarget), demoLink);
    ASSERT_EQ(demoResult.outcome, ChainOpOutcome::CreatedInstall);
    ASSERT_EQ(backend.installedCount(), 1u);
    ASSERT_EQ(backend.installAttempts(), 1u);
    ASSERT_EQ(chainRegistry.chainSize(kSharedTarget), 1u);

    // La mod esterna hooka lo STESSO simbolo; il suo entry point registra il
    // PULSE_HOOK (cade nella finestra di epoca del mod) e il resolver lo risolve
    // al medesimo Target_Address della demo.
    const ModId extMod = "com.pulse.allhooks";
    int extDetourStorage = 0;
    void* extTramp = nullptr;

    FakeModuleLoader::ModuleSpec spec;
    spec.exports.push_back({"pulse_mod_init", {0x90, 0x90}});
    moduleLoader.program(Bytes{0x11}, spec);

    auto resolver = [](std::string_view symbol) -> void* {
        if (symbol == kSharedSymbol) {
            return reinterpret_cast<void*>(kSharedTarget);
        }
        return nullptr;
    };
    auto invoker = [&](const ModId&, void*) -> EntryPointOutcome {
        pulse::hooks::register_hook(kSharedSymbol,
                                    static_cast<void*>(&extDetourStorage),
                                    &extTramp);
        return EntryPointOutcome::success();
    };

    ModManagerWiring wiring(moduleLoader, backend, rollback, ledger, makeCtx(),
                            resolver, invoker, sink);
    wiring.setChainRegistry(&chainRegistry);

    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{extMod, Bytes{0x11}, "pulse_mod_init"});
    const auto result = manager.enableAll({extMod});

    // La mod esterna è abilitata: nessun conflitto.
    ASSERT_EQ(result.enabled.size(), 1u);
    EXPECT_EQ(manager.stateOf(extMod), pulse::lifecycle::ModState::Enabled);

    // INVARIANTE CHIAVE (Req 8.1, 8.4): UNA sola Underlying_Installation, UN solo
    // tentativo di install sul backend → nessuna seconda DobbyHook, nessun
    // `codice -1`. La mod esterna è il SECONDO anello della stessa catena.
    EXPECT_EQ(backend.installedCount(), 1u);
    EXPECT_EQ(backend.installAttempts(), 1u);
    EXPECT_EQ(chainRegistry.chainSize(kSharedTarget), 2u);
    EXPECT_EQ(chainRegistry.installCount(), 1u);

    // Entrambi gli anelli sono attribuiti ai rispettivi Mod_Id nel ledger.
    EXPECT_EQ(ledger.hooksOf("pulse.demo").size(), 1u);
    EXPECT_EQ(ledger.hooksOf(extMod).size(), 1u);

    // Il Chain_Order contiene entrambi (demo e mod esterna) sull'unico target.
    const auto order = chainRegistry.chainOrder(kSharedTarget);
    ASSERT_EQ(order.size(), 2u);

    // Diagnostica osservabile: la mod esterna è stata inserita in catena (non un
    // secondo DobbyHook).
    bool chainInsertDiag = false;
    bool conflictDiag = false;
    for (const auto& m : messages) {
        if (m.find(extMod) != std::string::npos &&
            m.find("inserito in catena") != std::string::npos) {
            chainInsertDiag = true;
        }
        if (m.find("codice -1") != std::string::npos) {
            conflictDiag = true;
        }
    }
    EXPECT_TRUE(chainInsertDiag);
    EXPECT_FALSE(conflictDiag);
}

// --- Teardown: rimuovere la mod esterna relinka; rimuovere la demo disinstalla -
TEST(ModLoaderChainCoexistence, RemoveExternalRelinksThenRemoveDemoUninstalls) {
    resetRegistry();

    FakeModuleLoader moduleLoader;
    FakeBackend backend;
    HookOwnershipLedger ledger;
    RollbackStore rollback{tempRollbackPath("teardown")};
    std::vector<std::string> messages;
    auto sink = [&messages](std::string_view m) { messages.emplace_back(m); };

    HookChainRegistry chainRegistry{backend, rollback, ledger, sink};

    // Byte originali pristine del prologo (16 byte, coerenti col prologo letto).
    const FakeBackend::Bytes pristine{0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70,
                                      0x80, 0x11, 0x21, 0x31, 0x41, 0x51, 0x61,
                                      0x71, 0x81};
    backend.seedOriginal(kSharedTarget, pristine);

    // Demo come primo anello (0→1).
    int demoDetourStorage = 0;
    void* demoSlot = nullptr;
    LinkSpec demoLink;
    demoLink.owner = "pulse.demo";
    demoLink.symbol = kSharedSymbol;
    demoLink.priority = 500;
    demoLink.loadOrder = 0;
    demoLink.detour = static_cast<void*>(&demoDetourStorage);
    demoLink.slot = &demoSlot;
    chainRegistry.insertLink(kSharedTarget,
                             resolvedBinding(kSharedSymbol, kSharedTarget),
                             demoLink);
    ASSERT_NE(backend.liveBytes(kSharedTarget), pristine);  // prologo patchato

    // Mod esterna come secondo anello via il cablaggio.
    const ModId extMod = "com.pulse.allhooks";
    int extDetourStorage = 0;
    void* extTramp = nullptr;
    FakeModuleLoader::ModuleSpec spec;
    spec.exports.push_back({"pulse_mod_init", {0x90, 0x90}});
    moduleLoader.program(Bytes{0x11}, spec);
    auto resolver = [](std::string_view symbol) -> void* {
        return symbol == kSharedSymbol ? reinterpret_cast<void*>(kSharedTarget)
                                       : nullptr;
    };
    auto invoker = [&](const ModId&, void*) -> EntryPointOutcome {
        pulse::hooks::register_hook(kSharedSymbol,
                                    static_cast<void*>(&extDetourStorage),
                                    &extTramp);
        return EntryPointOutcome::success();
    };
    ModManagerWiring wiring(moduleLoader, backend, rollback, ledger, makeCtx(),
                            resolver, invoker, sink);
    wiring.setChainRegistry(&chainRegistry);
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{extMod, Bytes{0x11}, "pulse_mod_init"});
    manager.enableAll({extMod});
    ASSERT_EQ(chainRegistry.chainSize(kSharedTarget), 2u);
    ASSERT_EQ(backend.installedCount(), 1u);

    // Disabilita la mod esterna via il cablaggio (che invoca la rimozione degli
    // hook): rimozione non-ultima → relink, install MANTENUTA, byte NON
    // ripristinati (Req 5.1/5.5).
    wiring.disable(manager, extMod);
    EXPECT_EQ(chainRegistry.chainSize(kSharedTarget), 1u);
    EXPECT_EQ(chainRegistry.installCount(), 1u);
    EXPECT_TRUE(backend.isInstalled(kSharedTarget));
    EXPECT_NE(backend.liveBytes(kSharedTarget), pristine);  // ancora hookato
    EXPECT_TRUE(ledger.hooksOf(extMod).empty());
    EXPECT_EQ(ledger.hooksOf("pulse.demo").size(), 1u);

    // Rimuovi l'ultimo anello (la demo) direttamente via la Registry: 1→0 →
    // backend.remove + ripristino byte-esatto del prologo (Req 5.3/5.4).
    chainRegistry.removeOwner("pulse.demo");
    EXPECT_EQ(chainRegistry.installCount(), 0u);
    EXPECT_EQ(chainRegistry.chainSize(kSharedTarget), 0u);
    EXPECT_FALSE(backend.isInstalled(kSharedTarget));
    EXPECT_EQ(backend.liveBytes(kSharedTarget), pristine);  // byte-esatto
}

}  // namespace
