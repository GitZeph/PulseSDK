// loader/hooking/rollback_store.hpp — rollback persistente degli hook
// (Layer 3 — Hooking Engine, Requisiti 18.1, 18.4, 18.5).
//
// Definisce `RollbackRecord` (le informazioni necessarie a ripristinare il
// codice originale di una funzione modificata da una mod) e `RollbackStore`
// (la persistenza su disco di questi record e il loro ripristino).
//
// Contratto richiesto dai requisiti:
//   * Req 18.1 — PRIMA di applicare la modifica (install), conservare in
//     archiviazione persistente che sopravvive al riavvio del gioco
//     l'indirizzo della funzione e la sequenza di byte originali sovrascritti.
//   * Req 18.4 — al ripristino, riscrivere nella funzione esattamente i byte
//     originali conservati al momento dell'installazione.
//   * Req 18.5 — se il ripristino di una funzione fallisce, interrompere il
//     ripristino e segnalare la funzione interessata.
//
// Scelta del tipo `GdVersion`:
//   Il progetto contiene due definizioni di `GdVersion`:
//     - `pulse::loader::GdVersion`           (core, campi `std::uint32_t`)
//     - `pulse::loader::bindings::GdVersion` (bindings, campi `int`)
//   `RollbackRecord` adotta `pulse::loader::bindings::GdVersion` perché la
//   versione di un record di rollback è la stessa coppia (GD_Version,
//   piattaforma) che indicizza i bindings con cui l'hook è stato risolto
//   (Req 20.1): persistere la versione dei bindings rende il record
//   auto-descrittivo e direttamente confrontabile con la chiave dei bindings.
//
// Formato su disco: contenitore binario little-endian con magic + versione di
// formato, robusto rispetto a byte arbitrari in `originalBytes` (length-prefix
// esplicito per ogni campo a lunghezza variabile). La scrittura è atomica
// (file temporaneo + rename) così un crash a metà persistenza non corrompe lo
// store esistente.
//
// Stack: C++20/23 (Requisito 26.1). Logica originale Pulse (Requisito 27).
#ifndef PULSE_LOADER_HOOKING_ROLLBACK_STORE_HPP
#define PULSE_LOADER_HOOKING_ROLLBACK_STORE_HPP

