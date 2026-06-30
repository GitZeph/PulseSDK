// loader/core/runtime_entry.cpp — definizione del simbolo C esportato
// `pulse_loader_runtime_entry` (Requisito 1.2) e cablaggio reale del runtime.
//
// L'intera unità è compilata SOLO quando il loader è costruito come
// Loader_Artifact dinamico (`PULSE_BUILD_LOADER_ARTIFACT=ON` → macro
// `PULSE_LOADER_ARTIFACT=1`). Nella build statica per i test host la macro non
// è definita e il file compila a vuoto, così l'output statico non espone alcun
// simbolo aggiuntivo e i test host restano invariati (Req 1.3).
//
// Questo è il punto di ingresso unico e centralizzato del runtime: quando il
// `.dylib` è iniettato in Geometry Dash, orchestra i componenti già esistenti
// (detection → bindings → install dell'hook via Dobby → firing del detour demo)
// RIUSANDOLI, senza reimplementarli. La policy è fail-open: nessuna eccezione
// propagata, nessun abort; si restituisce sempre `true` così GD prosegue anche
// su qualsiasi fallimento, e le cause vengono loggate sul diagnostic sink.
#if defined(PULSE_LOADER_ARTIFACT)

#include "pulse_loader/runtime_entry.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

// Su Apple serve l'header REALMENTE mappato dell'eseguibile principale per
// ribasare l'RVA del `.pbind` sullo slide ASLR (vedi rebase più sotto).
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <pulse/hooks.hpp>

#include <filesystem>

#include "bindings/bindings.hpp"
#include "bindings/embedded_bindings_provider.hpp"
#include "bindings/pbind_format.hpp"
#include "core/centralized_loader.hpp"
#include "core/loader_core.hpp"
#include "core/runtime_context.hpp"
#include "core/version_detector.hpp"
#include "hooking/dobby_backend.hpp"
#include "hooking/hook_chain_registry.hpp"
#include "hooking/hook_gate.hpp"
#include "hooking/registry_grouping.hpp"
#include "hooking/rollback_store.hpp"
#include "lifecycle/hook_ownership.hpp"
#include "lifecycle/mod_loader.hpp"
#include "lifecycle/module_loader.hpp"
#include "mvp/menulayer_init_hook.hpp"
#include "package/pulse_zip_reader.hpp"

// Header generato da CMake che incorpora il `.pbind` verificato a compile time
// (nessun lookup su file a runtime). Espone `pulse::loader::kEmbeddedPbind`.
// L'include è guardato come tutto il resto da PULSE_LOADER_ARTIFACT.
#include "pulse_embedded_pbind.hpp"

namespace {

// Base REALE in memoria dell'eseguibile principale (l'immagine di Geometry
// Dash, indice 0 in dyld). Su Apple `_dyld_get_image_header(0)` restituisce
// l'header gia' mappato, ovvero (base_preferita + slide ASLR): sommandovi
// l'RVA del `.pbind` si ottiene l'indirizzo assoluto della funzione, valido
// sotto ASLR. Su piattaforme non-Apple non c'e' rebase (l'offset e' trattato
// come gia' assoluto, comportamento storico invariato).
std::uintptr_t main_image_base() noexcept {
#if defined(__APPLE__)
    return reinterpret_cast<std::uintptr_t>(_dyld_get_image_header(0));
#else
    return 0;
#endif
}

// Mods_Directory dell'installazione: per convenzione una cartella `mods`
// accanto all'eseguibile principale (l'immagine di Geometry Dash). La discovery
// gestisce in modo fail-open la directory assente/vuota (zero mod, GD prosegue),
// quindi un percorso non esistente non è un errore.
std::filesystem::path resolve_mods_directory() {
#if defined(__APPLE__)
    const char* imagePath = _dyld_get_image_name(0);
    if (imagePath != nullptr) {
        std::error_code ec;
        std::filesystem::path exe(imagePath);
        std::filesystem::path dir = exe.parent_path() / "mods";
        (void)ec;
        return dir;
    }
#endif
    return std::filesystem::path("mods");
}

}  // namespace

