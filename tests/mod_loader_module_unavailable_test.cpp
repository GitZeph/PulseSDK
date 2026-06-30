// tests/mod_loader_module_unavailable_test.cpp — unit test del fail-open su
// Module_Loader/backend non disponibile (task 7.23, Requisiti 9.1, 9.2, 9.3,
// 11.5).
//
// Verifica due fail-open complementari del Mod_Loader, entrambi a "byte
// invariati" (zero install sul backend, GD prosegue):
//
//   * Module_Loader non disponibile (Req 11.5): quando il Module_Loader della
//     piattaforma corrente riporta `available() == false`, il cablaggio carica
//     ZERO mod, registra una diagnostica che NOMINA la piattaforma del
//     Runtime_Context e il loader, e NON tocca il backend (zero install → byte
//     di eseguibile/asset invariati, Req 9.3). La query up front
//     `check_module_loader_availability` riflette la stessa condizione per il
//     pipeline entry (task 7.22).
//
//   * Backend non disponibile (Req 9.1/9.2): quando l'`IHookBackend` riporta
//     `available() == false`, `HookGate` (consultato da `installWindow` prima di
//     ogni install) NON installa alcun hook (zero install → byte invariati) e
//     registra una diagnostica che NOMINA il backend; la mod può comunque
//     raggiungere Enabled ma con ZERO hook installati.
//
// Host-testabile sui seam: FakeModuleLoader (con `setAvailable`), FakeBackend e
// un `UnavailableBackend` locale con `available()==false`.

#include "lifecycle/mod_loader.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <pulse/hooks.hpp>

#include "core/runtime_context.hpp"
#include "hooking/fake_backend.hpp"
#include "hooking/hook_backend.hpp"
#include "hooking/rollback_store.hpp"
#include "lifecycle/hook_ownership.hpp"
#include "lifecycle/mod_manager.hpp"
#include "lifecycle/module_loader.hpp"

namespace {

using pulse::hooking::FakeBackend;
using pulse::hooking::IHookBackend;
using pulse::hooking::Result;
using pulse::hooking::RollbackStore;
using pulse::hooking::Trampoline;
using pulse::lifecycle::Bytes;
using pulse::lifecycle::check_module_loader_availability;
using pulse::lifecycle::EntryPointOutcome;
using pulse::lifecycle::FakeModuleLoader;
using pulse::lifecycle::HookOwnershipLedger;
using pulse::lifecycle::ModId;
using pulse::lifecycle::ModManager;
using pulse::lifecycle::ModManagerWiring;
using pulse::lifecycle::ModState;
using pulse::lifecycle::ModuleLoaderAvailability;
using pulse::lifecycle::ModWiringSpec;

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
};

pulse::loader::RuntimeContext makeCtx() {
    pulse::loader::RuntimeContext ctx;
    ctx.gdVersion = pulse::loader::GdVersion{2, 2081};
    ctx.platform = pulse::loader::Platform::IOSArm64;
    ctx.platformId = "ios-arm64";
    return ctx;
}

std::filesystem::path tempRollbackPath(const std::string& name) {
    return std::filesystem::path(::testing::TempDir()) /
           ("pulse_modunavail_" + name + ".rbk");
}

// Backend con `available()==false` che traccia se `install` viene MAI invocato
// (non deve esserlo: il gate blocca prima di toccare il backend).
class UnavailableBackend final : public IHookBackend {
public:
    explicit UnavailableBackend(std::string name) : name_(std::move(name)) {}

    [[nodiscard]] std::size_t installAttempts() const noexcept {
        return installAttempts_;
    }

    Result<Trampoline> install(std::uintptr_t /*target*/, void* /*detour*/) override {
        ++installAttempts_;  // non deve mai accadere quando available()==false
        return Result<Trampoline>::ok(Trampoline{});
    }
    Result<void> remove(std::uintptr_t) override { return Result<void>::ok(); }
    Result<pulse::hooking::ByteSpan> readOriginal(std::uintptr_t,
                                                  std::size_t) override {
        return Result<pulse::hooking::ByteSpan>::err(
            pulse::hooking::HookError{pulse::hooking::HookErrorCode::Unsupported,
                                      "backend non disponibile"});
    }
    std::string_view name() const noexcept override { return name_; }
    bool available() const noexcept override { return false; }

private:
    std::string name_;
    std::size_t installAttempts_{0};
};

// Invocatore dell'entry point che registra i PULSE_HOOK del modulo (finestra di
// epoca) come farebbe il Mod_Module reale allo static-init.
ModManagerWiring::EntryPointInvoker makeRegisteringInvoker(
    std::vector<int>& detourStorage, std::vector<void*>& trampSlots,
    const std::string& symbol) {
    return [&detourStorage, &trampSlots, symbol](
               const ModId&, void*) -> EntryPointOutcome {
        detourStorage.push_back(0);
        trampSlots.push_back(nullptr);
        void* detour = static_cast<void*>(&detourStorage.back());
        void** tramp = &trampSlots.back();
        pulse::hooks::register_hook(symbol, detour, tramp);
        return EntryPointOutcome::success();
    };
}

// === Req 11.5 — Module_Loader non disponibile: zero mod, byte invariati ======

