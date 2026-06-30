// loader/lifecycle/compatibility.hpp — predicato di compatibilità tra il
// Mod_Manifest esteso (sezione `[compat]`) e il `Runtime_Context` rilevato
// (External Mod Loading, task 1.4).
//
// Requisiti coperti:
//   * Req 3.2 — confronto della piattaforma bersaglio e dell'intervallo di
//     versione di GD bersaglio del Mod_Manifest con la piattaforma e la
//     versione di GD del Runtime_Context prima di ammettere la mod.
//   * Req 3.3 — piattaforma diversa → esclusione con diagnostica che riporta la
//     piattaforma bersaglio dichiarata e quella del Runtime_Context.
//   * Req 3.4 — versione del Runtime_Context fuori dall'intervallo `[min, max]`
//     (ESTREMI INCLUSI) → esclusione con diagnostica che riporta la versione
//     rilevata e i due estremi dell'intervallo bersaglio.
//   * Req 3.6 — compat assente (piattaforma o intervallo non dichiarati) →
//     esclusione con diagnostica che nomina il campo mancante.
//
// È un predicato PURO: nessun effetto collaterale, nessun caricamento di
// codice. La promozione della versione di GD del Runtime_Context (coppia
// major/minor) a `SemVer{major, minor, 0}` e il confronto sull'insieme finito
// delle piattaforme bersaglio sono dettagli documentati nel design (§4).
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).
#ifndef PULSE_LOADER_LIFECYCLE_COMPATIBILITY_HPP
#define PULSE_LOADER_LIFECYCLE_COMPATIBILITY_HPP

#include <optional>
#include <string>

#include "core/runtime_context.hpp"
#include "lifecycle/manifest.hpp"

namespace pulse::lifecycle {

// ---------------------------------------------------------------------------
// CompatCause — insieme chiuso delle cause dell'esito di compatibilità.
//   * Ok                — la mod è compatibile.
//   * MissingCompat     — `[compat]` non dichiara piattaforma e/o intervallo
//                         (Req 3.6).
//   * PlatformMismatch  — piattaforma bersaglio diversa da quella del
//                         Runtime_Context (Req 3.3).
//   * VersionOutOfRange — versione di GD del Runtime_Context fuori da
//                         `[min, max]`, estremi inclusi (Req 3.4).
// ---------------------------------------------------------------------------
enum class CompatCause { Ok, MissingCompat, PlatformMismatch, VersionOutOfRange };

// ---------------------------------------------------------------------------
// CompatResult — esito del predicato di compatibilità. `compatible == true`
// implica `cause == CompatCause::Ok`; ogni esclusione porta esattamente una
// causa (≠ Ok) e un messaggio diagnostico leggibile.
// ---------------------------------------------------------------------------
struct CompatResult {
    bool compatible{false};
    CompatCause cause{CompatCause::Ok};
    std::string message;
};

// ---------------------------------------------------------------------------
// runtime_target_platform — proietta la `Platform` runtime sull'insieme finito
// delle `TargetPlatform` dichiarabili dal Mod_Manifest. Ritorna `nullopt` per
// le piattaforme host non rappresentabili nell'insieme finito (es. Linux,
// macOS x86-64, Unknown): in tal caso nessuna piattaforma bersaglio può
// combaciare e la mod è esclusa per `PlatformMismatch`.
// ---------------------------------------------------------------------------
[[nodiscard]] std::optional<pulse::manifest::TargetPlatform> runtime_target_platform(
    pulse::loader::Platform platform) noexcept;

// ---------------------------------------------------------------------------
// check_compatibility — predicato puro (Req 3.2/3.3/3.4/3.6). La versione di GD
// del Runtime_Context è promossa a `SemVer{major, minor, 0}` e confrontata con
// l'intervallo `[min, max]` del manifest a ESTREMI INCLUSI; la piattaforma è
// confrontata sull'insieme finito. Ordine dei controlli (esattamente una
// causa): compat assente → piattaforma → versione.
// ---------------------------------------------------------------------------
[[nodiscard]] CompatResult check_compatibility(const pulse::manifest::Manifest& m,
                                               const pulse::loader::RuntimeContext& ctx);

}  // namespace pulse::lifecycle

#endif  // PULSE_LOADER_LIFECYCLE_COMPATIBILITY_HPP
