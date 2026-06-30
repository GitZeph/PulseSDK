// loader/surface/surface_report.cpp — costruzione del Surface_Runtime_Report
// (task 13.1, 13.2, 13.4; Req 6.1-6.4, 7.1-7.4, 8.1-8.4).
#include "surface/surface_report.hpp"

#include <optional>

namespace pulse::loader::surface {

const IPrologueVerifier& default_prologue_verifier() {
    // Verificatore di prologo predefinito (host/CI): accetta la verifica offline
    // registrata nel `.pbind`. I controlli strutturali restano in
    // `address_is_verified`. Vita statica: il riferimento resta valido.
    static const bindings::TrustingPrologueVerifier verifier;
    return verifier;
}

SurfaceRuntimeReport classify_surface(const std::vector<std::string_view>& symbols,
                                      const BindingSet& setForPair,
                                      const IPrologueVerifier& prologueVerifier,
                                      const SurfaceProvenanceStore* provenance,
                                      std::uintptr_t sentinel) {
    SurfaceRuntimeReport report;
    const BindingKey& pair = setForPair.key();

    for (std::string_view symbol : symbols) {
        // (1) Risoluzione del binding per la coppia, ESCLUSIVAMENTE dal set di
        // quella coppia (Req 7.2): corrispondenza esatta del simbolo.
        std::optional<bindings::FunctionBinding> binding = setForPair.resolve(symbol);

        // (2) resolved sse verificato (Req 6.1): RIUSO di `binding_verifier`.
        // `mark_resolved_if_verified` ricalcola `resolved = verified && (offset
        // non-zero && != sentinel && prologo conforme)` — esattamente il
        // predicato richiesto, identico fra provider (Req 9.3).
        const bool verifierResolved =
            binding.has_value() &&
            bindings::mark_resolved_if_verified(*binding, prologueVerifier, sentinel)
                .resolved;

        if (!verifierResolved) {
            // (3) Binding non risolto → non installato, elencato separatamente
            // (Req 6.2, 6.3, 7.4). Nessun ProvenanceRef (non-null sse risolvibile).
            report.unresolvable.push_back(
                SurfaceElementStatus{symbol, /*resolvable=*/false, /*provenance=*/nullptr});
            continue;
        }

        // (4) Gate di provenienza (Req 8.1, 8.2, 8.3). Per riferimento, senza
        // ricomputazione: si LEGGE soltanto lo store (specchio di
        // `ProvenanceStore::get`).
        const ProvenanceRecord* record =
            provenance != nullptr ? provenance->get(symbol, pair) : nullptr;

        // Completezza:
        //   * record presente  → esito del gate runtime `record->complete()`
        //     (specchio di `gate_distributable`, Req 8.2);
        //   * record assente   → completezza implicata dal flag `verified` del
        //     `.pbind` + gate build-time già applicato (Req 8.1) → completa.
        const bool complete = (record != nullptr) ? record->complete() : true;

        if (!complete) {
            // `verified = true` ma provenienza incompleta → declassato a non
            // risolvibile e segnalato con simbolo + coppia (Req 8.2, 8.4).
            report.unresolvable.push_back(
                SurfaceElementStatus{symbol, /*resolvable=*/false, /*provenance=*/nullptr});
            report.auditErrors.push_back(SurfaceAuditError{symbol, pair});
            continue;
        }

        // Risolvibile: espone la provenienza per riferimento (Req 8.1, 8.3).
        report.provenanceStorage.push_back(
            ProvenanceRef{pair, /*complete=*/true, /*record=*/record});
        report.resolvable.push_back(SurfaceElementStatus{
            symbol, /*resolvable=*/true, /*provenance=*/&report.provenanceStorage.back()});
    }

    return report;
}

}  // namespace pulse::loader::surface
