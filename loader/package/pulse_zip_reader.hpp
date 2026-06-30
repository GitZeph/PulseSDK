// loader/package/pulse_zip_reader.hpp — lettore `.pulse` (ZIP) su disco,
// self-contained (Fase E — abilitazione del caricamento delle mod esterne).
//
// Questo modulo colma l'ultimo anello mancante della pipeline: leggere un
// container `.pulse` REALE dal filesystem e produrne il modello in memoria
// (`pulse::package::PackageArchive`, percorso-entry -> byte), così che la
// discovery (`discover_mods`) possa riconoscere e aprire le mod a runtime.
//
// SCELTA TECNICA (build priva di dipendenze, Req self-contained):
//   * La CLI `pulse build` scrive le voci del `.pulse` come ZIP con metodo
//     STORED (non compresso). Questo lettore implementa quindi un parser ZIP
//     basato sulla CENTRAL DIRECTORY (robusto ai data descriptor) che supporta
//     ESCLUSIVAMENTE il metodo 0 (STORED): i byte della voce sono copiati
//     direttamente, senza alcuna implementazione di inflate. Per il metodo 8
//     (Deflate) o qualunque altro metodo il parsing fallisce in modo PULITO
//     (nessuna eccezione), nominando la voce e il metodo: il chiamante tratta
//     il container come non leggibile (fail-open, come una voce `.pulse` non
//     apribile in discovery).
//   * Nessuna libreria esterna, nessun FetchContent, nessuna rete: puro C++20.
//
// CONTRATTO no-throw: nessuna funzione di questo modulo lancia eccezioni. Su
// qualunque struttura ZIP malformata (offset/limiti incoerenti, firma assente,
// buffer troppo corto, ...) si imposta `error` e si ritorna `std::nullopt`.
//
// LAYERING: `package` NON dipende da `lifecycle`. Si espongono quindi due
// funzioni libere (`read_pulse_archive`, `open_pulse_file`); è il layer
// runtime/lifecycle ad adattarle in un `pulse::lifecycle::PackageOpener` (evita
// il ciclo di dipendenza package -> lifecycle).
//
// Logica originale Pulse. Stack: C++20/23.
#ifndef PULSE_LOADER_PACKAGE_PULSE_ZIP_READER_HPP
#define PULSE_LOADER_PACKAGE_PULSE_ZIP_READER_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "package/pulse_package.hpp"

namespace pulse::package {

// ---------------------------------------------------------------------------
// read_pulse_archive — analizza i byte di un container `.pulse` (ZIP STORED) e
// ne ricostruisce il `PackageArchive` (percorso-entry -> byte).
//
// Parser basato sulla central directory:
//   * localizza l'End Of Central Directory (firma 0x06054b50) scandendo
//     all'indietro (comment trailing fino a 64KB);
//   * itera i record di central directory (firma 0x02014b50) leggendo metodo,
//     dimensioni, nome e offset dell'header locale;
//   * per ogni voce calcola l'inizio dei dati dall'header locale (firma
//     0x04034b50) e copia `compressedSize` byte (solo metodo 0 = STORED).
//
// Ritorna il `PackageArchive` ricostruito; in caso di struttura malformata o di
// metodo di compressione non supportato imposta `error` (messaggio leggibile,
// nomina la voce/il metodo quando rilevante) e ritorna `std::nullopt`. Non
// lancia mai. Le voci di directory (nome che termina con '/') sono ignorate.
[[nodiscard]] std::optional<PackageArchive> read_pulse_archive(
    const std::vector<std::uint8_t>& zipBytes, std::string& error);

// ---------------------------------------------------------------------------
// open_pulse_file — legge un file `.pulse` dal disco e lo apre come
// `PulsePackage`.
//
// (1) legge i byte del file (std::ifstream binario); su fallimento di
//     apertura/lettura ritorna un `OpenResult` con `ok==false`,
//     `error==OpenError::ManifestMissing` e un messaggio che nomina il path;
// (2) ricostruisce il `PackageArchive` via `read_pulse_archive`; su fallimento
//     ritorna `ok==false` con il messaggio del parser (manifest non
//     raggiungibile → `ManifestMissing`);
// (3) altrimenti delega a `PulsePackage::open(std::move(archive), opts)`, che
//     applica l'ordine garantito manifest → integrità → esposizione del codice.
//
// Non lancia mai eccezioni.
[[nodiscard]] OpenResult open_pulse_file(const std::filesystem::path& path,
                                         const PulsePackage::Options& opts);

}  // namespace pulse::package

#endif  // PULSE_LOADER_PACKAGE_PULSE_ZIP_READER_HPP
