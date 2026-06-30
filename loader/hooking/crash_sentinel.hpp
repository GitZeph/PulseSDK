// loader/hooking/crash_sentinel.hpp — sentinella crash + auto-disable
// (Layer 3 — Hooking Engine, Requisiti 18.2, 18.3, 18.5).
//
// Implementa la "crash attribution + auto-disable" descritta nel design:
//   «un sentinella all'avvio (primi 60 s) correla il crash all'ultima mod
//    attiva nello stack; al riavvio successivo quella mod resta disabilitata
//    con messaggio all'User; i byte originali vengono ripristinati via
//    RollbackStore; se il ripristino fallisce, si interrompe e si segnala la
//    funzione interessata.»
//
// Contratto richiesto dai requisiti:
//   * Req 18.2 — IF una Mod causa un crash riconducibile a uno dei suoi hook
//     ENTRO 60 SECONDI dall'avvio del gioco, THEN registrare quella Mod come
//     disabilitata e impedirne il caricamento all'avvio successivo.
//   * Req 18.3 — WHEN una Mod viene disabilitata a seguito di un crash,
//     mostrare all'User un messaggio col nome della Mod e il motivo
//     (crash all'avvio).
//   * Req 18.5 — IF il ripristino del codice originale di una funzione
//     fallisce, interrompere il ripristino, MANTENERE disabilitata la Mod e
//     segnalare all'User il fallimento e la funzione interessata.
//
// Modello di correlazione (logica originale Pulse, Requisito 27):
//   La sentinella persiste un "session marker" all'avvio che SOPRAVVIVE a un
//   crash del processo (file su disco, scrittura atomica). Il marker registra:
//     - l'istante di avvio della sessione (startupTime);
//     - l'ultima mod diventata attiva nello stack (activeMod);
//     - se la finestra di grazia dei primi 60 s è stata superata (graceSurvived);
//     - se la sessione si è chiusa correttamente (cleanShutdown).
//   Una chiusura pulita marca `cleanShutdown=true`. Se invece il processo
//   muore (crash), il marker resta "aperto". Al riavvio successivo,
//   `recoverFromPreviousSession` esamina il marker della sessione precedente:
//     - chiusura pulita o nessun marker        -> nessun auto-disable;
//     - crash DOPO i primi 60 s (graceSurvived) -> nessun auto-disable;
//     - crash entro i primi 60 s con una mod attiva -> auto-disable di quella
//       mod, ripristino dei suoi byte e messaggio all'User (Req 18.2/18.3);
//       se un ripristino fallisce, interruzione + segnalazione (Req 18.5).
//   La finestra dei "primi 60 s" è governata da `heartbeat()`/`markGraceSurvived()`
//   chiamati dal runtime: finché la grazia non è superata, un crash è imputato
//   all'avvio (Req 18.2). L'elapsed riportato è informativo.
//
// Testabilità: tempo e persistenza sono INIETTABILI. Il costruttore riceve i
// percorsi di marker e lista-disabilitate (in una directory temporanea nei
// test) e un clock `now()`; `recoverFromPreviousSession` riceve la callback di
// write-back dei byte (RestoreWriteFn), così l'intera logica è verificabile
// sull'host senza un crash reale né un backend di piattaforma.
//
// Stack: C++20/23 (Requisito 26.1). Non thread-safe: guidato dal thread di
// caricamento/lifecycle.
#ifndef PULSE_LOADER_HOOKING_CRASH_SENTINEL_HPP
#define PULSE_LOADER_HOOKING_CRASH_SENTINEL_HPP

