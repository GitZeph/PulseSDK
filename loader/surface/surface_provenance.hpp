// loader/surface/surface_provenance.hpp — gate di provenienza runtime in sola
// lettura per la GD_API_Surface (task 13.4, Req 8.1, 8.2, 8.3).
//
// CONTESTO E DECISIONE DI MODELLAZIONE
// ------------------------------------
// Il `Provenance_Record` "fonte di verità" vive nel Binding_Catalog (Rust,
// `cli/src/bindings/provenance.rs`) ed è applicato a BUILD-TIME dal gate
// `gate_distributable`: un offset passa a `Verified_Flag = true` SOLO se la
// provenienza documenta SIA l'esito dell'Observational_Cross_Check SIA l'esito
// della Prologue_Verification. Il `.pbind` distribuito porta quindi un
// `verified` che è già passato dal gate di completezza.
//
// Il modello dati runtime del loader (`FunctionBinding` in
// `loader/bindings/bindings.hpp`) NON trasporta la provenienza: porta solo
// `symbol`, `address`, `signature`, `resolved`. Di conseguenza, la
// completezza della provenienza a runtime è rappresentata in modo predefinito
// dal flag `verified` del `.pbind` (che il gate build-time ha già validato)
// più il predicato `binding_verifier` (`resolved sse verificato`). Questo file
// aggiunge un **gate runtime in sola lettura** che:
//
//   1) espone la provenienza di un `API_Element` risolvibile PER RIFERIMENTO,
//      senza rieseguire cross-check né prologo (Req 8.3) — specchio di
//      `ProvenanceStore::get`;
//   2) tratta un elemento `verified = true` ma con provenienza INCOMPLETA come
//      NON risolvibile, segnalandolo con simbolo+coppia (Req 8.2) — specchio di
//      `gate_distributable`.
//
// Quando nessun `ProvenanceRecord` è disponibile a runtime (caso predefinito:
// si dispone solo del `.pbind`), la completezza è implicata dal flag `verified`
// e dal gate build-time già applicato: il `ProvenanceRef` risultante riporta
// `complete = true` con `record = nullptr`. Iniettando un `SurfaceProvenanceStore`
// (test/host o un futuro caricatore di provenienza) si può esercitare il gate
// di completezza in modo esplicito.
//
// NESSUNA RICOMPUTAZIONE: lo store è di sola lettura; `get` restituisce un
// puntatore al record memorizzato senza eseguire alcun cross-check o prologo.
#ifndef PULSE_LOADER_SURFACE_SURFACE_PROVENANCE_HPP
#define PULSE_LOADER_SURFACE_SURFACE_PROVENANCE_HPP

#include <string>
#include <string_view>
#include <vector>

#include "bindings/bindings.hpp"

namespace pulse::loader::surface {

using bindings::BindingKey;

// Record di provenienza runtime di un offset, immagine in sola lettura del
// `Provenance_Record` della `Catalog_Entry` per una coppia (Req 8.1).
//
// La completezza (Req 8.2) replica `gate_distributable`: la provenienza è
// completa SSE documenta SIA l'esito dell'Observational_Cross_Check SIA l'esito
// della Prologue_Verification.
struct ProvenanceRecord {
    std::string symbol;            // simbolo canonico, es. "MenuLayer::init"
    BindingKey pair;               // coppia (GD_Version, piattaforma)
    std::string addressSource;     // contributor / address_source (auditabilità)
    bool crossCheckDocumented{false};  // esito Observational_Cross_Check presente
    bool prologueDocumented{false};    // esito Prologue_Verification presente

    // Gate di completezza (Req 8.2), specchio runtime di `gate_distributable`:
    // entrambi gli esiti devono essere documentati.
    [[nodiscard]] bool complete() const {
        return crossCheckDocumented && prologueDocumented;
    }

    friend bool operator==(const ProvenanceRecord&, const ProvenanceRecord&) = default;
};

// Riferimento (non copia) alla provenienza di un `API_Element` per una coppia
// (Req 8). `record` è un puntatore di SOLA LETTURA nel `SurfaceProvenanceStore`
// (o `nullptr` quando la provenienza è implicata dal flag `verified` del
// `.pbind` + gate build-time). `complete` è l'esito del gate (Req 8.2).
struct ProvenanceRef {
    BindingKey pair;
    bool complete{false};
    const ProvenanceRecord* record{nullptr};  // sola lettura, nessuna ricomputazione
};

// Store di provenienza runtime in SOLA LETTURA. Specchio di `ProvenanceStore`:
// indicizza i `ProvenanceRecord` per (simbolo, coppia) esatta e li restituisce
// per riferimento senza rieseguire cross-check né prologo (Req 8.3).
class SurfaceProvenanceStore {
public:
    SurfaceProvenanceStore() = default;

    // Inserisce/sostituisce il record per la sua (simbolo, coppia) esatta.
    // Pensato per il seeding (host/test) o un futuro caricatore di provenienza;
    // non è una ricomputazione, solo memorizzazione.
    void put(ProvenanceRecord record);

    // Restituisce il record per (simbolo, coppia) esatta, o `nullptr` se assente.
    // SOLA LETTURA: nessun cross-check, nessun prologo (Req 8.3). Specchio di
    // `ProvenanceStore::get`.
    [[nodiscard]] const ProvenanceRecord* get(std::string_view symbol,
                                              const BindingKey& pair) const;

    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

private:
    std::vector<ProvenanceRecord> records_;
};

}  // namespace pulse::loader::surface

#endif  // PULSE_LOADER_SURFACE_SURFACE_PROVENANCE_HPP
