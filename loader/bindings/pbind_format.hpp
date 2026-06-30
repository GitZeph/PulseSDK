// loader/bindings/pbind_format.hpp — parser/serializer del Binding_Set_File `.pbind`.
//
// Formato testuale deterministico, una coppia (GD_Version, piattaforma) per
// file, sotto `mod-index/bindings/{version}/{platform}.pbind` (vedi design →
// Data Models). Il modello logico riusa `BindingSet`/`FunctionBinding`/
// `Signature` definiti in `bindings.hpp` (nessun nuovo modello dati).
//
// Requisiti coperti:
//   - 4.1: formato del Binding_Set_File per le coppie (GD_Version, piattaforma).
//   - 4.6: round-trip parse→serialize→parse uguale a livello di campo
//          (version, platform, e per ogni funzione symbol/offset/signature),
//          senza voci aggiunte/rimosse/alterate. L'ordinamento delle funzioni
//          è canonicalizzato per `symbol` per garantire il determinismo.
//   - 4.7: su contenuto malformato `parse_pbind` restituisce un ERRORE che
//          identifica la causa (e la riga) SENZA lanciare/terminare il
//          processo, così il chiamante può continuare a usare il set embedded.
//
// Layout del formato (canonico prodotto da `serialize_pbind`):
//
//     # commento facoltativo
//     pbind_version = 1
//     gd_version    = 2.2081
//     platform      = macos-arm64
//
//     [function]
//     symbol     = MenuLayer::init
//     offset     = 0x00A1B2C0
//     return     = bool
//     params     = MenuLayer*
//     verified   = true
//
#ifndef PULSE_LOADER_BINDINGS_PBIND_FORMAT_HPP
#define PULSE_LOADER_BINDINGS_PBIND_FORMAT_HPP

#include <optional>
#include <string>
#include <string_view>

#include "bindings.hpp"

namespace pulse::loader::bindings {

// Versione del formato `.pbind` supportata da questo parser/serializer.
inline constexpr int kPbindFormatVersion = 1;

// Errore di parsing di un `.pbind` malformato (Req 4.7).
// `message` identifica la CAUSA del fallimento; `line` è il numero di riga
// 1-based su cui è stato rilevato il problema (0 se non riferito a una riga
// specifica, es. una chiave d'intestazione mancante).
struct PbindParseError {
    std::string message;
    int line{0};
};

// Esito del parsing: o un `BindingSet` valido (`value`) oppure un `error`.
// Esattamente uno dei due è valorizzato. Non si lancia mai un'eccezione né si
// termina il processo: il chiamante può continuare con il set embedded (Req 4.7).
struct PbindParseResult {
    std::optional<BindingSet> value;
    std::optional<PbindParseError> error;

    bool ok() const noexcept { return value.has_value(); }
};

// Analizza il contenuto testuale di un `.pbind` in un `BindingSet`.
// Su contenuto malformato restituisce un `PbindParseResult` con `error`
// valorizzato (causa + riga) e `value` vuoto — mai un'eccezione (Req 4.7).
PbindParseResult parse_pbind(std::string_view content);

// Serializza un `BindingSet` nel formato testuale canonico deterministico:
// chiavi d'intestazione in ordine fisso, poi le funzioni ordinate per `symbol`
// (ordinamento canonico, Req 4.6), ciascuna come blocco `[function]` con i
// campi in ordine fisso. La serializzazione seguita da `parse_pbind` riproduce
// un `BindingSet` uguale a livello di campo all'originale (Req 4.6).
std::string serialize_pbind(const BindingSet& set);

}  // namespace pulse::loader::bindings

#endif  // PULSE_LOADER_BINDINGS_PBIND_FORMAT_HPP
