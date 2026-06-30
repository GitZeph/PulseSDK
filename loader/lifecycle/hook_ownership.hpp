// loader/lifecycle/hook_ownership.hpp — attribuzione degli hook al Mod_Id
// proprietario via "finestre di epoca" attorno al caricamento del Mod_Module
// (External Mod Loading, task 5.5).
//
// Il problema (design §6): `pulse::hooks::registry()` è un vettore globale
// unico, popolato dagli inizializzatori statici dei `PULSE_HOOK`. Non porta
// alcuna informazione di proprietà: dopo che più Mod_Module hanno registrato i
// loro hook, il registro è una lista piatta. Per soddisfare:
//   * Req 5.8  — hook interrogabili/rimovibili per Mod_Id proprietario;
//   * Req 9.6  — l'insieme installato è esattamente l'unione degli hook delle
//                mod nello stato Enabled;
// serve un livello di proprietà SENZA modificare l'header `hooks.hpp` e SENZA
// rompere la demo `pulse-gd-integration`.
//
// La soluzione: gli inizializzatori `PULSE_HOOK` di un Mod_Module eseguono al
// `dlopen` (static init), quindi TUTTE le registrazioni di una mod cadono in un
// intervallo contiguo del registro globale delimitato da `pulse::hooks::count()`
// prima (`openEpoch`) e dopo (`closeEpoch`) il caricamento + l'invocazione
// dell'entry point. Il ledger registra questa finestra e ne deriva la proprietà.
//
// Assunzione di serializzazione (design §6): i caricamenti delle mod sono
// serializzati sul singolo thread di early-load (ordine `LoadPlan.order`), così
// le finestre di epoca non si sovrappongono. Stessa ipotesi single-thread di
// `HookContext`/`HookChain` di `pulse-gd-integration`.
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).
#ifndef PULSE_LOADER_LIFECYCLE_HOOK_OWNERSHIP_HPP
#define PULSE_LOADER_LIFECYCLE_HOOK_OWNERSHIP_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <pulse/hooks.hpp>

#include "hooking/hook_chain.hpp"  // pulse::hooking::ModId

namespace pulse::lifecycle {

// Identificatore della mod proprietaria di un hook, coerente con
// `pulse::hooking::ModId` (l'owner dei record di rollback) — è una stringa.
using ModId = pulse::hooking::ModId;

// ---------------------------------------------------------------------------
// kBuiltinDemoModId — Mod_Id riservato della demo interna `menulayer_init_hook`
// compilata nel Loader_Artifact. La demo registra il proprio `PULSE_HOOK` allo
// static-init del `.dylib`, PRIMA di qualunque epoca di mod esterna; all'avvio
// il ledger esegue `closeEpoch(kBuiltinDemoModId, 0)` (vedi `seedBuiltinDemo`)
// per attribuirle le registrazioni preesistenti `[0, count())`, così la demo
// resta un "mod" di prima classe nel ledger. Il valore coincide con il
// `kDemoModId` usato dal cablaggio MVP così la diagnostica end-to-end è
// coerente.
// ---------------------------------------------------------------------------
inline constexpr std::string_view kBuiltinDemoModId = "pulse.demo";

// ---------------------------------------------------------------------------
// OwnedHook — un hook EFFETTIVAMENTE installato attribuito al suo Mod_Id
// proprietario.
//   * owner          — Mod_Id che possiede l'hook (rimozione/teardown per owner).
//   * symbol         — simbolo bersaglio (per diagnostica, Req 10.3).
//   * target         — indirizzo risolto su cui è installato il detour.
//   * registryIndex  — posizione nel registro globale `pulse::hooks::registry()`.
// ---------------------------------------------------------------------------
struct OwnedHook {
    ModId owner;
    std::string symbol;
    std::uintptr_t target{0};
    std::size_t registryIndex{0};

