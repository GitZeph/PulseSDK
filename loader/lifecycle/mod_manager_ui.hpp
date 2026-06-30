// loader/lifecycle/mod_manager_ui.hpp — Layer 4/5, controller del Mod Manager
// in-game (Requisito 22).
//
// Questo modulo aggiunge la LOGICA di controllo dell'interfaccia in-game del
// Mod Manager SOPRA la state machine `ModManager` (loader/lifecycle/
// mod_manager.hpp): NON reimplementa il ciclo di vita, ma lo orchestra
// esponendo operazioni osservabili e host-testabili (non c'è una UI reale del
// gioco sull'host). Le responsabilità coperte (Req 22):
//
//   * `listMods()` — elenco delle mod installate con il relativo stato di
//     abilitazione (Req 22.1); elenco VUOTO con messaggio quando non è
//     installata alcuna mod (Req 22.2); budget ≤ 3 s misurato via clock
//     iniettabile;
//   * `enable(id)` / `disable(id)` — applicano la transizione di stato del
//     `ModManager` e aggiornano lo stato mostrato (Req 22.3, budget ≤ 2 s);
//     se la transizione NON viene completata, FALLBACK: mantengono lo stato di
//     abilitazione precedente e segnalano un messaggio di errore (Req 22.4)
//     instradato anche dal sink di transizioni rifiutate del `ModManager`;
//   * `searchMarketplace(query)` — ricerca sul Marketplace (interfaccia
//     INIETTABILE: reale nel loader, fittizia nei test) con budget ≤ 5 s
//     (Req 22.5);
//   * `install(modRef)` — installa una mod del Marketplace aggiungendola
//     all'elenco (Req 22.6); su Marketplace irraggiungibile o installazione
//     non completata ANNULLA l'installazione (ROLLBACK) senza aggiungere la mod
//     all'elenco e segnala l'errore (Req 22.7) — nessuna installazione
//     parziale;
//   * segnale di aggiornamenti disponibili (Req 22.8): le mod con un
//     aggiornamento sono marcate nell'elenco e un segnale iniettabile notifica
//     l'evento; `startUpdate(id)` avvia l'aggiornamento.
//
// Tutti i budget temporali (≤ 3 s elenco, ≤ 2 s abilita/disabilita, ≤ 5 s
// ricerca) sono modellati in modo host-testabile tramite un clock INIETTABILE
// (coerente con `core/centralized_loader.hpp` e `lifecycle/hot_reload.hpp`):
// nessuna chiamata di rete reale né API reali del gioco.
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).
#ifndef PULSE_LOADER_LIFECYCLE_MOD_MANAGER_UI_HPP
#define PULSE_LOADER_LIFECYCLE_MOD_MANAGER_UI_HPP

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "lifecycle/mod_manager.hpp"

namespace pulse::lifecycle {

// ---------------------------------------------------------------------------
// Clock iniettabile per i budget temporali (Req 22.1, 22.3, 22.5). Modellato
// su `steady_clock` (monotòno) perché i budget sono misure di durata.
// Iniettabile per i test, default all'orologio reale.
// ---------------------------------------------------------------------------
using ModManagerUiClock = std::function<std::chrono::steady_clock::time_point()>;

// Budget di completamento delle operazioni (Req 22).
inline constexpr std::chrono::milliseconds kModListBudget{3000};        // Req 22.1
inline constexpr std::chrono::milliseconds kModToggleBudget{2000};      // Req 22.3
inline constexpr std::chrono::milliseconds kMarketplaceSearchBudget{5000};  // Req 22.5

// Sink dei messaggi mostrati all'User (Req 22.4, 22.7). Iniettabile per
// instradare i messaggi verso la UI / il logging del loader.
using ModManagerUiMessageSink = std::function<void(const std::string&)>;

// Segnale di aggiornamenti disponibili (Req 22.8): riceve gli id delle mod per
// cui esiste un aggiornamento. Iniettabile per notificare la UI.
using UpdatesAvailableSignal = std::function<void(const std::vector<ModManagerModId>&)>;

// ---------------------------------------------------------------------------
// Marketplace — interfaccia INIETTABILE (Req 22.5, 22.6, 22.7, 22.8).
//
// Astrae la sorgente remota delle mod così il controller è host-testabile: il
// loader fornisce l'implementazione reale, i test una fittizia. Nessuna
// chiamata di rete reale a questo livello.
// ---------------------------------------------------------------------------

// Una mod come descritta dal Marketplace.
struct MarketplaceMod {
    ModManagerModId id;
    std::string name;
    std::string version;
};

// Esito di una ricerca sul Marketplace (Req 22.5, 22.7).
struct MarketplaceSearchResult {
    bool reachable{true};                  // false => Marketplace irraggiungibile (Req 22.7)
    std::vector<MarketplaceMod> results;   // risultati corrispondenti alla query
    std::string error;                     // valorizzato se !reachable
};

// Esito di un'installazione/aggiornamento sul Marketplace (Req 22.6, 22.7).
struct MarketplaceInstallResult {
    bool ok{true};        // false => installazione non completata (Req 22.7)
    MarketplaceMod mod;   // metadati dell'artefatto installato quando ok
    std::string error;    // valorizzato se !ok
};

class IMarketplace {
public:
    virtual ~IMarketplace() = default;

