// loader/lifecycle/mod_loader.cpp — implementazione della discovery della
// Mods_Directory (task 3.1, Requisiti 1.1–1.5). Vedi mod_loader.hpp per il
// contratto dei seam e il comportamento atteso.

#include "lifecycle/mod_loader.hpp"

#include <algorithm>
#include <exception>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <pulse/hooks.hpp>

#include "bindings/bindings.hpp"
#include "hooking/hook_chain_registry.hpp"
#include "hooking/hook_gate.hpp"

namespace pulse::lifecycle {

namespace {

// Una voce è un candidato `.pulse` riconoscibile solo se il suo nome termina
// con l'estensione `.pulse` (Req 1.2): la condizione necessaria prima ancora di
// tentarne l'apertura. La sufficienza (apertura riuscita) è verificata
// dall'`opener`.
[[nodiscard]] bool ends_with_pulse(std::string_view name) {
    constexpr std::string_view kExt = ".pulse";
    return name.size() > kExt.size() &&
           name.substr(name.size() - kExt.size()) == kExt;
}

void emit(const pulse::loader::DiagnosticSink& sink, const std::string& msg) {
    if (sink) sink(msg);
}

}  // namespace

DirectoryListing default_directory_lister(const std::filesystem::path& modsDir) {
    DirectoryListing listing;
    std::error_code ec;

    if (!std::filesystem::exists(modsDir, ec) || ec) {
        // Inesistente (o stat fallito): trattata come assente (Req 1.3).
        listing.status = DirectoryReadStatus::Absent;
        return listing;
    }

    if (!std::filesystem::is_directory(modsDir, ec) || ec) {
        // Il percorso esiste ma non è una directory leggibile (Req 1.4).
        listing.status = DirectoryReadStatus::NotReadable;
        listing.error = ec ? ec.message() : "il percorso non è una directory";
        return listing;
    }

    std::filesystem::directory_iterator it(modsDir, ec);
    if (ec) {
        listing.status = DirectoryReadStatus::NotReadable;
        listing.error = ec.message();
        return listing;
    }

    // Iterazione NON ricorsiva: solo le voci di primo livello (Req 1.1).
    const std::filesystem::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            listing.status = DirectoryReadStatus::NotReadable;
            listing.error = ec.message();
            listing.entryNames.clear();
            return listing;
        }
        listing.entryNames.push_back(it->path().filename().string());
    }

    listing.status = DirectoryReadStatus::Ok;
    return listing;
}

DiscoveredModSet discover_mods(const std::filesystem::path& modsDir,
                               const DirectoryLister& lister,
                               const PackageOpener& opener,
                               const pulse::loader::DiagnosticSink& sink) {
    DiscoveredModSet discovered;
    const std::string dirStr = modsDir.string();

    DirectoryListing listing =
        lister ? lister(modsDir)
               : DirectoryListing{DirectoryReadStatus::Absent, {}, {}};

    switch (listing.status) {
        case DirectoryReadStatus::Absent:
            // Req 1.3: assente → set vuoto, esito registrato, GD prosegue.
            emit(sink, "discovery: Mods_Directory assente '" + dirStr +
                           "'; nessuna mod scoperta.");
            return discovered;
        case DirectoryReadStatus::NotReadable:
            // Req 1.4: non leggibile → set vuoto + diagnostica con dir e causa.
            emit(sink, "discovery: Mods_Directory '" + dirStr +
                           "' non leggibile: " + listing.error +
                           "; nessuna mod scoperta.");
            return discovered;
        case DirectoryReadStatus::Ok:
            break;
    }

    if (listing.entryNames.empty()) {
        // Req 1.3: directory priva di voci di primo livello → set vuoto.
        emit(sink, "discovery: Mods_Directory '" + dirStr +
                       "' priva di voci di primo livello; nessuna mod scoperta.");
        return discovered;
    }

    // Determinismo (Req 1.6): processa le voci in ordine lessicografico, così il
    // `DiscoveredModSet` risultante è ordinato per `entryName`.
    std::vector<std::string> names = std::move(listing.entryNames);
    std::sort(names.begin(), names.end());

    for (const std::string& name : names) {
        if (!ends_with_pulse(name)) {
            // Req 1.5: voce non `.pulse` → ignorata con diagnostica, prosegue.
            emit(sink, "discovery: voce '" + name + "' in '" + dirStr +
                           "' ignorata: non è un container .pulse.");
            continue;
        }

        if (!opener) {
            // Nessun opener disponibile: non possiamo riconoscere il container
            // (Req 1.2). Voce ignorata con diagnostica, scoperta prosegue.
            emit(sink, "discovery: voce '" + name + "' in '" + dirStr +
                           "' ignorata: opener di package non disponibile.");
            continue;
        }

        const std::filesystem::path entryPath = modsDir / name;
        pulse::package::OpenResult res = opener(entryPath, name);
        if (!res.ok || !res.package.has_value()) {
            // Req 1.2/1.5: container non apribile → ignorato con diagnostica.
            const std::string cause =
                res.message.empty() ? pulse::package::to_string(res.error)
                                    : res.message;
            emit(sink, "discovery: voce '" + name + "' in '" + dirStr +
                           "' ignorata: apertura .pulse fallita (" + cause + ").");
            continue;
        }

        // Candidato riconosciuto (apertura riuscita, Req 1.2).
        ModId modId = res.package->manifest().id;
        discovered.push_back(
            DiscoveredMod{name, std::move(modId), std::move(*res.package)});
    }

    return discovered;
}

DiscoveredModSet discover_mods(const std::filesystem::path& modsDir,
                               const PackageOpener& opener,
                               const pulse::loader::DiagnosticSink& sink) {
    return discover_mods(modsDir, &default_directory_lister, opener, sink);
}

DiscoveredModSet dedup_by_mod_id(DiscoveredModSet candidates,
                                 const pulse::loader::DiagnosticSink& sink) {
    // Determinismo (Req 1.6, 1.7): riordina per `entryName` lessicografico, così
    // la prima occorrenza di ciascun Mod_Id è il minimo lessicografico (il
    // vincitore di ogni collisione) e l'output resta ordinato per `entryName`.
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const DiscoveredMod& a, const DiscoveredMod& b) {
                         return a.entryName < b.entryName;
                     });

    DiscoveredModSet deduped;
    deduped.reserve(candidates.size());

    // Mod_Id già mantenuti → `entryName` vincente (per la diagnostica, Req 1.7).
    std::unordered_map<std::string, std::string> winnerEntryByModId;

    for (DiscoveredMod& cand : candidates) {
        auto it = winnerEntryByModId.find(cand.modId);
        if (it == winnerEntryByModId.end()) {
            // Primo (e quindi minimo lessicografico per `entryName`) per questo
            // Mod_Id: lo si mantiene (Req 1.7).
            winnerEntryByModId.emplace(cand.modId, cand.entryName);
            deduped.push_back(std::move(cand));
            continue;
        }
        // Collisione di Mod_Id: si esclude questa voce mantenendo la vincente
        // (entryName lessicografico minore) e si registra la diagnostica che
        // nomina il Mod_Id duplicato e le voci coinvolte (Req 1.6, 1.7).
        emit(sink, "dedup: Mod_Id '" + cand.modId + "' duplicato; voce '" +
                       cand.entryName + "' esclusa, mantenuta '" + it->second +
                       "' (entryName lessicografico minore).");
    }

    return deduped;
}

// ---------------------------------------------------------------------------
// Stadio di risoluzione delle dipendenze (task 7.1, Req 4.1–4.7).
// ---------------------------------------------------------------------------

