// sdk/settings/settings_ui.hpp — UI auto-generata delle impostazioni e
// persistenza sull'edit dello SDK Pulse (Layer 5, Requisiti 9.2, 9.3, 9.5,
// 10.3, 10.6).
//
// Questo header completa il task 31.2: la dichiarazione TIPIZZATA delle
// impostazioni vive in <pulse/settings.hpp> (task 19.1, `SettingsRegistry`),
// mentre QUI risiede il controller — testabile sull'host, SENZA UI reale del
// gioco — che:
//
//   * (Req 9.2) genera UN controllo di input per CIASCUNA impostazione
//     dichiarata, iterando `SettingsRegistry::declarations()` nell'ordine di
//     dichiarazione. Il tipo di controllo (`ControlKind`) deriva dal
//     `SettingType`: Bool→Toggle, Int→NumericInt, Float→NumericFloat,
//     String→Text, Enum→Dropdown (con l'elenco delle etichette ammesse). I
//     controlli sono rappresentati come una struttura ispezionabile dai test.
//
//   * (Req 9.3) alla modifica di un valore conforme, lo PERSISTE entro 1
//     secondo (budget modellato da un clock iniettabile) tramite
//     `pulse::storage::ModStorage`, serializzando il `SettingValue`, e lo rende
//     disponibile alla lettura successiva.
//
//   * (Req 9.5) RIFIUTA un input non conforme alla `SettingDecl` (via
//     `pulse::conforms`): conserva il valore PRECEDENTEMENTE persistito, NON
//     persiste il valore non valido ed emette un messaggio che ne indica il
//     motivo su un message sink iniettabile.
//
//   * (Req 10.3) round-trip: un valore di impostazione persistito è riletto
//     correttamente (le semantiche di round-trip ≤ 500 ms sono già garantite
//     da `ModStorage`).
//
//   * (Req 10.6) alla rimozione della Mod, elimina i dati persistiti SOLO dopo
//     la conferma dell'User, modellata da una callback di conferma iniettabile
//     (`std::function<bool()>`); se non confermata, i dati sono conservati.
//
// Tutte le dipendenze "ambientali" (orologio per il budget, callback di
// conferma, sink dei messaggi) sono INIETTABILI dal costruttore, così il
// controller è interamente testabile sull'host senza la UI reale del gioco.
//
// Stack: C++20/23 (Requisito 26.1). Header-only, nessuna dipendenza esterna.
#ifndef PULSE_SETTINGS_SETTINGS_UI_HPP
#define PULSE_SETTINGS_SETTINGS_UI_HPP

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <pulse/settings.hpp>
#include <pulse/storage.hpp>

