// loader/bindings/online_bindings_provider.cpp — logica del provider online,
// del formato .pbind e del fallback embedded (task 25.1).
#include "online_bindings_provider.hpp"

#include <charconv>
#include <sstream>
#include <string>
#include <utility>

namespace pulse::loader::bindings {

namespace {

// Converte una stringa di byte in std::string (i .pbind sono testuali ASCII).
std::string toText(const std::vector<std::uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

std::vector<std::uint8_t> toBytes(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

// Suddivide `text` sulle nuove righe ('\n'), preservando le righe vuote.
std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string line;
    std::istringstream stream(text);
    while (std::getline(stream, line)) {
        // Tollera terminatori CRLF rimuovendo un eventuale '\r' finale.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        line.clear();
    }
    return lines;
}

// Parsing di un intero senza eccezioni. nullopt se la stringa non è un intero
// completo e valido.
template <typename T>
std::optional<T> parseInt(std::string_view sv, int base = 10) {
    T value{};
    const char* begin = sv.data();
    const char* end = sv.data() + sv.size();
    auto [ptr, ec] = std::from_chars(begin, end, value, base);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

// Sostituisce tutte le occorrenze di `token` in `text` con `value`.
void replaceAll(std::string& text, std::string_view token, const std::string& value) {
    std::size_t pos = 0;
    while ((pos = text.find(token, pos)) != std::string::npos) {
        text.replace(pos, token.size(), value);
        pos += value.size();
    }
}

constexpr std::string_view kMagic = "PBIND1";
constexpr std::string_view kSigPrefix = "SIG:";

}  // namespace

std::string serializePbindPayload(const BindingSet& set) {
    std::ostringstream out;
    const BindingKey& key = set.key();
    out << "VERSION:" << key.version.major << '.' << key.version.minor << '\n';
    out << "PLATFORM:" << key.platformId << '\n';
    out << "COUNT:" << set.functions().size() << '\n';
    for (const auto& fn : set.functions()) {
        // symbol \t address(hex) \t resolved(0/1) \t returnType \t p1,p2,...
        out << fn.symbol << '\t' << std::hex << fn.address << std::dec << '\t'
            << (fn.resolved ? 1 : 0) << '\t' << fn.signature.returnType << '\t';
        for (std::size_t i = 0; i < fn.signature.parameterTypes.size(); ++i) {
            if (i != 0) {
                out << ',';
            }
            out << fn.signature.parameterTypes[i];
        }
        out << '\n';
    }
    return out.str();
}

std::vector<std::uint8_t> serializePbind(const std::string& signature,
                                         const BindingSet& set) {
    std::string text;
    text.append(kMagic);
    text.push_back('\n');
    text.append(kSigPrefix);
    text.append(signature);
    text.push_back('\n');
    text.append(serializePbindPayload(set));
    return toBytes(text);
}

std::optional<SignedPbind> parsePbind(const std::vector<std::uint8_t>& bytes) {
    const std::string text = toText(bytes);
    // L'header è composto dalle prime due righe; il resto è il payload.
    const std::size_t firstNl = text.find('\n');
    if (firstNl == std::string::npos) {
        return std::nullopt;
    }
    if (std::string_view(text).substr(0, firstNl) != kMagic) {
        return std::nullopt;
    }
    const std::size_t secondNl = text.find('\n', firstNl + 1);
    if (secondNl == std::string::npos) {
        return std::nullopt;
    }
    const std::string sigLine = text.substr(firstNl + 1, secondNl - (firstNl + 1));
    if (std::string_view(sigLine).substr(0, kSigPrefix.size()) != kSigPrefix) {
        return std::nullopt;
    }

    SignedPbind result;
    result.signature = sigLine.substr(kSigPrefix.size());
    const std::string payload = text.substr(secondNl + 1);
    result.payload = toBytes(payload);
    return result;
}

std::optional<BindingSet> parsePbindPayload(const std::vector<std::uint8_t>& payload) {
    const std::vector<std::string> lines = splitLines(toText(payload));
    if (lines.size() < 3) {
        return std::nullopt;
    }

    // Riga VERSION:major.minor
    constexpr std::string_view kVersion = "VERSION:";
    constexpr std::string_view kPlatform = "PLATFORM:";
    constexpr std::string_view kCount = "COUNT:";

    if (std::string_view(lines[0]).substr(0, kVersion.size()) != kVersion ||
        std::string_view(lines[1]).substr(0, kPlatform.size()) != kPlatform ||
        std::string_view(lines[2]).substr(0, kCount.size()) != kCount) {
        return std::nullopt;
    }

    const std::string versionStr = lines[0].substr(kVersion.size());
    const std::size_t dot = versionStr.find('.');
    if (dot == std::string::npos) {
        return std::nullopt;
    }
    auto major = parseInt<int>(std::string_view(versionStr).substr(0, dot));
    auto minor = parseInt<int>(std::string_view(versionStr).substr(dot + 1));
    if (!major || !minor) {
        return std::nullopt;
    }

    const std::string platformId = lines[1].substr(kPlatform.size());
    auto count = parseInt<std::size_t>(lines[2].substr(kCount.size()));
    if (!count) {
        return std::nullopt;
    }
    if (lines.size() < 3 + *count) {
        return std::nullopt;
    }

    BindingSet set{BindingKey{GdVersion{*major, *minor}, platformId}};
    for (std::size_t i = 0; i < *count; ++i) {
        const std::string& row = lines[3 + i];
        // Suddivisione in 5 campi separati da TAB.
        std::vector<std::string> fields;
        std::string field;
        std::istringstream rowStream(row);
        while (std::getline(rowStream, field, '\t')) {
            fields.push_back(field);
        }
        if (fields.size() != 5) {
            return std::nullopt;
        }
        FunctionBinding fn;
        fn.symbol = fields[0];
        auto address = parseInt<std::uintptr_t>(fields[1], /*base=*/16);
        if (!address) {
            return std::nullopt;
        }
        fn.address = *address;
        if (fields[2] != "0" && fields[2] != "1") {
            return std::nullopt;
        }
        fn.resolved = (fields[2] == "1");
        fn.signature.returnType = fields[3];
        // Tipi dei parametri separati da virgola (campo vuoto => nessun parametro).
        if (!fields[4].empty()) {
            std::string param;
            std::istringstream paramStream(fields[4]);
            while (std::getline(paramStream, param, ',')) {
                fn.signature.parameterTypes.push_back(param);
            }
        }
        set.add(std::move(fn));
    }
    return set;
}

OnlineBindingsProvider::OnlineBindingsProvider(IBindingFetcher& fetcher,
                                               ISignatureVerifier& verifier,
                                               IBindingsProvider& fallback,
                                               std::string urlTemplate)
    : fetcher_(fetcher),
      verifier_(verifier),
      fallback_(fallback),
      urlTemplate_(std::move(urlTemplate)) {}

std::string OnlineBindingsProvider::buildUrl(const BindingKey& key) const {
    std::string url = urlTemplate_;
    const std::string version =
        std::to_string(key.version.major) + "." + std::to_string(key.version.minor);
    replaceAll(url, "{version}", version);
    replaceAll(url, "{platform}", key.platformId);
    return url;
}

const BindingSet* OnlineBindingsProvider::findCached(const BindingKey& key) const {
    for (const auto& set : cache_) {
        if (set.key() == key) {  // corrispondenza esatta della coppia
            return &set;
        }
    }
    return nullptr;
}

std::optional<BindingSet> OnlineBindingsProvider::load(const BindingKey& key) {
    // 1) Cache hit: riusa il set già scaricato e verificato senza ri-scaricare.
    if (const BindingSet* cached = findCached(key)) {
        current_ = *cached;
        return current_;
    }

    // 2) Tentativo online: fetch -> parse -> verifica firma -> parse payload.
    if (auto bytes = fetcher_.fetch(buildUrl(key))) {
        if (auto container = parsePbind(*bytes)) {
            if (verifier_.verify(container->payload, container->signature)) {
                if (auto set = parsePbindPayload(container->payload)) {
                    // Corrispondenza ESATTA della coppia (Req 20.2): il set
                    // scaricato deve dichiarare la stessa chiave richiesta.
                    if (set->key() == key) {
                        cache_.push_back(*set);
                        current_ = std::move(*set);
                        return current_;
                    }
                }
            }
        }
    }

    // 3) Fallback embedded (fetch/parse/verifica falliti o coppia non esatta).
    // I risultati del fallback NON vengono messi in cache, così un tentativo
    // online successivo può ancora avere successo.
    if (auto fallbackSet = fallback_.load(key)) {
        current_ = fallbackSet;
        return current_;
    }

    // Nessuna corrispondenza esatta né online né embedded: nessun fuzzy-match.
    return std::nullopt;
}

std::optional<FunctionBinding> OnlineBindingsProvider::resolve(
    std::string_view symbol) const {
    if (!current_) {
        return std::nullopt;
    }
    // Corrispondenza esatta del simbolo nel set corrente (Req 20.2).
    return current_->resolve(symbol);
}

}  // namespace pulse::loader::bindings
