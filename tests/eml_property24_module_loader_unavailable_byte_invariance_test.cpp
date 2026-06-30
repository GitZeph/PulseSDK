// tests/eml_property24_module_loader_unavailable_byte_invariance_test.cpp
// Feature: external-mod-loading, Property 24 — Fail-open su Module_Loader non
// disponibile, byte invariati.
// Validates: Requirements 9.3, 11.5 (Requisiti 9.3, 11.5)
//
// Property 24 (design.md): per ogni Mods_Directory (qui modellata come un
// "albero di file finto" — un batch arbitrario di mod con Mod_Id distinti e
// immagini di Mod_Module), quando il Module_Loader della piattaforma corrente
// riporta `available() == false`:
//   * il Mod_Loader carica ZERO mod (nessun enable, nessun dlopen tentato);
//   * registra una diagnostica che NOMINA la piattaforma del Runtime_Context e
//     il Module_Loader (Req 11.5);
//   * lascia l'eseguibile e gli asset (qui i byte modellati delle funzioni
//     bersaglio del FakeBackend) byte-per-byte INVARIATI — zero install
//     (Req 9.3);
//   * Geometry Dash prosegue (il cablaggio non lancia e ritorna un esito vuoto).
// Inoltre la STESSA invarianza di byte vale quando il Module_Loader È
// disponibile (`available() == true`) ma NESSUNA mod termina nello stato
// Enabled (qui: ogni Mod_Module fallisce il caricamento, oppure il suo entry
// point fallisce) — di nuovo zero install, byte invariati.
//
// Modello host-testable sui seam (coerente con
// tests/mod_loader_module_unavailable_test.cpp): si guida
// `ModManagerWiring::runNoThrow` con un `FakeModuleLoader` (con `setAvailable`)
// e un `FakeBackend` che modella i byte delle funzioni bersaglio. I byte
// bersaglio sono seminati e fotografati PRIMA e DOPO l'esecuzione: l'invarianza
// byte-per-byte equivale a "zero install".
//
// Header del loader in loader/lifecycle/ (include relativo alla radice loader/);
// la logica di fail-open è in mod_loader.cpp (compilata in pulse::loader).
// Integrazione RapidCheck+GoogleTest in extras/gtest.

#include "lifecycle/mod_loader.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

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

void resetRegistry() { pulse::hooks::registry().clear(); }

// Sink che accumula i messaggi diagnostici per le asserzioni di contenuto.
struct CapturingSink {
    std::vector<std::string> messages;
    pulse::loader::DiagnosticSink sink() {
        return [this](std::string_view m) { messages.emplace_back(m); };
    }
    bool anyContains(std::string_view needle) const {
        for (const std::string& m : messages)
            if (m.find(needle) != std::string::npos) return true;
        return false;
    }
};

// Un piccolo insieme di identificatori di piattaforma per esercitare la
// diagnostica "nomina la piattaforma" su valori diversi (Req 11.5).
const std::vector<std::string>& platformIds() {
    static const std::vector<std::string> kIds = {
        "macos-arm64", "ios-arm64", "windows-x64", "android-arm64",
    };
    return kIds;
}

pulse::loader::RuntimeContext makeCtx(const std::string& platformId) {
    pulse::loader::RuntimeContext ctx;
    ctx.gdVersion = pulse::loader::GdVersion{2, 2081};
    ctx.platform = pulse::loader::Platform::IOSArm64;
    ctx.platformId = platformId;
    return ctx;
}

std::filesystem::path tempRollbackPath(int salt) {
    return std::filesystem::path(::testing::TempDir()) /
           ("pulse_eml_p24_" + std::to_string(salt) + ".rbk");
}

// Snapshot dei byte "live" di tutti i bersagli seminati: l'oracolo
// dell'invarianza byte-per-byte (Req 9.3). L'invarianza ⟺ zero install.
std::vector<FakeBackend::Bytes> snapshotLive(
    const FakeBackend& backend, const std::vector<std::uintptr_t>& targets) {
    std::vector<FakeBackend::Bytes> out;
    out.reserve(targets.size());
    for (std::uintptr_t t : targets) {
        const auto live = backend.liveBytes(t);
        RC_ASSERT(live.has_value());
        out.push_back(*live);
    }
    return out;
}

