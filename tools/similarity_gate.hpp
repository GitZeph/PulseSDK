// =============================================================================
// Pulse — Gate di similarità del codice rispetto a Geode (task 39.3)
//
// Requisito 27 (Originalità rispetto a Geode):
//   27.1  Nessun blocco di codice sorgente identico a Geode di lunghezza pari o
//         superiore a 8 righe consecutive.
//   27.2  I frammenti derivati da Geode sotto licenza compatibile DEVONO
//         includere l'attribuzione di licenza richiesta prima del completamento
//         della build.
//   27.3  Alla build candidata al rilascio viene eseguito un controllo
//         automatico di similarità, registrando una percentuale per ciascun
//         file.
//   27.4  Se un file raggiunge una similarità >= 30% rispetto a Geode, la build
//         candidata viene BLOCCATA con un messaggio che indica il file e la
//         percentuale rilevata, preservando i sorgenti senza modifiche.
//   27.5  (vedi 27.2) l'attribuzione di licenza è imposta prima del
//         completamento della build.
//
// Proprietà 40 (design.md): per ogni file con una percentuale di similarità
//   calcolata rispetto a Geode, la build candidata è bloccata SE E SOLO SE la
//   similarità è >= 30%, e in tal caso il messaggio identifica il file e la
//   percentuale rilevata senza modificare i sorgenti.
//
// CARATTERISTICHE DI PROGETTO
//   - Header-only: includere e usare, nessuna unità di compilazione dedicata.
//   - SOLA LETTURA: il gate non scrive né modifica MAI alcun sorgente. Opera su
//     copie del contenuto passate come stringhe (input by value/const ref).
//   - Iniettabile e host-testabile: il corpus di riferimento Geode è fornito
//     dal chiamante (NON viene letto alcun sorgente reale di Geode), così il
//     gate è completamente testabile sull'host.
//   - API guidabile con input arbitrari: oltre al calcolo a partire dai
//     contenuti, è possibile valutare la decisione del gate a partire da una
//     percentuale di similarità precalcolata (utile per i property test sulla
//     soglia >= 30%, task 39.4).
// =============================================================================
#ifndef PULSE_TOOLS_SIMILARITY_GATE_HPP
#define PULSE_TOOLS_SIMILARITY_GATE_HPP

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace pulse::quality {

// ---------------------------------------------------------------------------
// Soglia di blocco (Requisito 27.3/27.4, Proprietà 40): la build è bloccata
// se e solo se la similarità è >= 30%.
// ---------------------------------------------------------------------------
inline constexpr double kSimilarityBlockThresholdPercent = 30.0;

// ---------------------------------------------------------------------------
// Lunghezza minima di un blocco identico consecutivo vietato (Requisito 27.1):
// nessun blocco identico di 8 o più righe consecutive.
// ---------------------------------------------------------------------------
inline constexpr std::size_t kMaxIdenticalConsecutiveLines = 8;

// ---------------------------------------------------------------------------
// Esito della valutazione di un singolo file rispetto al corpus Geode.
// È volutamente "osservabile": i test ispezionano direttamente questi campi.
// ---------------------------------------------------------------------------
struct FileSimilarityResult {
    std::string filePath;                 // file valutato (solo identificativo)
    double similarityPercent = 0.0;       // % di similarità calcolata vs Geode
    bool blocked = false;                 // build candidata bloccata?
    std::size_t longestIdenticalRun = 0;  // run massima di righe identiche
    bool hasForbiddenIdenticalBlock = false;  // run >= 8 righe (Req 27.1)
    bool isDerivedFragment = false;       // frammento marcato come derivato
    bool missingAttribution = false;      // attribuzione assente (Req 27.2/27.5)
    std::vector<std::string> reasons;     // motivazioni leggibili del blocco

    // Comodità per i test: true se è stata registrata almeno una motivazione.
    [[nodiscard]] bool hasReasons() const { return !reasons.empty(); }

    // Messaggio aggregato che identifica file + percentuale e le cause.
    [[nodiscard]] std::string message() const {
        std::ostringstream os;
        os << (blocked ? "BLOCCATA" : "OK") << ": file '" << filePath
           << "' similarita' " << similarityPercent << "% (soglia "
           << kSimilarityBlockThresholdPercent << "%)";
        for (const auto& r : reasons) {
            os << "; " << r;
        }
        return os.str();
    }
};

// ---------------------------------------------------------------------------
// Spezza un testo in righe (separatore '\n'), normalizzando i terminatori di
// riga Windows ('\r\n' -> '\n') e rimuovendo lo spazio bianco iniziale/finale
// di ciascuna riga. La normalizzazione opera su una COPIA: i sorgenti non
// vengono mai modificati (Requisito 27.4).
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::vector<std::string> splitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current.push_back(c);
        }
    }
    // Ultima riga senza newline finale.
    lines.push_back(current);

    // Trim degli spazi ai bordi per confronti robusti rispetto a indentazione.
    for (auto& line : lines) {
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos) {
            line.clear();
            continue;
        }
        const auto last = line.find_last_not_of(" \t");
        line = line.substr(first, last - first + 1);
    }
    return lines;
}

