// pulse/settings.hpp — impostazioni tipizzate dello SDK Pulse (Requisito 9).
//
// Questo header fornisce la dichiarazione TIPIZZATA delle impostazioni di una
// Mod (task 19.1):
//   * SettingType — l'insieme chiuso dei tipi supportati: booleano, intero,
//     numero in virgola mobile, stringa, enumerazione (Req 9.1).
//   * SettingValue — un valore tipizzato (variante etichettata) usato sia per i
//     default sia per i valori persistiti.
//   * SettingDecl — la dichiarazione di una singola impostazione: nome univoco
//     di lunghezza 1..64, tipo, valore predefinito conforme al tipo (Req 9.1).
//   * SettingsRegistry — il registro delle dichiarazioni di una Mod che:
//       - rifiuta i nomi fuori dall'intervallo 1..64 caratteri (Req 9.1);
//       - rifiuta un default non conforme al tipo dichiarato (Req 9.1);
//       - rifiuta i nomi duplicati producendo una segnalazione che indica il
//         nome in conflitto (Req 9.6);
//       - carica un valore persistito e, se non è valido per il tipo dichiarato,
//         ricade sul default dichiarato producendo una segnalazione che indica
//         l'incoerenza rilevata (Req 9.4).
//
// La generazione automatica dell'UI (Req 9.2) e la persistenza sull'edit
// (Req 9.3/9.5) sono demandate al task 31.2; questo header copre la
// dichiarazione tipizzata, il rifiuto dei duplicati e il fallback su valore
// persistito non valido.
//
// Header-only, coerente con il resto dello SDK pubblico (Requisito 26.1, C++20).
#ifndef PULSE_SETTINGS_HPP
#define PULSE_SETTINGS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulse {

// Insieme CHIUSO dei tipi supportati per un'impostazione (Req 9.1).
enum class SettingType {
    Bool,
    Int,
    Float,
    String,
    Enum,
};

// Nome leggibile di un SettingType (per le segnalazioni).
inline std::string_view to_string(SettingType type) noexcept {
    switch (type) {
        case SettingType::Bool:
            return "bool";
        case SettingType::Int:
            return "int";
        case SettingType::Float:
            return "float";
        case SettingType::String:
            return "string";
        case SettingType::Enum:
            return "enum";
    }
    return "unknown";
}

// Valore tipizzato di un'impostazione (variante etichettata). Usato sia per il
// valore predefinito di una SettingDecl sia per un valore persistito da
// validare. Il tipo è esplicito (type()) per consentire il controllo di
// conformità rispetto alla dichiarazione.
//
// Per le enumerazioni, il valore è l'etichetta selezionata (una stringa) che
// deve appartenere all'insieme delle etichette ammesse della dichiarazione.
class SettingValue {
public:
    SettingValue() = default;

    static SettingValue Bool(bool v) {
        SettingValue value;
        value.type_ = SettingType::Bool;
        value.bool_ = v;
        return value;
    }
    static SettingValue Int(std::int64_t v) {
        SettingValue value;
        value.type_ = SettingType::Int;
        value.int_ = v;
        return value;
    }
    static SettingValue Float(double v) {
        SettingValue value;
        value.type_ = SettingType::Float;
        value.float_ = v;
        return value;
    }
    static SettingValue String(std::string v) {
        SettingValue value;
        value.type_ = SettingType::String;
        value.str_ = std::move(v);
        return value;
    }
    // Etichetta selezionata di un'enumerazione (Req 9.1, tipo enum).
    static SettingValue Enum(std::string label) {
        SettingValue value;
        value.type_ = SettingType::Enum;
        value.str_ = std::move(label);
        return value;
    }

    SettingType type() const noexcept { return type_; }

    bool asBool() const noexcept { return bool_; }
    std::int64_t asInt() const noexcept { return int_; }
    double asFloat() const noexcept { return float_; }
    const std::string& asString() const noexcept { return str_; }
    // Per SettingType::Enum, l'etichetta selezionata.
    const std::string& asEnumLabel() const noexcept { return str_; }

    friend bool operator==(const SettingValue& a, const SettingValue& b) noexcept {
        if (a.type_ != b.type_) {
            return false;
        }
        switch (a.type_) {
            case SettingType::Bool:
                return a.bool_ == b.bool_;
            case SettingType::Int:
                return a.int_ == b.int_;
            case SettingType::Float:
                return a.float_ == b.float_;
            case SettingType::String:
            case SettingType::Enum:
                return a.str_ == b.str_;
        }
        return false;
    }
    friend bool operator!=(const SettingValue& a, const SettingValue& b) noexcept {
        return !(a == b);
    }

private:
    SettingType type_{SettingType::Bool};
    bool bool_{false};
    std::int64_t int_{0};
    double float_{0.0};
    std::string str_;
};

// Limiti di lunghezza del nome di un'impostazione (Req 9.1).
inline constexpr std::size_t kSettingNameMinLength = 1;
inline constexpr std::size_t kSettingNameMaxLength = 64;

// Dichiarazione di una singola impostazione tipizzata (Req 9.1).
//
//   * name           — univoco, lunghezza 1..64 caratteri;
//   * type           — uno dei SettingType supportati;
//   * defaultValue   — conforme a `type`;
//   * enumValues     — etichette ammesse (solo per type == Enum); il default,
//                      che è un'etichetta, deve appartenere a questo insieme.
struct SettingDecl {
    std::string name;
    SettingType type{SettingType::Bool};
    SettingValue defaultValue;
    std::vector<std::string> enumValues;  // usato solo per type == Enum
};

