// loader/surface/surface_provenance.cpp — store di provenienza runtime in sola
// lettura (task 13.4, Req 8.1, 8.2, 8.3).
#include "surface/surface_provenance.hpp"

#include <utility>

namespace pulse::loader::surface {

void SurfaceProvenanceStore::put(ProvenanceRecord record) {
    // (simbolo, coppia) esatta unica: sostituisce un eventuale record
    // preesistente con la stessa chiave (nessun duplicato, nessun fuzzy-match).
    for (auto& existing : records_) {
        if (existing.symbol == record.symbol && existing.pair == record.pair) {
            existing = std::move(record);
            return;
        }
    }
    records_.push_back(std::move(record));
}

const ProvenanceRecord* SurfaceProvenanceStore::get(std::string_view symbol,
                                                    const BindingKey& pair) const {
    // Corrispondenza ESATTA di simbolo + coppia. Sola lettura: nessuna
    // ricomputazione di cross-check o prologo (Req 8.3).
    for (const auto& record : records_) {
        if (record.symbol == symbol && record.pair == pair) {
            return &record;
        }
    }
    return nullptr;
}

}  // namespace pulse::loader::surface
