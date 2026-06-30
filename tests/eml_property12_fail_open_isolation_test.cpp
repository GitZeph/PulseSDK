// tests/eml_property12_fail_open_isolation_test.cpp
// Feature: external-mod-loading, Property 12 — Isolamento fail-open senza
// propagazione di eccezioni.
// Validates: Requirements 6.1, 6.3, 6.4, 6.6 (Requisiti 6.1, 6.3, 6.4, 6.6)
//
// Property 12 (design.md §"Property 12"): per OGNI fallimento iniettato in
// QUALUNQUE stadio e per QUALUNQUE numero di mod fallite, l'esecuzione del
// Mod_Loader (la barriera no-throw `ModManagerWiring::runNoThrow`):
//   (a) NON propaga alcuna eccezione fuori dalla barriera (Req 6.3, 6.4);
//   (b) carica e abilita TUTTE le mod valide restanti, indipendentemente dal
//       numero di mod fallite (Req 6.1, 6.6);
//   (c) produce una diagnostica per OGNI fallimento, con il Mod_Id quando
//       disponibile (Req 6.4).
//
// Il comportamento è host-testabile sul cablaggio reale `ModManagerWiring`
// (loader/lifecycle/mod_loader.hpp, task 7.7): `runNoThrow` è `noexcept` e
// confina ogni fallimento — caricamento del modulo, entry point in errore o che
// lancia, eccezione in uno stadio per-mod (resolver/`resolve_all`) — alla sola
// mod, lasciando proseguire le altre. I seam host-testabili (`FakeModuleLoader`,
// `FakeBackend`, resolver/invocatore iniettati) evitano un binario reale di GD;
// il path `dlopen` reale è coperto in Fase E, non da PBT.
//
// Strategia (RapidCheck, ≥100 iterazioni di default — RC_GTEST_PROP):
//   * si genera un batch di mod con Mod_Id DISTINTI; ogni mod cade in una
//     categoria che modella uno stadio di (eventuale) fallimento iniettato:
//       - Valid         → modulo caricabile, entry risolvibile, invocatore ok:
//                          la mod raggiunge Enabled e i suoi hook risolti sono
//                          installati;
//       - FailLoad      → caricamento del Mod_Module simulato fallito
//                          (ModuleNotLoadable): mod isolata (Disabled), 0 hook;
//       - EntryError    → l'invocatore dell'entry point restituisce errore: mod
//                          isolata (Disabled), 0 hook;
//       - EntryThrow    → l'invocatore dell'entry point LANCIA un'eccezione:
//                          confinata, mod isolata (Disabled), 0 hook;
//       - ResolverThrow → uno stadio per-mod lancia (il resolver dei simboli in
//                          `resolve_all`): confinata, mod isolata, 0 hook;
//   * tutte le mod sono registrate nel `ModManager` via il wiring e si esegue
//     `runNoThrow` sull'intero batch nell'ordine di caricamento;
//   * un oracolo indipendente dal wiring calcola l'insieme atteso di mod Valide
//     (→ Enabled) e di mod fallite (→ Disabled).
//
// Asserzioni chiave:
//   (1) `runNoThrow` non propaga eccezioni (è `noexcept`; l'iterazione completa);
//   (2) l'insieme delle mod abilitate coincide con le sole mod Valide, e ognuna
//       è in stato Enabled con i suoi hook installati (Req 6.1, 6.6);
//   (3) ogni mod fallita è isolata (Disabled), con 0 hook attribuiti, e produce
//       una diagnostica che ne nomina il Mod_Id (Req 6.4);
//   (4) gli hook installati nel backend sono esattamente l'unione degli hook
//       delle mod Valide: i fallimenti isolati non aggiungono/rimuovono hook
//       altrui (Req 6.6).
//
// Header del loader in loader/lifecycle/ (include relativo alla radice loader/);
// la logica è in mod_loader.cpp/mod_manager.cpp/hook_ownership.cpp (compilate in
// pulse::loader via glob lifecycle/*.cpp). Integrazione RapidCheck+GoogleTest in
// extras/gtest.

#include "lifecycle/mod_loader.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

// Azzera il registro globale dello SDK: singleton di processo condiviso fra le
// unità di traduzione; ogni iterazione deve partire da uno stato noto.
void resetRegistry() { pulse::hooks::registry().clear(); }

// Runtime_Context fittizio (macOS arm64, GD 2.2081).
pulse::loader::RuntimeContext makeCtx() {
    pulse::loader::RuntimeContext ctx;
    ctx.gdVersion = pulse::loader::GdVersion{2, 2081};
    ctx.platform = pulse::loader::Platform::MacOS;
    ctx.platformId = "macos-arm64";
    return ctx;
}

// Percorso temporaneo unico per il RollbackStore di ogni iterazione (il file di
// rollback è write-through: serve un percorso distinto per iterazione).
std::filesystem::path uniqueRollbackPath() {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
    return std::filesystem::path(::testing::TempDir()) /
           ("pulse_p12_" + std::to_string(n) + ".rbk");
}

