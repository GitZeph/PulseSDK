// tests/mod_loader_partial_install_rollback_test.cpp — unit test del rollback
// transazionale su install parziale fallita (task 7.14, Requisito 9.5).
//
// Verifica il cablaggio `ModManagerWiring` quando l'install degli hook di una
// mod fallisce DOPO che almeno un suo hook è già stato installato: gli hook già
// installati di quella mod sono rimossi con ripristino BYTE-ESATTO via
// RollbackStore/backend, il fallimento è confinato al solo Mod_Id (la mod
// termina Disabled con zero hook) e le mod restanti proseguono (Req 9.5, 6.6).
//
// Host-testabile sui seam: FakeModuleLoader (registro di simboli/byte senza
// dlopen reale), FakeBackend (memoria simulata byte-esatta con iniezione di
// fallimenti di install via `failInstallAt`), resolver e invocatore dell'entry
// point iniettati. Le registrazioni PULSE_HOOK del Mod_Module sono modellate
// nell'invocatore (static-init al dlopen). Coerente con il design: il path
// `dlopen` reale è coperto in Fase E, non qui.

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
using pulse::hooking::HookErrorCode;
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

constexpr std::size_t kPrologue = pulse::lifecycle::kRollbackPrologueBytes;

// Azzera il registro globale dello SDK (singleton di processo condiviso).
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
           ("pulse_partial_rollback_" + name + ".rbk");
}

struct CapturingSink {
    std::vector<std::string> messages;
    pulse::loader::DiagnosticSink sink() {
        return [this](std::string_view m) { messages.emplace_back(m); };
    }
    bool anyContains(std::string_view a, std::string_view b) const {
        for (const std::string& m : messages)
            if (m.find(a) != std::string::npos && m.find(b) != std::string::npos)
                return true;
        return false;
    }
};

// Banco di prova: programma i Mod_Module e registra i PULSE_HOOK una sola volta
// per mod nell'invocatore (static-init al dlopen). Gli indirizzi risolti sono
// seminati nel FakeBackend con byte originali deterministici (oracolo del
// rollback byte-esatto).
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

    explicit Harness(const std::string& name) : rollback(tempRollbackPath(name)) {}

    // Registra una mod con i suoi hook (tutti risolvibili). Restituisce gli
    // indirizzi risolti, nell'ordine, per poter iniettare il fallimento.
    std::vector<std::uintptr_t> programMod(
        const ModId& modId, const std::string& entrySymbol, const Bytes& image,
        const std::vector<std::string>& hooks) {
        FakeModuleLoader::ModuleSpec spec;
        spec.exports.push_back({entrySymbol, {0x90, 0x90}});
        moduleLoader.program(image, spec);

        std::vector<std::uintptr_t> addrs;
        for (const std::string& sym : hooks) {
            hooksByMod[modId].push_back(sym);
            const std::uintptr_t addr =
                0x4000 + static_cast<std::uintptr_t>(
                             std::hash<std::string>{}(sym) & 0xFFFF);
            resolved[sym] = addr;
            addrs.push_back(addr);

            // Semina byte originali deterministici (oracolo byte-esatto).
            FakeBackend::Bytes original(kPrologue);
            for (std::size_t i = 0; i < kPrologue; ++i)
                original[i] = static_cast<FakeBackend::Byte>((addr + i) & 0xFF);
            backend.seedOriginal(addr, original);
        }
        return addrs;
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

// --- Req 9.5: install parziale fallita → rollback byte-esatto, mod Disabled ---

TEST(ModLoaderPartialInstall, FailureAfterFirstHookRollsBackByteExact) {
    resetRegistry();
    Harness h("after_first_hook");

    const ModId mod = "mod.partial";
    // Tre hook risolti: l'install del TERZO viene forzato a fallire, dopo che
    // due hook della stessa mod sono già stati installati.
    const auto addrs =
        h.programMod(mod, "p_init", Bytes{0x01},
                     {"P::first", "P::second", "P::third"});
    ASSERT_EQ(addrs.size(), 3u);
    h.backend.failInstallAt(addrs[2], HookErrorCode::BackendFailure,
                            "fake: install del terzo hook forzato a fallire");

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), h.makeInvoker(),
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{mod, Bytes{0x01}, "p_init"});

    // enable: l'entry point ha successo, ma l'install parziale fallisce sul
    // terzo hook → la mod NON resta Enabled.
    const auto r = wiring.enable(manager, mod);
    EXPECT_FALSE(r.applied() && r.state == ModState::Enabled);

    // Fallimento confinato al solo Mod_Id: la mod termina Disabled con ZERO hook.
    EXPECT_EQ(manager.stateOf(mod), ModState::Disabled);
    EXPECT_TRUE(h.ledger.hooksOf(mod).empty());
    EXPECT_EQ(h.backend.installedCount(), 0u);  // i due hook installati rimossi

    // Ripristino BYTE-ESATTO dei due hook già installati (Req 9.5): i byte
    // "live" tornano identici agli originali seminati.
    for (std::size_t i = 0; i < 2; ++i) {
        const auto live = h.backend.liveBytes(addrs[i]);
        const auto orig = h.backend.snapshotOriginal(addrs[i]);
        ASSERT_TRUE(live.has_value());
        ASSERT_TRUE(orig.has_value());
        EXPECT_EQ(*live, *orig) << "hook " << i << " non ripristinato byte-esatto";
    }

    // Diagnostica del rollback transazionale attribuita al Mod_Id (Req 9.5).
    EXPECT_TRUE(h.sink.anyContains(mod, "9.5"));
}

