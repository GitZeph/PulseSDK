// loader/bindings/pbind_format.cpp — implementazione del parser/serializer
// del Binding_Set_File `.pbind` (Req 4.1, 4.6, 4.7).
//
// Scelte di robustezza (Req 4.7): nessuna conversione che lanci eccezioni —
// le stringhe numeriche sono convertite con `std::from_chars` (non-throwing) e
// ogni condizione malformata diventa un `PbindParseError` con causa e riga.
#include "pbind_format.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <sstream>
#include <vector>

namespace pulse::loader::bindings {

namespace {

// Rimuove gli spazi iniziali/finali (spazi, tab, CR) da una vista.
std::string_view trim(std::string_view s) {
    constexpr std::string_view kWs = " \t\r\n\f\v";
    const auto begin = s.find_first_not_of(kWs);
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = s.find_last_not_of(kWs);
    return s.substr(begin, end - begin + 1);
}

// Converte un intero senza segno da `text` in base 10 o 16 (prefisso "0x"/"0X").
// Restituisce nullopt se `text` è vuoto o contiene caratteri non validi
// (consumo TOTALE richiesto: nessun carattere residuo). Non lancia (Req 4.7).
std::optional<std::uintptr_t> parseUnsigned(std::string_view text) {
    text = trim(text);
    if (text.empty()) {
        return std::nullopt;
    }
    int base = 10;
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
        if (text.empty()) {
            return std::nullopt;  // "0x" senza cifre
        }
    }
    std::uintptr_t value{};
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value, base);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;  // cifre non valide o caratteri residui
    }
    return value;
}

// Converte un intero con segno (non negativo) per i campi di versione.
std::optional<int> parseVersionComponent(std::string_view text) {
    text = trim(text);
    if (text.empty()) {
        return std::nullopt;
    }
    int value{};
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last || value < 0) {
        return std::nullopt;
    }
    return value;
}

// Analizza "major.minor" (es. "2.2081") in GdVersion. nullopt se malformato.
std::optional<GdVersion> parseGdVersion(std::string_view text) {
    text = trim(text);
    const auto dot = text.find('.');
    if (dot == std::string_view::npos) {
        return std::nullopt;  // serve esattamente un separatore major.minor
    }
    const auto major = parseVersionComponent(text.substr(0, dot));
    const auto minor = parseVersionComponent(text.substr(dot + 1));
    if (!major || !minor) {
        return std::nullopt;
    }
    return GdVersion{*major, *minor};
}

// Suddivide una lista di parametri separati da virgola in tipi singoli,
// rimuovendo gli spazi. La stringa vuota produce una lista vuota (zero
// parametri), coerente con la serializzazione di una `Signature` senza
// parametri. Round-trip stabile con `joinParams`.
std::vector<std::string> splitParams(std::string_view text) {
    std::vector<std::string> out;
    text = trim(text);
    if (text.empty()) {
        return out;
    }
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto comma = text.find(',', start);
        const auto end = (comma == std::string_view::npos) ? text.size() : comma;
        const auto token = trim(text.substr(start, end - start));
        out.emplace_back(token);
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

// Unisce i tipi dei parametri in una lista separata da ", " (round-trip con
// `splitParams`).
std::string joinParams(const std::vector<std::string>& params) {
    std::string out;
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += params[i];
    }
    return out;
}

// Divide una riga "key = value" sul PRIMO '='. nullopt se manca '='.
struct KeyValue {
    std::string_view key;
    std::string_view value;
};
std::optional<KeyValue> splitKeyValue(std::string_view line) {
    const auto eq = line.find('=');
    if (eq == std::string_view::npos) {
        return std::nullopt;
    }
    return KeyValue{trim(line.substr(0, eq)), trim(line.substr(eq + 1))};
}

// Stato accumulato per il blocco [function] corrente durante il parsing.
struct PendingFunction {
    int lineOfHeader{0};
    std::optional<std::string> symbol;
    std::optional<std::uintptr_t> offset;
    std::optional<std::string> returnType;
    std::optional<std::vector<std::string>> params;
    std::optional<bool> verified;
};

PbindParseResult makeError(std::string message, int line) {
    PbindParseResult r;
    r.error = PbindParseError{std::move(message), line};
    return r;
}

}  // namespace

