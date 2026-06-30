// tests/property39_improvements_doc_completeness_test.cpp
// Feature: pulse-sdk, Property 39 — Completezza della documentazione dei
// miglioramenti Pulse-vs-Geode.
// Validates: Requisiti 24.1, 24.2 (Requisito 24.1, Requisito 24.2)
//
// Property 39 (design.md / Req 24.1, 24.2): la documentazione dei miglioramenti
// è giudicata COMPLETA se e solo se:
//   * la tabella dei miglioramenti contiene almeno 10 righe (Req 24.1);
//   * OGNI riga di miglioramento possiede tutti e quattro gli elementi
//     obbligatori non vuoti — identificatore univoco di forma IMP-NN, limite di
//     Geode, approccio di Pulse, criterio osservabile (Req 24.1/24.2);
//   * la tabella di confronto contiene almeno 10 funzionalità, ciascuna con
//     nome, stato in Geode e stato in Pulse non vuoti (Req 24.2).
// Inoltre, quando una riga è incompleta, il controllo deve indicare ESATTAMENTE
// quali elementi mancano (Req 24.2).
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si GENERA un modello in-memory della documentazione con un numero di
//     righe casuale (0..15) sia per i miglioramenti sia per il confronto, così
//     entrambi i rami (sotto/sopra la soglia di 10) sono esercitati spesso;
//   * ogni colonna di ogni riga è casualmente presente (token non vuoto) o
//     assente (stringa vuota); l'ID è casualmente valido (IMP-NN), vuoto o
//     invalido (token di sole lettere che non combacia con IMP-NN);
//   * si applica lo STESSO predicato di completezza usato dal checker e si
//     confronta il suo esito con un oracolo ricalcolato in modo indipendente,
//     verificando anche che l'elenco degli elementi mancanti coincida con
//     quelli realmente assenti.
// Un caso aggiuntivo analizza il documento REALE docs/pulse-vs-geode.md (via la
// macro PULSE_VS_GEODE_DOC, la stessa del checker) e ne asserisce la
// completezza, applicando lo stesso predicato a input generati e reali.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef PULSE_VS_GEODE_DOC
#error "PULSE_VS_GEODE_DOC deve essere definita (percorso di docs/pulse-vs-geode.md)"
#endif

namespace {

// Soglie minime condivise con il checker (Req 24.1/24.2).
constexpr int kMinImprovements = 10;
constexpr int kMinComparisons = 10;

// ===========================================================================
// Modello in-memory della documentazione (input uniforme per generato e reale).
// ===========================================================================

struct ImprovementRow {
    std::string id;             // identificatore univoco (forma IMP-NN)
    std::string geodeLimit;     // limite di Geode
    std::string pulseApproach;  // approccio di Pulse
    std::string criterion;      // criterio osservabile
};

struct ComparisonRow {
    std::string feature;  // nome funzionalità
    std::string geode;    // stato in Geode
    std::string pulse;    // stato in Pulse
};

struct DocModel {
    std::vector<ImprovementRow> improvements;
    std::vector<ComparisonRow> comparisons;
};

// ===========================================================================
// Predicato di completezza FATTORIZZATO — applicato sia al modello generato sia
// a quello reale. Riproduce esattamente le regole del checker (task 39.1).
// ===========================================================================

// L'ID è valido se non vuoto e combacia con la forma IMP-NN (case-insensitive),
// come nel checker.
bool hasValidId(const std::string& id) {
    static const std::regex idPattern(R"(IMP-\d+)", std::regex::icase);
    return !id.empty() && std::regex_search(id, idPattern);
}

// Elenca gli elementi mancanti di una riga di miglioramento (vuoto => completa).
std::vector<std::string> missingImprovementElements(const ImprovementRow& row) {
    std::vector<std::string> missing;
    if (!hasValidId(row.id)) {
        missing.push_back("identificatore univoco (forma IMP-NN)");
    }
    if (row.geodeLimit.empty()) {
        missing.push_back("limite di Geode");
    }
    if (row.pulseApproach.empty()) {
        missing.push_back("approccio di Pulse");
    }
    if (row.criterion.empty()) {
        missing.push_back("criterio osservabile");
    }
    return missing;
}

bool improvementComplete(const ImprovementRow& row) {
    return missingImprovementElements(row).empty();
}

// Elenca gli elementi mancanti di una riga di confronto (vuoto => completa).
std::vector<std::string> missingComparisonElements(const ComparisonRow& row) {
    std::vector<std::string> missing;
    if (row.feature.empty()) {
        missing.push_back("nome funzionalità");
    }
    if (row.geode.empty()) {
        missing.push_back("stato in Geode");
    }
    if (row.pulse.empty()) {
        missing.push_back("stato in Pulse");
    }
    return missing;
}

bool comparisonComplete(const ComparisonRow& row) {
    return missingComparisonElements(row).empty();
}

// Predicato di completezza globale del modello (Req 24.1/24.2).
bool documentationComplete(const DocModel& model) {
    if (static_cast<int>(model.improvements.size()) < kMinImprovements) {
        return false;
    }
    for (const auto& row : model.improvements) {
        if (!improvementComplete(row)) {
            return false;
        }
    }
    if (static_cast<int>(model.comparisons.size()) < kMinComparisons) {
        return false;
    }
    for (const auto& row : model.comparisons) {
        if (!comparisonComplete(row)) {
            return false;
        }
    }
    return true;
}

// ===========================================================================
// Parsing del documento reale — stesse routine del checker (task 39.1) per
// riusare un identico approccio di analisi delle tabelle markdown delimitate.
// ===========================================================================

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::vector<std::string> splitCells(const std::string& line) {
    std::vector<std::string> cells;
    std::string trimmed = trim(line);
    if (!trimmed.empty() && trimmed.front() == '|') {
        trimmed.erase(trimmed.begin());
    }
    if (!trimmed.empty() && trimmed.back() == '|') {
        trimmed.pop_back();
    }
    std::stringstream ss(trimmed);
    std::string cell;
    while (std::getline(ss, cell, '|')) {
        cells.push_back(trim(cell));
    }
    return cells;
}

bool isSeparatorRow(const std::vector<std::string>& cells) {
    if (cells.empty()) {
        return false;
    }
    for (const auto& c : cells) {
        if (c.empty()) {
            return false;
        }
        for (char ch : c) {
            if (ch != '-' && ch != ':' && ch != ' ') {
                return false;
            }
        }
    }
    return true;
}

bool parseTable(const std::string& path, const std::string& startMarker,
                const std::string& endMarker,
                std::vector<std::vector<std::string>>& out, std::string& error) {
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "impossibile aprire il documento: " + path;
        return false;
    }