// Categoria di una mod del batch: stadio di (eventuale) fallimento iniettato.
enum class Category {
    Valid,          // nessun fallimento → Enabled, hook installati
    FailLoad,       // caricamento del Mod_Module fallito → Disabled, 0 hook
    EntryError,     // entry point restituisce errore → Disabled, 0 hook
    EntryThrow,     // entry point LANCIA → confinato, Disabled, 0 hook
    ResolverThrow,  // stadio per-mod (resolve_all) LANCIA → confinato, Disabled
};

struct ModUnderTest {
    ModId modId;
    std::string entrySymbol;
    Bytes moduleImage;
    Category category{Category::Valid};
    int hookCount{0};  // numero di PULSE_HOOK risolvibili dichiarati dalla mod
};

// Banco di prova: instrada il caricamento del modulo, l'invocazione dell'entry
// point e la risoluzione dei simboli, iniettando i fallimenti per categoria.
struct Harness {
    FakeModuleLoader moduleLoader;
    FakeBackend backend;
    HookOwnershipLedger ledger;
    RollbackStore rollback;

    // simbolo → indirizzo risolto (per i PULSE_HOOK della mod).
    std::unordered_map<std::string, std::uintptr_t> resolved;
    // modId → simboli registrati all'enable.
    std::unordered_map<ModId, std::vector<std::string>> hooksByMod;
    // modId → categoria di comportamento dell'invocatore dell'entry point.
    std::unordered_map<ModId, Category> behaviorByMod;
    // simboli su cui il resolver deve lanciare (una sola volta ciascuno).
    std::unordered_set<std::string> throwSymbols;
    std::unordered_set<std::string> alreadyThrew;
    // oracolo indipendente: invocazioni dell'entry point per mod.
    std::unordered_map<ModId, int> invocations;
    // storage stabile per detour/trampolini registrati.
    std::deque<int> detourStorage;
    std::deque<void*> trampSlots;

    Harness() : rollback(uniqueRollbackPath()) {}

    // Programma una mod nel FakeModuleLoader e registra i suoi hook risolvibili.
    void programMod(const ModUnderTest& mod) {
        FakeModuleLoader::ModuleSpec spec;
        if (mod.category == Category::FailLoad) {
            spec.failLoad = true;  // caricamento del Mod_Module simulato fallito
        } else {
            // Entry point esportato (risolvibile) per tutte le altre categorie:
            // il fallimento è iniettato a uno stadio successivo (invocatore /
            // resolver), non sulla risoluzione dell'entry point.
            spec.exports.push_back({mod.entrySymbol, {0x90, 0x90}});
        }
        moduleLoader.program(mod.moduleImage, spec);

        behaviorByMod[mod.modId] = mod.category;

        // Ogni mod dichiara `hookCount` PULSE_HOOK risolvibili. Per la categoria
        // ResolverThrow, il primo simbolo è marcato come "lancia nel resolver".
        for (int h = 0; h < mod.hookCount; ++h) {
            const std::string sym = mod.modId + "::f" + std::to_string(h);
            hooksByMod[mod.modId].push_back(sym);
            resolved[sym] =
                0x4000 + static_cast<std::uintptr_t>(
                             std::hash<std::string>{}(sym) & 0xFFFF);
            if (mod.category == Category::ResolverThrow && h == 0) {
                throwSymbols.insert(sym);
            }
        }
    }

    ModManagerWiring::SymbolResolver makeResolver() {
        return [this](std::string_view symbol) -> void* {
            const std::string s(symbol);
            // Inietta un'eccezione in uno stadio per-mod (resolve_all percorre
            // l'intero registro): lancia UNA sola volta per simbolo, così le mod
            // successive possono risolvere il registro senza ri-lanciare sui
            // simboli residui delle mod già isolate.
            if (throwSymbols.count(s) && alreadyThrew.find(s) == alreadyThrew.end()) {
                alreadyThrew.insert(s);
                throw std::runtime_error("resolver: eccezione iniettata su '" + s +
                                         "'");
            }
            auto it = resolved.find(s);
            if (it == resolved.end() || it->second == 0) return nullptr;
            return reinterpret_cast<void*>(it->second);
        };
    }