#include "hooking/hook_chain.hpp"      // pulse::hooking::ModId
#include "hooking/rollback_store.hpp"  // RollbackStore, RestoreWriteFn, StoreResult

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pulse::hooking {

// Finestra di grazia dei "primi 60 s" dall'avvio (Req 18.2).
inline constexpr std::int64_t kCrashGraceWindowSeconds = 60;

// Clock iniettabile: ritorna "ora" in secondi (epoca arbitraria ma monotona
// nell'ambito di una sessione). Default: orologio di sistema.
using SentinelClock = std::function<std::int64_t()>;

// Sink del messaggio all'User (Req 18.3, 18.5). Coerente con DiagnosticSink:
// callback leggero; se nullo, il messaggio è disponibile solo in RecoveryReport.
using UserMessageSink = std::function<void(std::string_view)>;

// Clock di default: secondi dall'epoca di system_clock.
[[nodiscard]] SentinelClock default_sentinel_clock();

// ---------------------------------------------------------------------------
// Esito della procedura di recovery all'avvio.
// ---------------------------------------------------------------------------
enum class SentinelOutcome {
    NoPreviousSession,   // nessun marker precedente: primo avvio o già consumato
    CleanShutdown,       // sessione precedente chiusa correttamente: nessun crash
    CrashOutsideWindow,  // crash ma oltre i primi 60 s: nessun auto-disable (Req 18.2)
    NoActiveModToBlame,  // crash entro 60 s ma nessuna mod attiva da imputare
    AutoDisabled,        // crash entro 60 s: mod disabilitata e byte ripristinati (Req 18.2/18.3)
    RestoreFailed,       // crash entro 60 s: mod disabilitata ma ripristino fallito (Req 18.5)
};

// ---------------------------------------------------------------------------
// RecoveryReport — risultato strutturato di recoverFromPreviousSession.
// ---------------------------------------------------------------------------
struct RecoveryReport {
    SentinelOutcome outcome = SentinelOutcome::NoPreviousSession;
    std::optional<ModId> blamedMod;             // mod imputata (auto-disable)
    std::int64_t crashElapsedSeconds = 0;       // elapsed informativo dall'avvio
    std::size_t restoredFunctions = 0;          // funzioni ripristinate con successo
    std::optional<std::string> failedFunction;  // funzione interessata su RestoreFailed (Req 18.5)
    std::string userMessage;                    // messaggio mostrato all'User (Req 18.3/18.5)

    // True se è stato eseguito un auto-disable (con o senza ripristino riuscito).
    [[nodiscard]] bool didAutoDisable() const noexcept {
        return outcome == SentinelOutcome::AutoDisabled ||
               outcome == SentinelOutcome::RestoreFailed;
    }
};

// ---------------------------------------------------------------------------
// SessionMarker — stato della sessione corrente persistito su disco. Pubblico
// per ispezione nei test; il formato su disco è interno (vedi .cpp).
// ---------------------------------------------------------------------------
struct SessionMarker {
    std::int64_t startupTime = 0;          // istante di avvio (secondi)
    std::int64_t lastHeartbeat = 0;        // ultimo istante di liveness noto
    bool graceSurvived = false;            // i primi 60 s sono stati superati
    bool cleanShutdown = false;            // chiusura pulita (no crash)
    std::optional<ModId> activeMod;        // ultima mod diventata attiva

    friend bool operator==(const SessionMarker&, const SessionMarker&) = default;
};

// ---------------------------------------------------------------------------
// CrashSentinel — sentinella crash + auto-disable.
//
// Uso tipico (runtime del loader):
//   CrashSentinel sentinel{markerPath, disabledPath};
//   // All'avvio, PRIMA di caricare le mod: recupera dalla sessione precedente.
//   RecoveryReport rep = sentinel.recoverFromPreviousSession(rollbackStore, writeFn, userSink);
//   // Avvia la nuova sessione (il marker sopravvive a un crash).
//   sentinel.beginSession();
//   // Quando una mod diventa attiva nello stack:
//   sentinel.recordActiveMod(modId);
//   // Periodicamente / al superamento dei 60 s:
//   sentinel.heartbeat();           // o sentinel.markGraceSurvived();
//   // Alla chiusura ordinata:
//   sentinel.markCleanShutdown();
//
// Le mod imputate restano disabilitate fino a `enableMod` (Mod Manager).
// ---------------------------------------------------------------------------
class CrashSentinel {
public:
    CrashSentinel(std::filesystem::path markerPath,
                  std::filesystem::path disabledPath,
                  SentinelClock clock = nullptr,
                  std::int64_t graceWindowSeconds = kCrashGraceWindowSeconds);

    // -----------------------------------------------------------------------
    // Recovery all'avvio (Req 18.2, 18.3, 18.5).
    //
    // Esamina il marker della sessione precedente e, se è un crash entro i
    // primi 60 s con una mod attiva, disabilita quella mod (persistente),
    // ripristina i byte originali dei SOLI suoi hook tramite `store`/`write`
    // (Req 18.4) e produce il messaggio all'User (Req 18.3). Al primo
    // fallimento del ripristino interrompe, mantiene la mod disabilitata e
    // segnala la funzione interessata (Req 18.5). Consuma il marker precedente.
    // -----------------------------------------------------------------------
    RecoveryReport recoverFromPreviousSession(const RollbackStore& store,
                                              const RestoreWriteFn& write,
                                              const UserMessageSink& userSink = nullptr);

    // Avvia una nuova sessione: scrive un marker fresco che sopravvive a un crash.
    StoreResult beginSession();

    // Registra la mod diventata attiva (ultima mod attiva nello stack, Req 18.2).
    StoreResult recordActiveMod(const ModId& mod);

    // Heartbeat di liveness: aggiorna l'ultimo istante noto e, se sono passati
    // ≥ grace secondi dall'avvio, marca la finestra dei primi 60 s come superata.
    StoreResult heartbeat();

    // Marca esplicitamente come superata la finestra di grazia (Req 18.2): un
    // crash successivo NON sarà imputato all'avvio.
    StoreResult markGraceSurvived();

    // Marca la sessione come chiusa correttamente (no crash): al riavvio
    // successivo non scatta alcun auto-disable.
    StoreResult markCleanShutdown();

    // -----------------------------------------------------------------------
    // Lista delle mod disabilitate (impedirne il caricamento, Req 18.2).
    // -----------------------------------------------------------------------
    [[nodiscard]] bool isModDisabled(const ModId& mod) const;
    [[nodiscard]] const std::vector<ModId>& disabledMods() const noexcept {
        return disabled_;
    }

    // Riabilita una mod (Mod Manager): la rimuove dalla lista disabilitate.
    // No-op se la mod non era disabilitata.
    StoreResult enableMod(const ModId& mod);

    // -----------------------------------------------------------------------
    // Introspezione per i test.
    // -----------------------------------------------------------------------
    [[nodiscard]] std::optional<SessionMarker> currentMarker() const { return marker_; }
    [[nodiscard]] const std::filesystem::path& markerPath() const noexcept {
        return markerPath_;
    }
    [[nodiscard]] const std::filesystem::path& disabledPath() const noexcept {
        return disabledPath_;
    }

    // Serializzazione/deserializzazione esposte per i test di round-trip.
    [[nodiscard]] static std::vector<std::uint8_t> serializeMarker(const SessionMarker& m);
    static StoreResult deserializeMarker(const std::vector<std::uint8_t>& bytes,
                                         SessionMarker& out);

private:
    StoreResult persistMarker() const;
    StoreResult persistDisabled() const;
    StoreResult loadDisabled();
    void addDisabled(const ModId& mod);
    [[nodiscard]] std::int64_t now() const { return clock_(); }

    std::filesystem::path markerPath_;
    std::filesystem::path disabledPath_;
    SentinelClock clock_;
    std::int64_t graceWindowSeconds_;
    std::optional<SessionMarker> marker_{};  // marker della sessione corrente
    std::vector<ModId> disabled_{};          // mod disabilitate (persistite)
};

}  // namespace pulse::hooking

#endif  // PULSE_LOADER_HOOKING_CRASH_SENTINEL_HPP