    // Ricerca le mod corrispondenti alla query (Req 22.5).
    [[nodiscard]] virtual MarketplaceSearchResult search(const std::string& query) = 0;

    // Installa (o aggiorna) la mod identificata da `id` (Req 22.6, 22.8). In
    // caso di fallimento restituisce ok == false: il chiamante NON deve
    // applicare modifiche parziali (Req 22.7).
    [[nodiscard]] virtual MarketplaceInstallResult install(const ModManagerModId& id) = 0;

    // Restituisce gli id, tra quelli installati, per cui è disponibile un
    // aggiornamento (Req 22.8).
    [[nodiscard]] virtual std::vector<ModManagerModId> updatesFor(
        const std::vector<ModManagerModId>& installed) = 0;
};

// ---------------------------------------------------------------------------
// Risultati osservabili delle operazioni del controller.
// ---------------------------------------------------------------------------

// Voce dell'elenco delle mod installate (Req 22.1, 22.8).
struct ModListEntry {
    ModManagerModId id;
    ModState state{ModState::Installed};
    bool enabled{false};          // true sse state == Enabled (Req 22.1)
    bool updateAvailable{false};  // true se esiste un aggiornamento (Req 22.8)
};

// Esito di `listMods()` (Req 22.1, 22.2).
struct ModListResult {
    std::vector<ModListEntry> mods;  // in ordine di installazione (deterministico)
    bool empty{false};               // true sse non è installata alcuna mod (Req 22.2)
    std::string message;             // messaggio "nessuna mod" quando empty (Req 22.2)
    bool withinBudget{true};         // elenco prodotto entro il budget (Req 22.1)
    std::chrono::nanoseconds elapsed{0};
};

// Esito di `enable()` / `disable()` (Req 22.3, 22.4).
struct ModToggleResult {
    bool ok{false};                       // true sse la transizione è stata completata
    ModState previousState{ModState::Installed};  // stato di abilitazione precedente (Req 22.4)
    ModState currentState{ModState::Installed};    // stato dopo l'operazione
    std::string message;                  // messaggio di errore su fallimento (Req 22.4)
    bool withinBudget{true};              // aggiornamento entro il budget (Req 22.3)
    std::chrono::nanoseconds elapsed{0};
};

// Esito di `searchMarketplace()` (Req 22.5, 22.7).
struct SearchResult {
    bool reachable{true};                  // false => Marketplace irraggiungibile (Req 22.7)
    std::vector<MarketplaceMod> results;
    std::string message;                   // messaggio di errore se !reachable
    bool withinBudget{true};               // risultati entro il budget (Req 22.5)
    std::chrono::nanoseconds elapsed{0};
};

// Esito di `install()` / `startUpdate()` (Req 22.6, 22.7).
struct InstallResult {
    bool ok{false};          // true sse l'installazione è stata completata (Req 22.6)
    ModManagerModId id;      // id della mod installata quando ok
    std::string message;     // messaggio di errore su fallimento/rollback (Req 22.7)
};

// ---------------------------------------------------------------------------
// ModManagerUI — controller dell'interfaccia in-game del Mod Manager (Req 22).
//
// Orchestra il `ModManager` (iniettato per riferimento) e un `IMarketplace`
// opzionale (iniettato per puntatore). Mantiene un registro ORDINATO degli id
// installati per poter produrre l'elenco (il `ModManager` non espone
// l'enumerazione delle mod) e l'insieme delle mod con aggiornamento
// disponibile (Req 22.8).
// ---------------------------------------------------------------------------
class ModManagerUI {
public:
    // `manager` è la state machine sottostante (Req 4.x), riusata e NON
    // reimplementata. `marketplace` può essere nullo se il controller è usato
    // solo per elencare/abilitare mod locali. `clock` è iniettabile per i
    // budget (default: steady_clock di sistema).
    explicit ModManagerUI(ModManager& manager,
                          IMarketplace* marketplace = nullptr,
                          ModManagerUiClock clock = nullptr);

