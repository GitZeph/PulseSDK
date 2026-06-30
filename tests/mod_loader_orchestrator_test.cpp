// tests/mod_loader_orchestrator_test.cpp — unit test dell'orchestratore
// `ModLoader` (task 7.22, Requisiti 1.1, 6.3, 9.6).
//
// Verifica la pipeline host-testabile end-to-end del `ModLoader`:
// discovery → dedup → validazione → compatibilità → risoluzione dipendenze →
// cablaggio ed enable via `ModManagerWiring`, oltre all'adattatore
// `asInitStep` verso il `CentralizedLoader` (RuntimeInitFn che riporta
// `installedHooks` e non lancia mai) e al seeding della demo interna sul Mod_Id
// riservato del ledger.
//
// Tutto è host-testabile sui seam: un `PackageOpener` iniettato apre container
// `.pulse` modellati come `PackageArchive` in memoria; `FakeModuleLoader`
// modella i simboli/byte dei Mod_Module senza dlopen reale; `FakeBackend`
// modella la memoria delle funzioni bersaglio (readOriginal/install/remove
// byte-esatti); resolver e invocatore dell'entry point sono iniettati. Nessun
// binario reale di GD.

#include "lifecycle/mod_loader.hpp"

#include <cstdint>
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
#include "lifecycle/manifest.hpp"
#include "lifecycle/module_loader.hpp"
#include "package/pulse_package.hpp"

namespace {

using pulse::hooking::FakeBackend;
using pulse::hooking::RollbackStore;
using pulse::lifecycle::Bytes;
using pulse::lifecycle::EntryPointOutcome;
using pulse::lifecycle::FakeModuleLoader;
using pulse::lifecycle::ModId;
using pulse::lifecycle::ModLoader;
using pulse::lifecycle::ModLoadOutcome;
using pulse::lifecycle::ModManagerWiring;

// Azzera il registro globale dello SDK (singleton di processo condiviso).
void resetRegistry() { pulse::hooks::registry().clear(); }

// Cattura le diagnostiche emesse dall'orchestratore.
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

// Runtime_Context fittizio: macOS arm64, GD 2.2081 (primo deliverable).
pulse::loader::RuntimeContext makeCtx() {
    pulse::loader::RuntimeContext ctx;
    ctx.gdVersion = pulse::loader::GdVersion{2, 2081};
    ctx.platform = pulse::loader::Platform::MacOS;
    ctx.platformId = "macos-arm64";
    return ctx;
}

std::filesystem::path tempRollbackPath(const std::string& name) {
    return std::filesystem::path(::testing::TempDir()) /
           ("pulse_orchestrator_" + name + ".rbk");
}

// Costruisce il testo `pulse.toml` di una mod nativa compatibile macOS/2.2081
// con un solo entry point e dipendenze opzionali.
std::string makeManifestToml(const std::string& id, const std::string& entry,
                             const std::vector<std::string>& deps = {},
                             bool compatible = true) {
    std::string toml;
    toml += "schema_version = 1\n\n";
    toml += "[mod]\n";
    toml += "id = \"" + id + "\"\n";
    toml += "version = \"1.0.0\"\n";
    toml += "name = \"" + id + "\"\n";
    toml += "type = \"native\"\n";
    toml += "\n[[entry_points]]\n";
    toml += "kind = \"init\"\n";
    toml += "symbol = \"" + entry + "\"\n";
    for (const std::string& d : deps) {
        toml += "\n[[dependencies]]\n";
        toml += "id = \"" + d + "\"\n";
        toml += "version = \">=1.0.0\"\n";
    }
    toml += "\n[permissions]\nrequired = []\n";
    toml += "\n[compat]\n";
    if (compatible) {
        toml += "platform = \"macos-arm64\"\n";
        toml += "gd_min = \"2.2081.0\"\n";
        toml += "gd_max = \"2.2081.0\"\n";
    } else {
        // Piattaforma incompatibile → esclusione per VersionOrPlatformIncompatible.
        toml += "platform = \"windows\"\n";
        toml += "gd_min = \"2.2081.0\"\n";
        toml += "gd_max = \"2.2081.0\"\n";
    }
    return toml;
}

// Costruisce un `PackageArchive` (.pulse) con pulse.toml, code/module.pulsebin e
// MANIFEST.sha256 coerente, così `PulsePackage::open(verifyIntegrity=true)`
// riesce e il Mod_Manifest_Validator accetta la mod.
pulse::package::PackageArchive makeArchive(const std::string& toml,
                                           const Bytes& moduleImage) {
    pulse::package::PackageArchive ar;
    ar.addText(std::string(pulse::package::kManifestEntry), toml);
    ar.add("code/module.pulsebin", moduleImage);
    const std::string integrity =
        pulse::package::PulsePackage::buildIntegrityManifest(ar);
    ar.addText(std::string(pulse::package::kIntegrityEntry), integrity);
    return ar;
}

// Banco di prova: un filesystem virtuale di voci `.pulse` (nome → archivio) e i
// seam iniettati (lister/opener/resolver/invoker).
struct Harness {
    FakeModuleLoader moduleLoader;
    FakeBackend backend;
    CapturingSink sink;
    RollbackStore rollback;

