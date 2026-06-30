// tests/geode_parity_checker_test.cpp — checker della checklist di parità con
// Geode (task 33.1, Req 25.3).
//
// Analizza `docs/geode-parity.md` e FALLISCE se la mappatura di parità non è
// completa, ovvero se:
//   * una qualunque capability ATTESA è assente dalla tabella;
//   * una riga della tabella è priva del criterio di accettazione osservabile;
//   * una riga della tabella è priva del requisito Pulse mappato (forma `Req N`).
//
// La tabella è delimitata nel documento dai marcatori machine-readable
// `PARITY-TABLE-START` / `PARITY-TABLE-END`. Il percorso del documento è
// iniettato a compile-time tramite la macro PULSE_GEODE_PARITY_DOC (definita
// in tests/CMakeLists.txt) così il test è eseguibile da qualunque CWD.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef PULSE_GEODE_PARITY_DOC
#error "PULSE_GEODE_PARITY_DOC deve essere definita (percorso di docs/geode-parity.md)"
#endif

namespace {

// Capability di Geode attese per la parità (Req 25.3). Il confronto è
// case-insensitive sulla prima colonna della tabella.
const std::vector<std::string>& expectedCapabilities() {
    static const std::vector<std::string> kCaps = {
        "hooking",
        "field injection",
        "eventi",
        "UI/layout & node-ID",
        "settings",
        "storage",
        "async/task",
        "HTTP",
        "mod manager",
        "marketplace/signing",
        "multi-platform",
        "scripting",
    };
    return kCaps;
}

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Una riga della checklist: capability + criterio osservabile + requisito Pulse.
struct ParityRow {
    std::string capability;
    std::string criterion;
    std::string requirement;
};

// Suddivide una riga di tabella markdown `| a | b | c |` nelle sue celle,
// scartando i delimitatori esterni vuoti.
std::vector<std::string> splitCells(const std::string& line) {
    std::vector<std::string> cells;
    std::string trimmed = trim(line);
    // Rimuove il pipe iniziale/finale per non generare celle vuote spurie.
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

// Verifica se una cella è una riga separatrice di header markdown (es. |---|---|).
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

// Carica e analizza le righe-dati della tabella delimitata dai marcatori.
// Salta header e riga separatrice. Restituisce false se i marcatori o la
// tabella mancano.
bool parseParityRows(const std::string& path, std::vector<ParityRow>& out,
                     std::string& error) {
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
        if (line.find("PARITY-TABLE-START") != std::string::npos) {
            inTable = true;
            sawStart = true;
            continue;
        }
        if (line.find("PARITY-TABLE-END") != std::string::npos) {
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
            // La prima riga di tabella è l'header: lo consumiamo.
            headerConsumed = true;
            continue;
        }

        ParityRow row;
        row.capability = cells.size() > 0 ? cells[0] : "";
        row.criterion = cells.size() > 1 ? cells[1] : "";
        row.requirement = cells.size() > 2 ? cells[2] : "";
        out.push_back(row);
    }

    if (!sawStart || !sawEnd) {
        error = "marcatori PARITY-TABLE-START/END mancanti in " + path;
        return false;
    }
    if (out.empty()) {
        error = "nessuna riga-dati trovata nella tabella di parità";
        return false;
    }
    return true;
}

const std::string kDocPath = PULSE_GEODE_PARITY_DOC;

// Carica le righe una sola volta per il binario di test.
const std::vector<ParityRow>& parityRows() {
    static std::vector<ParityRow> rows;
    static bool loaded = false;
    static std::string err;
    if (!loaded) {
        loaded = true;
        if (!parseParityRows(kDocPath, rows, err)) {
            ADD_FAILURE() << "Parsing di geode-parity.md fallito: " << err;
        }
    }
    return rows;
}

// --- Ogni riga deve avere criterio osservabile e requisito Pulse mappato. ---

TEST(GeodeParityChecker, OgniRigaHaCriterioERequisito) {
    const std::regex reqPattern(R"(Req\s*\d+)", std::regex::icase);
    for (const auto& row : parityRows()) {
        EXPECT_FALSE(row.capability.empty())
            << "riga senza capability nella checklist di parità";
        EXPECT_FALSE(row.criterion.empty())
            << "capability '" << row.capability << "' priva del criterio osservabile";
        EXPECT_FALSE(row.requirement.empty())
            << "capability '" << row.capability << "' priva del requisito Pulse mappato";
        EXPECT_TRUE(std::regex_search(row.requirement, reqPattern))
            << "capability '" << row.capability << "' ha un requisito Pulse non valido: '"
            << row.requirement << "' (atteso formato 'Req N')";
    }
}

// --- Tutte le capability attese devono essere presenti (mappatura completa). ---

TEST(GeodeParityChecker, TutteLeCapabilityAtteseSonoPresenti) {
    const auto& rows = parityRows();
    for (const auto& expected : expectedCapabilities()) {
        const std::string expectedLower = toLower(expected);
        const bool present = std::any_of(
            rows.begin(), rows.end(), [&](const ParityRow& r) {
                return toLower(r.capability) == expectedLower;
            });
        EXPECT_TRUE(present)
            << "capability attesa assente dalla checklist di parità: '" << expected << "'";
    }
}

}  // namespace