    // Sink dei messaggi mostrati all'User (Req 22.4, 22.7).
    void setMessageSink(ModManagerUiMessageSink sink) { messageSink_ = std::move(sink); }

    // Segnale di aggiornamenti disponibili (Req 22.8).
    void setUpdatesSignal(UpdatesAvailableSignal signal) { updatesSignal_ = std::move(signal); }

    // Registra una mod GIÀ presente localmente (es. caricata dal loader) nel
    // controller e nel `ModManager` (stato iniziale `Installed`). La aggiunge
    // al registro ordinato così comparirà in `listMods()`. Re-registrare lo
    // stesso id ne aggiorna le callback senza duplicare la voce in elenco.
    void addInstalledMod(ModManagerModId id,
                         EntryPointFn entry = {},
                         TerminatorFn terminator = {});

    // Elenco delle mod installate con lo stato di abilitazione (Req 22.1).
    // Se non è installata alcuna mod, `empty == true` con un messaggio (Req
    // 22.2). Misura il tempo trascorso e lo confronta con il budget (≤ 3 s).
    [[nodiscard]] ModListResult listMods();

    // Abilita la mod `id` (transizione verso Enabled, Req 22.3). Su transizione
    // non completata mantiene lo stato precedente e segnala l'errore (Req 22.4).
    ModToggleResult enable(const ModManagerModId& id);

    // Disabilita la mod `id` (transizione verso Disabled, Req 22.3). Su
    // transizione non completata mantiene lo stato precedente e segnala
    // l'errore (Req 22.4).
    ModToggleResult disable(const ModManagerModId& id);

    // Ricerca sul Marketplace (Req 22.5). Se il Marketplace è irraggiungibile,
    // `reachable == false` con messaggio (Req 22.7). Misura il tempo trascorso
    // e lo confronta con il budget (≤ 5 s).
    [[nodiscard]] SearchResult searchMarketplace(const std::string& query);

    // Installa la mod del Marketplace identificata da `id` (Req 22.6). Su
    // Marketplace irraggiungibile o installazione non completata ANNULLA
    // l'installazione (ROLLBACK): non aggiunge la mod all'elenco né al
    // `ModManager` e segnala l'errore (Req 22.7) — nessun artefatto parziale.
    InstallResult install(const ModManagerModId& id);

    // Interroga il Marketplace per gli aggiornamenti disponibili delle mod
    // installate (Req 22.8), aggiorna l'insieme interno, marca le voci in
    // elenco e — se presente — emette il segnale di aggiornamenti disponibili.
    // Restituisce gli id con aggiornamento disponibile.
    std::vector<ModManagerModId> refreshUpdates();

    // True se per la mod `id` è disponibile un aggiornamento (Req 22.8).
    [[nodiscard]] bool hasUpdate(const ModManagerModId& id) const;

    // Avvia l'aggiornamento della mod `id` (Req 22.8). Come `install`, su
    // fallimento annulla senza modifiche (Req 22.7) e mantiene il flag di
    // aggiornamento; su successo azzera il flag.
    InstallResult startUpdate(const ModManagerModId& id);

private:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const { return clock_(); }
    void emitMessage(const std::string& message) const {
        if (messageSink_) messageSink_(message);
    }
    [[nodiscard]] bool isInstalled(const ModManagerModId& id) const;

    ModManager& manager_;
    IMarketplace* marketplace_;
    ModManagerUiClock clock_;
    ModManagerUiMessageSink messageSink_{};
    UpdatesAvailableSignal updatesSignal_{};

    // Registro ordinato degli id installati (ordine di installazione) per
    // produrre l'elenco in modo deterministico (Req 22.1).
    std::vector<ModManagerModId> installOrder_;
    // Mod con aggiornamento disponibile (Req 22.8).
    std::vector<ModManagerModId> updates_;
};

}  // namespace pulse::lifecycle

#endif  // PULSE_LOADER_LIFECYCLE_MOD_MANAGER_UI_HPP