#include "bindings/bindings.hpp"
#include "hooking/hook_chain.hpp"  // pulse::hooking::ModId

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pulse::hooking {

// ---------------------------------------------------------------------------
// RollbackRecord — informazioni di ripristino conservate per un singolo hook
// (Req 18.1). Vedi design "Rollback Record (persistente) — Requisito 18".
// ---------------------------------------------------------------------------
struct RollbackRecord {
    ModId owner;                                  // mod proprietaria dell'hook
    std::string symbol;                           // es. "MenuLayer::init"
    std::uintptr_t address = 0;                   // indirizzo della funzione
    std::vector<std::uint8_t> originalBytes;      // prologo originale sovrascritto
    pulse::loader::bindings::GdVersion version{}; // (GD_Version) dei bindings
    std::string platformId;                       // es. "windows-x64"

    friend bool operator==(const RollbackRecord&, const RollbackRecord&) = default;
};

// ---------------------------------------------------------------------------
// Esiti / errori dello store.
// ---------------------------------------------------------------------------
enum class RollbackErrorCode {
    None,
    IoError,          // impossibile aprire/scrivere/leggere il file
    CorruptData,      // contenuto del file non conforme al formato atteso
    RestoreFailed,    // la write-back dei byte originali è fallita (Req 18.5)
    InvalidArgument,  // argomento non valido (es. callback di write nulla)
};

struct RollbackError {
    RollbackErrorCode code{RollbackErrorCode::None};
    std::string message;
    std::string symbol;  // funzione interessata, valorizzata su RestoreFailed (Req 18.5)
};

// Esito di una persistenza/caricamento (operazioni senza valore di ritorno).
struct StoreResult {
    bool ok = false;
    RollbackError error{};

    static StoreResult success() { return StoreResult{true, {}}; }
    static StoreResult failure(RollbackErrorCode code, std::string message) {
        return StoreResult{false, RollbackError{code, std::move(message), {}}};
    }
    explicit operator bool() const noexcept { return ok; }
};

// Callback di ripristino: riscrive `original` all'indirizzo `address` della
// funzione e ritorna true sse il write-back è riuscito (Req 18.4). Astrae il
// meccanismo concreto (backend di piattaforma, mprotect+memcpy, o un doppio di
// test) così lo store resta indipendente dal backend (Requisito 27).
using RestoreWriteFn =
    std::function<bool(std::uintptr_t address, const std::vector<std::uint8_t>& original)>;

// Esito di un ripristino (singolo o batch) (Req 18.4, 18.5).
struct RestoreOutcome {
    std::size_t restored = 0;             // funzioni ripristinate con successo
    bool aborted = false;                 // true sse interrotto su fallimento (Req 18.5)
    std::optional<RollbackError> error;   // valorizzato sse aborted (funzione + causa)

    [[nodiscard]] bool ok() const noexcept { return !aborted; }
};

// ---------------------------------------------------------------------------
// RollbackStore — persistenza e ripristino dei RollbackRecord.
//
// Uso tipico (Hooking Engine):
//   RollbackStore store{path};
//   store.add(record);          // persiste su disco PRIMA di install (Req 18.1)
//   ... install dell'hook ...
//
// Dopo un riavvio:
//   auto loaded = RollbackStore::load(path);   // i record sopravvivono
//   loaded.restoreAll(writeFn);                // ripristina i byte (Req 18.4)
//
// Non thread-safe: guidato dal thread di caricamento/lifecycle.
// ---------------------------------------------------------------------------
class RollbackStore {
public:
    explicit RollbackStore(std::filesystem::path filePath)
        : filePath_(std::move(filePath)) {}

    // Carica uno store dal file indicato. Un file inesistente NON è un errore:
    // restituisce uno store vuoto associato a quel percorso (nessun record da
    // ripristinare). Un file presente ma corrotto produce CorruptData.
    static StoreResult load(const std::filesystem::path& filePath, RollbackStore& out);

    // Aggiunge un record e lo persiste immediatamente su disco (write-through),
    // così l'informazione di rollback sopravvive a un crash subito dopo
    // l'install (Req 18.1). In caso di errore di IO il record NON viene aggiunto
    // in memoria e si restituisce l'errore.
    StoreResult add(RollbackRecord record);

    // Riscrive sul disco l'intero insieme di record corrente (scrittura
    // atomica). Usato internamente da add(); esposto per riscritture esplicite.
    [[nodiscard]] StoreResult persist() const;

    // Ripristina i byte originali di un singolo record tramite `write`
    // (Req 18.4). Su fallimento del write-back restituisce RestoreFailed con la
    // funzione interessata (Req 18.5).
    RestoreOutcome restore(const RollbackRecord& record, const RestoreWriteFn& write) const;

    // Ripristina i byte originali di TUTTI i record nell'ordine di inserimento
    // (Req 18.4). Al primo fallimento interrompe il ripristino e segnala la
    // funzione interessata (Req 18.5); i record già ripristinati restano tali.
    RestoreOutcome restoreAll(const RestoreWriteFn& write) const;

    // Rimuove dal disco il file dello store (e svuota i record in memoria).
    // Idempotente: l'assenza del file non è un errore.
    StoreResult clear();

    // -----------------------------------------------------------------------
    // Introspezione.
    // -----------------------------------------------------------------------
    [[nodiscard]] const std::vector<RollbackRecord>& records() const noexcept {
        return records_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
    [[nodiscard]] bool empty() const noexcept { return records_.empty(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return filePath_; }

    // Serializza/deserializza in/da un buffer di byte (esposto per i test di
    // round-trip in-memory, riusato dalla persistenza su file).
    [[nodiscard]] std::vector<std::uint8_t> serialize() const;
    static StoreResult deserialize(const std::vector<std::uint8_t>& bytes,
                                   std::vector<RollbackRecord>& out);

private:
    std::filesystem::path filePath_;
    std::vector<RollbackRecord> records_{};
};

}  // namespace pulse::hooking

#endif  // PULSE_LOADER_HOOKING_ROLLBACK_STORE_HPP
