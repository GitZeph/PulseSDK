// loader/lifecycle/manifest.hpp — Modello `Manifest` e parser/serializer del
// formato `pulse.toml` (Layer 4 — Lifecycle & Manifest, task 13.1).
//
// Requisiti coperti da questo modulo:
//   * Req 16.1 — il Manifest dichiara almeno: id univoco non vuoto (<=256
//     caratteri), versione SemVer, dipendenze con vincolo di versione, elenco
//     dei permessi e almeno un punto di ingresso. Modelliamo qui tutti questi
//     campi (più `name`, `type`, `settings`, `schemaVersion`) come da design
//     (`### Manifest (pulse.toml)`, IMP-05).
//   * Req 16.5 — round-trip: per ogni Manifest valido,
//     parse(serialize(m)) == m  e  parse(serialize(parse(s))) == parse(s).
//
// Scelta tecnica (documentata): si usa un reader/writer TOML *self-contained
// minimale* invece di integrare una libreria esterna (es. toml++) via
// FetchContent. Motivazioni:
//   1. il sottoinsieme di TOML necessario al manifest è ristretto e fisso
//      (tabelle `[mod]`/`[permissions]`, array di tabelle `[[entry_points]]`/
//      `[[dependencies]]`/`[[settings]]`, scalari stringa/intero/float/bool e
//      array di stringhe): un parser dedicato è piccolo, verificabile e privo
//      di dipendenze di rete in fase di build;
//   2. controlliamo la forma *canonica* emessa dal serializer, il che rende la
//      proprietà di round-trip dimostrabile in modo deterministico (niente
//      ambiguità di formattazione introdotte da una libreria generica);
//   3. header-only: nessuna modifica a `loader/CMakeLists.txt` necessaria (il
//      file ricade comunque nel glob `lifecycle/*.cpp` quando servisse un .cpp,
//      ma qui non serve).
//
// Riconciliazione dei tipi (documentata): la versione del Manifest e le
// dipendenze riusano direttamente `pulse::lifecycle::SemVer`,
// `pulse::lifecycle::VersionConstraint` e `pulse::lifecycle::Dependency`
// definiti in `dependency_resolver.hpp`. Un Manifest si proietta nel
// `ResolverManifest` consumato dal `DependencyResolver` tramite
// `toResolverManifest()`, mappando `id`/`version`/`dependencies` senza toccare
// l'API pubblica del resolver. La `GdVersion` dei bindings
// (`pulse::loader::bindings::GdVersion`, coppia major/minor del binario di GD)
// è un concetto distinto dalla SemVer della mod e non viene confuso con essa.
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).
#ifndef PULSE_LOADER_LIFECYCLE_MANIFEST_HPP
#define PULSE_LOADER_LIFECYCLE_MANIFEST_HPP

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "lifecycle/dependency_resolver.hpp"

namespace pulse::manifest {

// Riuso dei tipi di versionamento/dipendenza già definiti per il resolver
// (riconciliazione dei tipi, vedi nota in testata).
using SemVer = pulse::lifecycle::SemVer;
using VersionConstraint = pulse::lifecycle::VersionConstraint;
using Dependency = pulse::lifecycle::Dependency;
using ModId = pulse::lifecycle::ModId;

// ---------------------------------------------------------------------------
// ModType — tipo della mod dichiarato nel Manifest (`type` in `[mod]`).
// ---------------------------------------------------------------------------
enum class ModType { Native, Script };

[[nodiscard]] inline std::string to_string(ModType t) {
    return t == ModType::Script ? "script" : "native";
}

// ---------------------------------------------------------------------------
// EntryPoint — un punto di ingresso della mod (Req 16.1, almeno uno).
// `kind` (es. "init") + `symbol` (simbolo esportato richiamato all'enable).
// ---------------------------------------------------------------------------
struct EntryPoint {
    std::string kind;
    std::string symbol;

    friend bool operator==(const EntryPoint&, const EntryPoint&) = default;
};

// ---------------------------------------------------------------------------
// SettingValue — valore di default tipizzato di una dichiarazione di setting.
// La variante attiva è coerente con il campo `type` della SettingDecl.
// ---------------------------------------------------------------------------
using SettingValue = std::variant<std::int64_t, double, bool, std::string>;

// ---------------------------------------------------------------------------
// SettingDecl — dichiarazione di un'impostazione (nome, tipo, default).
// ---------------------------------------------------------------------------
struct SettingDecl {
    std::string name;
    std::string type;            // "int" | "float" | "bool" | "string"
    SettingValue defaultValue{};  // coerente con `type`