// Restituisce true se `value` è conforme alla dichiarazione `decl`:
//   * il tipo combacia;
//   * per un'enumerazione, l'etichetta appartiene alle etichette ammesse.
inline bool conforms(const SettingDecl& decl, const SettingValue& value) noexcept {
    if (value.type() != decl.type) {
        return false;
    }
    if (decl.type == SettingType::Enum) {
        for (const auto& allowed : decl.enumValues) {
            if (allowed == value.asEnumLabel()) {
                return true;
            }
        }
        return false;
    }
    return true;
}

// Esito della dichiarazione di un'impostazione (Req 9.1, 9.6).
struct DeclareResult {
    bool ok{false};
    // Segnalazione leggibile in caso di rifiuto (vuota se ok == true).
    std::string report;
};

// Esito del caricamento di un valore persistito (Req 9.4).
struct LoadResult {
    // Valore effettivo: quello persistito se valido, altrimenti il default.
    SettingValue value;
    // true se si è ricaduti sul default dichiarato a causa di un valore
    // persistito non valido o di un'impostazione sconosciuta.
    bool usedDefault{false};
    // Segnalazione dell'incoerenza rilevata (Req 9.4); presente solo quando
    // usedDefault è true a causa di un valore non conforme.
    std::optional<std::string> report;
};

// Registro delle impostazioni tipizzate dichiarate da una Mod.
//
// Mantiene l'ordine di dichiarazione (utile per la futura UI auto-generata,
// task 31.2) e impone l'unicità dei nomi (Req 9.6). Le segnalazioni prodotte
// dai rifiuti e dai fallback sono accumulate in reports() per ispezione.
class SettingsRegistry {
public:
    // Dichiara un'impostazione tipizzata (Req 9.1, 9.6).
    //
    // Rifiuta e segnala se:
    //   * il nome ha lunghezza fuori dall'intervallo 1..64 (Req 9.1);
    //   * il valore predefinito non è conforme al tipo dichiarato, oppure, per
    //     un'enumerazione, le etichette ammesse sono vuote o il default non vi
    //     appartiene (Req 9.1);
    //   * il nome è già stato dichiarato — la segnalazione indica il nome in
    //     conflitto (Req 9.6).
    DeclareResult declare(const SettingDecl& decl) {
        const std::size_t len = decl.name.size();
        if (len < kSettingNameMinLength || len > kSettingNameMaxLength) {
            return reject("nome di impostazione non valido: '" + decl.name +
                          "' ha lunghezza " + std::to_string(len) +
                          " caratteri, atteso 1..64");
        }

        if (decl.type == SettingType::Enum && decl.enumValues.empty()) {
            return reject("impostazione enum '" + decl.name +
                          "' deve dichiarare almeno un'etichetta ammessa");
        }

        if (!conforms(decl, decl.defaultValue)) {
            return reject("valore predefinito di '" + decl.name +
                          "' non conforme al tipo dichiarato (" +
                          std::string(to_string(decl.type)) + ")");
        }

        if (index_.find(decl.name) != index_.end()) {
            // Req 9.6 — rifiuto del duplicato con il nome in conflitto.
            return reject("nome di impostazione duplicato: '" + decl.name + "'");
        }

        index_.emplace(decl.name, decls_.size());
        decls_.push_back(decl);
        return DeclareResult{true, {}};
    }

    // true se un'impostazione con questo nome è stata dichiarata.
    bool contains(std::string_view name) const {
        return index_.find(std::string(name)) != index_.end();
    }

    // Dichiarazione corrispondente al nome, o nullptr se assente.
    const SettingDecl* find(std::string_view name) const {
        const auto it = index_.find(std::string(name));
        if (it == index_.end()) {
            return nullptr;
        }
        return &decls_[it->second];
    }

    // Dichiarazioni nell'ordine di inserimento.
    const std::vector<SettingDecl>& declarations() const noexcept { return decls_; }

    // Segnalazioni accumulate (rifiuti di dichiarazione + fallback).
    const std::vector<std::string>& reports() const noexcept { return reports_; }

    // Carica un valore persistito per l'impostazione `name` (Req 9.4).
    //
    // Se il valore persistito è conforme al tipo dichiarato, lo restituisce.
    // Altrimenti ricade sul valore predefinito dichiarato, conserva invariata
    // la dichiarazione e produce una segnalazione che indica l'incoerenza.
    LoadResult loadPersisted(std::string_view name, const SettingValue& persisted) {
        const SettingDecl* decl = find(name);
        if (decl == nullptr) {
            // Impostazione sconosciuta: nessun default a cui ricadere.
            LoadResult result;
            result.value = persisted;
            result.usedDefault = false;
            const std::string report =
                "impostazione sconosciuta: '" + std::string(name) + "'";
            reports_.push_back(report);
            result.report = report;
            return result;
        }

        if (conforms(*decl, persisted)) {
            return LoadResult{persisted, false, std::nullopt};
        }

        // Req 9.4 — valore persistito non valido: usa il default + segnala.
        std::string report = "valore persistito non valido per '" + decl->name +
                             "' (tipo dichiarato " +
                             std::string(to_string(decl->type)) +
                             ", trovato " + std::string(to_string(persisted.type()));
        if (decl->type == SettingType::Enum &&
            persisted.type() == SettingType::Enum) {
            report += " con etichetta '" + persisted.asEnumLabel() +
                      "' non ammessa";
        }
        report += "): uso del valore predefinito dichiarato";
        reports_.push_back(report);

        return LoadResult{decl->defaultValue, true, std::move(report)};
    }

private:
    DeclareResult reject(std::string message) {
        reports_.push_back(message);
        return DeclareResult{false, std::move(message)};
    }

    std::vector<SettingDecl> decls_;
    std::unordered_map<std::string, std::size_t> index_;
    std::vector<std::string> reports_;
};

}  // namespace pulse

#endif  // PULSE_SETTINGS_HPP