    // entryName (".pulse") → archivio in memoria.
    std::unordered_map<std::string, pulse::package::PackageArchive> entries;
    // simbolo → indirizzo risolto (0 = non risolvibile).
    std::unordered_map<std::string, std::uintptr_t> resolved;
    // modId → simboli PULSE_HOOK registrati dall'entry point.
    std::unordered_map<ModId, std::vector<std::string>> hooksByMod;
    // storage stabile per detour/trampolini fittizi.
    std::vector<std::unique_ptr<int>> detourStorage;
    std::vector<std::unique_ptr<void*>> trampSlots;

    explicit Harness(const std::string& name) : rollback(tempRollbackPath(name)) {}

    // Aggiunge una mod nativa al filesystem virtuale + al FakeModuleLoader.
    void addMod(const std::string& entryName, const std::string& modId,
                const std::string& entrySymbol, const Bytes& image,
                const std::vector<std::pair<std::string, bool>>& hooks,
                const std::vector<std::string>& deps = {},
                bool compatible = true) {
        entries.emplace(entryName,
                        makeArchive(makeManifestToml(modId, entrySymbol, deps,
                                                     compatible),
                                    image));
        FakeModuleLoader::ModuleSpec spec;
        spec.exports.push_back({entrySymbol, {0x90, 0x90}});
        moduleLoader.program(image, spec);
        for (const auto& [sym, resolvable] : hooks) {
            hooksByMod[modId].push_back(sym);
            resolved[sym] = resolvable
                                ? (0x5000 + static_cast<std::uintptr_t>(
                                               std::hash<std::string>{}(sym) &
                                               0xFFFF))
                                : 0;
        }
    }

    // Opener: apre la voce dal filesystem virtuale (verifica integrità).
    pulse::lifecycle::PackageOpener makeOpener() {
        return [this](const std::filesystem::path& entryPath,
                      std::string_view entryName) -> pulse::package::OpenResult {
            (void)entryPath;
            auto it = entries.find(std::string(entryName));
            if (it == entries.end()) {
                pulse::package::OpenResult r;
                r.ok = false;
                r.error = pulse::package::OpenError::ManifestMissing;
                r.message = "voce non trovata nel fs virtuale";
                return r;
            }
            // Copia l'archivio: l'opener può essere invocato e l'archivio resta
            // disponibile per altri test/aperture.
            return pulse::package::PulsePackage::open(it->second);
        };
    }

    // Lister: enumera le sole voci di primo livello del fs virtuale.
    pulse::lifecycle::DirectoryLister makeLister() {
        return [this](const std::filesystem::path&)
                   -> pulse::lifecycle::DirectoryListing {
            pulse::lifecycle::DirectoryListing listing;
            listing.status = pulse::lifecycle::DirectoryReadStatus::Ok;
            for (const auto& [name, _] : entries) listing.entryNames.push_back(name);
            return listing;
        };
    }

    ModManagerWiring::SymbolResolver makeResolver() {
        return [this](std::string_view symbol) -> void* {
            auto it = resolved.find(std::string(symbol));
            if (it == resolved.end() || it->second == 0) return nullptr;
            return reinterpret_cast<void*>(it->second);
        };
    }

    // Invocatore dell'entry point: registra i PULSE_HOOK del modulo nel registro
    // globale (cadono nella finestra di epoca del mod). `fail` simula errore.
    ModManagerWiring::EntryPointInvoker makeInvoker(bool fail = false) {
        return [this, fail](const ModId& modId, void* entry) -> EntryPointOutcome {
            (void)entry;
            if (fail) return EntryPointOutcome::failure("entry point fallito (test)");
            for (const std::string& sym : hooksByMod[modId]) {
                detourStorage.push_back(std::make_unique<int>(0));
                trampSlots.push_back(std::make_unique<void*>(nullptr));
                pulse::hooks::register_hook(sym, detourStorage.back().get(),
                                            trampSlots.back().get());
            }
            return EntryPointOutcome::success();
        };
    }