// --- Req 9.5 + 6.6: il fallimento isola la sola mod, le restanti proseguono ---

TEST(ModLoaderPartialInstall, FailureIsolatedToModOthersProceed) {
    resetRegistry();
    Harness h("isolated_others_proceed");

    const ModId bad = "mod.bad";
    const ModId good = "mod.good";

    const auto badAddrs =
        h.programMod(bad, "bad_init", Bytes{0x10}, {"B::a", "B::b"});
    const auto goodAddrs =
        h.programMod(good, "good_init", Bytes{0x20}, {"G::a", "G::b"});
    ASSERT_EQ(badAddrs.size(), 2u);
    ASSERT_EQ(goodAddrs.size(), 2u);

    // La mod "bad" fallisce l'install sul SECONDO hook (dopo 1 installato).
    h.backend.failInstallAt(badAddrs[1]);

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), h.makeInvoker(),
                            h.sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{bad, Bytes{0x10}, "bad_init"});
    wiring.registerMod(manager, ModWiringSpec{good, Bytes{0x20}, "good_init"});

    // Abilita entrambe nell'ordine [bad, good] via la barriera no-throw.
    const auto out = wiring.runNoThrow(manager, {bad, good});

    // "bad" isolata: Disabled, zero hook, fallimento registrato.
    EXPECT_EQ(manager.stateOf(bad), ModState::Disabled);
    EXPECT_TRUE(h.ledger.hooksOf(bad).empty());
    ASSERT_EQ(out.failed.size(), 1u);
    EXPECT_EQ(out.failed.front().mod, bad);

    // "good" prosegue ed è abilitata con i suoi due hook (Req 6.6).
    EXPECT_EQ(manager.stateOf(good), ModState::Enabled);
    EXPECT_EQ(h.ledger.hooksOf(good).size(), 2u);
    ASSERT_EQ(out.enabled.size(), 1u);
    EXPECT_EQ(out.enabled.front(), good);

    // Solo gli hook della mod "good" risultano installati sul backend: il
    // fallimento isolato non aggiunge né lascia hook della mod "bad".
    EXPECT_EQ(h.backend.installedCount(), 2u);
    EXPECT_FALSE(h.backend.isInstalled(badAddrs[0]));
    EXPECT_TRUE(h.backend.isInstalled(goodAddrs[0]));
    EXPECT_TRUE(h.backend.isInstalled(goodAddrs[1]));
}

}  // namespace