extern "C" PULSE_LOADER_EXPORT bool pulse_loader_runtime_entry(void) {
    using namespace pulse::loader;

    // Sink diagnostico del Loader Core: le cause dei fallimenti sono loggate
    // qui (default: stderr). Il lambda `log` evita ripetizioni.
    DiagnosticSink sink = default_diagnostic_sink();
    const auto log = [&sink](const std::string& message) {
        if (sink) {
            sink(message);
        }
    };

    // Fail-open assoluto: qualunque eccezione è catturata e loggata; il runtime
    // restituisce comunque `true` così Geometry Dash continua senza mod.
    try {
        // 1) Garantisce la registrazione del detour dimostrativo PULSE_HOOK
        //    (link della TU del hook anche da artefatto): registra
        //    "MenuLayer_init" presso il registro dello SDK.
        mvp::ensure_menulayer_init_hook_registered();

        // Cabla il sink di log del detour dimostrativo. IMPORTANTE: il detour
        // viene eseguito QUANDO MenuLayer::init gira (caricamento del menu),
        // cioè DOPO il ritorno di questa funzione. Si passa quindi un sink per
        // VALORE che resta valido per tutta la vita del processo (no riferimenti
        // a variabili locali). Senza questo, i log `[pulse.demo]` del detour
        // sarebbero silenziati e il firing non sarebbe osservabile.
        mvp::set_demo_log_sink(default_diagnostic_sink());

        // 2) Costruzione del provider embedded dal `.pbind` verificato.
        //    DECISIONE DI INGEGNERIA: la GD_Version è presa dal `.pbind`
        //    SPEDITO (non da una firma section-hash), perché una tabella di
        //    firme section-hash per 2.2081 non è disponibile; ci fidiamo del
        //    Binding_Set_File verificato per la versione. La PIATTAFORMA è
        //    invece derivata dal Mach-O realmente caricato via
        //    `DefaultVersionDetector::detected_platform_id()`, e deve combaciare
        //    con la piattaforma dichiarata nel set, altrimenti si fallisce.
        bindings::EmbeddedBindingsProvider provider;

        const auto parsed = bindings::parse_pbind(kEmbeddedPbind);
        if (!parsed.ok()) {
            const std::string cause =
                parsed.error ? parsed.error->message : std::string{"sconosciuta"};
            log(std::string{"runtime: .pbind embedded non parsabile ("} + cause +
                "); fail-open, zero hook");
            return true;
        }
        const bindings::BindingSet& set = *parsed.value;

        DefaultVersionDetector detector;
        const std::string detectedPlatform = detector.detected_platform_id();
        if (detectedPlatform != set.key().platformId) {
            log(std::string{"runtime: piattaforma rilevata dal Mach-O '"} +
                detectedPlatform + "' diversa da quella del set embedded '" +
                set.key().platformId + "'; fail-open, zero hook");
            return true;
        }

        // Carica il set ESATTO usando la chiave del set stesso (versione dal
        // `.pbind`, piattaforma dal parse). `addVerifiedSet` applica la verifica
        // "resolved sse verificato": con offset placeholder/`verified=false` il
        // binding resterà NON risolto (comportamento atteso, Req 4.2/4.3).
        provider.addVerifiedSet(set);
        const bindings::BindingKey& key = set.key();
        log(std::string{"runtime: bindings per (GD "} +
            std::to_string(key.version.major) + "." +
            std::to_string(key.version.minor) + ", " + key.platformId +
            ") derivati dal .pbind verificato (versione dal .pbind, piattaforma "
            "dal Mach-O)");
        if (!provider.load(key)) {
            log("runtime: nessun set di bindings per la coppia esatta; "
                "fail-open, zero hook");
            return true;
        }

        // 3) Runtime_Context rilevato dal Mach-O (Req 1.5): GD_Version dal
        //    `.pbind` verificato, piattaforma dal Mach-O caricato. È il contesto
        //    con cui il Mod_Loader valuta la compatibilità delle mod esterne
        //    (Req 3) e persiste i record di rollback (Req 8.3).
        RuntimeContext ctx;
        ctx.gdVersion = GdVersion{static_cast<std::uint32_t>(key.version.major),
                                  static_cast<std::uint32_t>(key.version.minor)};
        ctx.platform = current_platform();
        ctx.platformId = key.platformId;

        // 4) Collaboratori RIUSATI dal Mod_Loader (Layer 3 / seam di
        //    piattaforma): backend di hooking Dobby, RollbackStore persistente,
        //    Module_Loader della piattaforma corrente. Vivono per tutta la
        //    durata di questo entry (gli hook installati persistono nel processo
        //    via le patch di Dobby e il registro statico dello SDK).
        auto backend = pulse::hooking::make_dobby_backend();
        pulse::hooking::RollbackStore rollback{
            std::filesystem::temp_directory_path() / "pulse_runtime_rollback.rbk"};
        auto moduleLoader = pulse::lifecycle::make_platform_module_loader();

        // 5) Mod_Loader: orchestratore della pipeline delle mod esterne. Il
        //    resolver dei simboli risolve i bersagli del Runtime_Context contro
        //    il provider `.pbind` verificato, ribasati sullo slide ASLR
        //    dell'immagine principale; gli indirizzi non risolti restano tali
        //    (0 hook su non risolti, Req 5.7/9.1).
        const std::uintptr_t imageBase = main_image_base();
        pulse::lifecycle::ModLoader modLoader(ctx, *moduleLoader, *backend,
                                              rollback, sink);
        modLoader.setSymbolResolver(
            [&provider, imageBase](std::string_view symbol) -> void* {
                const auto b = provider.resolve(std::string(symbol));
                if (!pulse::hooking::binding_is_installable(b)) return nullptr;
                return reinterpret_cast<void*>(imageBase + b->address);
            });

        // PackageOpener reale: legge un `.pulse` su disco (ZIP STORED) e lo apre
        // come PulsePackage. Senza opener la discovery scopre zero mod; con esso
        // le mod nella Mods_Directory diventano realmente caricabili (Fase E).
        modLoader.setPackageOpener(
            [](const std::filesystem::path& entryPath, std::string_view /*entryName*/)
                -> pulse::package::OpenResult {
                pulse::package::PulsePackage::Options opts;
                opts.verifyIntegrity = true;          // verifica MANIFEST.sha256 se presente
                opts.requireIntegrityFile = false;    // assenza non è errore (fail-open)
                return pulse::package::open_pulse_file(entryPath, opts);
            });

        const std::filesystem::path modsDir = resolve_mods_directory();

        // 5-bis) HookChainRegistry (Hook_Chaining, task 7.5): orchestratore di
        //    catena chain-aware. Sostituisce il punto di cablaggio in cui il
        //    runtime installava una singola `HookGate::install` per
        //    registrazione: ora i gruppi del registro SDK ordinati per
        //    Target_Address risolto sono passati alla Registry (primo anello →
        //    install reale verso l'Head_Thunk + Head_Thunk; successivi → relink
        //    dei vicini + attribuzione), così più Hook_Link sullo stesso
        //    indirizzo coesistono su UN'UNICA Underlying_Installation (Req 8,
        //    10.1) senza una seconda DobbyHook (nessun `codice -1`).
        //
        //    LIFETIME DI PROCESSO: la Registry possiede, per ogni
        //    Target_Address, la `HeadCell` (`currentHead`) e il Real_Trampoline
        //    letti dall'Head_Thunk al DISPATCH (quando MenuLayer::init gira,
        //    DOPO il ritorno di questo entry). Deve quindi sopravvivere a
        //    `pulse_loader_runtime_entry`: la allochiamo e la "leakiamo"
        //    intenzionalmente (singleton di processo, come gli effetti delle
        //    patch Dobby che persistono nella memoria di GD). I riferimenti a
        //    backend/rollback/ledger sono usati SOLO durante l'inserimento
        //    (load-time, dentro questo entry); il teardown ordinato della catena
        //    è cablato altrove (Req 7).
        auto* chainRegistry = new pulse::hooking::HookChainRegistry(
            *backend, rollback, modLoader.ledger(), sink);

        // Instrada ANCHE l'install/rimozione degli hook delle mod esterne
        // (pipeline `ModManagerWiring`) attraverso la STESSA catena: così la
        // demo interna e una mod esterna che hookano lo stesso `MenuLayer::init`
        // diventano due Hook_Link della medesima Hook_Chain sull'UNICA
        // Underlying_Installation (il primo anello crea l'install + Head_Thunk,
        // il secondo solo ri-cabla i vicini) — nessuna seconda DobbyHook, nessun
        // `codice -1` (Req 8.1-8.4). Senza questo, la pipeline esterna
        // installerebbe un secondo DobbyHook diretto sullo stesso indirizzo e
        // confliggerebbe con la catena della demo.
        modLoader.setChainRegistry(chainRegistry);

        // 6) Init step verso il CentralizedLoader (Req 1.1, 6.3): installa la
        //    demo interna (attribuita al Mod_Id riservato del ledger, così
        //    l'osservabilità end-to-end di `pulse-gd-integration` resta valida)
        //    e poi esegue la pipeline delle mod esterne via `ModLoader::run`. È
        //    composto con `ModLoader::asInitStep` per la parte esterna e NON
        //    lancia mai; riporta `installedHooks` al termine (Req 9.6). Passato
        //    al CentralizedLoader, eredita watchdog/timeout (10 s) e la
        //    degradazione "GD senza mod".
        auto externalInit = modLoader.asInitStep(modsDir);
        RuntimeInitFn initStep =
            [&, externalInit](const WatchdogToken& wd) -> RuntimeInitResult {
            // (a) Install chain-aware del registro SDK via HookChainRegistry
            //     (task 7.5). Invece di una singola `HookGate::install` per
            //     registrazione, si raggruppa l'INTERO registro dello SDK per
            //     Target_Address risolto e si passa ciascun gruppo ordinato
            //     (Chain_Order) alla Registry: il PRIMO anello di ogni target
            //     crea l'unica Underlying_Installation verso l'Head_Thunk
            //     (transizione 0→1), gli anelli successivi si limitano a
            //     ri-cablare gli slot dei vicini (n→n+1) e ad attribuirsi nel
            //     ledger. La demo interna (`pulse.demo`) diventa così un normale
            //     Hook_Link nel registro: passa per l'orchestratore di catena e
            //     coesiste con gli anelli delle mod esterne sullo stesso
            //     Target_Address (Req 8, 10.1) senza una seconda DobbyHook.
            //
            //     Il palliativo PULSE_DISABLE_BUILTIN_DEMO è stato ELIMINATO
            //     (task 7.6): la demo interna (`pulse.demo`) e le mod esterne
            //     che hookano lo stesso `MenuLayer::init` condividono ora l'unico
            //     percorso di catena e coesistono sull'unica
            //     Underlying_Installation, eseguite nel Chain_Order, senza che
            //     alcuna installazione riporti un errore di indirizzo già hookato
            //     (Req 8.1, 8.2, 8.3, 8.4). Non è più necessaria alcuna env var
            //     per liberare il target a una mod esterna.
            try {
                // Marca resolved/target su OGNI registrazione del registro SDK
                // (Req 10.1). Il resolver riconcilia il simbolo di
                // registrazione con quello del binding: la macro `PULSE_HOOK`
                // usa un identificatore C++ (privo di `::`), quindi la demo si
                // registra come "MenuLayer_init" mentre il binding `.pbind` è
                // "MenuLayer::init". Gli indirizzi risolti sono ribasati sullo
                // slide ASLR; i simboli non risolti non producono alcun anello.
                const auto reconcilingResolver =
                    [&provider, imageBase](std::string_view regSymbol) -> void* {
                    std::string symbol(regSymbol);
                    if (regSymbol == mvp::kMenuLayerInitRegistrationSymbol) {
                        symbol = std::string{mvp::kMenuLayerInitBindingSymbol};
                    }
                    const auto b = provider.resolve(symbol);
                    if (!pulse::hooking::binding_is_installable(b)) {
                        return nullptr;
                    }
                    return reinterpret_cast<void*>(imageBase + b->address);
                };
                pulse::hooks::resolve_all(reconcilingResolver);

                // Attribuzione del proprietario: si preferiscono le finestre di
                // epoca del ledger; in assenza (la demo si registra allo
                // static-init, PRIMA di qualunque epoca di mod esterna) si
                // ricade sul Mod_Id riservato della demo interna.
                const auto ownerOf =
                    [&modLoader](std::size_t index) -> pulse::hooking::ModId {
                    pulse::hooking::ModId owner =
                        modLoader.ledger().ownerOfIndex(index);
                    if (owner.empty()) {
                        owner = pulse::hooking::ModId(
                            pulse::lifecycle::kBuiltinDemoModId);
                    }
                    return owner;
                };

                // Raggruppa l'intero registro per Target_Address risolto e
                // ordina ciascun gruppo nel Chain_Order (priority DESC, loadOrder
                // ASC) — non `find()`, che restituisce solo la prima
                // registrazione (Req 10.1).
                const auto groups = pulse::hooking::group_registry_by_target(
                    pulse::hooks::registry(), ownerOf);

                std::size_t insertedLinks = 0;
                for (const auto& group : groups) {
                    // Binding risolto del target: `address == Target_Address`
                    // risolto (già ribasato sullo slide ASLR dal resolver),
                    // `resolved == true` così il gate ammette la creazione
                    // dell'unica Underlying_Installation del primo anello.
                    bindings::FunctionBinding resolved;
                    resolved.address = group.target;
                    resolved.resolved = true;
                    if (!group.links.empty()) {
                        resolved.symbol = group.links.front().symbol;
                    }
                    for (const auto& link : group.links) {
                        const auto result = chainRegistry->insertLink(
                            group.target, resolved, link);
                        if (result.outcome ==
                            pulse::hooking::ChainOpOutcome::Rejected) {
                            log(std::string{"runtime: anello mod '"} + link.owner +
                                "' (" + link.symbol +
                                ") non inserito in catena: " +
                                result.error.message + "; fail-open.");
                        } else {
                            ++insertedLinks;
                        }
                    }
                }
                log(std::string{"runtime: HookChainRegistry — "} +
                    std::to_string(insertedLinks) +
                    " anello/i inserito/i (demo interna inclusa, UN'UNICA "
                    "Underlying_Installation per Target_Address; nessuna seconda "
                    "DobbyHook).");
            } catch (...) {
                log("runtime: eccezione durante l'install via HookChainRegistry "
                    "confinata; fail-open.");
            }

            // (b) Pipeline delle mod esterne (discovery → ... → enable). NON
            //     lancia mai; riporta installedHooks (demo + mod esterne).
            return externalInit(wd);
        };

        // 7) CentralizedLoader: unico entry del caricamento. Bootstrap nullo
        //    (l'iniezione di piattaforma è già avvenuta: siamo dentro il
        //    processo di GD). Lo step gira sotto il watchdog 10 s; su timeout o
        //    fallimento, GD parte comunque senza mod (Req 1.6 / 1.3).
        CentralizedLoader centralized{
            /*bootstrap=*/nullptr, std::move(initStep), /*clock=*/nullptr, sink};
        const CentralizedStartResult started = centralized.start();
        log(std::string{"runtime: caricamento centralizzato — "} +
            started.message);
        return true;
    } catch (const std::exception& e) {
        log(std::string{"runtime: eccezione catturata ("} + e.what() +
            "); fail-open, GD prosegue senza mod");
        return true;
    } catch (...) {
        log("runtime: eccezione non tipizzata catturata; fail-open, GD prosegue "
            "senza mod");
        return true;
    }
}

#endif  // PULSE_LOADER_ARTIFACT
