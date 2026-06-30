// loader/lifecycle/mod_manager.hpp — Layer 4 (Mod Lifecycle), state machine
// del `ModManager` (Requisiti 4.4, 4.5, 4.6, 4.8).
//
// Il `ModManager` traccia lo stato di ciascuna mod e applica SOLO le
// transizioni ammesse dalla state machine definita nel design (Layer 4):
//
//   [*] --> Installed
//   Installed --> Enabled   : enable (invoca l'entry point, Req 4.6)
//   Enabled   --> Disabled  : disable / crash / init error
//   Disabled  --> Enabled   : enable (invoca l'entry point, Req 4.6)
//   Installed --> Removed    : remove
//   Disabled  --> Removed    : remove
//   Removed   --> [*]        : terminale
//
// Responsabilità coperte da questo modulo (task 12.1, 12.2):
//   * `ModState` {Installed, Enabled, Disabled, Removed} (Req 4.4);
//   * `transition()` applica solo le transizioni ammesse; le altre sono
//     rifiutate mantenendo lo stato corrente e producendo una segnalazione
//     che identifica la mod e la transizione rifiutata (Req 4.4, 4.5);
//   * all'enable invoca l'entry point dichiarato dalla mod, tramite un
//     registry di callback iniettabile (host-testabile) (Req 4.6);
//   * isolamento del fallimento di inizializzazione (task 12.2, Req 4.7,
//     28.4, 28.5): se l'entry point fallisce — restituendo errore O lanciando
//     un'eccezione — la mod viene portata a `Disabled` con una segnalazione
//     (mod + causa) e il caricamento delle altre mod prosegue; vedi
//     `enableAll()`;
//   * alla chiusura del gioco invoca il terminator di ciascuna mod abilitata
//     in ordine INVERSO rispetto all'ordine di caricamento (Req 4.8).
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).
#ifndef PULSE_LOADER_LIFECYCLE_MOD_MANAGER_HPP
#define PULSE_LOADER_LIFECYCLE_MOD_MANAGER_HPP

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulse::lifecycle {

// ---------------------------------------------------------------------------
// ModId — identità della mod (Requisito 16.1). Modellato come stringa,
// coerente con `pulse::lifecycle::ModId` del DependencyResolver e con
// `pulse::hooking::ModId`.
// ---------------------------------------------------------------------------
using ModManagerModId = std::string;

// ---------------------------------------------------------------------------
// ModState — i quattro stati del ciclo di vita di una mod (Requisito 4.4).
// ---------------------------------------------------------------------------
enum class ModState {
    Installed,  // installata, non ancora abilitata
    Enabled,    // abilitata: entry point invocato
    Disabled,   // disabilitata
    Removed,    // rimossa (stato terminale)
};

// Rappresentazione testuale di uno stato, per diagnostica/segnalazioni.
[[nodiscard]] std::string_view to_string(ModState state) noexcept;

// ---------------------------------------------------------------------------
// Entry point / terminator dichiarati dalla mod (Manifest, Req 16.1).
//
// Sono modellati come callback iniettabili così il ciclo di vita è
// host-testabile senza un binario reale di Geometry Dash: il loader registra
// le callback reali, i test ne registrano di fittizie.
// ---------------------------------------------------------------------------

// Esito dell'invocazione dell'entry point di inizializzazione (Req 4.6, 4.7).
enum class EntryPointStatus {
    NotInvoked,  // nessun entry point registrato / transizione non di enable
    Ok,          // inizializzazione completata con successo
    Error,       // inizializzazione fallita: l'entry point ha restituito errore (Req 4.7)
    Threw,       // inizializzazione fallita: l'entry point ha lanciato un'eccezione,
                 // isolata dal manager (Req 4.7, 28.4)
};

struct EntryPointOutcome {
    bool ok{true};
    std::string message{};  // descrizione dell'errore se ok == false

    [[nodiscard]] static EntryPointOutcome success() noexcept { return EntryPointOutcome{true, {}}; }
    [[nodiscard]] static EntryPointOutcome failure(std::string msg) {
        return EntryPointOutcome{false, std::move(msg)};
    }
};

// Entry point di inizializzazione invocato all'enable (Req 4.6).
using EntryPointFn = std::function<EntryPointOutcome()>;
// Terminator invocato alla chiusura del gioco (Req 4.8).
using TerminatorFn = std::function<void()>;

// ---------------------------------------------------------------------------
// Esito di una richiesta di transizione (Req 4.4, 4.5).
// ---------------------------------------------------------------------------
enum class TransitionStatus {
    Applied,     // transizione ammessa e applicata
    Rejected,    // transizione non ammessa: stato corrente mantenuto (Req 4.5)
    UnknownMod,  // mod non registrata nel manager
};

// Segnalazione di una transizione rifiutata (Req 4.5): identifica la mod e la
// transizione rifiutata (stato di partenza -> stato richiesto).
struct TransitionRejection {
    ModManagerModId mod;
    ModState from{ModState::Installed};
    ModState requested{ModState::Installed};
    std::string message;  // descrizione leggibile della transizione rifiutata
};

struct TransitionResult {
    TransitionStatus status{TransitionStatus::UnknownMod};
    // Stato della mod DOPO la chiamata: in caso di rifiuto è invariato (Req 4.5).
    ModState state{ModState::Installed};
    // Popolata se status == Rejected.
    std::optional<TransitionRejection> rejection{};
    // Esito dell'entry point, popolato quando l'enable invoca l'entry point
    // (Req 4.6). Per le altre transizioni resta NotInvoked. Seam per task 12.2.
    EntryPointStatus entryPoint{EntryPointStatus::NotInvoked};
    std::string entryPointMessage{};

    [[nodiscard]] bool applied() const noexcept { return status == TransitionStatus::Applied; }
    [[nodiscard]] bool rejected() const noexcept { return status == TransitionStatus::Rejected; }
};

// Sink di segnalazione delle transizioni rifiutate (Req 4.5). Iniettabile per
// instradare le segnalazioni verso il sistema di logging del loader.
using TransitionReportFn = std::function<void(const TransitionRejection&)>;

// ---------------------------------------------------------------------------
// Isolamento del fallimento di inizializzazione (Req 4.7, 28.4, 28.5).
//
// Quando l'entry point di una mod fallisce — sia restituendo un esito di
// errore (`EntryPointOutcome::failure`) sia lanciando un'eccezione — il
// fallimento è ISOLATO a quella sola mod: la mod viene portata allo stato
// `Disabled` e si produce una segnalazione che identifica la mod e la CAUSA
// del fallimento (Req 4.7, 28.5). Il caricamento delle altre mod prosegue
// (Req 28.4).
// ---------------------------------------------------------------------------
struct InitFailure {
    ModManagerModId mod;   // mod il cui entry point è fallito
    std::string cause;     // messaggio di errore / descrizione dell'eccezione
    bool threw{false};     // true se l'entry point ha lanciato un'eccezione (Req 28.4)
};

// Sink di segnalazione dei fallimenti di inizializzazione (Req 4.7, 28.5).
// Iniettabile per instradare le segnalazioni verso il sistema di logging.
using InitFailureReportFn = std::function<void(const InitFailure&)>;

// ---------------------------------------------------------------------------
// Esito del caricamento/abilitazione in blocco di un insieme di mod nell'ordine
// di caricamento risolto (Req 4.7, 28.4): le mod abilitate con successo e le
// mod il cui entry point è fallito (ciascuna isolata e portata a `Disabled`).
// ---------------------------------------------------------------------------
struct EnableAllResult {
    std::vector<ModManagerModId> enabled;  // mod abilitate con successo, in ordine
    std::vector<InitFailure> failed;       // mod fallite (isolate), in ordine
};

// ---------------------------------------------------------------------------
// ModManager — state machine del ciclo di vita delle mod.
// ---------------------------------------------------------------------------
class ModManager {
public:
    // True se la transizione `from -> to` è ammessa dalla state machine.
    [[nodiscard]] static bool isAllowed(ModState from, ModState to) noexcept;

    // Registra una mod nello stato iniziale `Installed` (Req 4.4). `entry` è
    // l'entry point invocato all'enable (Req 4.6); `terminator` è invocato alla
    // chiusura (Req 4.8). Entrambi sono opzionali. Re-registrare la stessa mod
    // ne aggiorna le callback mantenendo lo stato corrente.
    void registerMod(ModManagerModId id, EntryPointFn entry = {}, TerminatorFn terminator = {});

    // True se la mod è registrata nel manager.
    [[nodiscard]] bool contains(std::string_view id) const noexcept;

    // Stato corrente della mod, o nullopt se non registrata.
    [[nodiscard]] std::optional<ModState> stateOf(std::string_view id) const noexcept;

    // Richiede una transizione di stato (Req 4.4). Applica la transizione solo
    // se ammessa; altrimenti la rifiuta mantenendo lo stato corrente e
    // producendo una segnalazione (Req 4.5). Se la transizione ammessa porta a
    // `Enabled`, invoca l'entry point della mod (Req 4.6) e ne riporta l'esito.
    // Se l'entry point fallisce — restituendo errore O lanciando un'eccezione —
    // il fallimento è ISOLATO: la mod è portata allo stato `Disabled` e si
    // produce una segnalazione che identifica la mod e la causa (Req 4.7, 28.5).
    TransitionResult transition(std::string_view id, ModState target);

    // Helper espressivi sopra `transition`.
    TransitionResult enable(std::string_view id) { return transition(id, ModState::Enabled); }
    TransitionResult disable(std::string_view id) { return transition(id, ModState::Disabled); }
    TransitionResult remove(std::string_view id) { return transition(id, ModState::Removed); }

    // Abilita in blocco le mod elencate in `loadOrder` (tipicamente
    // `LoadPlan.order` del DependencyResolver), nell'ordine fornito (Req 4.6).
    // L'eventuale fallimento dell'entry point di una mod è ISOLATO a quella sola
    // mod — sia che l'entry point restituisca un errore sia che lanci
    // un'eccezione: la mod è portata allo stato `Disabled` con una segnalazione
    // (mod + causa, Req 4.7, 28.5) e il caricamento delle ALTRE mod PROSEGUE
    // (Req 28.4). Le mod non registrate sono ignorate. Restituisce l'elenco
    // delle mod abilitate con successo e quello delle mod fallite, in ordine.
    EnableAllResult enableAll(const std::vector<ModManagerModId>& loadOrder);

    // Chiusura del gioco (Req 4.8): invoca il terminator di OGNI mod abilitata
    // in ordine INVERSO rispetto a `loadOrder`. Restituisce gli id delle mod il
    // cui terminator è stato invocato, nell'ordine di invocazione (reverse load
    // order). Le mod non abilitate o assenti da `loadOrder` sono ignorate.
    std::vector<ModManagerModId> shutdown(const std::vector<ModManagerModId>& loadOrder);

    // Imposta il sink di segnalazione delle transizioni rifiutate (Req 4.5).
    void setReportSink(TransitionReportFn sink) { reportSink_ = std::move(sink); }

    // Imposta il sink di segnalazione dei fallimenti di inizializzazione
    // (Req 4.7, 28.5): instrada mod + causa verso il logging del loader.
    void setInitFailureSink(InitFailureReportFn sink) { initFailureSink_ = std::move(sink); }

    // Tutte le segnalazioni di transizione rifiutata accumulate (Req 4.5).
    [[nodiscard]] const std::vector<TransitionRejection>& rejections() const noexcept {
        return rejections_;
    }

    // Tutte le segnalazioni di fallimento di inizializzazione accumulate
    // (Req 4.7, 28.5): identificano la mod e la causa del fallimento.
    [[nodiscard]] const std::vector<InitFailure>& initFailures() const noexcept {
        return initFailures_;
    }

private:
    struct ModEntry {
        ModState state{ModState::Installed};
        EntryPointFn entry{};
        TerminatorFn terminator{};
    };

    std::unordered_map<ModManagerModId, ModEntry> mods_;
    std::vector<TransitionRejection> rejections_;
    TransitionReportFn reportSink_{};
    std::vector<InitFailure> initFailures_;
    InitFailureReportFn initFailureSink_{};

    void reportRejection(TransitionRejection rejection);
    void reportInitFailure(InitFailure failure);
};

}  // namespace pulse::lifecycle

#endif  // PULSE_LOADER_LIFECYCLE_MOD_MANAGER_HPP
