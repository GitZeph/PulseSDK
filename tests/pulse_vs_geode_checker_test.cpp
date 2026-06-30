// tests/pulse_vs_geode_checker_test.cpp — checker di completezza della
// documentazione dei vantaggi competitivi Pulse-vs-Geode (task 39.1,
// Req 24.1/24.2/24.3/24.4).
//
// Analizza `docs/pulse-vs-geode.md` e FALLISCE se la documentazione non è
// completa, ovvero se:
//   * la tabella dei MIGLIORAMENTI ha meno di 10 righe (Req 24.1);
//   * una riga dei miglioramenti è priva di uno qualsiasi degli elementi
//     obbligatori — ID, limite di Geode, approccio di Pulse, criterio
//     osservabile — indicando nel messaggio quali mancano (Req 24.2);
//   * la tabella di CONFRONTO ha meno di 10 funzionalità (Req 24.3);
//   * una riga di confronto è priva del nome funzionalità, dello stato in
//     Geode o dello stato in Pulse (Req 24.3);
//   * la versione di Geode di riferimento è assente dal documento (Req 24.4).
//
// Le tabelle sono delimitate nel documento dai marcatori machine-readable
// `PULSE-IMPROVEMENTS-START` / `PULSE-IMPROVEMENTS-END` e
// `PULSE-COMPARISON-START` / `PULSE-COMPARISON-END`. Il percorso del documento
// è iniettato a compile-time tramite la macro PULSE_VS_GEODE_DOC (definita in
// tests/CMakeLists.txt) così il test è eseguibile da qualunque CWD.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef PULSE_VS_GEODE_DOC
#error "PULSE_VS_GEODE_DOC deve essere definita (percorso di docs/pulse-vs-geode.md)"
#endif

namespace {

constexpr int kMinImprovements = 10;
constexpr int kMinComparisons = 10;

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// Suddivide una riga di tabella markdown `| a | b | c |` nelle sue celle,
// scartando i delimitatori esterni vuoti.
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

// Verifica se una riga è il separatore di header markdown (es. |---|---|).
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

// Carica le righe-dati della tabella delimitata dai marcatori indicati.
// Salta header e riga separatrice. Restituisce false se i marcatori o la
// tabella mancano.
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
            continue;  // riga non-tabella tra i marcatori
        }

        std::vector<std::string> cells = splitCells(line);
        if (isSeparatorRow(cells)) {
            continue;
        }
        if (!headerConsumed) {
            headerConsumed = true;  // la prima riga è l'header: la consumiamo
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

// --- Caricamento una sola volta delle due tabelle. ---

const std::vector<std::vector<std::string>>& improvementRows() {
    static std::vector<std::vector<std::string>> rows;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::string err;
        if (!parseTable(kDocPath, "PULSE-IMPROVEMENTS-START", "PULSE-IMPROVEMENTS-END",
                        rows, err)) {
            ADD_FAILURE() << "Parsing della tabella dei miglioramenti fallito: " << err;
        }
    }
    return rows;
}

const std::vector<std::vector<std::string>>& comparisonRows() {
    static std::vector<std::vector<std::string>> rows;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::string err;
        if (!parseTable(kDocPath, "PULSE-COMPARISON-START", "PULSE-COMPARISON-END",
                        rows, err)) {
            ADD_FAILURE() << "Parsing della tabella di confronto fallito: " << err;
        }
    }
    return rows;
}

// --- Req 24.1: almeno 10 miglioramenti documentati. ---

TEST(PulseVsGeodeChecker, AlmenoDieciMiglioramenti) {
    EXPECT_GE(static_cast<int>(improvementRows().size()), kMinImprovements)
        << "la tabella dei miglioramenti deve contenere almeno " << kMinImprovements
        << " righe (Req 24.1); trovate " << improvementRows().size();
}

// --- Req 24.1/24.2: ogni miglioramento ha tutti gli elementi obbligatori. ---