PbindParseResult parse_pbind(std::string_view content) {
    std::optional<int> pbindVersion;
    std::optional<GdVersion> gdVersion;
    std::optional<std::string> platform;

    std::vector<FunctionBinding> functions;
    std::optional<PendingFunction> pending;
    bool seenFunctionSection = false;

    // Chiude il blocco [function] corrente, validandone i campi obbligatori.
    auto flushPending = [&](int atLine) -> std::optional<PbindParseResult> {
        if (!pending) {
            return std::nullopt;
        }
        const int hdr = pending->lineOfHeader;
        if (!pending->symbol) {
            return makeError("blocco [function] privo della chiave 'symbol'", hdr);
        }
        if (!pending->offset) {
            return makeError("blocco [function] privo della chiave 'offset'", hdr);
        }
        if (!pending->returnType) {
            return makeError("blocco [function] privo della chiave 'return'", hdr);
        }
        if (!pending->verified) {
            return makeError("blocco [function] privo della chiave 'verified'", hdr);
        }
        FunctionBinding fn;
        fn.symbol = *pending->symbol;
        fn.address = *pending->offset;
        fn.signature = Signature{*pending->returnType,
                                 pending->params.value_or(std::vector<std::string>{})};
        fn.resolved = *pending->verified;
        functions.push_back(std::move(fn));
        pending.reset();
        (void)atLine;
        return std::nullopt;
    };

    int lineNo = 0;
    std::size_t pos = 0;
    while (pos <= content.size()) {
        // Estrae la riga successiva (gestisce sia '\n' sia EOF).
        const auto nl = content.find('\n', pos);
        const auto rawEnd = (nl == std::string_view::npos) ? content.size() : nl;
        std::string_view raw = content.substr(pos, rawEnd - pos);
        const bool atEof = (nl == std::string_view::npos);
        pos = rawEnd + 1;
        ++lineNo;

        const std::string_view line = trim(raw);

        // Righe vuote e commenti ('#') sono ignorate.
        if (line.empty() || line.front() == '#') {
            if (atEof) {
                break;
            }
            continue;
        }

        // Inizio di un nuovo blocco [function].
        if (line == "[function]") {
            if (auto err = flushPending(lineNo)) {
                return *err;
            }
            pending = PendingFunction{};
            pending->lineOfHeader = lineNo;
            seenFunctionSection = true;
            if (atEof) {
                break;
            }
            continue;
        }

        // Qualunque altra riga deve essere "key = value".
        const auto kv = splitKeyValue(line);
        if (!kv) {
            return makeError("riga malformata: atteso 'key = value' oppure '[function]'",
                             lineNo);
        }
        const std::string_view key = kv->key;
        const std::string_view value = kv->value;
        if (key.empty()) {
            return makeError("chiave vuota a sinistra di '='", lineNo);
        }

        if (!pending) {
            // Sezione d'intestazione (prima di qualunque [function]).
            if (key == "pbind_version") {
                const auto v = parseVersionComponent(value);
                if (!v) {
                    return makeError("'pbind_version' non e' un intero valido", lineNo);
                }
                if (*v != kPbindFormatVersion) {
                    return makeError("versione del formato 'pbind_version' non supportata",
                                     lineNo);
                }
                pbindVersion = v;
            } else if (key == "gd_version") {
                const auto v = parseGdVersion(value);
                if (!v) {
                    return makeError("'gd_version' malformata: atteso 'major.minor'", lineNo);
                }
                gdVersion = v;
            } else if (key == "platform") {
                if (value.empty()) {
                    return makeError("'platform' non puo' essere vuota", lineNo);
                }
                platform = std::string{value};
            } else {
                return makeError("chiave d'intestazione sconosciuta: '" +
                                     std::string{key} + "'",
                                 lineNo);
            }
        } else {
            // Campo all'interno di un blocco [function].
            if (key == "symbol") {
                if (value.empty()) {
                    return makeError("'symbol' non puo' essere vuoto", lineNo);
                }
                pending->symbol = std::string{value};
            } else if (key == "offset") {
                const auto off = parseUnsigned(value);
                if (!off) {
                    return makeError("'offset' non e' un intero valido (atteso decimale o 0x...)",
                                     lineNo);
                }
                pending->offset = off;
            } else if (key == "return") {
                if (value.empty()) {
                    return makeError("'return' non puo' essere vuoto", lineNo);
                }
                pending->returnType = std::string{value};
            } else if (key == "params") {
                pending->params = splitParams(value);
            } else if (key == "verified") {
                const auto v = trim(value);
                if (v == "true") {
                    pending->verified = true;
                } else if (v == "false") {
                    pending->verified = false;
                } else {
                    return makeError("'verified' deve essere 'true' o 'false'", lineNo);
                }
            } else {
                return makeError("chiave di [function] sconosciuta: '" +
                                     std::string{key} + "'",
                                 lineNo);
            }
        }

        if (atEof) {
            break;
        }
    }

    // Chiude l'ultimo blocco [function] eventualmente aperto.
    if (auto err = flushPending(lineNo)) {
        return *err;
    }

    // Validazione delle chiavi d'intestazione obbligatorie (Req 4.7).
    if (!pbindVersion) {
        return makeError("chiave d'intestazione mancante: 'pbind_version'", 0);
    }
    if (!gdVersion) {
        return makeError("chiave d'intestazione mancante: 'gd_version'", 0);
    }
    if (!platform) {
        return makeError("chiave d'intestazione mancante: 'platform'", 0);
    }
    (void)seenFunctionSection;  // un set senza funzioni e' valido (zero binding).

    BindingSet set{BindingKey{*gdVersion, *platform}};
    for (auto& fn : functions) {
        set.add(std::move(fn));
    }

    PbindParseResult result;
    result.value = std::move(set);
    return result;
}

std::string serialize_pbind(const BindingSet& set) {
    std::ostringstream out;

    // Intestazione in ordine fisso (determinismo).
    out << "pbind_version = " << kPbindFormatVersion << '\n';
    out << "gd_version = " << set.key().version.major << '.'
        << set.key().version.minor << '\n';
    out << "platform = " << set.key().platformId << '\n';

    // Ordinamento canonico delle funzioni per `symbol` (Req 4.6).
    std::vector<FunctionBinding> ordered = set.functions();
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const FunctionBinding& a, const FunctionBinding& b) {
                         return a.symbol < b.symbol;
                     });

    for (const auto& fn : ordered) {
        out << '\n';
        out << "[function]\n";
        out << "symbol = " << fn.symbol << '\n';
        // Offset in esadecimale per leggibilita'; il round-trip e' garantito
        // dal parser che accetta sia decimale sia 0x...
        out << "offset = 0x" << std::hex << std::uppercase
            << static_cast<std::uintptr_t>(fn.address) << std::dec << std::nouppercase
            << '\n';
        out << "return = " << fn.signature.returnType << '\n';
        out << "params = " << joinParams(fn.signature.parameterTypes) << '\n';
        out << "verified = " << (fn.resolved ? "true" : "false") << '\n';
    }

    return out.str();
}

}  // namespace pulse::loader::bindings