std::string_view to_string(ModOutcome outcome) noexcept {
    switch (outcome) {
        case ModOutcome::Loaded:   return "caricata";
        case ModOutcome::Excluded: return "esclusa";
        case ModOutcome::Isolated: return "isolata";
    }
    return "esclusa";  // irraggiungibile: insieme finito coperto sopra
}

std::string_view to_string(CauseCategory cause) noexcept {
    switch (cause) {
        case CauseCategory::InvalidPackage:                return "pacchetto non valido";
        case CauseCategory::VersionOrPlatformIncompatible: return "incompatibilità di versione o piattaforma";
        case CauseCategory::DependencyUnsatisfied:         return "dipendenza non soddisfatta";
        case CauseCategory::DependencyCycle:               return "ciclo di dipendenze";
        case CauseCategory::ModuleNotLoadable:             return "modulo non caricabile";
        case CauseCategory::EntryPointFailed:              return "entry point fallito";
        case CauseCategory::SymbolUnresolved:              return "simbolo non risolto";
    }
    return "pacchetto non valido";  // irraggiungibile: insieme finito coperto sopra
}

std::string_view to_string(HookOp op) noexcept {
    switch (op) {
        case HookOp::Install: return "install";
        case HookOp::Remove:  return "remove";
    }
    return "install";  // irraggiungibile: insieme finito coperto sopra
}

CauseCategory cause_category_for(const ExclusionReason& reason) noexcept {
    switch (reason.kind) {
        case ExclusionReason::Kind::DependencyCycle:
            return CauseCategory::DependencyCycle;  // Req 4.4
        case ExclusionReason::Kind::MissingDependency:
        case ExclusionReason::Kind::IncompatibleDependency:
        case ExclusionReason::Kind::DependencyExcluded:
            // Dipendenza mancante / di versione incompatibile / esclusa
            // transitivamente: tutte mappano su "dipendenza non soddisfatta"
            // (Req 4.3, 4.5).
            return CauseCategory::DependencyUnsatisfied;
    }
    return CauseCategory::DependencyUnsatisfied;  // irraggiungibile
}

namespace {

std::string semver_to_string(const pulse::lifecycle::SemVer& v) {
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." +
           std::to_string(v.patch);
}

}  // namespace

std::string exclusion_message(const ModId& mod, const ExclusionReason& reason) {
    switch (reason.kind) {
        case ExclusionReason::Kind::MissingDependency:
            return "resolver: mod '" + mod + "' esclusa: dipendenza mancante '" +
                   reason.dependency + "' (Req 4.3).";
        case ExclusionReason::Kind::IncompatibleDependency: {
            std::string found =
                reason.foundVersion.has_value()
                    ? (" (versione trovata " + semver_to_string(*reason.foundVersion) + ")")
                    : std::string{};
            return "resolver: mod '" + mod + "' esclusa: dipendenza '" +
                   reason.dependency + "' di versione incompatibile" + found +
                   " (Req 4.3).";
        }
        case ExclusionReason::Kind::DependencyExcluded:
            return "resolver: mod '" + mod +
                   "' esclusa: dipende (anche transitivamente) dalla mod esclusa '" +
                   reason.dependency + "' (Req 4.5).";
        case ExclusionReason::Kind::DependencyCycle: {
            std::string members;
            for (std::size_t i = 0; i < reason.cycle.size(); ++i) {
                if (i) members += ", ";
                members += reason.cycle[i];
            }
            return "resolver: mod '" + mod +
                   "' esclusa: coinvolta in un ciclo di dipendenze [" + members +
                   "] (Req 4.4).";
        }
    }
    return "resolver: mod '" + mod + "' esclusa.";
}

ResolvedLoadPlan resolve_load_plan(
    const std::vector<pulse::manifest::Manifest>& compatible,
    const pulse::loader::DiagnosticSink& sink) {
    ResolvedLoadPlan resolved;

    // Proiezione di ogni Mod_Manifest compatibile in ResolverManifest (Req 4.1).
    std::vector<ResolverManifest> resolverInputs;
    resolverInputs.reserve(compatible.size());
    for (const pulse::manifest::Manifest& m : compatible) {
        resolverInputs.push_back(m.toResolverManifest());
    }

    // Invocazione del DependencyResolver → LoadPlan{order, excluded}.
    const DependencyResolver resolver;
    const LoadPlan plan = resolver.resolve(resolverInputs);

    // Carica SOLO le mod in `order`, nell'ordine dato (Req 4.2).
    resolved.order = plan.order;

    // Traduce ogni mod esclusa in una DiagnosticEntry attribuita con la causa
    // del resolver (Req 4.3/4.4/4.5/4.6).
    resolved.excluded.reserve(plan.excluded.size());
    for (const Exclusion& ex : plan.excluded) {
        DiagnosticEntry entry;
        entry.modId = ex.mod;
        entry.outcome = ModOutcome::Excluded;
        entry.cause = cause_category_for(ex.reason);
        entry.message = exclusion_message(ex.mod, ex.reason);
        emit(sink, entry.message);
        resolved.excluded.push_back(std::move(entry));
    }

    // `order` vuoto → zero mod + esito registrato, GD prosegue (Req 4.7).
    if (resolved.order.empty()) {
        emit(sink,
             "resolver: LoadPlan.order vuoto; zero mod da caricare, GD prosegue "
             "(Req 4.7).");
    }

    return resolved;
}

// ---------------------------------------------------------------------------
// DiagnosticLedger (task 7.19, Req 10.1–10.4).
// ---------------------------------------------------------------------------

bool DiagnosticLedger::insertOutcome(DiagnosticEntry entry) {
    // Partizione (Req 10.4) + unicità del caricato (Req 10.1): un Mod_Id già
    // registrato in QUALUNQUE esito non può essere registrato di nuovo. Il
    // primo esito registrato vince; i successivi sono rifiutati senza alterare
    // la voce esistente.
    if (byMod_.find(entry.modId) != byMod_.end()) {
        return false;
    }
    byMod_.emplace(entry.modId, entries_.size());
    entries_.push_back(std::move(entry));
    return true;
}

bool DiagnosticLedger::recordLoaded(const ModId& modId, std::string message) {
    DiagnosticEntry entry;
    entry.modId = modId;
    entry.outcome = ModOutcome::Loaded;
    entry.cause = std::nullopt;  // Loaded non ha causa (Req 10.1)
    entry.message = std::move(message);
    return insertOutcome(std::move(entry));
}

bool DiagnosticLedger::recordExcluded(const ModId& modId, CauseCategory cause,
                                      std::string message) {
    DiagnosticEntry entry;
    entry.modId = modId;
    entry.outcome = ModOutcome::Excluded;
    entry.cause = cause;  // esattamente una causa dell'insieme chiuso (Req 10.2)
    entry.message = std::move(message);
    return insertOutcome(std::move(entry));
}

bool DiagnosticLedger::recordIsolated(const ModId& modId, CauseCategory cause,
                                      std::string message) {
    DiagnosticEntry entry;
    entry.modId = modId;
    entry.outcome = ModOutcome::Isolated;
    entry.cause = cause;  // esattamente una causa dell'insieme chiuso (Req 10.2)
    entry.message = std::move(message);
    return insertOutcome(std::move(entry));
}

bool DiagnosticLedger::record(DiagnosticEntry entry) {
    // Normalizza l'invariante delle cause: Loaded non porta causa, Excluded/
    // Isolated ne portano esattamente una (Req 10.1/10.2). Se un esito non-Loaded
    // arrivasse senza causa, lo si lascia comunque registrare (il chiamante è
    // responsabile della causa); la garanzia forte è data dalle record*().
    if (entry.outcome == ModOutcome::Loaded) {
        entry.cause = std::nullopt;
    }
    return insertOutcome(std::move(entry));
}

void DiagnosticLedger::recordHookInstalled(const ModId& modId,
                                           std::string symbol) {
    hookEvents_.push_back(HookEvent{modId, std::move(symbol), HookOp::Install});
}

