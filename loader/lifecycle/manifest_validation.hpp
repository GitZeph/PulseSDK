// loader/lifecycle/manifest_validation.hpp — Validazione del `Manifest` contro
// lo schema definito (Layer 4 — Lifecycle & Manifest, task 13.2).
//
// Requisiti coperti da questo modulo:
//   * Req 16.2 — il Pulse_Loader valida il Manifest contro lo schema definito
//     PRIMA di caricare qualsiasi codice della Mod. Questo modulo espone una
//     `validate(...)` pura (nessun side-effect, nessun caricamento) pensata per
//     essere invocata come gate a monte del caricamento del codice; la
//     proiezione "valida-prima-di-caricare" è realizzata fornendo l'API anche
//     nella forma `std::optional<Manifest>` così che l'esito (accettazione /
//     rifiuto) sia disponibile senza che alcun codice della Mod venga eseguito.
//   * Req 16.3 — Manifest assente → rifiuto con segnalazione della mancanza.
//     L'overload `validate(const std::optional<Manifest>&)` (e l'helper
//     `validatePresence`) tratta l'assenza come un rifiuto esplicito con una
//     violazione dedicata sul campo virtuale `manifest`.
//   * Req 16.4 — Manifest non conforme → rifiuto con l'ELENCO COMPLETO dei
//     campi non conformi. `validate(const Manifest&)` NON si ferma alla prima
//     violazione: accumula tutte le non conformità e le restituisce in
//     `ValidationResult::violations`, ciascuna con il percorso del campo e una
//     descrizione leggibile.
//
// Regole di schema applicate (derivate da Req 16.1 e Req 17.1, design
// "### Manifest (pulse.toml)" e l'enum `Permission`):
//   - `schema_version`            : intero >= 1;
//   - `[mod].id`                  : stringa non vuota, lunghezza <= 256 (Req 16.1);
//   - `[mod].version`             : SemVer (già tipizzata; vincolo strutturale
//                                   garantito dal tipo). Non si impone il
//                                   non-zero: Req 16.1 richiede solo "una
//                                   versione conforme a uno schema di
//                                   versionamento definito" e non vieta 0.0.0;
//   - `[[entry_points]]`          : almeno uno (Req 16.1); ciascuno con `kind`
//                                   e `symbol` non vuoti;
//   - `[[dependencies]]`          : ciascuna con `id` non vuoto (il vincolo di
//                                   versione è strutturalmente garantito dal
//                                   tipo `VersionConstraint`);
//   - `[permissions].required`    : ogni permesso è una stringa non vuota e
//                                   appartiene all'insieme dei permessi
//                                   riconosciuti (Req 17.1). NB: l'obbligo che
//                                   la sezione sia *presente/non vuota* e il
//                                   consenso dell'User sono enforcement di
//                                   installazione del Mod_Manager (Req 17.2),
//                                   non vincoli di forma del Manifest, e non
//                                   sono imposti qui;
//   - `[[settings]]`              : ciascuna con `name` non vuoto, `type` in
//                                   {int, float, bool, string} e `default`
//                                   coerente col `type` dichiarato.
//
// Header-only: ricade nella include path del modulo lifecycle e non richiede
// modifiche a `loader/CMakeLists.txt` né un nuovo .cpp; non tocca l'API di
// parse/serialize di `manifest.hpp` (estensione puramente additiva).
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).
#ifndef PULSE_LOADER_LIFECYCLE_MANIFEST_VALIDATION_HPP
#define PULSE_LOADER_LIFECYCLE_MANIFEST_VALIDATION_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "lifecycle/manifest.hpp"

namespace pulse::manifest {

// ---------------------------------------------------------------------------
// FieldViolation — una singola non conformità: il percorso del campo (es.
// "mod.id", "entry_points[1].symbol", "permissions.required[2]") più un
// messaggio leggibile che descrive la regola violata.
// ---------------------------------------------------------------------------
struct FieldViolation {
    std::string field;
    std::string message;