namespace pulse::settings {

// ---------------------------------------------------------------------------
// ControlKind — il tipo di controllo di input generato per un'impostazione
// (Req 9.2). Deriva in modo deterministico dal SettingType dichiarato.
// ---------------------------------------------------------------------------
enum class ControlKind {
    Toggle,        // SettingType::Bool   — interruttore on/off
    NumericInt,    // SettingType::Int    — campo numerico intero
    NumericFloat,  // SettingType::Float  — campo numerico in virgola mobile
    Text,          // SettingType::String — campo di testo
    Dropdown,      // SettingType::Enum   — menù a tendina con le etichette
};

// Nome leggibile di un ControlKind (per diagnostica/test).
inline std::string_view to_string(ControlKind kind) noexcept {
    switch (kind) {
        case ControlKind::Toggle:
            return "toggle";
        case ControlKind::NumericInt:
            return "numeric-int";
        case ControlKind::NumericFloat:
            return "numeric-float";
        case ControlKind::Text:
            return "text";
        case ControlKind::Dropdown:
            return "dropdown";
    }
    return "unknown";
}

// Mappa un SettingType al ControlKind dell'UI auto-generata (Req 9.2).
inline ControlKind controlKindFor(SettingType type) noexcept {
    switch (type) {
        case SettingType::Bool:
            return ControlKind::Toggle;
        case SettingType::Int:
            return ControlKind::NumericInt;
        case SettingType::Float:
            return ControlKind::NumericFloat;
        case SettingType::String:
            return ControlKind::Text;
        case SettingType::Enum:
            return ControlKind::Dropdown;
    }
    return ControlKind::Text;
}

// ---------------------------------------------------------------------------
// ControlDescriptor — descrizione ispezionabile di un singolo controllo di
// input generato per un'impostazione dichiarata (Req 9.2).
// ---------------------------------------------------------------------------
struct ControlDescriptor {
    std::string name;                    // nome dell'impostazione
    ControlKind kind{ControlKind::Text};  // tipo di controllo derivato dal tipo
    SettingType type{SettingType::Bool};  // tipo dichiarato dell'impostazione
    SettingValue currentValue;           // valore corrente (persistito o default)
    std::vector<std::string> enumOptions;  // etichette del dropdown (solo Enum)
};

// ---------------------------------------------------------------------------
// Serializzazione di un SettingValue verso/da pulse::storage::Value.
//
// Formato testuale binary-safe con prefisso di tipo "<tag>:":
//   B:0|1            booleano
//   I:<decimale>     intero a 64 bit
//   F:<bit decimali> double, serializzato come pattern di bit a 64 bit per un
//                    round-trip ESATTO (Req 10.3)
//   S:<raw>          stringa (binary-safe, può contenere ':' e '\0')
//   E:<etichetta>    etichetta di enumerazione
// ---------------------------------------------------------------------------
inline storage::Value serialize(const SettingValue& value) {
    switch (value.type()) {
        case SettingType::Bool:
            return std::string("B:") + (value.asBool() ? "1" : "0");
        case SettingType::Int:
            return "I:" + std::to_string(value.asInt());
        case SettingType::Float: {
            std::uint64_t bits = 0;
            const double d = value.asFloat();
            std::memcpy(&bits, &d, sizeof(bits));
            return "F:" + std::to_string(bits);
        }
        case SettingType::String:
            return "S:" + value.asString();
        case SettingType::Enum:
            return "E:" + value.asEnumLabel();
    }
    return {};
}

// Deserializza un pulse::storage::Value in un SettingValue. Restituisce
// std::nullopt se il dato è malformato (verrà trattato come "non conforme",
// ricadendo sul default — Req 9.4 / 10.3).
inline std::optional<SettingValue> deserialize(const storage::Value& raw) {
    if (raw.size() < 2 || raw[1] != ':') {
        return std::nullopt;
    }
    const char tag = raw[0];
    const std::string payload = raw.substr(2);
    switch (tag) {
        case 'B':
            if (payload == "1") {
                return SettingValue::Bool(true);
            }
            if (payload == "0") {
                return SettingValue::Bool(false);
            }
            return std::nullopt;
        case 'I':
            try {
                return SettingValue::Int(
                    static_cast<std::int64_t>(std::stoll(payload)));
            } catch (...) {
                return std::nullopt;
            }
        case 'F':
            try {
                const std::uint64_t bits = std::stoull(payload);
                double d = 0.0;
                std::memcpy(&d, &bits, sizeof(d));
                return SettingValue::Float(d);
            } catch (...) {
                return std::nullopt;
            }
        case 'S':
            return SettingValue::String(payload);
        case 'E':
            return SettingValue::Enum(payload);
        default:
            return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// Esito di un'operazione di edit (Req 9.3, 9.5).
// ---------------------------------------------------------------------------
struct EditResult {
    // true se il valore è conforme ed è stato accettato (Req 9.3); false se è
    // stato rifiutato perché non conforme o per impostazione sconosciuta
    // (Req 9.5).
    bool accepted{false};
    // true se il valore è stato effettivamente scritto nello storage.
    bool persisted{false};
    // true se la persistenza è avvenuta entro il budget di 1 s (Req 9.3).
    bool withinBudget{false};
    // Valore corrente risultante: il nuovo valore se accettato, altrimenti il
    // valore PRECEDENTEMENTE persistito conservato (Req 9.5).
    SettingValue value;
    // Messaggio che indica il motivo del rifiuto (presente solo se !accepted).
    std::optional<std::string> message;
};

// Esito dell'eliminazione dei dati persistiti su rimozione della Mod (Req 10.6).
struct RemovalResult {
    bool confirmed{false};      // true se l'User ha confermato l'eliminazione
    bool deleted{false};        // true se i dati sono stati effettivamente rimossi
    std::size_t removedKeys{0};  // numero di chiavi di impostazione rimosse
};

// ---------------------------------------------------------------------------
// SettingsUiController — controller dell'UI auto-generata delle impostazioni.
//
// Non dipende dalla UI reale del gioco: genera descrittori di controllo
// ispezionabili e instrada gli edit verso lo storage per-mod. Orologio,
// callback di conferma e sink dei messaggi sono iniettabili (host-testable).
// ---------------------------------------------------------------------------
class SettingsUiController {
public:
    // Orologio iniettabile per misurare il budget di persistenza (Req 9.3).
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    // Callback di conferma dell'User per l'eliminazione dei dati (Req 10.6).
    using Confirm = std::function<bool()>;
    // Sink dei messaggi mostrati all'User (Req 9.5).
    using MessageSink = std::function<void(const std::string&)>;

    // Budget massimo di persistenza di un edit: 1 secondo (Req 9.3).
    static constexpr std::chrono::milliseconds kPersistBudget{1000};

    // Costruisce il controller su un registro e uno storage esistenti. Le
    // dipendenze ambientali sono iniettabili; in assenza, usa default sensati
    // (clock di sistema, conferma negata, nessun sink).
    SettingsUiController(SettingsRegistry& registry,
                         storage::ModStorage& store,
                         Clock clock = defaultClock(),
                         Confirm confirm = Confirm{},
                         MessageSink messageSink = MessageSink{})
        : registry_(registry),
          storage_(store),
          clock_(std::move(clock)),
          confirm_(std::move(confirm)),
          messageSink_(std::move(messageSink)) {
        if (!clock_) {
            clock_ = defaultClock();
        }
    }

    // (Req 9.2) Genera UN ControlDescriptor per CIASCUNA impostazione
    // dichiarata, nell'ordine di dichiarazione, con il valore corrente
    // (persistito se valido, altrimenti il default — Req 9.4 / 10.3).
    [[nodiscard]] std::vector<ControlDescriptor> generateControls() const {
        std::vector<ControlDescriptor> controls;
        const auto& decls = registry_.declarations();
        controls.reserve(decls.size());
        for (const auto& decl : decls) {
            ControlDescriptor control;
            control.name = decl.name;
            control.type = decl.type;
            control.kind = controlKindFor(decl.type);
            control.enumOptions = decl.enumValues;
            control.currentValue = currentValue(decl.name);
            controls.push_back(std::move(control));
        }
        return controls;
    }

    // Valore corrente di un'impostazione: il valore persistito se presente e
    // conforme, altrimenti il valore predefinito dichiarato (Req 9.4, 10.3).
    [[nodiscard]] SettingValue currentValue(std::string_view name) const {
        const SettingDecl* decl = registry_.find(name);
        if (decl == nullptr) {
            return SettingValue{};
        }
        const auto stored = storage_.get(name);
        if (stored.has_value()) {
            const auto parsed = deserialize(*stored);
            if (parsed.has_value() && conforms(*decl, *parsed)) {
                return *parsed;
            }
        }
        return decl->defaultValue;
    }

    // (Req 9.3 / 9.5) Applica un edit dell'User a un'impostazione.
    //
    //   * Se l'impostazione è sconosciuta o il valore NON è conforme alla
    //     dichiarazione, RIFIUTA: conserva il valore precedente, non persiste
    //     nulla, emette un messaggio (Req 9.5).
    //   * Se conforme, PERSISTE il nuovo valore entro il budget di 1 s e lo
    //     rende disponibile alla lettura successiva (Req 9.3). Se lo storage
    //     rifiuta la scrittura (es. capacità superata), conserva il valore
    //     precedente ed emette il messaggio dell'errore.
    EditResult edit(std::string_view name, const SettingValue& value) {
        EditResult result;
        const SettingDecl* decl = registry_.find(name);

        if (decl == nullptr) {
            result.accepted = false;
            result.value = value;
            const std::string message =
                "impostazione sconosciuta: '" + std::string(name) + "'";
            result.message = message;
            emit(message);
            return result;
        }

        const SettingValue previous = currentValue(name);

        if (!conforms(*decl, value)) {
            // Req 9.5 — input non conforme: rifiuta, conserva il precedente.
            result.accepted = false;
            result.persisted = false;
            result.value = previous;
            std::string message =
                "valore non conforme per '" + decl->name +
                "' (tipo dichiarato " + std::string(to_string(decl->type)) +
                ", ricevuto " + std::string(to_string(value.type()));
            if (decl->type == SettingType::Enum &&
                value.type() == SettingType::Enum) {
                message += " con etichetta '" + value.asEnumLabel() +
                           "' non ammessa";
            }
            message += "): valore precedente conservato";
            result.message = message;
            emit(message);
            return result;
        }

        // Req 9.3 — valore conforme: persiste misurando il budget.
        const auto start = clock_();
        const storage::SetResult setResult = storage_.set(name, serialize(value));
        const auto end = clock_();
        result.withinBudget = (end - start) <= kPersistBudget;

        if (!setResult.isOk()) {
            // La scrittura è stata rifiutata (es. capacità superata, Req 10.5):
            // conserva il valore precedente e segnala.
            result.accepted = false;
            result.persisted = false;
            result.value = previous;
            result.message = setResult.error().message;
            emit(setResult.error().message);
            return result;
        }

        result.accepted = true;
        result.persisted = true;
        result.value = value;
        return result;
    }

    // (Req 10.6) Elimina i dati persistiti delle impostazioni della Mod SOLO
    // dopo la conferma dell'User (callback di conferma iniettabile). Se non
    // confermata, i dati sono CONSERVATI invariati.
    RemovalResult deletePersistedData() {
        RemovalResult result;
        const bool confirmed = confirm_ ? confirm_() : false;
        result.confirmed = confirmed;

        if (!confirmed) {
            // Conferma negata/assente: i dati restano invariati (Req 10.6).
            result.deleted = false;
            emit("eliminazione dei dati annullata: dati persistiti conservati");
            return result;
        }

        std::size_t removed = 0;
        for (const auto& decl : registry_.declarations()) {
            if (storage_.remove(decl.name)) {
                ++removed;
            }
        }
        result.deleted = true;
        result.removedKeys = removed;
        emit("dati persistiti eliminati su conferma dell'User (" +
             std::to_string(removed) + " impostazioni)");
        return result;
    }

    // Orologio di sistema di default (steady_clock), per la misura del budget.
    static Clock defaultClock() {
        return [] { return std::chrono::steady_clock::now(); };
    }

private:
    void emit(const std::string& message) const {
        if (messageSink_) {
            messageSink_(message);
        }
    }

    SettingsRegistry& registry_;
    storage::ModStorage& storage_;
    Clock clock_;
    Confirm confirm_;
    MessageSink messageSink_;
};

}  // namespace pulse::settings

#endif  // PULSE_SETTINGS_SETTINGS_UI_HPP