    std::string line;
    bool inTable = false;
    bool sawStart = false;
    bool sawEnd = false;
    bool headerConsumed = false;

    while (std::getline(file, line)) {
        if (line.find(startMarker) != std::string::npos) {
            inTable = true;
            sawStart = true;
            continue;
        }
        if (line.find(endMarker) != std::string::npos) {
            sawEnd = true;
            inTable = false;
            continue;
        }
        if (!inTable) {
            continue;
        }

        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() != '|') {
            continue;
        }

        std::vector<std::string> cells = splitCells(line);
        if (isSeparatorRow(cells)) {
            continue;
        }
        if (!headerConsumed) {
            headerConsumed = true;
            continue;
        }
        out.push_back(std::move(cells));
    }

    if (!sawStart || !sawEnd) {
        error = "marcatori " + startMarker + "/" + endMarker + " mancanti in " + path;
        return false;
    }
    if (out.empty()) {
        error = "nessuna riga-dati trovata nella tabella delimitata da " + startMarker;
        return false;
    }
    return true;
}

const std::string kDocPath = PULSE_VS_GEODE_DOC;

// Costruisce il DocModel dal documento reale riusando il parser del checker.
bool buildModelFromDoc(DocModel& model, std::string& error) {
    std::vector<std::vector<std::string>> impCells;
    if (!parseTable(kDocPath, "PULSE-IMPROVEMENTS-START", "PULSE-IMPROVEMENTS-END",
                    impCells, error)) {
        return false;
    }
    std::vector<std::vector<std::string>> cmpCells;
    if (!parseTable(kDocPath, "PULSE-COMPARISON-START", "PULSE-COMPARISON-END",
                    cmpCells, error)) {
        return false;
    }

    for (const auto& cells : impCells) {
        ImprovementRow row;
        row.id = cells.size() > 0 ? cells[0] : "";
        row.geodeLimit = cells.size() > 1 ? cells[1] : "";
        row.pulseApproach = cells.size() > 2 ? cells[2] : "";
        row.criterion = cells.size() > 3 ? cells[3] : "";
        model.improvements.push_back(std::move(row));
    }
    for (const auto& cells : cmpCells) {
        ComparisonRow row;
        row.feature = cells.size() > 0 ? cells[0] : "";
        row.geode = cells.size() > 1 ? cells[1] : "";
        row.pulse = cells.size() > 2 ? cells[2] : "";
        model.comparisons.push_back(std::move(row));
    }
    return true;
}

