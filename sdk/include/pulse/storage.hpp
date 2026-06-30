// pulse/storage.hpp — persistenza dei dati per Mod (Layer 5, Requisito 10).
//
// Questo header fornisce `ModStorage`: uno spazio di archiviazione chiave/valore
// ISOLATO per identità della Mod (Requisito 10.1), con una capacità massima
// configurabile che vale di default almeno 10 MB per Mod. Le semantiche
// osservabili implementate qui sono:
//
//   * `set(key, value)`  — scrive un valore. Se la scrittura supererebbe la
//                          capacità dello spazio, viene RIFIUTATA preservando
//                          invariati i dati già presenti e restituendo un esito
//                          di errore che segnala il superamento della capacità
//                          (Requisito 10.5).
//   * `get(key)`         — legge un valore. Una chiave inesistente restituisce
//                          un valore ASSENTE (`std::nullopt`) senza generare un
//                          errore bloccante (Requisito 10.4).
//
// Isolamento (Requisito 10.1): ogni `ModStorage` è costruito con un `ModId` e
// possiede il proprio spazio dei dati; istanze con identità diverse non
// condividono né leggono i dati l'una dell'altra. In un'implementazione
// file-backed l'identità della Mod determina la directory di archiviazione; qui
// l'implementazione è in-memory con le stesse semantiche osservabili (il
// round-trip read-after-write entro 500 ms — Requisito 10.2 — e i dettagli di
// persistenza su file sono validati dai property test 20.2–20.4).
//
// La capacità è iniettabile dal costruttore per consentire il test del rifiuto
// su capacità superata senza dover allocare 10 MB reali.
//
// Stack: C++20/23 (Requisito 26.1). Header-only, nessuna dipendenza esterna.
#ifndef PULSE_STORAGE_HPP
#define PULSE_STORAGE_HPP

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace pulse::storage {

// ---------------------------------------------------------------------------
// Tipi di base.
// ---------------------------------------------------------------------------

// Identità della Mod proprietaria dello spazio di archiviazione (Req 10.1).
using ModId = std::string;

// Valore persistito. Contenitore di byte binary-safe (può contenere '\0').
using Value = std::string;

// Capacità di default: almeno 10 MB per Mod (Requisito 10.1).
inline constexpr std::size_t kDefaultCapacityBytes = 10u * 1024u * 1024u;

// Categoria dell'errore di scrittura.
enum class StorageErrorCode {
    CapacityExceeded,  // la scrittura supererebbe la capacità (Req 10.5).
};

// Esito di errore di una scrittura, con categoria e descrizione leggibile.
struct StorageError {
    StorageErrorCode code{StorageErrorCode::CapacityExceeded};
    std::string message;
};

// ---------------------------------------------------------------------------
// SetResult — esito di `set()` in stile Result<void>.
//
// Header-only e privo di eccezioni sul percorso felice: `set()` non lancia per
// la chiave assente né per il superamento di capacità (Req 10.4, 10.5).
// ---------------------------------------------------------------------------
class SetResult {
public:
    [[nodiscard]] static SetResult ok() { return SetResult{true, {}}; }
    [[nodiscard]] static SetResult fail(StorageError error) {
        return SetResult{false, std::move(error)};
    }

    [[nodiscard]] bool isOk() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    // Precondizione: !isOk(). Descrive la causa del rifiuto.
    [[nodiscard]] const StorageError& error() const noexcept { return error_; }

private:
    SetResult(bool ok, StorageError error) : ok_(ok), error_(std::move(error)) {}

    bool ok_;
    StorageError error_;
};

// ---------------------------------------------------------------------------
// ModStorage — spazio chiave/valore isolato per identità della Mod (Req 10.1).
// ---------------------------------------------------------------------------
class ModStorage {
public:
    // Costruisce lo spazio isolato per `modId` con capacità `capacityBytes`
    // (default ≥ 10 MB, Req 10.1). La capacità è iniettabile per i test.
    explicit ModStorage(ModId modId,
                         std::size_t capacityBytes = kDefaultCapacityBytes)
        : modId_(std::move(modId)), capacityBytes_(capacityBytes) {}

    // Identità della Mod proprietaria.
    [[nodiscard]] const ModId& modId() const noexcept { return modId_; }

    // Capacità massima dello spazio in byte (Req 10.1).
    [[nodiscard]] std::size_t capacityBytes() const noexcept {
        return capacityBytes_;
    }

    // Byte attualmente occupati (somma di chiave + valore di ogni voce).
    [[nodiscard]] std::size_t usedBytes() const noexcept { return usedBytes_; }

    // Numero di chiavi presenti.
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

    // True se `key` è presente.
    [[nodiscard]] bool contains(std::string_view key) const {
        return data_.find(key) != data_.end();
    }

    // Scrive `value` sotto `key`. Se la scrittura porterebbe l'occupazione oltre
    // la capacità, RIFIUTA la scrittura preservando invariati i dati già
    // presenti e restituisce un errore CapacityExceeded (Req 10.5).
    // La scrittura più recente per una chiave sovrascrive la precedente.
    SetResult set(std::string_view key, const Value& value) {
        const std::size_t entryCost = entrySize(key.size(), value.size());

        auto it = data_.find(key);
        const std::size_t previousCost =
            (it != data_.end())
                ? entrySize(it->first.size(), it->second.size())
                : 0u;

        // Occupazione proiettata se accettassimo la scrittura.
        const std::size_t projected = usedBytes_ - previousCost + entryCost;

        if (projected > capacityBytes_) {
            // Rifiuto: i dati esistenti restano invariati (Req 10.5).
            return SetResult::fail(StorageError{
                StorageErrorCode::CapacityExceeded,
                "scrittura rifiutata: capacità dello spazio di archiviazione "
                "superata"});
        }

        if (it != data_.end()) {
            it->second = value;  // sovrascrittura della voce esistente.
        } else {
            data_.emplace(std::string(key), value);
        }
        usedBytes_ = projected;
        return SetResult::ok();
    }

    // Legge il valore associato a `key`. Restituisce `std::nullopt` se la chiave
    // non esiste, senza generare un errore bloccante (Req 10.4).
    [[nodiscard]] std::optional<Value> get(std::string_view key) const {
        auto it = data_.find(key);
        if (it == data_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    // Elimina la voce associata a `key` liberando lo spazio occupato e
    // restituisce true se la chiave era presente, false altrimenti (operazione
    // idempotente, senza errore bloccante). Usato dall'eliminazione dei dati
    // persistiti su rimozione della Mod previa conferma dell'User (Req 10.6).
    bool remove(std::string_view key) {
        auto it = data_.find(key);
        if (it == data_.end()) {
            return false;
        }
        usedBytes_ -= entrySize(it->first.size(), it->second.size());
        data_.erase(it);
        return true;
    }

private:
    // Costo in byte di una voce: chiave + valore.
    [[nodiscard]] static std::size_t entrySize(std::size_t keyLen,
                                               std::size_t valueLen) noexcept {
        return keyLen + valueLen;
    }

    ModId modId_;
    std::size_t capacityBytes_;
    std::size_t usedBytes_{0};
    // `std::less<>` abilita la ricerca eterogenea con std::string_view senza
    // costruire una std::string temporanea.
    std::map<std::string, Value, std::less<>> data_;
};

}  // namespace pulse::storage

#endif  // PULSE_STORAGE_HPP