    // Invocatore dell'entry point: conta l'invocazione e inietta il fallimento
    // per categoria. Per Valid/ResolverThrow registra i PULSE_HOOK del modulo
    // nella finestra di epoca (per ResolverThrow il fallimento scatta dopo, in
    // resolve_all).
    ModManagerWiring::EntryPointInvoker makeInvoker() {
        return [this](const ModId& modId, void* entry) -> EntryPointOutcome {
            (void)entry;
            invocations[modId] += 1;
            const Category c = behaviorByMod[modId];
            if (c == Category::EntryThrow) {
                throw std::runtime_error("entry point: eccezione iniettata per '" +
                                         modId + "'");
            }
            if (c == Category::EntryError) {
                return EntryPointOutcome::failure("entry point fallito (test)");
            }
            // Valid o ResolverThrow: registra i PULSE_HOOK del Mod_Module.
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

// --- Property 12 — isolamento fail-open senza propagazione di eccezioni ------
// Feature: external-mod-loading, Property 12.
// Validates: Requirements 6.1, 6.3, 6.4, 6.6.
RC_GTEST_PROP(EmlProperty12FailOpenIsolation,
              FailuresIsolatedNoThrowAllValidModsLoadedDiagnosticsPerFailure,
              ()) {
    resetRegistry();

    const int count = *rc::gen::inRange(0, 13);

    Harness h;
    std::vector<ModUnderTest> mods;
    std::vector<ModId> loadOrder;

    // Oracolo indipendente dal wiring.
    std::set<ModId> expectedEnabled;  // sole mod Valide
    std::set<ModId> expectedFailed;   // mod isolate (qualunque stadio)
    std::size_t expectedInstalledHooks = 0;  // unione degli hook delle mod Valide

    for (int i = 0; i < count; ++i) {
        const std::string idx = std::to_string(i);
        const int catPick = *rc::gen::inRange(0, 5);
        const Category category = static_cast<Category>(catPick);

        ModUnderTest mod;
        mod.modId = "mod." + idx;
        mod.entrySymbol = "entry_" + idx;
        // Byte del Mod_Module distinti per mod (match esatto nel fake).
        mod.moduleImage = Bytes{static_cast<std::uint8_t>(0xC0 + (i & 0x0F)),
                                static_cast<std::uint8_t>((i >> 4) & 0xFF),
                                static_cast<std::uint8_t>(i & 0xFF)};
        mod.category = category;
        // 1..3 hook risolvibili per mod (ResolverThrow ne richiede ≥1 per il
        // simbolo che lancia; le Valide installano esattamente questo numero).
        mod.hookCount = *rc::gen::inRange(1, 4);

        switch (category) {
            case Category::Valid:
                expectedEnabled.insert(mod.modId);
                expectedInstalledHooks += static_cast<std::size_t>(mod.hookCount);
                break;
            case Category::FailLoad:
            case Category::EntryError:
            case Category::EntryThrow:
            case Category::ResolverThrow:
                expectedFailed.insert(mod.modId);
                break;
        }

        h.programMod(mod);
        loadOrder.push_back(mod.modId);
        mods.push_back(std::move(mod));
    }

    // Sink che cattura le diagnostiche per verificare l'attribuzione al Mod_Id.
    std::vector<std::string> messages;
    pulse::loader::DiagnosticSink sink =
        [&messages](std::string_view m) { messages.emplace_back(m); };

    ModManagerWiring wiring(h.moduleLoader, h.backend, h.rollback, h.ledger,
                            makeCtx(), h.makeResolver(), h.makeInvoker(), sink);
    ModManager manager;
    for (const ModUnderTest& mod : mods) {
        wiring.registerMod(manager, ModWiringSpec{mod.modId, mod.moduleImage,
                                                  mod.entrySymbol});
    }

    // --- (1) Nessuna eccezione propagata: `runNoThrow` è `noexcept` e
    //         l'iterazione completa anche con fallimenti in qualunque stadio. --
    const auto result = wiring.runNoThrow(manager, loadOrder);

    // --- (2) Tutte e sole le mod Valide sono abilitate, indipendentemente dal
    //         numero di mod fallite (Req 6.1, 6.6). ----------------------------
    std::set<ModId> actuallyEnabled(result.enabled.begin(), result.enabled.end());
    RC_ASSERT(actuallyEnabled == expectedEnabled);

    std::set<ModId> actuallyFailed;
    for (const auto& f : result.failed) actuallyFailed.insert(f.mod);
    RC_ASSERT(actuallyFailed == expectedFailed);

    // Ogni mod Valida è Enabled con i suoi hook attribuiti/installati.
    for (const ModUnderTest& mod : mods) {
        if (mod.category == Category::Valid) {
            RC_ASSERT(manager.stateOf(mod.modId) == ModState::Enabled);
            RC_ASSERT(h.ledger.hooksOf(mod.modId).size() ==
                      static_cast<std::size_t>(mod.hookCount));
        } else {
            // --- (3) Ogni mod fallita è isolata (Disabled), 0 hook attribuiti,
            //         e produce una diagnostica che ne nomina il Mod_Id. -------
            RC_ASSERT(manager.stateOf(mod.modId) == ModState::Disabled);
            RC_ASSERT(h.ledger.hooksOf(mod.modId).empty());
            bool diagMentionsMod = false;
            for (const std::string& m : messages) {
                if (m.find(mod.modId) != std::string::npos) {
                    diagMentionsMod = true;
                    break;
                }
            }
            RC_ASSERT(diagMentionsMod);
        }
    }

    // --- (4) Gli hook installati nel backend sono esattamente l'unione degli
    //         hook delle mod Valide: i fallimenti isolati non aggiungono né
    //         rimuovono hook altrui (Req 6.6). ----------------------------------
    RC_ASSERT(h.backend.installedCount() == expectedInstalledHooks);
}

}  // namespace