    friend bool operator==(const SettingDecl&, const SettingDecl&) = default;
};

// ---------------------------------------------------------------------------
// TargetPlatform — insieme finito delle piattaforme bersaglio dichiarabili dal
// Mod_Manifest nella sezione `[compat]` (Req 3.1). È distinto dal `Platform`
// runtime: qui modella ciò che la mod DICHIARA come bersaglio, non la
// piattaforma su cui il loader sta effettivamente girando.
// ---------------------------------------------------------------------------
enum class TargetPlatform { MacOSArm64, Windows, Android, IOS };

[[nodiscard]] inline std::string to_string(TargetPlatform p) {
    switch (p) {
        case TargetPlatform::MacOSArm64: return "macos-arm64";
        case TargetPlatform::Windows:    return "windows";
        case TargetPlatform::Android:    return "android";
        case TargetPlatform::IOS:        return "ios";
    }
    return "macos-arm64";  // irraggiungibile: insieme finito coperto sopra
}

[[nodiscard]] inline std::optional<TargetPlatform> parse_target_platform(
    std::string_view s) {
    if (s == "macos-arm64") return TargetPlatform::MacOSArm64;
    if (s == "windows")     return TargetPlatform::Windows;
    if (s == "android")     return TargetPlatform::Android;
    if (s == "ios")         return TargetPlatform::IOS;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// GdVersionRange — intervallo SemVer della versione di GD bersaglio, con
// estremi inclusi `[min, max]` (Req 3.1, 3.4).
// ---------------------------------------------------------------------------
struct GdVersionRange {
    SemVer min{};  // estremo minimo, incluso
    SemVer max{};  // estremo massimo, incluso

    // Test di appartenenza con ESTREMI INCLUSI (Req 3.4).
    [[nodiscard]] bool contains(const SemVer& v) const {
        return (min <= v) && (v <= max);
    }

    friend bool operator==(const GdVersionRange&, const GdVersionRange&) = default;
};

// ---------------------------------------------------------------------------
// Compatibility — sezione `[compat]` del Mod_Manifest. Entrambi i campi sono
// opzionali a livello di tipo, ma la loro assenza esclude la mod dal
// caricamento in fase di check di compatibilità (Req 3.6).
// ---------------------------------------------------------------------------
struct Compatibility {
    std::optional<TargetPlatform> platform;    // assente → Req 3.6
    std::optional<GdVersionRange> gdVersion;   // assente → Req 3.6

    friend bool operator==(const Compatibility&, const Compatibility&) = default;
};

// ---------------------------------------------------------------------------
// Manifest — modello logico del `pulse.toml` (Req 16.1, design IMP-05).
// ---------------------------------------------------------------------------
struct Manifest {
    int schemaVersion{1};
    std::string id;                          // non vuoto, <=256 (Req 16.1)
    SemVer version{};                        // SemVer (Req 16.1)
    std::string name;
    ModType type{ModType::Native};
    std::vector<EntryPoint> entryPoints;     // >=1 (Req 16.1)
    std::vector<Dependency> dependencies;    // id + vincolo (Req 16.1)
    std::vector<std::string> permissions;    // sezione permessi (Req 16.1/17.1)
    std::vector<SettingDecl> settings;
    Compatibility compat;                     // sezione `[compat]` (Req 3.1)

    // Uguaglianza campo-per-campo: `Dependency`/`VersionConstraint` non hanno un
    // `operator==` propri, quindi li confrontiamo esplicitamente.
    [[nodiscard]] bool operator==(const Manifest& o) const {
        if (schemaVersion != o.schemaVersion) return false;
        if (id != o.id) return false;
        if (!(version == o.version)) return false;
        if (name != o.name) return false;
        if (type != o.type) return false;
        if (entryPoints != o.entryPoints) return false;
        if (permissions != o.permissions) return false;
        if (settings != o.settings) return false;
        if (!(compat == o.compat)) return false;
        if (dependencies.size() != o.dependencies.size()) return false;
        for (std::size_t i = 0; i < dependencies.size(); ++i) {
            const Dependency& a = dependencies[i];
            const Dependency& b = o.dependencies[i];
            if (a.id != b.id) return false;
            if (!(a.versionConstraint.min == b.versionConstraint.min)) return false;
            if (a.versionConstraint.maxExclusive != b.versionConstraint.maxExclusive) return false;
        }
        return true;
    }
    [[nodiscard]] bool operator!=(const Manifest& o) const { return !(*this == o); }

    // Proiezione verso l'input del DependencyResolver (riconciliazione tipi):
    // mappa id/version/dependencies, scartando i campi non rilevanti al resolver.
    [[nodiscard]] pulse::lifecycle::ResolverManifest toResolverManifest() const {
        pulse::lifecycle::ResolverManifest rm;
        rm.id = id;
        rm.version = version;
        rm.dependencies = dependencies;
        return rm;
    }
};

// ---------------------------------------------------------------------------
// ParseResult — esito del parsing del `pulse.toml`.
// `ok == true`  -> `manifest` valorizzato.
// `ok == false` -> `error` descrive il problema strutturale (la validazione
//                  semantica completa contro lo schema è il task 13.2).
// ---------------------------------------------------------------------------
struct ParseResult {
    bool ok{false};
    Manifest manifest{};
    std::string error{};
};

// =====================  Dettagli di (de)serializzazione  ====================
namespace detail {

inline std::string_view trim(std::string_view s) {
    const auto isws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && isws(s.front())) s.remove_prefix(1);
    while (!s.empty() && isws(s.back())) s.remove_suffix(1);
    return s;
}

// Codifica una stringa TOML "basic" (tra apici doppi) con escaping minimale.
inline std::string encodeString(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

// Decodifica una stringa TOML "basic". `in` deve iniziare con '"'.
// In uscita `out` contiene il testo decodificato e ritorna true se la stringa
// è ben formata (apice di chiusura trovato).
inline bool decodeString(std::string_view in, std::string& out) {
    out.clear();
    if (in.empty() || in.front() != '"') return false;
    std::size_t i = 1;
    for (; i < in.size(); ++i) {
        char c = in[i];
        if (c == '"') return true;  // chiusura
        if (c == '\\') {
            if (i + 1 >= in.size()) return false;
            char e = in[++i];
            switch (e) {
                case '\\': out.push_back('\\'); break;
                case '"':  out.push_back('"'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                default:   out.push_back(e); break;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;  // apice di chiusura mancante
}

inline std::string encodeSemVer(const SemVer& v) {
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." +
           std::to_string(v.patch);
}

// Parsa "MAJOR.MINOR.PATCH" in SemVer. Ritorna false se malformato.
inline bool parseSemVer(std::string_view s, SemVer& out) {
    out = SemVer{};
    std::uint32_t parts[3] = {0, 0, 0};
    int idx = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '.') {
            if (idx > 2) return false;
            std::string_view tok = s.substr(start, i - start);
            if (tok.empty()) return false;
            std::uint32_t val = 0;
            auto [ptr, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), val);
            if (ec != std::errc{} || ptr != tok.data() + tok.size()) return false;
            parts[idx++] = val;
            start = i + 1;
        }
    }
    if (idx != 3) return false;
    out.major = parts[0];
    out.minor = parts[1];
    out.patch = parts[2];
    return true;
}

// Forma canonica di un vincolo di versione: ">=MIN" oppure ">=MIN <MAXEXCL".
inline std::string encodeConstraint(const VersionConstraint& c) {
    std::string out = ">=" + encodeSemVer(c.min);
    if (c.maxExclusive.has_value()) {
        out += " <" + encodeSemVer(*c.maxExclusive);
    }
    return out;
}

// Parsa un vincolo di versione: token separati da spazi, ciascuno ">=X.Y.Z",
// "<X.Y.Z" o una versione nuda (trattata come ">="). Ritorna false se invalido.
inline bool parseConstraint(std::string_view s, VersionConstraint& out) {
    out = VersionConstraint{};
    bool sawMin = false;
    std::size_t i = 0;
    const auto skipws = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i; };
    skipws();
    if (i >= s.size()) return false;
    while (i < s.size()) {
        skipws();
        if (i >= s.size()) break;
        bool isMax = false;
        if (s[i] == '<') {
            isMax = true;
            ++i;
        } else if (s[i] == '>' ) {
            ++i;
            if (i < s.size() && s[i] == '=') ++i;  // ">=" o ">"
        }
        std::size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
        std::string_view tok = s.substr(start, i - start);
        SemVer v{};
        if (!parseSemVer(tok, v)) return false;
        if (isMax) {
            out.maxExclusive = v;
        } else {
            out.min = v;
            sawMin = true;
        }
    }
    if (!sawMin) return false;  // un vincolo deve fissare almeno il minimo
    return true;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// serialize — produce la forma canonica `pulse.toml` di un Manifest (Req 16.5).
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::string serialize(const Manifest& m) {
    using detail::encodeString;
    std::string out;

    out += "schema_version = " + std::to_string(m.schemaVersion) + "\n\n";

    out += "[mod]\n";
    out += "id = " + encodeString(m.id) + "\n";
    out += "version = " + encodeString(detail::encodeSemVer(m.version)) + "\n";
    out += "name = " + encodeString(m.name) + "\n";
    out += "type = " + encodeString(to_string(m.type)) + "\n";

    for (const EntryPoint& ep : m.entryPoints) {
        out += "\n[[entry_points]]\n";
        out += "kind = " + encodeString(ep.kind) + "\n";
        out += "symbol = " + encodeString(ep.symbol) + "\n";
    }

    for (const Dependency& d : m.dependencies) {
        out += "\n[[dependencies]]\n";
        out += "id = " + encodeString(d.id) + "\n";
        out += "version = " + encodeString(detail::encodeConstraint(d.versionConstraint)) + "\n";
    }

    out += "\n[permissions]\n";
    out += "required = [";
    for (std::size_t i = 0; i < m.permissions.size(); ++i) {
        if (i) out += ", ";
        out += encodeString(m.permissions[i]);
    }
    out += "]\n";

    for (const SettingDecl& s : m.settings) {
        out += "\n[[settings]]\n";
        out += "name = " + encodeString(s.name) + "\n";
        out += "type = " + encodeString(s.type) + "\n";
        out += "default = ";
        if (std::holds_alternative<std::int64_t>(s.defaultValue)) {
            out += std::to_string(std::get<std::int64_t>(s.defaultValue));
        } else if (std::holds_alternative<double>(s.defaultValue)) {
            char buf[64];
            double d = std::get<double>(s.defaultValue);
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), d);
            if (ec == std::errc{}) {
                out.append(buf, ptr);
            } else {
                out += std::to_string(d);
            }
        } else if (std::holds_alternative<bool>(s.defaultValue)) {
            out += std::get<bool>(s.defaultValue) ? "true" : "false";
        } else {
            out += encodeString(std::get<std::string>(s.defaultValue));
        }
        out += "\n";
    }

    // Tabella `[compat]` (Req 3.1, 3.5): emessa solo quando almeno uno dei due
    // campi è presente, così un Manifest senza compatibilità dichiarata non
    // emette la tabella e il round-trip preserva un `compat` vuoto. Forma
    // canonica deterministica: `platform`, poi `gd_min`, poi `gd_max`; ciascun
    // campo è emesso solo se presente. `gd_min`/`gd_max` derivano sempre insieme
    // dal `GdVersionRange`.
    if (m.compat.platform.has_value() || m.compat.gdVersion.has_value()) {
        out += "\n[compat]\n";
        if (m.compat.platform.has_value()) {
            out += "platform = " + encodeString(to_string(*m.compat.platform)) + "\n";
        }
        if (m.compat.gdVersion.has_value()) {
            out += "gd_min = " + encodeString(detail::encodeSemVer(m.compat.gdVersion->min)) + "\n";
            out += "gd_max = " + encodeString(detail::encodeSemVer(m.compat.gdVersion->max)) + "\n";
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// parse — analizza una stringa `pulse.toml` nel modello Manifest (Req 16.5).
// Gestisce il sottoinsieme TOML emesso dal serializer (più tolleranza per
// commenti `#` e spaziatura). La validazione semantica è il task 13.2.
// ---------------------------------------------------------------------------
[[nodiscard]] inline ParseResult parse(std::string_view text) {
    using detail::trim;

    ParseResult res;
    Manifest m;
    m.entryPoints.clear();  // azzera i default

    enum class Section { Top, Mod, EntryPoint, Dependency, Permissions, Setting, Compat };
    Section section = Section::Top;

    const auto fail = [&](std::string msg) {
        res.ok = false;
        res.error = std::move(msg);
        return res;
    };

    // Itera per righe.
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        std::string_view raw =
            (nl == std::string_view::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;

        std::string_view line = trim(raw);
        if (line.empty() || line.front() == '#') continue;

        // Header di sezione / array di tabelle.
        if (line.front() == '[') {
            if (line == "[mod]") {
                section = Section::Mod;
            } else if (line == "[permissions]") {
                section = Section::Permissions;
            } else if (line == "[compat]") {
                section = Section::Compat;
            } else if (line == "[[entry_points]]") {
                m.entryPoints.emplace_back();
                section = Section::EntryPoint;
            } else if (line == "[[dependencies]]") {
                m.dependencies.emplace_back();
                section = Section::Dependency;
            } else if (line == "[[settings]]") {
                m.settings.emplace_back();
                section = Section::Setting;
            } else {
                return fail("Sezione sconosciuta: " + std::string(line));
            }
            continue;
        }

        // Coppia key = value.
        std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            return fail("Riga non valida (atteso 'key = value'): " + std::string(line));
        }
        std::string_view key = trim(line.substr(0, eq));
        std::string_view val = trim(line.substr(eq + 1));

        // Helper: decodifica una stringa quotata.
        const auto wantString = [&](std::string& dst) -> bool {
            if (val.empty() || val.front() != '"') return false;
            return detail::decodeString(val, dst);
        };

        switch (section) {
            case Section::Top: {
                if (key == "schema_version") {
                    int v = 0;
                    auto [ptr, ec] =
                        std::from_chars(val.data(), val.data() + val.size(), v);
                    (void)ptr;
                    if (ec != std::errc{}) return fail("schema_version non intero");
                    m.schemaVersion = v;
                } else {
                    return fail("Chiave inattesa al top-level: " + std::string(key));
                }
                break;
            }
            case Section::Mod: {
                if (key == "id") {
                    if (!wantString(m.id)) return fail("[mod].id non è una stringa");
                } else if (key == "name") {
                    if (!wantString(m.name)) return fail("[mod].name non è una stringa");
                } else if (key == "version") {
                    std::string sv;
                    if (!wantString(sv)) return fail("[mod].version non è una stringa");
                    if (!detail::parseSemVer(sv, m.version))
                        return fail("[mod].version SemVer malformata: " + sv);
                } else if (key == "type") {
                    std::string t;
                    if (!wantString(t)) return fail("[mod].type non è una stringa");
                    if (t == "native") {
                        m.type = ModType::Native;
                    } else if (t == "script") {
                        m.type = ModType::Script;
                    } else {
                        return fail("[mod].type sconosciuto: " + t);
                    }
                } else {
                    return fail("Chiave inattesa in [mod]: " + std::string(key));
                }
                break;
            }
            case Section::EntryPoint: {
                EntryPoint& ep = m.entryPoints.back();
                if (key == "kind") {
                    if (!wantString(ep.kind)) return fail("entry_point.kind non è una stringa");
                } else if (key == "symbol") {
                    if (!wantString(ep.symbol)) return fail("entry_point.symbol non è una stringa");
                } else {
                    return fail("Chiave inattesa in [[entry_points]]: " + std::string(key));
                }
                break;
            }
            case Section::Dependency: {
                Dependency& d = m.dependencies.back();
                if (key == "id") {
                    if (!wantString(d.id)) return fail("dependency.id non è una stringa");
                } else if (key == "version") {
                    std::string c;
                    if (!wantString(c)) return fail("dependency.version non è una stringa");
                    if (!detail::parseConstraint(c, d.versionConstraint))
                        return fail("dependency.version vincolo malformato: " + c);
                } else {
                    return fail("Chiave inattesa in [[dependencies]]: " + std::string(key));
                }
                break;
            }
            case Section::Permissions: {
                if (key != "required") {
                    return fail("Chiave inattesa in [permissions]: " + std::string(key));
                }
                if (val.empty() || val.front() != '[' || val.back() != ']') {
                    return fail("[permissions].required non è un array");
                }
                std::string_view inner = trim(val.substr(1, val.size() - 2));
                m.permissions.clear();
                std::size_t i = 0;
                while (i < inner.size()) {
                    // salta spazi e virgole
                    while (i < inner.size() && (inner[i] == ' ' || inner[i] == ',' ||
                                                inner[i] == '\t')) {
                        ++i;
                    }
                    if (i >= inner.size()) break;
                    if (inner[i] != '"') return fail("permesso non quotato nell'array");
                    // decodifica una stringa e avanza fino all'apice di chiusura
                    std::string item;
                    item.clear();
                    std::size_t j = i + 1;
                    bool closed = false;
                    for (; j < inner.size(); ++j) {
                        char c = inner[j];
                        if (c == '\\' && j + 1 < inner.size()) {
                            char e = inner[++j];
                            switch (e) {
                                case '\\': item.push_back('\\'); break;
                                case '"':  item.push_back('"'); break;
                                case 'n':  item.push_back('\n'); break;
                                case 'r':  item.push_back('\r'); break;
                                case 't':  item.push_back('\t'); break;
                                default:   item.push_back(e); break;
                            }
                        } else if (c == '"') {
                            closed = true;
                            ++j;
                            break;
                        } else {
                            item.push_back(c);
                        }
                    }
                    if (!closed) return fail("stringa permesso non terminata");
                    m.permissions.push_back(std::move(item));
                    i = j;
                }
                break;
            }
            case Section::Setting: {
                SettingDecl& s = m.settings.back();
                if (key == "name") {
                    if (!wantString(s.name)) return fail("setting.name non è una stringa");
                } else if (key == "type") {
                    if (!wantString(s.type)) return fail("setting.type non è una stringa");
                } else if (key == "default") {
                    // Il tipo del default è guidato dal campo `type` già letto.
                    if (s.type == "int") {
                        std::int64_t v = 0;
                        auto [ptr, ec] =
                            std::from_chars(val.data(), val.data() + val.size(), v);
                        (void)ptr;
                        if (ec != std::errc{}) return fail("setting.default int malformato");
                        s.defaultValue = v;
                    } else if (s.type == "float") {
                        double v = 0.0;
                        auto [ptr, ec] =
                            std::from_chars(val.data(), val.data() + val.size(), v);
                        (void)ptr;
                        if (ec != std::errc{}) return fail("setting.default float malformato");
                        s.defaultValue = v;
                    } else if (s.type == "bool") {
                        if (val == "true") {
                            s.defaultValue = true;
                        } else if (val == "false") {
                            s.defaultValue = false;
                        } else {
                            return fail("setting.default bool malformato");
                        }
                    } else {  // string (o tipo non riconosciuto: trattato come stringa)
                        std::string sv;
                        if (!wantString(sv)) return fail("setting.default string malformato");
                        s.defaultValue = sv;
                    }
                } else {
                    return fail("Chiave inattesa in [[settings]]: " + std::string(key));
                }
                break;
            }
            case Section::Compat: {
                // Tabella `[compat]` (Req 3.1). `platform` è validato contro
                // l'insieme finito; `gd_min`/`gd_max` sono SemVer che insieme
                // formano il `GdVersionRange` (estremi inclusi).
                if (key == "platform") {
                    std::string p;
                    if (!wantString(p)) return fail("[compat].platform non è una stringa");
                    std::optional<TargetPlatform> tp = parse_target_platform(p);
                    if (!tp.has_value())
                        return fail("[compat].platform sconosciuta: " + p);
                    m.compat.platform = *tp;
                } else if (key == "gd_min") {
                    std::string sv;
                    if (!wantString(sv)) return fail("[compat].gd_min non è una stringa");
                    SemVer v{};
                    if (!detail::parseSemVer(sv, v))
                        return fail("[compat].gd_min SemVer malformata: " + sv);
                    if (!m.compat.gdVersion.has_value()) m.compat.gdVersion = GdVersionRange{};
                    m.compat.gdVersion->min = v;
                } else if (key == "gd_max") {
                    std::string sv;
                    if (!wantString(sv)) return fail("[compat].gd_max non è una stringa");
                    SemVer v{};
                    if (!detail::parseSemVer(sv, v))
                        return fail("[compat].gd_max SemVer malformata: " + sv);
                    if (!m.compat.gdVersion.has_value()) m.compat.gdVersion = GdVersionRange{};
                    m.compat.gdVersion->max = v;
                } else {
                    return fail("Chiave inattesa in [compat]: " + std::string(key));
                }
                break;
            }
        }
    }

    res.ok = true;
    res.manifest = std::move(m);
    return res;
}

}  // namespace pulse::manifest

#endif  // PULSE_LOADER_LIFECYCLE_MANIFEST_HPP