    friend bool operator==(const FieldViolation&, const FieldViolation&) = default;
};

// ---------------------------------------------------------------------------
// ValidationResult — esito della validazione.
//   valid == true   -> nessuna violazione, il Manifest è conforme allo schema.
//   valid == false  -> `violations` contiene l'ELENCO COMPLETO dei campi non
//                      conformi (Req 16.4); per Manifest assente contiene la
//                      singola violazione di mancanza (Req 16.3).
// ---------------------------------------------------------------------------
struct ValidationResult {
    bool valid{false};
    std::vector<FieldViolation> violations{};

    [[nodiscard]] bool ok() const noexcept { return valid; }
    explicit operator bool() const noexcept { return valid; }

    // Comodità: elenco dei soli percorsi dei campi non conformi.
    [[nodiscard]] std::vector<std::string> fieldNames() const {
        std::vector<std::string> names;
        names.reserve(violations.size());
        for (const FieldViolation& v : violations) names.push_back(v.field);
        return names;
    }
};

// ---------------------------------------------------------------------------
// Insieme dei permessi riconosciuti dal sistema (Req 17.1). Coerente con l'enum
// `Permission { Network, FileSystem, Hooking, UI, Events }` del design
// (loader/sandbox/sandbox.hpp): forma canonica testuale in minuscolo usata nel
// Manifest (`[permissions].required = ["hooking", "ui", "network"]`).
// ---------------------------------------------------------------------------
[[nodiscard]] inline const std::array<std::string_view, 5>& recognizedPermissions() noexcept {
    static const std::array<std::string_view, 5> kPermissions{
        "network", "filesystem", "hooking", "ui", "events"};
    return kPermissions;
}

[[nodiscard]] inline bool isRecognizedPermission(std::string_view p) noexcept {
    for (std::string_view known : recognizedPermissions()) {
        if (known == p) return true;
    }
    return false;
}

namespace detail {

// Limite di lunghezza dell'identificatore (Req 16.1).
inline constexpr std::size_t kMaxIdLength = 256;

// Verifica la coerenza fra il `type` dichiarato di un setting e la variante
// effettivamente attiva nel suo `defaultValue`.
[[nodiscard]] inline bool settingDefaultMatchesType(const SettingDecl& s) noexcept {
    if (s.type == "int") return std::holds_alternative<std::int64_t>(s.defaultValue);
    if (s.type == "float") return std::holds_alternative<double>(s.defaultValue);
    if (s.type == "bool") return std::holds_alternative<bool>(s.defaultValue);
    if (s.type == "string") return std::holds_alternative<std::string>(s.defaultValue);
    return false;  // tipo non riconosciuto
}

[[nodiscard]] inline bool isRecognizedSettingType(std::string_view t) noexcept {
    return t == "int" || t == "float" || t == "bool" || t == "string";
}

}  // namespace detail

// ---------------------------------------------------------------------------
// validate — valida un Manifest contro lo schema, raccogliendo TUTTE le
// violazioni (Req 16.2, 16.4). Funzione pura: nessun side-effect, pensata per
// essere chiamata come gate prima di caricare codice della Mod.
// ---------------------------------------------------------------------------
[[nodiscard]] inline ValidationResult validate(const Manifest& m) {
    ValidationResult res;
    auto add = [&res](std::string field, std::string message) {
        res.violations.push_back(FieldViolation{std::move(field), std::move(message)});
    };

    // schema_version: intero positivo.
    if (m.schemaVersion < 1) {
        add("schema_version",
            "schema_version deve essere un intero >= 1 (trovato " +
                std::to_string(m.schemaVersion) + ")");
    }

    // [mod].id: non vuoto, <= 256 caratteri (Req 16.1).
    if (m.id.empty()) {
        add("mod.id", "l'identificatore della Mod non puo' essere vuoto");
    } else if (m.id.size() > detail::kMaxIdLength) {
        add("mod.id",
            "l'identificatore della Mod supera i 256 caratteri (lunghezza " +
                std::to_string(m.id.size()) + ")");
    }

    // [mod].version: SemVer strutturalmente garantita dal tipo. Nessun ulteriore
    // vincolo imposto (Req 16.1 non vieta 0.0.0). Voce mantenuta per chiarezza.

    // [[entry_points]]: almeno uno (Req 16.1), ciascuno con kind e symbol non vuoti.
    if (m.entryPoints.empty()) {
        add("entry_points",
            "il Manifest deve dichiarare almeno un punto di ingresso");
    } else {
        for (std::size_t i = 0; i < m.entryPoints.size(); ++i) {
            const EntryPoint& ep = m.entryPoints[i];
            const std::string base = "entry_points[" + std::to_string(i) + "]";
            if (ep.kind.empty()) {
                add(base + ".kind", "il tipo del punto di ingresso non puo' essere vuoto");
            }
            if (ep.symbol.empty()) {
                add(base + ".symbol", "il simbolo del punto di ingresso non puo' essere vuoto");
            }
        }
    }

    // [[dependencies]]: id non vuoto (il vincolo di versione è tipizzato).
    for (std::size_t i = 0; i < m.dependencies.size(); ++i) {
        const Dependency& d = m.dependencies[i];
        if (d.id.empty()) {
            add("dependencies[" + std::to_string(i) + "].id",
                "l'id della dipendenza non puo' essere vuoto");
        }
    }

    // [permissions].required: ogni permesso non vuoto e riconosciuto (Req 17.1).
    for (std::size_t i = 0; i < m.permissions.size(); ++i) {
        const std::string& p = m.permissions[i];
        const std::string field = "permissions.required[" + std::to_string(i) + "]";
        if (p.empty()) {
            add(field, "il permesso dichiarato non puo' essere una stringa vuota");
        } else if (!isRecognizedPermission(p)) {
            add(field, "permesso non riconosciuto: '" + p +
                           "' (attesi: network, filesystem, hooking, ui, events)");
        }
    }

    // [[settings]]: name non vuoto, type riconosciuto, default coerente col type.
    for (std::size_t i = 0; i < m.settings.size(); ++i) {
        const SettingDecl& s = m.settings[i];
        const std::string base = "settings[" + std::to_string(i) + "]";
        if (s.name.empty()) {
            add(base + ".name", "il nome del setting non puo' essere vuoto");
        }
        if (!detail::isRecognizedSettingType(s.type)) {
            add(base + ".type",
                "tipo di setting non riconosciuto: '" + s.type +
                    "' (attesi: int, float, bool, string)");
        } else if (!detail::settingDefaultMatchesType(s)) {
            add(base + ".default",
                "il valore di default non e' coerente col tipo dichiarato '" +
                    s.type + "'");
        }
    }

    res.valid = res.violations.empty();
    return res;
}

// ---------------------------------------------------------------------------
// validate (overload) — gate "valida-prima-di-caricare" con manifest opzionale.
// Manifest assente (std::nullopt) → rifiuto con violazione di mancanza
// (Req 16.3); presente → delega a validate(const Manifest&) (Req 16.2, 16.4).
// ---------------------------------------------------------------------------
[[nodiscard]] inline ValidationResult validate(const std::optional<Manifest>& maybe) {
    if (!maybe.has_value()) {
        ValidationResult res;
        res.valid = false;
        res.violations.push_back(FieldViolation{
            "manifest",
            "Manifest assente dal Pulse_Package: caricamento della Mod rifiutato, "
            "nessun codice eseguito"});
        return res;
    }
    return validate(*maybe);
}

// ---------------------------------------------------------------------------
// validatePresence — overload esplicito sull'esito di un parse (ParseResult).
// Se il parsing è fallito (manifest assente o malformato) → rifiuto con la
// causa; altrimenti valida il Manifest ottenuto. Utile per cablare il gate
// direttamente sull'output di `parse(...)` prima di caricare codice (Req 16.2,
// 16.3).
// ---------------------------------------------------------------------------
[[nodiscard]] inline ValidationResult validatePresence(const ParseResult& parsed) {
    if (!parsed.ok) {
        ValidationResult res;
        res.valid = false;
        res.violations.push_back(FieldViolation{
            "manifest",
            parsed.error.empty()
                ? std::string("Manifest assente o non analizzabile: caricamento rifiutato")
                : ("Manifest non analizzabile: " + parsed.error)});
        return res;
    }
    return validate(parsed.manifest);
}

}  // namespace pulse::manifest

#endif  // PULSE_LOADER_LIFECYCLE_MANIFEST_VALIDATION_HPP
