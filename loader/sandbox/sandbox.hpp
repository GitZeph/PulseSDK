// loader/sandbox/sandbox.hpp — Sandbox a permessi e validazione del consenso
// (Layer 6 — Sandbox & Scripting, task 35.1).
//
// Requisiti coperti da questo modulo:
//   * Req 17.1 — Il Manifest dichiara l'insieme dei permessi richiesti, dove
//     ogni permesso appartiene all'insieme dei tipi riconosciuti dal sistema
//     (`Permission`: Network, FileSystem, Hooking, UI, Events).
//   * Req 17.2 — Se il Manifest dichiara un permesso NON riconosciuto, oppure
//     OMETTE del tutto la sezione dei permessi, l'installazione è rifiutata con
//     un'indicazione di errore che specifica la dichiarazione non valida.
//   * Req 17.3 — Un'operazione è consentita SOLO se il permesso corrispondente
//     è dichiarato nel Manifest ED è stato approvato dall'User.
//   * Req 17.4 — Un'operazione priva del permesso dichiarato+approvato è negata
//     PRIMA di qualsiasi effetto sullo stato del sistema; la violazione è
//     registrata (id della Mod + permesso mancante) e alla Mod è restituito un
//     errore di permesso negato senza alterare lo stato del sistema.
//   * Req 17.5 — All'installazione, l'elenco completo dei permessi richiesti è
//     presentato all'User, che deve fornire una decisione esplicita di
//     approvazione o rifiuto (modellata da un callback iniettabile).
//   * Req 17.6 — Se l'User RIFIUTA l'installazione, la Mod non viene abilitata e
//     lo stato del sistema resta invariato (nessun consenso registrato).
//
// Scelta tecnica (documentata): modulo HEADER-ONLY e self-contained, coerente
// con `loader/package/pulse_package.hpp`. Non introduce un nuovo .cpp sotto
// `loader/sandbox/` (il glob di `loader/CMakeLists.txt` non copre questa dir).
// Non dipende dagli header del Manifest: la sezione dei permessi dichiarati è
// passata come `std::optional<std::vector<std::string>>`, dove `std::nullopt`
// rappresenta fedelmente la sezione OMESSA (distinta da una lista vuota),
// necessaria per Req 17.2. Le stringhe sono validate contro l'insieme dei tipi
// riconosciuti (`parsePermission`), così un token sconosciuto è rilevabile.
//
// Riconciliazione 17.3 / 17.6 (documentata): il callback di approvazione
// restituisce sia l'esito (approvato/rifiutato) sia il sottoinsieme di permessi
// effettivamente CONCESSI dall'User. Un rifiuto dell'installazione (Req 17.6)
// non abilita la Mod e non registra alcun consenso. Un'approvazione con
// sottoinsieme ridotto modella il consenso GRANULARE (Req 17.3): `check`
// consente un permesso solo se è sia dichiarato sia presente nel sottoinsieme
// concesso; un permesso dichiarato ma non concesso viene negato come uno non
// dichiarato.
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).
#ifndef PULSE_LOADER_SANDBOX_SANDBOX_HPP
#define PULSE_LOADER_SANDBOX_SANDBOX_HPP

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulse::sandbox {

// ModId è una stringa (coerente con il resto del loader).
using ModId = std::string;

// ---------------------------------------------------------------------------
// Permission — l'insieme dei tipi di permesso riconosciuti dal sistema
// (Req 17.1). Qualsiasi token non mappabile su questo enum è "non riconosciuto"
// e causa il rifiuto dell'installazione (Req 17.2).
// ---------------------------------------------------------------------------
enum class Permission { Network, FileSystem, Hooking, UI, Events };

// Nome canonico (minuscolo) usato nel Manifest `pulse.toml`.
[[nodiscard]] inline std::string permissionName(Permission p) {
    switch (p) {
        case Permission::Network:    return "network";
        case Permission::FileSystem: return "filesystem";
        case Permission::Hooking:    return "hooking";
        case Permission::UI:         return "ui";
        case Permission::Events:     return "events";
    }
    return "unknown";
}

// Analizza un token del Manifest nel `Permission` corrispondente. Ritorna
// std::nullopt se il token NON appartiene all'insieme riconosciuto (Req 17.2).
[[nodiscard]] inline std::optional<Permission> parsePermission(
    std::string_view token) {
    if (token == "network")    return Permission::Network;
    if (token == "filesystem") return Permission::FileSystem;
    if (token == "hooking")    return Permission::Hooking;
    if (token == "ui")         return Permission::UI;
    if (token == "events")     return Permission::Events;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Violation — record di una violazione di permesso (Req 17.4): id della Mod +
// permesso mancante + messaggio leggibile.
// ---------------------------------------------------------------------------
struct Violation {
    ModId modId;
    Permission permission;
    std::string message;

    friend bool operator==(const Violation&, const Violation&) = default;
};

// ---------------------------------------------------------------------------
// ViolationSink — destinazione INIETTABILE per la registrazione delle
// violazioni (Req 17.4). Astratto così i test possono catturare le violazioni.
// ---------------------------------------------------------------------------
class ViolationSink {
public:
    virtual ~ViolationSink() = default;
    virtual void record(const Violation& v) = 0;
};

// Sink in-memory di comodo (per test e diagnostica).
class InMemoryViolationSink final : public ViolationSink {
public:
    void record(const Violation& v) override { violations_.push_back(v); }

    [[nodiscard]] const std::vector<Violation>& violations() const noexcept {
        return violations_;
    }
    [[nodiscard]] std::size_t count() const noexcept { return violations_.size(); }
    void clear() noexcept { violations_.clear(); }

private:
    std::vector<Violation> violations_;
};

// ---------------------------------------------------------------------------
// PermissionError — errore restituito da `check` quando un permesso è negato
// (Req 17.4): identifica Mod + permesso e porta un messaggio leggibile.
// ---------------------------------------------------------------------------
struct PermissionError {
    ModId modId;
    Permission permission;
    std::string message;
};

// ---------------------------------------------------------------------------
// Result<void> — esito di `check` in stile minimale senza eccezioni, coerente
// con i `Result` del backend di hooking. `has_value()/operator bool` indicano
// il consenso; `error()` descrive la negazione.
// ---------------------------------------------------------------------------
template <class T>
class Result;

template <>
class Result<void> {
public:
    static Result ok() {
        Result r;
        r.ok_ = true;
        return r;
    }
    static Result err(PermissionError error) {
        Result r;
        r.ok_ = false;
        r.error_ = std::move(error);
        return r;
    }

    [[nodiscard]] bool has_value() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    [[nodiscard]] const PermissionError& error() const noexcept { return error_; }

private:
    bool ok_{false};
    PermissionError error_{};
};

// ---------------------------------------------------------------------------
// Flusso di approvazione all'installazione (Req 17.5/17.6).
// ---------------------------------------------------------------------------

// Richiesta presentata all'User: elenco COMPLETO dei permessi richiesti.
struct ApprovalRequest {
    ModId modId;
    std::vector<Permission> requested;
};

// Decisione esplicita dell'User. `approved == false` => rifiuto (Req 17.6).
// `granted` è il sottoinsieme effettivamente concesso quando `approved == true`
// (consenso granulare, Req 17.3). Helper `grantAll`/`refuse` per i casi comuni.
struct ApprovalDecision {
    bool approved{false};
    std::vector<Permission> granted;

    [[nodiscard]] static ApprovalDecision grantAll(
        const std::vector<Permission>& requested) {
        return ApprovalDecision{true, requested};
    }
    [[nodiscard]] static ApprovalDecision grantSubset(
        std::vector<Permission> subset) {
        return ApprovalDecision{true, std::move(subset)};
    }
    [[nodiscard]] static ApprovalDecision refuse() {
        return ApprovalDecision{false, {}};
    }
};

// Callback iniettabile che modella la decisione dell'User (Req 17.5). I test lo
// pilotano per simulare approvazione/rifiuto.
using ApprovalCallback = std::function<ApprovalDecision(const ApprovalRequest&)>;

// ---------------------------------------------------------------------------
// InstallError / InstallResult — esito di `installMod`.
// ---------------------------------------------------------------------------
enum class InstallError {
    None,
    PermissionsSectionOmitted,  // sezione [permissions] omessa (Req 17.2)
    UnrecognizedPermission,     // permesso non riconosciuto (Req 17.2)
    UserRefused,                // l'User ha rifiutato l'installazione (Req 17.6)
};

[[nodiscard]] inline std::string to_string(InstallError e) {
    switch (e) {
        case InstallError::None: return "none";
        case InstallError::PermissionsSectionOmitted: return "permissions-omitted";
        case InstallError::UnrecognizedPermission: return "unrecognized-permission";
        case InstallError::UserRefused: return "user-refused";
    }
    return "unknown";
}

struct InstallResult {
    bool ok{false};
    InstallError error{InstallError::None};
    std::string message;
    std::vector<Permission> approved;  // valorizzato sse ok
};

// ===========================================================================
// Sandbox — confina le operazioni di una Mod ai permessi dichiarati e approvati.
//
// INVARIANTE CHIAVE (Req 17.3/17.4): `check(modId, permission)` consente SOLO
// se `permission` è sia dichiarato nel Manifest sia concesso dall'User; in ogni
// altro caso nega PRIMA di qualsiasi effetto e registra la violazione sul sink.
// Una Mod ottiene un consenso SOLO tramite `installMod` con esito positivo;
// finché ciò non accade, ogni `check` per quella Mod è negato.
// ===========================================================================
class Sandbox {
public:
    Sandbox() = default;

    // Il sink di violazioni è iniettabile (Req 17.4). Può essere nullo: in tal
    // caso le violazioni non vengono registrate ma la negazione resta in vigore.
    explicit Sandbox(ViolationSink* sink) : sink_(sink) {}

    void setViolationSink(ViolationSink* sink) noexcept { sink_ = sink; }

    // -----------------------------------------------------------------------
    // installMod — flusso di installazione con approvazione dell'User.
    //
    // `declaredPermissions == std::nullopt` rappresenta la sezione dei permessi
    // OMESSA dal Manifest (Req 17.2) => rifiuto. Una lista presente ma vuota è
    // invece valida (la Mod non richiede alcun permesso).
    //
    // Ordine (nessun effetto prima della validazione):
    //   (1) sezione presente?         -> altrimenti PermissionsSectionOmitted
    //   (2) ogni token riconosciuto?  -> altrimenti UnrecognizedPermission
    //   (3) presenta i permessi all'User e attende la decisione (Req 17.5)
    //   (4) rifiuto                   -> UserRefused, nessun consenso (Req 17.6)
    //   (5) approvazione              -> registra il consenso granulare e abilita
    // -----------------------------------------------------------------------
    InstallResult installMod(
        const ModId& modId,
        const std::optional<std::vector<std::string>>& declaredPermissions,
        const ApprovalCallback& onApproval) {
        InstallResult res;

        // (1) Sezione dei permessi OMESSA (Req 17.2).
        if (!declaredPermissions.has_value()) {
            res.ok = false;
            res.error = InstallError::PermissionsSectionOmitted;
            res.message =
                "Installazione rifiutata: il Manifest della Mod '" + modId +
                "' omette la sezione dei permessi.";
            return res;  // nessuno stato modificato
        }

        // (2) Validazione di OGNI permesso dichiarato (Req 17.1/17.2).
        std::vector<Permission> declared;
        std::set<Permission> declaredSet;
        declared.reserve(declaredPermissions->size());
        for (const std::string& token : *declaredPermissions) {
            std::optional<Permission> p = parsePermission(token);
            if (!p.has_value()) {
                res.ok = false;
                res.error = InstallError::UnrecognizedPermission;
                res.message =
                    "Installazione rifiutata: il Manifest della Mod '" + modId +
                    "' dichiara un permesso non riconosciuto: '" + token + "'.";
                return res;  // nessuno stato modificato
            }
            // Deduplica preservando il primo ordine d'apparizione.
            if (declaredSet.insert(*p).second) {
                declared.push_back(*p);
            }
        }

        // (3) Presenta l'elenco completo all'User e attendi la decisione
        //     esplicita (Req 17.5).
        ApprovalRequest request{modId, declared};
        ApprovalDecision decision =
            onApproval ? onApproval(request) : ApprovalDecision::refuse();

        // (4) Rifiuto: la Mod NON viene abilitata, stato invariato (Req 17.6).
        if (!decision.approved) {
            res.ok = false;
            res.error = InstallError::UserRefused;
            res.message =
                "Installazione rifiutata: l'User ha negato i permessi della "
                "Mod '" + modId + "'.";
            return res;  // nessun consenso registrato
        }

        // (5) Approvazione: registra il consenso granulare (intersezione tra
        //     dichiarati e concessi) e abilita la Mod (Req 17.3).
        Consent consent;
        consent.declared = declaredSet;
        for (Permission g : decision.granted) {
            if (declaredSet.count(g) != 0) {
                consent.approved.insert(g);
            }
        }
        consents_[modId] = std::move(consent);

        res.ok = true;
        res.error = InstallError::None;
        res.message = "Mod '" + modId + "' installata con i permessi approvati.";
        res.approved.assign(consents_[modId].approved.begin(),
                            consents_[modId].approved.end());
        return res;
    }

    // -----------------------------------------------------------------------
    // check — autorizza un'operazione (Req 17.3/17.4).
    //
    // Consente SOLO se `permission` è dichiarato nel Manifest ED è stato
    // approvato dall'User. Altrimenti NEGA prima di qualsiasi effetto: registra
    // la violazione (id Mod + permesso) sul sink e restituisce un errore di
    // permesso negato. Questa funzione non altera mai lo stato del sistema.
    // -----------------------------------------------------------------------
    [[nodiscard]] Result<void> check(const ModId& modId, Permission permission) {
        const bool allowed = isDeclared(modId, permission) &&
                             isApproved(modId, permission);
        if (allowed) {
            return Result<void>::ok();
        }

        // Negazione PRIMA di qualsiasi effetto + registrazione della violazione.
        const std::string msg =
            "Permesso negato: la Mod '" + modId + "' ha tentato un'operazione '" +
            permissionName(permission) +
            "' non dichiarata e approvata. Operazione negata prima di ogni "
            "effetto.";
        if (sink_ != nullptr) {
            sink_->record(Violation{modId, permission, msg});
        }
        return Result<void>::err(PermissionError{modId, permission, msg});
    }

    // -- Ispezione (sola lettura) -------------------------------------------
    [[nodiscard]] bool isInstalled(const ModId& modId) const {
        return consents_.find(modId) != consents_.end();
    }

    [[nodiscard]] bool isDeclared(const ModId& modId, Permission p) const {
        auto it = consents_.find(modId);
        return it != consents_.end() && it->second.declared.count(p) != 0;
    }

    [[nodiscard]] bool isApproved(const ModId& modId, Permission p) const {
        auto it = consents_.find(modId);
        return it != consents_.end() && it->second.approved.count(p) != 0;
    }

private:
    // Consenso registrato per una Mod installata: permessi dichiarati nel
    // Manifest e sottoinsieme effettivamente approvato dall'User.
    struct Consent {
        std::set<Permission> declared;
        std::set<Permission> approved;
    };

    ViolationSink* sink_{nullptr};
    std::map<ModId, Consent> consents_;
};

}  // namespace pulse::sandbox

#endif  // PULSE_LOADER_SANDBOX_SANDBOX_HPP