// --- Property 24 — fail-open Module_Loader non disponibile, byte invariati --
// Feature: external-mod-loading, Property 24.
// Validates: Requirements 9.3, 11.5.
RC_GTEST_PROP(EmlProperty24ModuleLoaderUnavailableByteInvariance,
              UnavailableLoaderLoadsZeroModsAndLeavesBytesUnchanged,
              ()) {
    resetRegistry();

    // --- "albero di file finto": batch arbitrario di mod con Mod_Id distinti --
    const int numMods = *rc::gen::inRange(0, 8);  // 0..7 mod individuate

    // Scenario: false = Module_Loader non disponibile (Req 11.5); true =
    // disponibile ma NESSUNA mod termina Enabled (Req 9.3, stessa invarianza).
    const bool loaderAvailable = *rc::gen::arbitrary<bool>();

    // Quando il loader è disponibile, ogni mod deve comunque NON raggiungere
    // Enabled: si sceglie per mod tra "modulo non caricabile" e "entry point in
    // errore" — entrambi senza alcun install (byte invariati).
    const std::string platformId =
        platformIds()[static_cast<std::size_t>(
            *rc::gen::inRange<int>(0, static_cast<int>(platformIds().size())))];

    FakeModuleLoader moduleLoader;
    moduleLoader.setAvailable(loaderAvailable);
    FakeBackend backend;
    HookOwnershipLedger ledger;
    CapturingSink sink;
    RollbackStore rollback(tempRollbackPath(*rc::gen::inRange(0, 1 << 20)));

    // --- byte modellati delle funzioni bersaglio (eseguibile/asset) ----------
    // Si seminano alcune regioni con byte originali arbitrari: rappresentano lo
    // stato "prima" della GD_Installation. Un install (che NON deve avvenire) ne
    // muterebbe i byte live.
    const int numTargets = *rc::gen::inRange(1, 5);
    std::vector<std::uintptr_t> targets;
    targets.reserve(static_cast<std::size_t>(numTargets));
    for (int i = 0; i < numTargets; ++i) {
        const auto addr =
            static_cast<std::uintptr_t>(0x1000 + (i + 1) * 0x40);
        const auto len = static_cast<std::size_t>(
            pulse::lifecycle::kRollbackPrologueBytes);
        FakeBackend::Bytes original(len);
        for (std::size_t b = 0; b < len; ++b) {
            original[b] = static_cast<FakeBackend::Byte>(
                *rc::gen::inRange<int>(0, 256));
        }
        backend.seedOriginal(addr, std::move(original));
        targets.push_back(addr);
    }

    // Resolver: ogni simbolo è risolto a uno dei bersagli seminati, così, se
    // gli install avvenissero, toccherebbero proprio questi byte (l'invarianza
    // diventa significativa: ciò che la preserva è l'ASSENZA di install).
    auto resolver = [targets](std::string_view symbol) -> void* {
        if (targets.empty()) return nullptr;
        std::size_t h = 0;
        for (char c : symbol) h = h * 131u + static_cast<unsigned char>(c);
        return reinterpret_cast<void*>(targets[h % targets.size()]);
    };

    // Invocatore dell'entry point: registra un PULSE_HOOK (come farebbe il
    // Mod_Module reale allo static-init) e poi, nello scenario "disponibile",
    // FALLISCE per metà delle mod così che nessuna raggiunga Enabled senza
    // dipendere da un install.
    std::vector<int> detourStorage;
    std::vector<void*> trampSlots;
    detourStorage.reserve(64);
    trampSlots.reserve(64);
    auto invoker = [&detourStorage, &trampSlots](
                       const ModId& modId, void*) -> EntryPointOutcome {
        detourStorage.push_back(0);
        trampSlots.push_back(nullptr);
        void* detour = static_cast<void*>(&detourStorage.back());
        void** tramp = &trampSlots.back();
        pulse::hooks::register_hook("sym." + modId, detour, tramp);
        // L'entry point fallisce sempre: nello scenario "disponibile" garantisce
        // che la mod NON raggiunga Enabled (Disabled con rollback byte-esatto),
        // quindi zero install persistiti.
        return EntryPointOutcome::failure("entry point forzato a fallire (P24)");
    };

    ModManagerWiring wiring(moduleLoader, backend, rollback, ledger,
                            makeCtx(platformId), resolver, invoker, sink.sink());
    ModManager manager;

    std::vector<ModId> order;
    order.reserve(static_cast<std::size_t>(numMods));
    for (int i = 0; i < numMods; ++i) {
        ModId mod = "mod." + std::to_string(i);  // distinti per costruzione
        const Bytes image{static_cast<std::uint8_t>(i + 1)};
        // Quando il loader è disponibile, una parte delle mod ha un modulo non
        // caricabile (failLoad) e il resto un entry point in errore: in entrambi
        // i casi la mod NON raggiunge Enabled.
        FakeModuleLoader::ModuleSpec spec;
        spec.failLoad = loaderAvailable && (i % 2 == 0);
        moduleLoader.program(image, spec);
        wiring.registerMod(manager, ModWiringSpec{mod, image, "init"});
        order.push_back(std::move(mod));
    }

    // --- byte "prima" --------------------------------------------------------
    const std::vector<FakeBackend::Bytes> before = snapshotLive(backend, targets);

    // --- esecuzione fail-open (non deve lanciare) ----------------------------
    const auto result = wiring.runNoThrow(manager, order);

    // --- byte "dopo": INVARIANZA byte-per-byte (Req 9.3) ---------------------
    const std::vector<FakeBackend::Bytes> after = snapshotLive(backend, targets);
    RC_ASSERT(after == before);

    // Zero install sul backend, in OGNI scenario (l'invarianza ⟺ zero install).
    RC_ASSERT(backend.installedCount() == 0u);
    RC_ASSERT(backend.installAttempts() == 0u);

    // Nessuna mod nello stato Enabled e nessun hook attribuito ad alcuna mod.
    for (const ModId& mod : order) {
        RC_ASSERT(manager.stateOf(mod) != ModState::Enabled);
        RC_ASSERT(ledger.hooksOf(mod).empty());
    }

    if (!loaderAvailable) {
        // Fail-open Req 11.5: zero mod, nessun dlopen tentato, esito vuoto.
        RC_ASSERT(result.enabled.empty());
        RC_ASSERT(result.failed.empty());
        RC_ASSERT(moduleLoader.loadCount() == 0u);
        RC_ASSERT(!wiring.moduleLoaderAvailable());
        // Diagnostica che NOMINA la piattaforma del Runtime_Context e il loader.
        RC_ASSERT(sink.anyContains(platformId));
        RC_ASSERT(sink.anyContains(moduleLoader.name()));
    } else {
        // Loader disponibile ma nessuna mod Enabled: nessuna mod nell'insieme
        // delle abilitate (byte comunque invariati — stessa invarianza).
        RC_ASSERT(result.enabled.empty());
        RC_ASSERT(wiring.moduleLoaderAvailable());
    }
}

}  // namespace
