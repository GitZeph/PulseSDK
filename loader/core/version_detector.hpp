// loader/core/version_detector.hpp — astrazione per il rilevamento di GD_Version.
//
// Il rilevamento reale confronta hash/firme di sezioni note del binario di
// Geometry Dash contro la tabella dei bindings. Per renderlo testabile sull'host
// (dove il processo di GD non è presente), la logica è dietro l'interfaccia
// `IVersionDetector`: i test possono iniettare un detector fittizio, mentre
// `DefaultVersionDetector` fornisce l'implementazione basata su firme.
#ifndef PULSE_LOADER_CORE_VERSION_DETECTOR_HPP
#define PULSE_LOADER_CORE_VERSION_DETECTOR_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/runtime_context.hpp"

namespace pulse::loader {

// Firma di una sezione nota del binario: una versione di GD è identificata
// quando l'hash dei byte di una sezione corrisponde a `sectionHash`.
struct VersionSignature {
    GdVersion version{};
    std::uint64_t sectionHash{0};
};

// Interfaccia di rilevamento di GD_Version. Iniettabile per i test host.
class IVersionDetector {
public:
    virtual ~IVersionDetector() = default;

    // Restituisce la GD_Version rilevata, oppure `std::nullopt` se il binario
    // del gioco non può essere identificato (Requisito 1.7).
    virtual std::optional<GdVersion> detect() = 0;
};

// Hash stabile e deterministico (FNV-1a 64-bit) dei byte di una sezione.
// Esposto per consentire ai test e al provider dei bindings di calcolare le
// firme in modo coerente con il detector di default.
std::uint64_t section_hash(std::span<const std::byte> bytes) noexcept;

// Implementazione di default: confronta l'hash della sezione nota del binario
// di GD contro una tabella di firme. In assenza di una sezione leggibile (es.
// esecuzione host senza il processo di gioco) il rilevamento fallisce
// restituendo `std::nullopt`, coerentemente con il comportamento atteso.
class DefaultVersionDetector final : public IVersionDetector {
public:
    DefaultVersionDetector();
    explicit DefaultVersionDetector(std::vector<VersionSignature> signatures);

    std::optional<GdVersion> detect() override;

    // Rileva e costruisce un `RuntimeContext` in sola lettura con la coppia
    // *esatta* (GD_Version, piattaforma) ricavata dal binario realmente
    // caricato (Requisiti 5.1, 5.2, 5.5). Restituisce `std::nullopt` quando la
    // GD_Version non è identificabile, così il chiamante può fallire in modo
    // sicuro senza esporre una coppia incompleta. Il valore restituito è una
    // copia immutabile da inoltrare a `IBindingsProvider` con la coppia esatta.
    [[nodiscard]] std::optional<RuntimeContext> detect_context();

    // Fornisce i byte della sezione nota da ispezionare. Restituisce uno span
    // vuoto quando nessun binario di GD è mappato nel processo corrente.
    // Virtuale per permettere ai test di simulare un binario caricato.
    //
    // Sul Prioritized_Target (macOS) e quando il loader è compilato come
    // Loader_Artifact (`PULSE_LOADER_ARTIFACT`), questo metodo legge la sezione
    // nota direttamente dall'immagine Mach-O caricata in memoria; sull'host di
    // test (macro non definita) resta uno span vuoto/iniettabile.
    virtual std::span<const std::byte> known_section_bytes() const;

    // Deriva la `Platform` runtime dall'immagine realmente caricata. Sul
    // Loader_Artifact macOS la ricava dal `cputype/cpusubtype` dell'header
    // Mach-O; sull'host di test ricade su `current_platform()` (compile-time).
    // Virtuale per consentire ai test di simulare la piattaforma rilevata.
    [[nodiscard]] virtual Platform detected_platform() const;

    // Identificatore testuale stabile della piattaforma rilevata
    // ("macos-arm64"/"macos-x64" derivato da `cputype/cpusubtype` sul
    // Loader_Artifact macOS; `platform_id(detected_platform())` sull'host).
    // Virtuale per consentire ai test di simulare l'identificatore rilevato.
    [[nodiscard]] virtual std::string detected_platform_id() const;

private:
    std::vector<VersionSignature> signatures_;
};

}  // namespace pulse::loader

#endif  // PULSE_LOADER_CORE_VERSION_DETECTOR_HPP