void DiagnosticLedger::recordHookRemoved(const ModId& modId,
                                         std::string symbol) {
    hookEvents_.push_back(HookEvent{modId, std::move(symbol), HookOp::Remove});
}

bool DiagnosticLedger::contains(const ModId& modId) const {
    return byMod_.find(modId) != byMod_.end();
}

std::optional<ModOutcome> DiagnosticLedger::outcomeOf(const ModId& modId) const {
    auto it = byMod_.find(modId);
    if (it == byMod_.end()) return std::nullopt;
    return entries_[it->second].outcome;
}

std::vector<ModId> DiagnosticLedger::loaded() const {
    std::vector<ModId> out;
    for (const DiagnosticEntry& e : entries_) {
        if (e.outcome == ModOutcome::Loaded) out.push_back(e.modId);
    }
    return out;
}

std::vector<DiagnosticEntry> DiagnosticLedger::excluded() const {
    std::vector<DiagnosticEntry> out;
    for (const DiagnosticEntry& e : entries_) {
        if (e.outcome == ModOutcome::Excluded) out.push_back(e);
    }
    return out;
}

std::vector<DiagnosticEntry> DiagnosticLedger::isolated() const {
    std::vector<DiagnosticEntry> out;
    for (const DiagnosticEntry& e : entries_) {
        if (e.outcome == ModOutcome::Isolated) out.push_back(e);
    }
    return out;
}

std::vector<ModId> DiagnosticLedger::missingFrom(
    const std::vector<ModId>& discovered) const {
    std::vector<ModId> missing;
    for (const ModId& id : discovered) {
        if (byMod_.find(id) == byMod_.end()) missing.push_back(id);
    }
    return missing;
}