TEST(ModLoaderModuleUnavailable, ModuleLoaderUnavailableLoadsZeroMods) {
    resetRegistry();
    FakeModuleLoader moduleLoader;
    moduleLoader.setAvailable(false);  // piattaforma non-deliverable
    FakeBackend backend;
    HookOwnershipLedger ledger;
    CapturingSink sink;
    RollbackStore rollback(tempRollbackPath("zero_mods"));

    const ModId mod = "mod.alpha";
    moduleLoader.program(Bytes{0x01}, FakeModuleLoader::ModuleSpec{});

    ModManagerWiring wiring(
        moduleLoader, backend, rollback, ledger, makeCtx(),
        [](std::string_view) -> void* { return nullptr; },
        [](const ModId&, void*) -> EntryPointOutcome {
            return EntryPointOutcome::success();
        },
        sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{mod, Bytes{0x01}, "a_init"});

    const auto result = wiring.runNoThrow(manager, {mod});

    // Zero mod abilitate, zero install sul backend (byte invariati, Req 9.3).
    EXPECT_TRUE(result.enabled.empty());
    EXPECT_TRUE(result.failed.empty());
    EXPECT_EQ(backend.installedCount(), 0u);
    EXPECT_EQ(moduleLoader.loadCount(), 0u);  // nessun dlopen tentato
    EXPECT_TRUE(ledger.hooksOf(mod).empty());
    EXPECT_NE(manager.stateOf(mod), ModState::Enabled);

    // Diagnostica che nomina la piattaforma del Runtime_Context e il loader.
    EXPECT_TRUE(sink.anyContains("ios-arm64"));
    EXPECT_TRUE(sink.anyContains("pulse-module-fake"));
    EXPECT_FALSE(wiring.moduleLoaderAvailable());
}

// === Req 11.5 — check_module_loader_availability up front =====================

TEST(ModLoaderModuleUnavailable, CheckAvailabilityReportsPlatformWhenUnavailable) {
    FakeModuleLoader unavailable;
    unavailable.setAvailable(false);
    const ModuleLoaderAvailability down =
        check_module_loader_availability(unavailable, makeCtx());
    EXPECT_FALSE(down.available);
    EXPECT_FALSE(static_cast<bool>(down));
    EXPECT_NE(down.diagnostic.find("ios-arm64"), std::string::npos);
    EXPECT_NE(down.diagnostic.find("pulse-module-fake"), std::string::npos);

    FakeModuleLoader available;  // default: available()==true
    const ModuleLoaderAvailability up =
        check_module_loader_availability(available, makeCtx());
    EXPECT_TRUE(up.available);
    EXPECT_TRUE(static_cast<bool>(up));
    EXPECT_TRUE(up.diagnostic.empty());
}

// === Req 11.5 — piattaforma reale (stub non-Apple) via make_platform_* ========

TEST(ModLoaderModuleUnavailable, PlatformStubAvailabilityIsConsistent) {
    auto platformLoader = pulse::lifecycle::make_platform_module_loader();
    ASSERT_NE(platformLoader, nullptr);
    const ModuleLoaderAvailability res =
        check_module_loader_availability(*platformLoader, makeCtx());
    // Coerenza: la diagnostica è vuota sse e solo se il loader è disponibile.
    EXPECT_EQ(res.available, platformLoader->available());
    if (!res.available) {
        EXPECT_NE(res.diagnostic.find("ios-arm64"), std::string::npos);
    }
}

// === Req 9.1/9.2 — backend non disponibile: zero hook, byte invariati =========

TEST(ModLoaderModuleUnavailable, UnavailableBackendInstallsNoHooks) {
    resetRegistry();
    FakeModuleLoader moduleLoader;  // modulo caricabile (available()==true)
    UnavailableBackend backend{"pulse-dobby"};  // backend giù
    HookOwnershipLedger ledger;
    CapturingSink sink;
    RollbackStore rollback(tempRollbackPath("backend_down"));

    const ModId mod = "mod.beta";
    FakeModuleLoader::ModuleSpec spec;
    spec.exports.push_back({"b_init", {0x90, 0x90}});
    moduleLoader.program(Bytes{0x02}, spec);

    std::vector<int> detourStorage;
    std::vector<void*> trampSlots;
    detourStorage.reserve(8);
    trampSlots.reserve(8);

    ModManagerWiring wiring(
        moduleLoader, backend, rollback, ledger, makeCtx(),
        [](std::string_view) -> void* {
            // Simbolo risolto a un indirizzo non nullo: l'unico blocco è il
            // backend non disponibile, non la risoluzione.
            return reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x4242));
        },
        makeRegisteringInvoker(detourStorage, trampSlots, "B::f"), sink.sink());
    ModManager manager;
    wiring.registerMod(manager, ModWiringSpec{mod, Bytes{0x02}, "b_init"});

    const auto result = wiring.runNoThrow(manager, {mod});

    // Il gate consulta available() prima di ogni install: zero hook installati,
    // backend MAI invocato (byte invariati, Req 9.1/9.2).
    EXPECT_EQ(backend.installAttempts(), 0u);
    EXPECT_TRUE(ledger.hooksOf(mod).empty());
    // Diagnostica che nomina il backend (Req 9.2).
    EXPECT_TRUE(sink.anyContains("pulse-dobby"));
    // La mod raggiunge comunque Enabled, ma senza alcun hook installato.
    EXPECT_EQ(manager.stateOf(mod), ModState::Enabled);
    EXPECT_EQ(result.enabled, std::vector<ModId>{mod});
}

}  // namespace