TEST(PulseVsGeodeChecker, OgniMiglioramentoHaTuttiGliElementi) {
    const std::regex idPattern(R"(IMP-\d+)", std::regex::icase);
    int index = 0;
    for (const auto& row : improvementRows()) {
        ++index;
        const std::string id = row.size() > 0 ? row[0] : "";
        const std::string geodeLimit = row.size() > 1 ? row[1] : "";
        const std::string pulseApproach = row.size() > 2 ? row[2] : "";
        const std::string criterion = row.size() > 3 ? row[3] : "";

        // Elenca esplicitamente gli elementi mancanti nel messaggio (Req 24.2).
        std::vector<std::string> missing;
        if (id.empty() || !std::regex_search(id, idPattern)) {
            missing.push_back("identificatore univoco (forma IMP-NN)");
        }
        if (geodeLimit.empty()) {
            missing.push_back("limite di Geode");
        }
        if (pulseApproach.empty()) {
            missing.push_back("approccio di Pulse");
        }
        if (criterion.empty()) {
            missing.push_back("criterio osservabile");
        }

        std::string missingList;
        for (size_t i = 0; i < missing.size(); ++i) {
            missingList += (i ? ", " : "") + missing[i];
        }

        EXPECT_TRUE(missing.empty())
            << "miglioramento incompleto (riga " << index << ", ID='"
            << (id.empty() ? "<vuoto>" : id) << "'): elementi mancanti -> " << missingList;
    }
}

// --- Req 24.3: almeno 10 funzionalità nella tabella di confronto. ---

TEST(PulseVsGeodeChecker, AlmenoDieciFunzionalitaDiConfronto) {
    EXPECT_GE(static_cast<int>(comparisonRows().size()), kMinComparisons)
        << "la tabella di confronto deve contenere almeno " << kMinComparisons
        << " funzionalità (Req 24.3); trovate " << comparisonRows().size();
}

// --- Req 24.3: ogni riga di confronto ha nome + stato Geode + stato Pulse. ---

TEST(PulseVsGeodeChecker, OgniRigaDiConfrontoHaTuttiGliStati) {
    int index = 0;
    for (const auto& row : comparisonRows()) {
        ++index;
        const std::string feature = row.size() > 0 ? row[0] : "";
        const std::string geode = row.size() > 1 ? row[1] : "";
        const std::string pulse = row.size() > 2 ? row[2] : "";

        std::vector<std::string> missing;
        if (feature.empty()) {
            missing.push_back("nome funzionalità");
        }
        if (geode.empty()) {
            missing.push_back("stato in Geode");
        }
        if (pulse.empty()) {
            missing.push_back("stato in Pulse");
        }

        std::string missingList;
        for (size_t i = 0; i < missing.size(); ++i) {
            missingList += (i ? ", " : "") + missing[i];
        }

        EXPECT_TRUE(missing.empty())
            << "riga di confronto incompleta (riga " << index << ", funzionalità='"
            << (feature.empty() ? "<vuoto>" : feature) << "'): elementi mancanti -> "
            << missingList;
    }
}

// --- Req 24.4: versione di Geode di riferimento presente nel documento. ---

TEST(PulseVsGeodeChecker, VersioneGeodeDiRiferimentoPresente) {
    std::ifstream file(kDocPath);
    ASSERT_TRUE(file.is_open()) << "impossibile aprire il documento: " << kDocPath;

    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string content = buffer.str();

    // Cerca un riferimento esplicito alla versione di Geode (forma "Geode 4.x"
    // o simili), che identifica la baseline del confronto (Req 24.4).
    const std::regex versionPattern(R"(Geode\s*\d+(\.\w+)?)", std::regex::icase);
    EXPECT_TRUE(std::regex_search(content, versionPattern))
        << "versione di Geode di riferimento assente dal documento (Req 24.4); "
           "attesa una menzione esplicita del tipo 'Geode 4.x'";
}

}  // namespace