    friend bool operator==(const OwnedHook&, const OwnedHook&) = default;
};

// ---------------------------------------------------------------------------
// HookOwnershipLedger — ledger a finestre di epoca (design §6).
//
// Due responsabilità distinte e indipendenti:
//   (1) Proprietà delle REGISTRAZIONI: `closeEpoch` registra che le voci del
//       registro globale in `[start, count())` appartengono a un Mod_Id e
//       restituisce i relativi indici. La mappa indice→owner persiste anche
//       quando la mod è Disabled (il modulo resta caricato, le sue
//       registrazioni restano nel registro): consente il re-enable riusando la
//       finestra nota (Req 7.5).
//   (2) Proprietà degli hook INSTALLATI: `attribute` registra un `OwnedHook`
//       effettivamente installato; `hooksOf`/`allInstalled` lo interrogano;
//       `release` lo rimuove allo scaricamento/disabilitazione. Questo insieme
//       è esattamente l'unione degli hook delle mod Enabled (Req 9.6).
//
// La classe NON modifica `pulse::hooks` e non installa/rimuove hook: si limita
// ad attribuire e contabilizzare. Single-thread (vedi nota in testa al file).
// ---------------------------------------------------------------------------
class HookOwnershipLedger {
public:
    // Inizio finestra: snapshot di `pulse::hooks::count()` PRIMA del `dlopen`
    // del Mod_Module. Funzione pura (nessun effetto sul ledger).
    [[nodiscard]] std::size_t openEpoch() const { return pulse::hooks::count(); }

    // Chiusura finestra: tutte le registrazioni in `[start, count())` sono del
    // `owner`. Registra l'attribuzione indice→owner e restituisce gli indici
    // della finestra, in ordine crescente (Req 5.8). `start` oltre `count()` è
    // trattato fail-soft come finestra vuota.
    std::vector<std::size_t> closeEpoch(const ModId& owner, std::size_t start);

    // Seeding della demo (design §6): attribuisce le registrazioni preesistenti
    // `[0, count())` al Mod_Id riservato `kBuiltinDemoModId`. Da invocare una
    // sola volta all'avvio, prima di aprire qualunque epoca di mod esterna.
    // Restituisce gli indici attribuiti alla demo.
    std::vector<std::size_t> seedBuiltinDemo() {
        return closeEpoch(ModId(kBuiltinDemoModId), 0);
    }

    // Registra un hook effettivamente installato come di proprietà del Mod_Id
    // (Req 5.8). L'ordine di inserimento è preservato.
    void attribute(OwnedHook hook);

    // Hook attualmente installati di proprietà del Mod_Id, in ordine di
    // attribuzione (Req 5.8, 7.2, 8.2). Vuoto se la mod non ha hook installati.
    [[nodiscard]] std::vector<OwnedHook> hooksOf(const ModId& owner) const;

    // Rimuove l'attribuzione degli hook installati del Mod_Id dopo la loro
    // disinstallazione (Req 7.3, 8.6). NON tocca la mappa indice→owner: il
    // modulo può restare caricato (re-enable, Req 7.5).
    void release(const ModId& owner);

    // Unione di tutti gli hook attribuiti (ordine di attribuzione globale): a
    // regime coincide con gli hook delle mod nello stato Enabled (Req 9.6).
    [[nodiscard]] std::vector<OwnedHook> allInstalled() const;

    // --- Introspezione (diagnostica/test) ---------------------------------

    // Mod_Id proprietario della registrazione all'indice `index` del registro
    // globale, secondo le finestre di epoca chiuse; stringa vuota se l'indice
    // non è stato attribuito ad alcuna finestra.
    [[nodiscard]] ModId ownerOfIndex(std::size_t index) const;

    // Numero di hook attualmente attribuiti come installati.
    [[nodiscard]] std::size_t installedCount() const { return installed_.size(); }

private:
    // Attribuzione delle REGISTRAZIONI del registro globale (finestre di epoca):
    // indice nel registro → Mod_Id proprietario.
    std::unordered_map<std::size_t, ModId> registryOwner_;

    // Hook EFFETTIVAMENTE installati, in ordine di attribuzione.
    std::vector<OwnedHook> installed_;
};

}  // namespace pulse::lifecycle

#endif  // PULSE_LOADER_LIFECYCLE_HOOK_OWNERSHIP_HPP
