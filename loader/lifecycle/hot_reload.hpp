// loader/lifecycle/hot_reload.hpp — Layer 4 (Mod Lifecycle), orchestrazione
// dell'hot-reload in modalità sviluppo (Requisiti 15.1, 15.2, 15.3, 15.4, 15.5;
// design IMP-11 "Hot-reload dev ≤ 5 s con rimozione+reinstallazione hook
// atomica").
//
// Il servizio `HotReload` ricarica una mod SENZA chiudere il processo di
// Geometry Dash, riusando i mattoni dell'Hooking Engine già presenti:
//   * `hooking::HookEngine` — installazione/rimozione degli hook con retry e
//     rollback atomico (Req 2.4, 2.5) e rimozione selettiva per owner;
//   * `hooking::HookRequest` — descrizione di un singolo hook di una mod.
//
// Sequenza di un reload (in modalità sviluppo, Req 15.1):
//   1. (Req 15.5) se la modalità sviluppo NON è attiva, la richiesta è
//      RIFIUTATA: la mod attualmente caricata resta invariata e si segnala che
//      il reload è disponibile solo in modalità sviluppo;
//   2. (Req 15.2) si RIMUOVONO tutti gli hook della versione precedente della
//      mod (via `HookEngine::remove(owner)`) PRIMA di installare quelli nuovi,
//      e si conferma l'avvenuta rimozione;
//   3. (Req 15.3) si INSTALLANO gli hook della nuova versione (via
//      `HookEngine::installAll`, atomico) e si conferma l'avvenuta
//      installazione al loader;
//   4. (Req 15.4) se l'installazione della nuova versione FALLISCE, si
//      RIPRISTINA lo stato precedente al reload (reinstallazione degli hook
//      della versione precedente), il processo di GD resta attivo e si segnala
//      al developer un messaggio di errore con la causa del fallimento;
//   5. (Req 15.1) l'intero reload deve completare entro un budget temporale (5
//      secondi di default). Il budget è modellato con un `Clock` INIETTABILE
//      così l'intera logica è host-testabile senza dipendere dal tempo reale.
//      Un reload che eccede il budget è trattato come fallimento e ripristina
//      lo stato precedente (Req 15.4), mantenendo GD attivo.
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).
// Non thread-safe: l'hot-reload è guidato dal thread di lifecycle del loader.
#ifndef PULSE_LOADER_LIFECYCLE_HOT_RELOAD_HPP
#define PULSE_LOADER_LIFECYCLE_HOT_RELOAD_HPP

#include "hooking/hook_engine.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::lifecycle {

// ---------------------------------------------------------------------------
// ModId — identità della mod (Requisito 16.1). Coerente con
// `pulse::hooking::ModId` (owner degli hook) e con il ModManager.
// ---------------------------------------------------------------------------
using ModId = std::string;

// Orologio iniettabile: restituisce l'istante corrente. Modellato su
// `steady_clock` (monotòno) perché il budget di reload è una misura di durata
// (Req 15.1). Iniettabile per i test, default all'orologio reale.
using HotReloadClock = std::function<std::chrono::steady_clock::time_point()>;

// Sink diagnostico opzionale: riceve i messaggi delle fasi del reload (per il
// logging del loader). Se nullo, gli eventi sono scartati.
using HotReloadLogSink = std::function<void(const std::string&)>;

// Budget di completamento del reload (Req 15.1): 5 secondi.
inline constexpr std::chrono::milliseconds kHotReloadBudget{5000};

// ---------------------------------------------------------------------------
// HotReloadStatus — esito di una richiesta di reload.
// ---------------------------------------------------------------------------
enum class HotReloadStatus {
    Reloaded,            // reload completato con successo entro il budget (Req 15.1, 15.3)
    RejectedNotDevMode,  // richiesto fuori dalla modalità sviluppo (Req 15.5)
    Failed,              // installazione fallita o budget superato: stato precedente
                         // ripristinato, GD attivo (Req 15.4)
};

// ---------------------------------------------------------------------------
// HotReloadResult — esito strutturato e osservabile di un reload.
// ---------------------------------------------------------------------------
struct HotReloadResult {
    HotReloadStatus status{HotReloadStatus::Failed};
    std::string message;  // descrizione leggibile (causa del fallimento se !ok)