bool DiagnosticLedger::partitionComplete(
    const std::vector<ModId>& discovered) const {
    // Completezza: ogni mod individuata ha esattamente un esito (a due a due
    // disgiunti per costruzione) → ogni `discovered` è presente nel ledger.
    std::unordered_set<ModId> discoveredSet(discovered.begin(),
                                            discovered.end());
    for (const ModId& id : discovered) {
        if (byMod_.find(id) == byMod_.end()) return false;
    }
    // Nessun esito riguarda una mod NON individuata (unione = individuate).
    for (const auto& [id, idx] : byMod_) {
        (void)idx;
        if (discoveredSet.find(id) == discoveredSet.end()) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Cablaggio del ModManager (task 7.4, Req 5.4–5.8, 9.1, 9.4).
// ---------------------------------------------------------------------------

ModuleLoaderAvailability check_module_loader_availability(
    const IModuleLoader& moduleLoader,
    const pulse::loader::RuntimeContext& runtimeContext) {
    ModuleLoaderAvailability out;
    out.available = moduleLoader.available();
    if (!out.available) {
        // Fail-open Req 11.5: nomina la piattaforma del Runtime_Context e il
        // Module_Loader; zero mod, byte invariati, GD prosegue.
        const std::string platform = runtimeContext.platformId.empty()
                                          ? std::string("(piattaforma ignota)")
                                          : runtimeContext.platformId;
        out.diagnostic =
            "Mod_Loader: Module_Loader '" + std::string(moduleLoader.name()) +
            "' non disponibile sulla piattaforma '" + platform +
            "' (available()==false); zero mod caricate, eseguibile e asset "
            "byte-per-byte invariati, Geometry Dash prosegue (Req 11.5, 9.3).";
    }
    return out;
}

namespace {

// Invocatore di default dell'entry point: invoca l'indirizzo risolto come
// `void(*)()`. Il comportamento reale è verificato in Fase E; nei test host si
// inietta un invocatore fittizio (vedi `ModManagerWiring::EntryPointInvoker`).
EntryPointOutcome default_entry_point_invoker(const ModId& modId, void* entry) {
    if (entry == nullptr) {
        return EntryPointOutcome::failure("entry point nullo per la mod '" +
                                          modId + "'");
    }
    auto fn = reinterpret_cast<void (*)()>(entry);
    fn();
    return EntryPointOutcome::success();
}

// Promuove la (GD_Version) del Runtime_Context (loader) alla GdVersion dei
// bindings, così il RollbackRecord è auto-descrittivo (Req 8.3).
pulse::loader::bindings::GdVersion to_bindings_version(
    const pulse::loader::GdVersion& v) {
    return pulse::loader::bindings::GdVersion{static_cast<int>(v.major),
                                              static_cast<int>(v.minor)};
}

}  // namespace

ModManagerWiring::ModManagerWiring(IModuleLoader& moduleLoader,
                                   pulse::hooking::IHookBackend& backend,
                                   pulse::hooking::RollbackStore& rollback,
                                   HookOwnershipLedger& ledger,
                                   pulse::loader::RuntimeContext runtimeContext,
                                   SymbolResolver resolver,
                                   EntryPointInvoker invoker,
                                   pulse::loader::DiagnosticSink sink)
    : moduleLoader_(moduleLoader),
      backend_(backend),
      rollback_(rollback),
      ledger_(ledger),
      runtimeContext_(std::move(runtimeContext)),
      resolver_(std::move(resolver)),
      invoker_(invoker ? std::move(invoker) : &default_entry_point_invoker),
      sink_(std::move(sink)) {}

void ModManagerWiring::emit(const std::string& message) const {
    if (sink_) sink_(message);
}

void ModManagerWiring::registerMod(ModManager& manager, ModWiringSpec spec) {
    const ModId modId = spec.modId;

    // Stato runtime per-mod (riusato dal terminator e dal re-enable in 7.11).
    ModRuntimeState& st = state_[modId];
    st.spec = std::move(spec);

    // EntryPointFn invocata UNA volta dalla state machine del ModManager alla
    // transizione verso Enabled (Req 5.4): apre l'epoca, carica, risolve+invoca
    // l'entry point, esegue resolve_all e installa la finestra attribuita.
    EntryPointFn entry = [this, modId]() -> EntryPointOutcome {
        return onEnable(modId);
    };

    // TerminatorFn: rimuove i soli hook del Mod_Id (byte-esatto) + release.
    TerminatorFn terminator = [this, modId]() { onTerminate(modId); };

    manager.registerMod(modId, std::move(entry), std::move(terminator));
}

EntryPointOutcome ModManagerWiring::onEnable(const ModId& modId) {
    // Barriera no-throw dell'EntryPointFn (task 7.7, Req 6.2/6.3/6.4): qualunque
    // eccezione del corpo (caricamento del modulo, risoluzione/invocazione
    // dell'entry point, resolve_all, install della finestra) è confinata alla
    // sola mod e convertita in `EntryPointOutcome::failure` con diagnostica
    // attribuita al Mod_Id; gli eventuali hook già installati del mod nel
    // tentativo sono ripristinati byte-esatto (Req 6.2). Nessuna eccezione
    // lascia mai questo `EntryPointFn`.
    try {
        return onEnableImpl(modId);
    } catch (const std::exception& ex) {
        emit("mod '" + modId +
             "': eccezione durante l'enable confinata (Req 6.3, 6.4): " +
             ex.what());
        rollbackModHooks(modId);  // Req 6.2: rollback byte-esatto degli hook del mod
        return EntryPointOutcome::failure(
            "eccezione durante l'enable della mod '" + modId + "': " + ex.what());
    } catch (...) {
        emit("mod '" + modId +
             "': eccezione sconosciuta durante l'enable confinata (Req 6.3, 6.4).");
        rollbackModHooks(modId);  // Req 6.2
        return EntryPointOutcome::failure(
            "eccezione sconosciuta durante l'enable della mod '" + modId + "'");
    }
}

EntryPointOutcome ModManagerWiring::onEnableImpl(const ModId& modId) {
    auto it = state_.find(modId);
    if (it == state_.end()) {
        // Mod non registrata da questo wiring: fallimento isolato.
        emit("enable: mod '" + modId + "' non registrata nel cablaggio.");
        return EntryPointOutcome::failure("mod '" + modId +
                                          "' non registrata");
    }
    ModRuntimeState& st = it->second;

    // Re-enable (Req 7.5): il PRIMO enato è andato a buon fine (finestra
    // registrata) e il Mod_Module è ancora caricato (non scaricato al disable,
    // Req 7.7). In tal caso NON si esegue un nuovo `dlopen`, NON si apre una
    // nuova epoca: si RIUSANO gli indici della finestra memorizzata.
    const bool reEnable = st.loaded && st.windowRecorded;

    // (a) Finestra di epoca. Primo enable: apri l'epoca PRIMA del caricamento
    // del Mod_Module (le registrazioni PULSE_HOOK static-init al `dlopen` cadono
    // in [start, count())). Re-enable: riusa l'inizio della finestra memorizzata
    // (nessuna nuova epoca, nessun nuovo `dlopen`).
    std::size_t start;
    if (reEnable) {
        start = st.windowStart;
        emit("mod '" + modId +
             "': re-enable senza nuovo dlopen, riuso della finestra di epoca "
             "memorizzata (Req 7.5).");
    } else {
        start = ledger_.openEpoch();
        // Carica il Mod_Module solo se non già caricato: un re-enable dopo un
        // primo enable che ha caricato il modulo ma è poi fallito non ripete il
        // `dlopen` (il modulo resta caricato, Req 7.7).
        if (!st.loaded) {
            auto loaded = moduleLoader_.load(modId, st.spec.moduleImage);
            if (!loaded.has_value()) {
                // Modulo non caricabile → 0 hook del mod + diagnostica (Req 5.3).
                emit("mod '" + modId + "' non caricabile: " +
                     loaded.error().message);
                return EntryPointOutcome::failure("modulo non caricabile: " +
                                                  loaded.error().message);
            }
            st.handle = loaded.value();
            st.loaded = true;
        }
    }

    // (b) Risolvi l'entry point dichiarato via il seam Module_Loader (Req 5.2).
    auto entry = moduleLoader_.resolveEntryPoint(st.handle, st.spec.entrySymbol);
    if (!entry.has_value()) {
        // Entry point non risolvibile: NESSUNA invocazione, diagnostica con
        // Mod_Id + nome dell'entry point, fallimento isolato (Req 5.3).
        emit("mod '" + modId + "': entry point '" + st.spec.entrySymbol +
             "' non risolvibile (non esportato dal Mod_Module).");
        return EntryPointOutcome::failure("entry point '" + st.spec.entrySymbol +
                                          "' non risolvibile");
    }

    // Invoca l'entry point ESATTAMENTE una volta a questa transizione verso
    // Enabled (Req 7.1; re-invocazione al re-enable, Req 7.5). Un fallimento
    // dell'entry point è propagato al ModManager, che porta la mod a Disabled.
    EntryPointOutcome outcome = invoker_(modId, entry.value());
    st.entryInvocations += 1;
    if (!outcome.ok) {
        // Req 6.2/7.6: entry point in errore → la mod sarà portata a Disabled
        // dal ModManager; qui rimuoviamo byte-esatto gli eventuali hook già
        // installati di questa mod nel tentativo (al re-enable potrebbero
        // essercene) prima di propagare il fallimento.
        emit("mod '" + modId + "': entry point fallito: " + outcome.message);
        rollbackModHooks(modId);
        return outcome;
    }

    // (c) Risolvi l'INTERO registro globale contro i bindings del
    // Runtime_Context (Req 5.5/7.5). La risoluzione è idempotente e non lancia.
    pulse::hooks::resolve_all(
        [this](std::string_view symbol) -> void* {
            return resolver_ ? resolver_(symbol) : nullptr;
        });

    // Memorizza la finestra di epoca del mod al PRIMO enable; il re-enable la
    // riusa invariata (Req 7.5), così reinstalla esattamente i medesimi indici.
    if (!reEnable) {
        st.windowStart = start;
        st.windowEnd = pulse::hooks::count();
        st.windowRecorded = true;
    }

    // (d/e) Installa i soli hook risolti della finestra (memorizzata); nessun
    // install su indirizzi non risolti. Install TRANSAZIONALE (Req 9.5): se
    // `installWindow` riporta `false`, l'install di un binding risolto è
    // fallito; rimuoviamo byte-esatto via RollbackStore/backend gli hook GIÀ
    // installati di questa mod (`rollbackModHooks`), confiniamo il fallimento al
    // solo Mod_Id e portiamo la mod a Disabled con zero hook restituendo un
    // esito di errore (la state machine del ModManager applica Enabled→Disabled,
    // Req 7.6). Le altre mod proseguono indipendentemente (Req 6.6).
    if (!installWindow(modId, st.windowStart, st.windowEnd)) {
        emit("mod '" + modId +
             "': install parziale fallita; rollback byte-esatto degli hook già "
             "installati del mod e fallimento isolato al solo Mod_Id (Req 9.5).");
        rollbackModHooks(modId);
        return EntryPointOutcome::failure(
            "install degli hook della mod '" + modId +
            "' fallita: rollback transazionale eseguito (Req 9.5)");
    }

    return EntryPointOutcome::success();
}

bool ModManagerWiring::installWindow(const ModId& modId, std::size_t start,
                                     std::size_t end) {
    auto& registry = pulse::hooks::registry();
    end = std::min(end, registry.size());
    if (start > end) start = end;

    // HookGate: garantisce per costruzione l'invariante 0 hook su indirizzi non
    // risolti (Req 9.1) e consulta backend.available() prima di ogni install.
    pulse::hooking::HookGate gate{backend_, [this](std::string_view m) {
                                      emit(std::string(m));
                                  }};

    // Conteggio degli hook RISOLTI già installati per questa mod in questa
    // finestra: usato dal rollback transazionale (Req 9.5) per riportare se il
    // fallimento è avvenuto "dopo almeno un hook installato".
    std::size_t installedSoFar = 0;

    for (std::size_t i = start; i < end; ++i) {
        pulse::hooks::HookRegistration& reg = registry[i];
        const std::string symbol{reg.symbol};

        if (!reg.resolved || reg.target == nullptr) {
            // Indirizzo non risolto → NESSUN install (Req 5.7, 9.1, 9.4), solo
            // diagnostica attribuita con Mod_Id + simbolo. NON è un fallimento
            // transazionale: si prosegue con le voci successive.
            emit("mod '" + modId + "': simbolo '" + symbol +
                 "' non risolto; nessun hook installato su indirizzo non "
                 "risolto (Req 5.7, 9.4).");
            continue;
        }

        // Costruisci il binding risolto a partire dalla risoluzione del registro.
        pulse::loader::bindings::FunctionBinding binding;
        binding.symbol = symbol;
        binding.address = reinterpret_cast<std::uintptr_t>(reg.target);
        binding.resolved = true;

        // -------------------------------------------------------------------
        // Percorso CHAIN-AWARE (Hook_Chaining, Req 8): se è cablato un
        // HookChainRegistry, l'install passa per la catena dell'unico
        // Target_Address. Il PRIMO anello su quell'indirizzo crea l'unica
        // Underlying_Installation reale (gate + Head_Thunk) e persiste i byte
        // originali nel RollbackStore; gli anelli SUCCESSIVI (es. una mod
        // esterna che hooka lo stesso `MenuLayer::init` della demo) si limitano
        // a ri-cablare gli slot dei vicini — NESSUNA seconda DobbyHook, nessun
        // `codice -1`. La Registry attribuisce l'anello al Mod_Id nel ledger e
        // scrive direttamente lo slot del trampolino della registrazione. Qui
        // quindi NON si esegue `gate.install`/`rollback_.add`/`bind_trampoline`/
        // `ledger_.attribute`: li fa la Registry.
        // -------------------------------------------------------------------
        if (chainRegistry_ != nullptr) {
            pulse::hooking::LinkSpec link;
            link.owner = modId;
            link.symbol = symbol;
            link.priority = reg.priority;
            link.loadOrder = static_cast<std::uint64_t>(i);
            link.detour = reg.detour;
            link.slot = reg.trampoline;  // Trampoline_Slot pulse_original

            const pulse::hooking::ChainOpResult result =
                chainRegistry_->insertLink(binding.address, binding, link);

            if (result.outcome == pulse::hooking::ChainOpOutcome::Rejected) {
                // Ammissione/install del PRIMO anello negata o fallita: nessuna
                // catena, nessuna install parziale (la Registry ha già dismesso
                // il ChainSlot). Backend non disponibile → fail-open (Req
                // 9.1/9.3), si prosegue; altrimenti fallimento transazionale
                // (Req 9.5): si abortisce e il chiamante rimuove via
                // `removeOwner` gli anelli già inseriti di questa mod.
                if (!backend_.available()) {
                    emit("mod '" + modId + "': install dell'hook per il simbolo '" +
                         symbol + "' non eseguito: " + result.error.message);
                    continue;
                }
                emit("mod '" + modId + "': inserimento in catena dell'hook per il "
                     "simbolo '" + symbol + "' non riuscito dopo " +
                     std::to_string(installedSoFar) +
                     " anelli già inseriti: " + result.error.message +
                     "; rollback transazionale degli hook del mod (Req 9.5).");
                return false;
            }

            ++installedSoFar;
            if (diagnostics_) diagnostics_->recordHookInstalled(modId, symbol);
            emit("mod '" + modId + "': hook inserito in catena sul simbolo '" +
                 symbol + "' (Hook_Chaining, Req 8; nessuna seconda DobbyHook).");
            continue;
        }

        // -------------------------------------------------------------------
        // Percorso DIRETTO storico (chainRegistry_ == nullptr): install singola
        // via HookGate. Invariato (i test host costruiscono il cablaggio senza
        // chain registry e continuano a esercitare questo percorso).
        // -------------------------------------------------------------------

        // Persisti i byte originali nel RollbackStore con owner=Mod_Id PRIMA
        // dell'install (Req 8.3, 9.5), così la rimozione è byte-esatta.
        auto original =
            backend_.readOriginal(binding.address, kRollbackPrologueBytes);
        if (original.has_value()) {
            pulse::hooking::RollbackRecord record;
            record.owner = modId;
            record.symbol = symbol;
            record.address = binding.address;
            record.originalBytes = original.value().bytes();
            record.version = to_bindings_version(runtimeContext_.gdVersion);
            record.platformId = runtimeContext_.platformId;
            rollback_.add(std::move(record));
        }

        // Install via gate (Req 5.6). Il gate verifica binding.resolved prima di
        // toccare il backend: l'unico percorso verso install è un binding risolto.
        pulse::hooking::GateResult result = gate.install(binding, reg.detour);
        if (!result.installed()) {
            if (result.backendUnavailable()) {
                // Backend non disponibile a runtime: fail-open (Req 9.1/9.3) —
                // 0 hook installati per costruzione, nessuna transazione da
                // annullare. Si prosegue (ogni install successivo sarà anch'esso
                // bloccato dal gate); NON è un fallimento transazionale.
                emit("mod '" + modId + "': install dell'hook per il simbolo '" +
                     symbol + "' non eseguito: " + result.error.message);
                continue;
            }

            // Install di un binding RISOLTO fallito (Req 9.5): ABORTIAMO
            // l'installazione (transazione) e NON tentiamo gli hook successivi,
            // così il chiamante (`onEnableImpl`) può rimuovere byte-esatto via
            // `RollbackStore`/backend gli hook GIÀ installati di questa mod
            // (`rollbackModHooks`), confinare il fallimento al solo Mod_Id e
            // portare la mod a Disabled con zero hook; le altre mod proseguono.
            // Il caso "dopo almeno un hook installato" (Req 9.5) è
            // `installedSoFar >= 1`; con zero hook già installati l'install è
            // comunque fallito e si abortisce, lasciando zero hook (nessun
            // parziale residuo).
            emit("mod '" + modId + "': install dell'hook per il simbolo '" +
                 symbol + "' non riuscito dopo " +
                 std::to_string(installedSoFar) +
                 " hook già installati: " + result.error.message +
                 "; rollback transazionale degli hook del mod (Req 9.5).");
            return false;
        }

        // Cabla il trampolino verso l'originale (Req 5.6) e attribuisci l'hook
        // installato al Mod_Id (Req 5.8).
        pulse::hooks::bind_trampoline(reg.symbol, result.trampoline.address());
        ledger_.attribute(OwnedHook{modId, symbol, binding.address, i});
        ++installedSoFar;
        // Evento osservabile di install (Req 10.3): Mod_Id, simbolo, operazione.
        if (diagnostics_) diagnostics_->recordHookInstalled(modId, symbol);
        emit("mod '" + modId + "': hook installato sul simbolo '" + symbol +
             "' (Req 5.6, 5.8).");
    }

    return true;
}

void ModManagerWiring::rollbackModHooks(const ModId& modId) noexcept {
    // Rimuove i SOLI hook del Mod_Id (Req 6.2/7.2/7.3/8.6) ripristinando i byte
    // originali byte-esatto via il backend (i byte sono stati persistiti nel
    // `RollbackStore` con owner=Mod_Id PRIMA dell'install, Req 8.3/9.5); isola
    // gli eventuali fallimenti di rimozione del singolo hook e prosegue. Non
    // lancia: usato sia dal terminator sia dalle barriere di rollback (6.2).
    try {
        // -------------------------------------------------------------------
        // Percorso CHAIN-AWARE (Hook_Chaining, Req 8): la rimozione passa per
        // `HookChainRegistry::removeOwner`, che ri-cabla i vicini finché restano
        // anelli sul Target_Address (install mantenuta, byte NON ripristinati) e
        // rimuove l'unica Underlying_Installation + ripristina i byte byte-esatto
        // SOLO sull'ULTIMO anello (transizione 1→0). `removeOwner` rilascia anche
        // l'attribuzione del solo Mod_Id nel ledger, quindi qui NON si chiama
        // `ledger_.release`. Gli eventi diagnostici per-hook (Req 10.3) sono
        // emessi prima della rimozione, leggendo gli hook attribuiti.
        // -------------------------------------------------------------------
        if (chainRegistry_ != nullptr) {
            if (diagnostics_ != nullptr) {
                for (const OwnedHook& hook : ledger_.hooksOf(modId)) {
                    diagnostics_->recordHookRemoved(modId, hook.symbol);
                }
            }
            chainRegistry_->removeOwner(modId);
            emit("mod '" + modId +
                 "': hook rimossi dalla catena (relink finché restano anelli; "
                 "uninstall byte-esatto sull'ultimo) (Hook_Chaining, Req 8).");
            return;
        }

        // Percorso DIRETTO storico (chainRegistry_ == nullptr): rimozione per
        // hook via backend, invariato.
        const std::vector<OwnedHook> hooks = ledger_.hooksOf(modId);
        for (const OwnedHook& hook : hooks) {
            auto removed = backend_.remove(hook.target);
            if (!removed.has_value()) {
                emit("mod '" + modId + "': rimozione dell'hook sul simbolo '" +
                     hook.symbol + "' non riuscita: " + removed.error().message);
                continue;
            }
            // Evento osservabile di remove (Req 10.3): Mod_Id, simbolo, operazione.
            if (diagnostics_) diagnostics_->recordHookRemoved(modId, hook.symbol);
            emit("mod '" + modId +
                 "': hook rimosso (ripristino byte-esatto) dal simbolo '" +
                 hook.symbol + "'.");
        }
        // Rilascia l'attribuzione degli hook installati del Mod_Id.
        ledger_.release(modId);
    } catch (...) {
        // Barriera no-throw difensiva: anche il rollback non propaga eccezioni.
    }
}

void ModManagerWiring::onTerminate(const ModId& modId) {
    // Teardown/disable del Mod_Id: rimuove i suoi soli hook byte-esatto e
    // rilascia l'attribuzione (Req 7.3/8.6), riusando la barriera di rollback.
    rollbackModHooks(modId);
}

EnableAllResult ModManagerWiring::runNoThrow(
    ModManager& manager, const std::vector<ModId>& order) noexcept {
    // Barriera no-throw per-mod (task 7.7, Req 6.1–6.6). Itera `order`
    // abilitando ogni mod registrata e confinando OGNI fallimento alla singola
    // mod: il fallimento di init è già isolato dal ModManager (la mod va a
    // Disabled), ma qui aggiungiamo una barriera esterna `noexcept` così
    // nessuna eccezione di alcuno stadio possa propagarsi all'orchestratore
    // (Req 6.3, 6.4) e tutte le mod restanti valide vengano comunque abilitate
    // (Req 6.6). Gli hook delle mod abilitate con successo non sono toccati da
    // un fallimento isolato (Req 6.5): ogni mod è gestita indipendentemente.
    EnableAllResult out;
    out.enabled.reserve(order.size());

    // Guard di disponibilità del Module_Loader (task 7.23, Req 11.5): se il
    // Module_Loader della piattaforma corrente non è disponibile
    // (`available()==false`), NON si tenta alcun caricamento. Zero mod, zero
    // install → byte di eseguibile/asset invariati (Req 9.3, stessa invarianza
    // del caso "nessuna mod Enabled"); si registra la causa nominando la
    // piattaforma del Runtime_Context e GD prosegue. Il pipeline entry (task
    // 7.22) interroga la stessa condizione up front via
    // `check_module_loader_availability`; questo guard è la difesa interna del
    // cablaggio così l'invarianza vale anche se invocato direttamente.
    const ModuleLoaderAvailability availability =
        check_module_loader_availability(moduleLoader_, runtimeContext_);
    if (!availability.available) {
        emit(availability.diagnostic);
        return out;  // zero mod abilitate, zero install
    }

    for (const ModId& id : order) {
        try {
            if (!manager.contains(id)) {
                // Mod non registrata in questo wiring: ignorata (coerente con
                // ModManager::enableAll), nessun effetto sulle altre.
                continue;
            }

            const TransitionResult r = manager.enable(id);

            if (r.entryPoint == EntryPointStatus::Error ||
                r.entryPoint == EntryPointStatus::Threw) {
                // Fallimento di init isolato: mod a Disabled, prosegue (Req 6.6).
                out.failed.push_back(InitFailure{
                    id, r.entryPointMessage,
                    /*threw*/ r.entryPoint == EntryPointStatus::Threw});
                continue;
            }

            if (r.applied() && r.state == ModState::Enabled) {
                out.enabled.push_back(id);
            }
        } catch (const std::exception& ex) {
            // Rete di sicurezza esterna (Req 6.3, 6.4): qualunque eccezione
            // imprevista è convertita in diagnostica con Mod_Id, gli hook del
            // mod sono ripristinati byte-esatto e si prosegue con le restanti.
            emit("barriera no-throw: eccezione confinata alla mod '" + id +
                 "' (Req 6.3, 6.4): " + ex.what());
            rollbackModHooks(id);
            out.failed.push_back(InitFailure{id, ex.what(), /*threw*/ true});
        } catch (...) {
            emit("barriera no-throw: eccezione sconosciuta confinata alla mod '" +
                 id + "' (Req 6.3, 6.4).");
            rollbackModHooks(id);
            out.failed.push_back(
                InitFailure{id, "eccezione sconosciuta", /*threw*/ true});
        }
    }

    return out;
}

TransitionResult ModManagerWiring::enable(ModManager& manager,
                                          const ModId& modId) {
    // L'`EntryPointFn` registrato è la barriera no-throw `onEnable`, che gestisce
    // sia il primo enable sia il re-enable (Req 7.1/7.5): la state machine del
    // ModManager invoca l'entry point alla transizione verso Enabled e, se
    // fallisce, porta la mod a Disabled (Req 7.6). Una transizione non ammessa è
    // rifiutata dal ModManager mantenendo stato e hook + diagnostica (Req 7.4).
    return manager.enable(modId);
}

TransitionResult ModManagerWiring::disable(ModManager& manager,
                                           const ModId& modId) {
    // Transizione Enabled→Disabled della state machine. Una transizione non
    // ammessa (es. da Installed/Disabled/Removed) è rifiutata dal ModManager
    // mantenendo invariati stato corrente e hook + diagnostica (Req 7.4): in tal
    // caso NON tocchiamo gli hook del mod.
    const TransitionResult r = manager.disable(modId);

    if (r.applied() && r.state == ModState::Disabled) {
        // Disable applicato: rimuove TUTTI gli hook del Mod_Id byte-esatto via
        // RollbackStore/backend (Req 7.2) lasciando ZERO hook del mod mentre è
        // Disabled (Req 7.3). Il Mod_Module NON viene scaricato e il Mod_Package
        // resta nella Mods_Directory (Req 7.7): `st.loaded` resta true e la
        // finestra di epoca memorizzata consente il re-enable senza `dlopen`.
        emit("mod '" + modId +
             "': disable → rimozione byte-esatto di tutti gli hook del mod "
             "(Req 7.2, 7.3); Mod_Module conservato (Req 7.7).");
        rollbackModHooks(modId);
    }

    return r;
}

std::vector<ModId> ModManagerWiring::teardown(
    ModManager& manager, const std::vector<ModId>& order) noexcept {
    // Teardown pulito alla chiusura del gioco (task 7.17, Req 8.1–8.8).
    // Costruito sopra `ModManager::shutdown`. `noexcept`: la sequenza di
    // chiusura del processo non deve mai propagare un'eccezione.
    std::vector<ModId> tornDown;
    try {
        // (Req 8.1) Invoca il terminator di OGNI mod Enabled ESATTAMENTE una
        // volta nell'ordine ESATTAMENTE inverso rispetto a `order`. La state
        // machine del ModManager itera in reverse, salta le mod non Enabled e
        // invoca ciascun terminator una sola volta. Il terminator registrato
        // (`onTerminate` → `rollbackModHooks`) rimuove gli hook del solo Mod_Id
        // byte-esatto via RollbackStore/backend (Req 8.2, 8.3, 8.4), isolando i
        // fallimenti di rimozione del singolo hook (Req 8.5), e ne rilascia
        // l'attribuzione nel ledger.
        const std::vector<ModId> invoked = manager.shutdown(order);
        tornDown.reserve(invoked.size());

        for (const ModId& id : invoked) {
            // (Req 8.5) Anche se il terminator "fallisce" (rimozione parziale di
            // un hook), riesegue `rollbackModHooks` (idempotente: a hook già
            // rimossi `hooksOf` è vuoto) così TUTTI gli hook del Mod_Id sono
            // rimossi e l'attribuzione rilasciata; isola e prosegue con le mod
            // restanti senza interrompere il teardown.
            rollbackModHooks(id);

            // (Req 8.7) A hook rimossi, scarica il Mod_Module via il
            // Module_Loader. Un unload fallito è isolato (diagnostica) e non
            // interrompe il teardown; lo stato runtime è comunque azzerato.
            auto sit = state_.find(id);
            if (sit != state_.end() && sit->second.loaded) {
                auto unloaded = moduleLoader_.unload(sit->second.handle);
                if (!unloaded.has_value()) {
                    emit("teardown: mod '" + id +
                         "': unload del Mod_Module non riuscito: " +
                         unloaded.error().message + " (isolato, Req 8.5/8.7).");
                } else {
                    emit("teardown: mod '" + id +
                         "': Mod_Module scaricato a hook rimossi (Req 8.7).");
                }
                sit->second.loaded = false;
                sit->second.handle = ModuleHandle{};
                sit->second.windowRecorded = false;
            }

            // (Req 8.8) A Mod_Module scaricato, porta la mod a Removed. La state
            // machine ammette solo Enabled→Disabled e Disabled→Removed: la mod
            // è Enabled (il terminator è stato invocato solo per le Enabled), si
            // passa quindi per Disabled.
            manager.disable(id);
            manager.remove(id);
            emit("teardown: mod '" + id +
                 "' portata a Removed dopo rimozione hook e unload (Req 8.8).");

            tornDown.push_back(id);
        }
    } catch (...) {
        // Barriera no-throw difensiva: il teardown non propaga MAI eccezioni.
    }
    // (Req 8.6) Al termine, zero hook attribuiti ad alcuna mod restano nel
    // ledger: ogni terminator invocato ha rilasciato l'attribuzione del proprio
    // Mod_Id (la verifica è esercitata dai test su `ledger.installedCount()`).
    return tornDown;
}

bool ModManagerWiring::moduleLoaded(const ModId& modId) const {
    auto it = state_.find(modId);
    return it != state_.end() && it->second.loaded && it->second.handle.valid();
}

bool ModManagerWiring::moduleLoaderAvailable() const noexcept {
    return moduleLoader_.available();
}

int ModManagerWiring::entryPointInvocations(const ModId& modId) const {
    auto it = state_.find(modId);
    return it == state_.end() ? 0 : it->second.entryInvocations;
}

std::pair<std::size_t, std::size_t> ModManagerWiring::epochWindow(
    const ModId& modId) const {
    auto it = state_.find(modId);
    if (it == state_.end()) return {0, 0};
    return {it->second.windowStart, it->second.windowEnd};
}

// ---------------------------------------------------------------------------
// ModLoader — orchestratore di Layer 4 (task 7.22, Req 1.1, 6.3, 9.6).
// ---------------------------------------------------------------------------

namespace {

// Invocatore di default dell'entry point usato dal ModLoader quando nessun
// invocatore è iniettato: invoca l'indirizzo risolto come `void(*)()`. Il
// comportamento reale è verificato in Fase E.
EntryPointOutcome mod_loader_default_invoker(const ModId& modId, void* entry) {
    if (entry == nullptr) {
        return EntryPointOutcome::failure("entry point nullo per la mod '" +
                                          modId + "'");
    }
    reinterpret_cast<void (*)()>(entry)();
    return EntryPointOutcome::success();
}

}  // namespace

ModLoader::ModLoader(const pulse::loader::RuntimeContext& ctx,
                     IModuleLoader& moduleLoader,
                     pulse::hooking::IHookBackend& backend,
                     pulse::hooking::RollbackStore& rollback,
                     pulse::loader::DiagnosticSink sink)
    : ctx_(ctx),
      moduleLoader_(moduleLoader),
      backend_(backend),
      rollback_(rollback),
      sink_(std::move(sink)),
      lister_(&default_directory_lister) {
    // Il cablaggio `ModManagerWiring` è costruito UNA volta con lambda che
    // inoltrano ai seam membri (resolver_/invoker_): così i seam possono essere
    // impostati dopo la costruzione (setter) e l'install attribuito li userà al
    // momento dell'enable. `ctx_` è passato per valore al cablaggio (copia).
    wiring_ = std::make_unique<ModManagerWiring>(
        moduleLoader_, backend_, rollback_, ledger_, ctx_,
        // SymbolResolver: inoltra al resolver iniettato (assente → nessun simbolo
        // risolto, zero install — fail-open "GD senza mod").
        [this](std::string_view symbol) -> void* {
            return resolver_ ? resolver_(symbol) : nullptr;
        },
        // EntryPointInvoker: inoltra all'invocatore iniettato o, in sua assenza,
        // al default reale (`void(*)()`).
        [this](const ModId& id, void* entry) -> EntryPointOutcome {
            return invoker_ ? invoker_(id, entry)
                            : mod_loader_default_invoker(id, entry);
        },
        sink_);
    // Eventi hook install/remove osservabili (Req 10.3) sul DiagnosticLedger.
    wiring_->setDiagnosticLedger(&diagnostics_);
    // Orchestratore di catena del Hook_Chaining (Req 8), se impostato prima
    // della costruzione del cablaggio: install/rimozione passano per la catena.
    wiring_->setChainRegistry(chainRegistry_);
}

void ModLoader::emit(const std::string& message) const {
    if (sink_) sink_(message);
}

namespace {

// select_native_module_image — seleziona i byte del Mod_Module nativo dal
// container, riconciliando il nome canonico storico con i nomi emessi dal CLI.
//
// Il CLI emette il modulo nativo come `code/<platform>.<ext>` (es.
// `code/macos-arm64.dylib`), mentre il nome canonico è `code/module.pulsebin`.
// Ordine di preferenza (deterministico):
//   (1) `code/module.pulsebin` (canonico);
//   (2) `code/<platformId>.<ext>` per ext in {dylib, so, dll} (match esatto
//       della piattaforma del Runtime_Context);
//   (3) la prima voce `code/*.{dylib,so,dll}` in ordine lessicografico
//       (`archive().paths()` è ordinato).
// Ritorna nullptr se nessuna voce idonea è presente.
[[nodiscard]] const pulse::package::Bytes* select_native_module_image(
    const pulse::package::PulsePackage& package, const std::string& platformId) {
    // (1) Nome canonico.
    if (const pulse::package::Bytes* code = package.codeEntry("module.pulsebin")) {
        return code;
    }

    static constexpr std::string_view kExts[] = {".dylib", ".so", ".dll"};

    // (2) Match esatto per piattaforma del Runtime_Context.
    if (!platformId.empty()) {
        for (std::string_view ext : kExts) {
            const std::string name = "code/" + platformId + std::string(ext);
            if (const pulse::package::Bytes* code =
                    package.archive().find(name)) {
                return code;
            }
        }
    }

    // (3) Prima libreria nativa sotto code/ in ordine lessicografico.
    constexpr std::string_view kCodePrefix = "code/";
    for (const std::string& path : package.archive().paths()) {
        if (path.size() <= kCodePrefix.size() ||
            path.compare(0, kCodePrefix.size(), kCodePrefix) != 0) {
            continue;
        }
        const auto ends_with = [&path](std::string_view ext) {
            return path.size() > ext.size() &&
                   path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
        };
        for (std::string_view ext : kExts) {
            if (ends_with(ext)) {
                return package.archive().find(path);
            }
        }
    }

    return nullptr;
}

}  // namespace

ModLoadOutcome ModLoader::run(const std::filesystem::path& modsDir) {
    ModLoadOutcome outcome;

    // Barriera fail-open assoluta (Req 6.3, 6.4): nessuna eccezione di alcuno
    // stadio lascia mai `run()`. Ogni fallimento è confinato e registrato.
    try {
        // (0) Seeding della demo PRIMA di qualunque epoca di mod esterna
        // (Req 9.6): le registrazioni `PULSE_HOOK` preesistenti del
        // Loader_Artifact (la demo interna) sono attribuite al Mod_Id riservato
        // `kBuiltinDemoModId`. Idempotente sull'istanza.
        if (!demoSeeded_) {
            ledger_.seedBuiltinDemo();
            demoSeeded_ = true;
            emit("Mod_Loader: demo interna attribuita al Mod_Id riservato '" +
                 std::string(kBuiltinDemoModId) + "' (Req 9.6).");
        }

        // (1) Discovery + dedup (Req 1.1–1.7). Senza un PackageOpener nessun
        // container è riconoscibile: zero mod scoperte, fail-open.
        DiscoveredModSet discovered;
        if (opener_) {
            discovered = dedup_by_mod_id(
                discover_mods(modsDir,
                              lister_ ? lister_
                                      : DirectoryLister(&default_directory_lister),
                              opener_, sink_),
                sink_);
        } else {
            emit("Mod_Loader: nessun PackageOpener configurato; zero mod "
                 "scoperte (fail-open, GD prosegue).");
        }

        // (2) Validazione (nessun codice caricato) + compatibilità. Le mod
        // accettate e compatibili sono proiettate nei Mod_Manifest passati al
        // resolver; per ciascuna si memorizza la `ModWiringSpec` (byte del
        // Mod_Module + simbolo dell'entry point) per il cablaggio.
        ModManifestValidator validator;
        std::vector<pulse::manifest::Manifest> compatible;
        std::unordered_map<ModId, ModWiringSpec> specs;

        for (DiscoveredMod& mod : discovered) {
            // Validazione strutturale e di integrità (Req 2): opera su una COPIA
            // dell'archivio del container già aperto in discovery. Nessun codice
            // viene caricato.
            ValidationResult vr =
                validator.validate(mod.package.archive());
            if (!vr.accepted || !vr.manifest.has_value()) {
                DiagnosticEntry entry{mod.modId, ModOutcome::Excluded,
                                      CauseCategory::InvalidPackage, vr.message};
                diagnostics_.record(entry);
                outcome.excluded.push_back(entry);
                emit("Mod_Loader: mod '" + mod.modId +
                     "' esclusa (pacchetto non valido): " + vr.message);
                continue;
            }

            const pulse::manifest::Manifest& manifest = *vr.manifest;

            // Compatibilità piattaforma/versione di GD (Req 3).
            CompatResult cr = check_compatibility(manifest, ctx_);
            if (!cr.compatible) {
                DiagnosticEntry entry{
                    manifest.id, ModOutcome::Excluded,
                    CauseCategory::VersionOrPlatformIncompatible, cr.message};
                diagnostics_.record(entry);
                outcome.excluded.push_back(entry);
                emit("Mod_Loader: mod '" + manifest.id +
                     "' esclusa (incompatibile): " + cr.message);
                continue;
            }

            // Memorizza la spec di cablaggio: byte del Mod_Module (già
            // verificati) e simbolo del primo entry point dichiarato.
            ModWiringSpec spec;
            spec.modId = manifest.id;
            if (const pulse::package::Bytes* code =
                    select_native_module_image(mod.package, ctx_.platformId)) {
                spec.moduleImage = *code;
            }
            spec.entrySymbol = manifest.entryPoints.empty()
                                   ? std::string{}
                                   : manifest.entryPoints.front().symbol;
            specs.emplace(manifest.id, std::move(spec));

            compatible.push_back(manifest);
        }

        // (3) Risoluzione delle dipendenze → LoadPlan{order, excluded}
        // (Req 4.1–4.7). Le mod escluse dal resolver sono già diagnostica
        // attribuita con la causa dell'insieme chiuso.
        ResolvedLoadPlan plan = resolve_load_plan(compatible, sink_);
        for (DiagnosticEntry& entry : plan.excluded) {
            diagnostics_.record(entry);
            outcome.excluded.push_back(entry);
        }

        // (4) Cablaggio + enable. Registra SOLO le mod di `order` (nell'ordine
        // dato, Req 4.2) e le abilita via la barriera no-throw `runNoThrow`
        // (Req 5, 6, 9): ogni fallimento è isolato (mod a Disabled), nessuna
        // eccezione propagata, byte invariati su Module_Loader/backend non
        // disponibile (Req 9.1–9.3, 11.5).
        for (const ModId& id : plan.order) {
            auto it = specs.find(id);
            if (it != specs.end()) {
                wiring_->registerMod(manager_, it->second);
            }
        }

        loadOrder_ = plan.order;
        const EnableAllResult enabled = wiring_->runNoThrow(manager_, plan.order);

        // (5) Esito complessivo. Caricate (Req 10.1, una sola volta), isolate
        // (fail-open, Req 6 + 10.2) con causa dell'insieme chiuso, hook
        // installati al termine (Req 9.6).
        for (const ModId& id : enabled.enabled) {
            diagnostics_.recordLoaded(id);
            outcome.loaded.push_back(id);
        }
        for (const InitFailure& failure : enabled.failed) {
            const CauseCategory cause = CauseCategory::EntryPointFailed;
            const std::string message =
                "Mod_Loader: mod '" + failure.mod +
                "' isolata (fail-open): " + failure.cause;
            diagnostics_.recordIsolated(failure.mod, cause, message);
            outcome.isolated.push_back(
                DiagnosticEntry{failure.mod, ModOutcome::Isolated, cause, message});
        }

        outcome.installedHooks = ledger_.allInstalled().size();

        emit("Mod_Loader: pipeline completata — " +
             std::to_string(outcome.loaded.size()) + " caricate, " +
             std::to_string(outcome.excluded.size()) + " escluse, " +
             std::to_string(outcome.isolated.size()) + " isolate; " +
             std::to_string(outcome.installedHooks) +
             " hook installati al termine (Req 9.6).");
    } catch (const std::exception& ex) {
        emit(std::string("Mod_Loader: eccezione confinata nella pipeline "
                         "run() (Req 6.3, 6.4): ") +
             ex.what());
    } catch (...) {
        emit("Mod_Loader: eccezione sconosciuta confinata nella pipeline run() "
             "(Req 6.3, 6.4).");
    }

    return outcome;
}

pulse::loader::RuntimeInitFn ModLoader::asInitStep(
    const std::filesystem::path& modsDir) {
    // Init step verso il CentralizedLoader: incapsula `run()` e NON lancia MAI
    // (Req 6.3, 6.4). Riporta sempre `installedHooks` (Req 9.6) e un esito "mod
    // caricate" fail-open — anche con zero mod GD prosegue ereditando il
    // watchdog/timeout del CentralizedLoader. Il `WatchdogToken` non è usato: la
    // pipeline è limitata e cooperativa.
    return [this, modsDir](const pulse::loader::WatchdogToken&)
               -> pulse::loader::RuntimeInitResult {
        try {
            const ModLoadOutcome outcome = run(modsDir);
            std::string message =
                "Mod_Loader: init step — " +
                std::to_string(outcome.loaded.size()) + " mod caricate, " +
                std::to_string(outcome.installedHooks) + " hook installati.";
            return pulse::loader::RuntimeInitResult::loaded(
                outcome.installedHooks, std::move(message));
        } catch (...) {
            // Barriera difensiva: l'init step non propaga MAI (Req 6.3, 6.4).
            return pulse::loader::RuntimeInitResult::loaded(
                0,
                "Mod_Loader: init step fail-open (eccezione confinata); GD "
                "prosegue senza mod.");
        }
    };
}

void ModLoader::teardown() noexcept {
    // Teardown ordinato (Req 8): delega alla barriera no-throw del cablaggio
    // sull'ordine di caricamento dell'ultima `run`.
    if (wiring_) {
        wiring_->teardown(manager_, loadOrder_);
    }
}

}  // namespace pulse::lifecycle