    ModLoader makeLoader() {
        ModLoader loader(makeCtx(), moduleLoader, backend, rollback, sink.sink());
        loader.setDirectoryLister(makeLister());
        loader.setPackageOpener(makeOpener());
        loader.setSymbolResolver(makeResolver());
        loader.setEntryPointInvoker(makeInvoker());
        return loader;
    }
};

// --- Req 1.1 / 5 / 9.6: pipeline completa carica e installa gli hook ----------

TEST(ModLoaderOrchestrator, RunLoadsCompatibleModsAndInstallsHooks) {
    resetRegistry();
    Harness h("run_loads");
    h.addMod("a.pulse", "mod.a", "a_init", Bytes{0x01},
             {{"A::f", true}, {"A::g", true}});
    h.addMod("b.pulse", "mod.b", "b_init", Bytes{0x02}, {{"B::h", true}});

    ModLoader loader = h.makeLoader();
    const ModLoadOutcome outcome = loader.run("/virtual/mods");

    // Entrambe caricate (Req 1.1), nessuna esclusa/isolata.
    EXPECT_EQ(outcome.loaded.size(), 2u);
    EXPECT_TRUE(outcome.excluded.empty());
    EXPECT_TRUE(outcome.isolated.empty());

    // 3 hook risolti installati e attribuiti (Req 9.6); il backend ne conta 3.
    EXPECT_EQ(outcome.installedHooks, 3u);
    EXPECT_EQ(h.backend.installedCount(), 3u);
    EXPECT_EQ(loader.installedHooks(), 3u);
}

// --- Req 9.6: la demo interna è attribuita al Mod_Id riservato del ledger -----

TEST(ModLoaderOrchestrator, BuiltinDemoSeededOnReservedModId) {
    resetRegistry();
    // Simula la registrazione PULSE_HOOK preesistente della demo (static-init).
    int demoDetour = 0;
    void* demoTramp = nullptr;
    pulse::hooks::register_hook("MenuLayer_init", &demoDetour, &demoTramp);

    Harness h("demo_seed");
    ModLoader loader = h.makeLoader();
    loader.run("/virtual/mods");  // dir vuota: solo il seeding della demo

    // La registrazione [0, count()) è attribuita al Mod_Id riservato.
    EXPECT_EQ(loader.ledger().ownerOfIndex(0),
              std::string(pulse::lifecycle::kBuiltinDemoModId));
}

// --- Req 3: mod incompatibile esclusa (piattaforma diversa) -------------------

TEST(ModLoaderOrchestrator, IncompatibleModExcluded) {
    resetRegistry();
    Harness h("incompatible");
    h.addMod("ok.pulse", "mod.ok", "ok_init", Bytes{0x01}, {{"OK::f", true}});
    h.addMod("bad.pulse", "mod.bad", "bad_init", Bytes{0x02}, {{"BAD::f", true}},
             /*deps=*/{}, /*compatible=*/false);

    ModLoader loader = h.makeLoader();
    const ModLoadOutcome outcome = loader.run("/virtual/mods");

    EXPECT_EQ(outcome.loaded.size(), 1u);
    EXPECT_EQ(outcome.loaded.front(), "mod.ok");
    ASSERT_EQ(outcome.excluded.size(), 1u);
    EXPECT_EQ(outcome.excluded.front().modId, "mod.bad");
    EXPECT_EQ(outcome.excluded.front().cause,
              pulse::lifecycle::CauseCategory::VersionOrPlatformIncompatible);
}

// --- Req 4: dipendenza mancante esclude la mod dipendente ---------------------

TEST(ModLoaderOrchestrator, MissingDependencyExcluded) {
    resetRegistry();
    Harness h("missing_dep");
    // mod.dep dipende da "mod.absent", che non è presente nel fs virtuale.
    h.addMod("dep.pulse", "mod.dep", "dep_init", Bytes{0x01}, {{"D::f", true}},
             /*deps=*/{"mod.absent"});

    ModLoader loader = h.makeLoader();
    const ModLoadOutcome outcome = loader.run("/virtual/mods");

    EXPECT_TRUE(outcome.loaded.empty());
    ASSERT_FALSE(outcome.excluded.empty());
    EXPECT_EQ(outcome.excluded.front().modId, "mod.dep");
    EXPECT_EQ(outcome.excluded.front().cause,
              pulse::lifecycle::CauseCategory::DependencyUnsatisfied);
    EXPECT_EQ(outcome.installedHooks, 0u);
}

// --- Req 6: entry point in errore → mod isolata, le altre proseguono ----------

TEST(ModLoaderOrchestrator, FailingEntryPointIsolated) {
    resetRegistry();
    Harness h("entry_fail");
    h.addMod("good.pulse", "mod.good", "g_init", Bytes{0x01}, {{"G::f", true}});
    h.addMod("bad.pulse", "mod.bad", "b_init", Bytes{0x02}, {{"B::f", true}});

    ModLoader loader = h.makeLoader();
    // Invocatore che fa SEMPRE fallire l'entry point: entrambe falliscono.
    loader.setEntryPointInvoker(h.makeInvoker(/*fail=*/true));
    const ModLoadOutcome outcome = loader.run("/virtual/mods");

    EXPECT_TRUE(outcome.loaded.empty());
    EXPECT_EQ(outcome.isolated.size(), 2u);
    for (const auto& e : outcome.isolated) {
        EXPECT_EQ(e.cause, pulse::lifecycle::CauseCategory::EntryPointFailed);
    }
    // Nessun hook installato: fallimento isolato (Req 6), byte invariati.
    EXPECT_EQ(outcome.installedHooks, 0u);
    EXPECT_EQ(h.backend.installedCount(), 0u);
}

// --- Req 11.5 / 9.3: Module_Loader non disponibile → zero mod, byte invariati --

TEST(ModLoaderOrchestrator, ModuleLoaderUnavailableLoadsNothing) {
    resetRegistry();
    Harness h("module_unavailable");
    h.addMod("a.pulse", "mod.a", "a_init", Bytes{0x01}, {{"A::f", true}});
    h.moduleLoader.setAvailable(false);  // piattaforma non disponibile

    ModLoader loader = h.makeLoader();
    const ModLoadOutcome outcome = loader.run("/virtual/mods");

    EXPECT_TRUE(outcome.loaded.empty());
    EXPECT_EQ(outcome.installedHooks, 0u);
    EXPECT_EQ(h.backend.installedCount(), 0u);
}

// --- Req 6.3 / 9.6: asInitStep non lancia mai e riporta installedHooks ---------

TEST(ModLoaderOrchestrator, AsInitStepReportsHooksAndNeverThrows) {
    resetRegistry();
    Harness h("init_step");
    h.addMod("a.pulse", "mod.a", "a_init", Bytes{0x01},
             {{"A::f", true}, {"A::g", true}});

    ModLoader loader = h.makeLoader();
    pulse::loader::RuntimeInitFn step = loader.asInitStep("/virtual/mods");

    // WatchdogToken con scadenza lontana (lo step non lo usa comunque).
    pulse::loader::WatchdogToken token(
        []() { return std::chrono::steady_clock::now(); },
        std::chrono::steady_clock::now() + std::chrono::seconds(10));

    pulse::loader::RuntimeInitResult result = step(token);

    // Esito fail-open "mod caricate" con installedHooks riportato (Req 9.6).
    EXPECT_TRUE(result.modsLoaded);
    EXPECT_EQ(result.installedHooks, 2u);
    EXPECT_EQ(loader.installedHooks(), 2u);
}

// --- Req 8: teardown rimuove tutti gli hook (byte-esatto) ---------------------

TEST(ModLoaderOrchestrator, TeardownRemovesAllHooks) {
    resetRegistry();
    Harness h("teardown");
    h.addMod("a.pulse", "mod.a", "a_init", Bytes{0x01}, {{"A::f", true}});
    h.addMod("b.pulse", "mod.b", "b_init", Bytes{0x02},
             {{"B::g", true}, {"B::h", true}});

    ModLoader loader = h.makeLoader();
    const ModLoadOutcome outcome = loader.run("/virtual/mods");
    ASSERT_EQ(outcome.installedHooks, 3u);
    ASSERT_EQ(h.backend.installedCount(), 3u);

    loader.teardown();

    // Tutti gli hook rimossi (byte-esatto), zero residui (Req 8.6).
    EXPECT_EQ(h.backend.installedCount(), 0u);
    EXPECT_EQ(loader.installedHooks(), 0u);
}

// --- Req 1.6 / 1.7: dedup deterministica per Mod_Id ---------------------------

TEST(ModLoaderOrchestrator, DuplicateModIdDeduped) {
    resetRegistry();
    Harness h("dedup");
    // Due voci con lo STESSO Mod_Id: vince la voce lessicograficamente minore.
    h.addMod("a-first.pulse", "mod.dup", "init1", Bytes{0x01}, {{"D::f", true}});
    h.addMod("z-second.pulse", "mod.dup", "init2", Bytes{0x02}, {{"D::g", true}});

    ModLoader loader = h.makeLoader();
    const ModLoadOutcome outcome = loader.run("/virtual/mods");

    // Un solo Mod_Id caricato (dedup), una sola entry vincente.
    EXPECT_EQ(outcome.loaded.size(), 1u);
    EXPECT_EQ(outcome.loaded.front(), "mod.dup");
}

}  // namespace