// ---------------------------------------------------------------------------
// Estrae solo le righe "significative": esclude righe vuote e commenti di riga
// (// e #) coerentemente con l'eccezione del Requisito 27.1 (righe vuote e
// commenti non concorrono al conteggio del blocco identico).
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::vector<std::string> significantLines(
    const std::vector<std::string>& lines) {
    std::vector<std::string> out;
    out.reserve(lines.size());
    for (const auto& line : lines) {
        if (line.empty()) {
            continue;
        }
        if (line.rfind("//", 0) == 0 || line.rfind("#", 0) == 0) {
            continue;  // commento di riga
        }
        out.push_back(line);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Metrica di similarità: indice di Jaccard sulle righe significative.
//
//   J(C, R) = |insieme(C) ∩ insieme(R)| / |insieme(C) ∪ insieme(R)|
//
// La percentuale è J * 100. È una metrica deterministica, simmetrica e
// normalizzata in [0, 100]; due file identici (a meno di spazi/commenti)
// danno 100%, due file completamente disgiunti danno 0%. La scelta di Jaccard
// sulle righe è robusta al riordino e indipendente dalla lunghezza assoluta.
//
// Caso limite: se entrambi i file sono privi di righe significative, l'unione è
// vuota e la similarità è definita 0% (nessun contenuto condiviso da bloccare).
// ---------------------------------------------------------------------------
[[nodiscard]] inline double computeSimilarityPercent(std::string_view candidate,
                                                     std::string_view reference) {
    const auto candLines = significantLines(splitLines(candidate));
    const auto refLines = significantLines(splitLines(reference));

    std::unordered_set<std::string> candSet(candLines.begin(), candLines.end());
    std::unordered_set<std::string> refSet(refLines.begin(), refLines.end());

    if (candSet.empty() && refSet.empty()) {
        return 0.0;
    }

    std::size_t intersection = 0;
    for (const auto& line : candSet) {
        if (refSet.find(line) != refSet.end()) {
            ++intersection;
        }
    }
    const std::size_t unionSize = candSet.size() + refSet.size() - intersection;
    if (unionSize == 0) {
        return 0.0;
    }
    return (static_cast<double>(intersection) / static_cast<double>(unionSize)) *
           100.0;
}

// ---------------------------------------------------------------------------
// Calcola la run massima di righe IDENTICHE e CONSECUTIVE comuni a candidato e
// riferimento (Requisito 27.1). Si confronta sulle righe significative
// (escludendo vuote e commenti) per allinearsi all'eccezione del requisito.
// Usa una scansione tipo "longest common substring" su righe via DP rolling.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::size_t longestIdenticalRun(std::string_view candidate,
                                                     std::string_view reference) {
    const auto a = significantLines(splitLines(candidate));
    const auto b = significantLines(splitLines(reference));
    if (a.empty() || b.empty()) {
        return 0;
    }

    // DP a due righe: prev[j] = lunghezza run che termina in a[i-1], b[j-1].
    std::vector<std::size_t> prev(b.size() + 1, 0);
    std::vector<std::size_t> curr(b.size() + 1, 0);
    std::size_t best = 0;

    for (std::size_t i = 1; i <= a.size(); ++i) {
        for (std::size_t j = 1; j <= b.size(); ++j) {
            if (a[i - 1] == b[j - 1]) {
                curr[j] = prev[j - 1] + 1;
                best = std::max(best, curr[j]);
            } else {
                curr[j] = 0;
            }
        }
        std::swap(prev, curr);
        std::fill(curr.begin(), curr.end(), 0);
    }
    return best;
}

// ---------------------------------------------------------------------------
// Decisione di blocco a partire da una percentuale PRECALCOLATA (Proprietà 40,
// Requisito 27.4). Espone esplicitamente la regola "blocked sse pct >= 30",
// così i property test della soglia (task 39.4) possono guidarla con input
// arbitrari senza dipendere dal calcolo della metrica.
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool isBlockedBySimilarity(double similarityPercent) {
    return similarityPercent >= kSimilarityBlockThresholdPercent;
}

// ---------------------------------------------------------------------------
// Descrittore di un file candidato da valutare. Il contenuto del riferimento è
// INIETTATO (corpus Geode fornito dal chiamante): il gate non legge sorgenti
// reali di Geode.
//
//   isDerived             : true se il frammento è dichiarato derivato da Geode
//                           sotto licenza compatibile (Requisito 27.2/27.5).
//   attributionMarker     : marcatore richiesto in presenza di un frammento
//                           derivato (es. "SPDX-License-Identifier").
// ---------------------------------------------------------------------------
struct CandidateFile {
    std::string path;
    std::string content;
    std::string referenceContent;  // corpus Geode iniettato per questo file
    bool isDerived = false;
    std::string attributionMarker = "SPDX-License-Identifier";
};

// ---------------------------------------------------------------------------
// Verifica la presenza dell'attribuzione di licenza per un frammento derivato
// (Requisito 27.2/27.5): se il file è marcato derivato, il marcatore di
// attribuzione DEVE comparire nel contenuto, altrimenti l'attribuzione è
// considerata mancante.
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool hasRequiredAttribution(const CandidateFile& file) {
    if (!file.isDerived) {
        return true;  // nessuna attribuzione richiesta
    }
    if (file.attributionMarker.empty()) {
        return false;  // un derivato senza marcatore definito non è attribuibile
    }
    return file.content.find(file.attributionMarker) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Valuta un singolo file candidato rispetto al corpus Geode iniettato. È una
// operazione di SOLA LETTURA: non modifica né l'input né alcun sorgente.
//
// La build candidata viene BLOCCATA se almeno una delle condizioni vale:
//   (a) similarità >= 30%                          (Req 27.4 / Proprietà 40)
//   (b) blocco identico di >= 8 righe consecutive  (Req 27.1)
//   (c) frammento derivato privo di attribuzione   (Req 27.2 / 27.5)
//
// In ogni caso il risultato riporta file + percentuale e le motivazioni.
// ---------------------------------------------------------------------------
[[nodiscard]] inline FileSimilarityResult evaluateFile(const CandidateFile& file) {
    FileSimilarityResult result;
    result.filePath = file.path;

    // (a) Similarità complessiva (Req 27.3) e soglia di blocco (Req 27.4).
    result.similarityPercent =
        computeSimilarityPercent(file.content, file.referenceContent);
    const bool blockedBySimilarity =
        isBlockedBySimilarity(result.similarityPercent);
    if (blockedBySimilarity) {
        std::ostringstream os;
        os << "similarita' " << result.similarityPercent
           << "% >= soglia " << kSimilarityBlockThresholdPercent
           << "% nel file '" << file.path << "'";
        result.reasons.push_back(os.str());
    }

    // (b) Blocco identico di >= 8 righe consecutive (Req 27.1).
    result.longestIdenticalRun =
        longestIdenticalRun(file.content, file.referenceContent);
    result.hasForbiddenIdenticalBlock =
        result.longestIdenticalRun >= kMaxIdenticalConsecutiveLines;
    if (result.hasForbiddenIdenticalBlock) {
        std::ostringstream os;
        os << "blocco identico di " << result.longestIdenticalRun
           << " righe consecutive (>= " << kMaxIdenticalConsecutiveLines
           << ") nel file '" << file.path << "'";
        result.reasons.push_back(os.str());
    }

    // (c) Attribuzione di licenza per frammenti derivati (Req 27.2/27.5).
    result.isDerivedFragment = file.isDerived;
    result.missingAttribution = !hasRequiredAttribution(file);
    if (result.missingAttribution) {
        std::ostringstream os;
        os << "frammento derivato da Geode privo di attribuzione di licenza ('"
           << file.attributionMarker << "') nel file '" << file.path << "'";
        result.reasons.push_back(os.str());
    }

    result.blocked = blockedBySimilarity || result.hasForbiddenIdenticalBlock ||
                     result.missingAttribution;
    return result;
}

// ---------------------------------------------------------------------------
// Esito complessivo del gate sull'intera build candidata: la build è bloccata
// se almeno un file è bloccato. Espone i risultati per-file affinché i test (e
// la CI) possano identificare i file interessati e le percentuali.
// ---------------------------------------------------------------------------
struct GateReport {
    std::vector<FileSimilarityResult> files;

    [[nodiscard]] bool buildBlocked() const {
        return std::any_of(files.begin(), files.end(),
                           [](const FileSimilarityResult& r) { return r.blocked; });
    }

    [[nodiscard]] std::vector<FileSimilarityResult> blockedFiles() const {
        std::vector<FileSimilarityResult> out;
        for (const auto& r : files) {
            if (r.blocked) {
                out.push_back(r);
            }
        }
        return out;
    }

    [[nodiscard]] std::string summary() const {
        std::ostringstream os;
        os << "Gate similarita': " << files.size() << " file valutati, "
           << blockedFiles().size() << " bloccati"
           << (buildBlocked() ? " -> BUILD CANDIDATA BLOCCATA" : " -> OK");
        for (const auto& r : files) {
            if (r.blocked) {
                os << "\n  - " << r.message();
            }
        }
        return os.str();
    }
};

// ---------------------------------------------------------------------------
// Esegue il gate sull'intero insieme di file candidati (Requisito 27.3). Sola
// lettura: nessun sorgente viene modificato.
// ---------------------------------------------------------------------------
[[nodiscard]] inline GateReport runGate(const std::vector<CandidateFile>& files) {
    GateReport report;
    report.files.reserve(files.size());
    for (const auto& file : files) {
        report.files.push_back(evaluateFile(file));
    }
    return report;
}

}  // namespace pulse::quality

#endif  // PULSE_TOOLS_SIMILARITY_GATE_HPP