    std::size_t previousHooksRemoved = 0;  // hook della versione precedente rimossi (Req 15.2)
    std::size_t newHooksInstalled = 0;     // hook della nuova versione installati (Req 15.3)

    // True se il percorso di fallimento ha ripristinato lo stato precedente al
    // reload (Req 15.4). Falso per successo o rifiuto.
    bool previousStateRestored = false;

    // Durata misurata del reload e rispetto del budget (Req 15.1). `elapsed` è
    // valorizzata anche per il rifiuto (≈0).
    std::chrono::nanoseconds elapsed{0};
    bool withinBudget = true;

    [[nodiscard]] bool ok() const noexcept { return status == HotReloadStatus::Reloaded; }
    [[nodiscard]] bool rejected() const noexcept {
        return status == HotReloadStatus::RejectedNotDevMode;
    }
};

// ---------------------------------------------------------------------------
// HotReload — servizio di hot-reload dev (Req 15.1–15.5).
//
// Mantiene il registro degli hook attualmente installati per ciascuna mod
// (necessario per ripristinare lo stato precedente su fallimento, Req 15.4) e
// guida l'Hooking Engine per la rimozione+reinstallazione atomica.
// ---------------------------------------------------------------------------
class HotReload {
public:
    // `engine` coordina backend + catena (retry/rollback). `clock` è
    // iniettabile per il budget (Req 15.1): se nullo, usa `steady_clock::now`.
    // `log` è il sink diagnostico opzionale.
    explicit HotReload(hooking::HookEngine& engine,
                       HotReloadClock clock = {},
                       HotReloadLogSink log = {});

    // Imposta il budget di completamento del reload (Req 15.1). Default 5 s.
    void setBudget(std::chrono::milliseconds budget) noexcept { budget_ = budget; }
    [[nodiscard]] std::chrono::milliseconds budget() const noexcept { return budget_; }

    // Carica per la prima volta gli hook di una mod (installazione iniziale,
    // NON un hot-reload: non è gated dalla modalità sviluppo). Installa gli
    // hook via `HookEngine::installAll` (atomico) e, in caso di successo,
    // registra gli hook come "versione corrente" della mod così da poterli
    // ripristinare su un futuro reload fallito (Req 15.4). In caso di
    // fallimento dell'installazione, nessun hook resta installato (atomicità
    // dell'engine) e il registro della mod non viene modificato.
    HotReloadResult installInitial(const ModId& mod, std::vector<hooking::HookRequest> hooks);

    // Ricarica la mod `mod` con gli hook della nuova versione (Req 15.1–15.5).
    //
    // `devMode`: se false, la richiesta è rifiutata e la mod corrente resta
    //   invariata (Req 15.5).
    // `onPreviousRemoved`: callback opzionale invocata DOPO la rimozione degli
    //   hook della versione precedente e PRIMA dell'installazione di quelli
    //   nuovi (Req 15.2). Espone l'ordine "rimozione prima dell'installazione"
    //   in modo osservabile (i test la usano per ispezionare lo stato del
    //   backend tra le due fasi). Non invocata sul percorso di rifiuto.
    HotReloadResult reload(const ModId& mod,
                           std::vector<hooking::HookRequest> newHooks,
                           bool devMode,
                           std::function<void()> onPreviousRemoved = {});

    // Hook attualmente registrati come "versione corrente" della mod (vuoto se
    // la mod non ha hook registrati).
    [[nodiscard]] std::vector<hooking::HookRequest> currentHooks(const ModId& mod) const;

    // True se la mod ha almeno un hook registrato come versione corrente.
    [[nodiscard]] bool hasHooks(const ModId& mod) const noexcept {
        return installed_.find(mod) != installed_.end();
    }

private:
    // Istante corrente secondo il clock iniettato.
    [[nodiscard]] std::chrono::steady_clock::time_point now() const { return clock_(); }

    void logEvent(const std::string& message) const {
        if (log_) {
            log_(message);
        }
    }

    hooking::HookEngine& engine_;
    HotReloadClock clock_;
    HotReloadLogSink log_;
    std::chrono::milliseconds budget_{kHotReloadBudget};

    // Hook installati per ciascuna mod (versione corrente), per il ripristino
    // su reload fallito (Req 15.4).
    std::unordered_map<ModId, std::vector<hooking::HookRequest>> installed_{};
};

}  // namespace pulse::lifecycle

#endif  // PULSE_LOADER_LIFECYCLE_HOT_RELOAD_HPP