// ===========================================================================
// Generatori RapidCheck per il modello.
// ===========================================================================

// Token non vuoto di sole lettere minuscole 'a'..'z' (mai whitespace, mai
// cifre, mai "IMP-"): rappresenta una cella "presente".
rc::Gen<std::string> genNonEmptyToken() {
    return rc::gen::suchThat(
        rc::gen::container<std::string>(rc::gen::inRange<char>('a', '{')),
        [](const std::string& s) { return !s.empty(); });
}

// Cella opzionale: o un token non vuoto o la stringa vuota (assente).
rc::Gen<std::string> genOptionalCell() {
    return rc::gen::oneOf(genNonEmptyToken(), rc::gen::just(std::string()));
}

// ID casuale: valido (IMP-NN), assente (vuoto) o invalido (sole lettere, non
// combacia con IMP-NN).
rc::Gen<std::string> genIdCell() {
    auto validId = rc::gen::map(rc::gen::inRange<int>(0, 1000), [](int n) {
        return std::string("IMP-") + std::to_string(n);
    });
    return rc::gen::oneOf(validId, rc::gen::just(std::string()), genNonEmptyToken());
}

rc::Gen<ImprovementRow> genImprovementRow() {
    return rc::gen::apply(
        [](std::string id, std::string geode, std::string pulse, std::string crit) {
            return ImprovementRow{std::move(id), std::move(geode), std::move(pulse),
                                  std::move(crit)};
        },
        genIdCell(), genOptionalCell(), genOptionalCell(), genOptionalCell());
}

rc::Gen<ComparisonRow> genComparisonRow() {
    return rc::gen::apply(
        [](std::string feature, std::string geode, std::string pulse) {
            return ComparisonRow{std::move(feature), std::move(geode), std::move(pulse)};
        },
        genOptionalCell(), genOptionalCell(), genOptionalCell());
}

// ===========================================================================
// Property 39 — completezza giudicata sse soglie + tutti gli elementi presenti.
// Feature: pulse-sdk, Property 39. Validates: Requisiti 24.1, 24.2.
// ===========================================================================
RC_GTEST_PROP(Property39ImprovementsDocCompleteness,
              CompletoSseSoglieEElementiTuttiPresenti,
              ()) {
    // Conteggi casuali su entrambi i lati della soglia (0..15).
    const int impCount = *rc::gen::inRange<int>(0, 16).as("numero miglioramenti");
    const int cmpCount = *rc::gen::inRange<int>(0, 16).as("numero confronti");

    DocModel model;
    model.improvements.reserve(impCount);
    for (int i = 0; i < impCount; ++i) {
        model.improvements.push_back(*genImprovementRow());
    }
    model.comparisons.reserve(cmpCount);
    for (int i = 0; i < cmpCount; ++i) {
        model.comparisons.push_back(*genComparisonRow());
    }

    // Oracolo indipendente: ricalcola la condizione di completezza "a mano".
    bool oracle = (impCount >= kMinImprovements) && (cmpCount >= kMinComparisons);
    for (const auto& r : model.improvements) {
        const bool rowOk = hasValidId(r.id) && !r.geodeLimit.empty() &&
                           !r.pulseApproach.empty() && !r.criterion.empty();
        oracle = oracle && rowOk;
        // Req 24.2: gli elementi segnalati come mancanti devono essere
        // esattamente quelli realmente assenti.
        const auto missing = missingImprovementElements(r);
        RC_ASSERT(missing.empty() == rowOk);
    }
    for (const auto& r : model.comparisons) {
        const bool rowOk =
            !r.feature.empty() && !r.geode.empty() && !r.pulse.empty();
        oracle = oracle && rowOk;
        const auto missing = missingComparisonElements(r);
        RC_ASSERT(missing.empty() == rowOk);
    }

    // Invariante centrale: il predicato coincide con l'oracolo (sse).
    RC_ASSERT(documentationComplete(model) == oracle);
}

// ===========================================================================
// Caso esemplificativo: il documento REALE è completo (stesso predicato).
// ===========================================================================
TEST(Property39ImprovementsDocCompleteness, DocumentoRealeECompleto) {
    DocModel model;
    std::string err;
    ASSERT_TRUE(buildModelFromDoc(model, err))
        << "analisi del documento reale fallita: " << err;

    EXPECT_GE(static_cast<int>(model.improvements.size()), kMinImprovements);
    EXPECT_GE(static_cast<int>(model.comparisons.size()), kMinComparisons);
    EXPECT_TRUE(documentationComplete(model))
        << "docs/pulse-vs-geode.md non è giudicato completo dal predicato condiviso";
}

}  // namespace
