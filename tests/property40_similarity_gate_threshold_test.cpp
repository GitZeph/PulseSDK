// =============================================================================
// Pulse — Property test P40: soglia del gate di similarità (task 39.4)
//
// Feature: pulse-sdk, Property 40
// Validates: Requisiti 27.3, 27.4
//
// Proprietà 40 (design.md): "Per ogni file sorgente con una percentuale di
// similarità calcolata rispetto a Geode, la build candidata al rilascio è
// bloccata SE E SOLO SE la similarità è >= 30%, e in tal caso il messaggio
// identifica il file e la percentuale rilevata senza modificare i sorgenti."
//
// Strategia (RapidCheck, >= 100 iterazioni di default):
//   - Invariante della soglia (Req 27.3/27.4): per percentuali arbitrarie su
//     tutto l'intervallo [0, 100] — inclusi valori a cavallo del confine
//     29.99 / 30.0 / 30.01 — il file è bloccato dalla similarità SE E SOLO SE
//     pct >= 30.0; quando bloccato, il messaggio riporta il percorso del file
//     e la percentuale rilevata.
//   - Regola delle >= 8 righe identiche consecutive (Req 27.4 in combinato con
//     27.1): generando lunghezze di run intorno al confine (7 / 8 / 9) il flag
//     hasForbiddenIdenticalBlock (e quindi il blocco) scatta SE E SOLO SE la
//     run >= 8.
//   - Immutabilità: il gate è di sola lettura e non muta i propri input.
//
// Il gate è guidabile con input arbitrari: si usano sia la regola di decisione
// esplicita `isBlockedBySimilarity(pct)` sia il calcolo end-to-end su contenuti
// sintetici a similarità controllata e su run identiche di lunghezza scelta.
// =============================================================================
#include "tools/similarity_gate.hpp"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <string>
#include <vector>

namespace {

using namespace pulse::quality;

// Costruisce un sorgente sintetico di `count` righe distinte con un prefisso.
std::string makeLines(const std::string& prefix, int count) {
    std::string out;
    for (int i = 0; i < count; ++i) {
        out += prefix + std::to_string(i) + "\n";
    }
    return out;
}

// ---------------------------------------------------------------------------
// Req 27.3/27.4 — invariante della soglia sulla regola di decisione esplicita.
// Per percentuali arbitrarie in [0, 100] (inclusi i confini critici intorno a
// 30), isBlockedBySimilarity è true SSE pct >= 30.0.
// ---------------------------------------------------------------------------
RC_GTEST_PROP(Property40, BloccoSseSimilaritaSopraSoglia,
              (double rawPercent)) {
    // Normalizza in [0, 100] mantenendo la possibilità di colpire i confini.
    double pct = rawPercent;
    if (pct < 0.0) {
        pct = -pct;
    }
    // Riporta valori grandi nell'intervallo utile preservandone la frazione.
    pct = pct - static_cast<double>(static_cast<long long>(pct / 100.0)) * 100.0;

    const bool blocked = isBlockedBySimilarity(pct);
    const bool expected = (pct >= kSimilarityBlockThresholdPercent);
    RC_ASSERT(blocked == expected);
}

// ---------------------------------------------------------------------------
// Req 27.3/27.4 — confini critici espliciti: campiona valori a cavallo della
// soglia (29.99 / 30.0 / 30.01 e dintorni) tramite un piccolo delta generato.
// ---------------------------------------------------------------------------
RC_GTEST_PROP(Property40, ConfineDellaSogliaPrecisato, ()) {
    // delta in centesimi nell'intervallo [-100, +100] cent => [-1.0, +1.0].
    const int deltaCenti = *rc::gen::inRange(-100, 101);
    const double pct = kSimilarityBlockThresholdPercent +
                       static_cast<double>(deltaCenti) / 100.0;

    const bool blocked = isBlockedBySimilarity(pct);
    if (deltaCenti < 0) {
        RC_ASSERT(!blocked);  // strettamente sotto soglia => non bloccato
    } else {
        RC_ASSERT(blocked);   // >= 30.0 (delta >= 0) => bloccato
    }
}

// ---------------------------------------------------------------------------
// Req 27.3/27.4 — calcolo end-to-end su contenuti a similarità controllata.
// Costruiamo candidato e riferimento con `shared` righe in comune e `unique`
// righe esclusive ciascuno: l'indice di Jaccard è shared/(shared + 2*unique).
// Verifichiamo l'invariante blocked-by-similarity SSE percentuale >= 30 e che
// il messaggio identifichi file + percentuale quando bloccato.
// ---------------------------------------------------------------------------
RC_GTEST_PROP(Property40, CalcoloEndToEndRispettaSogliaEMessaggio,
              (std::string path)) {
    if (path.empty()) {
        path = "sdk/case.cpp";
    }
    const int shared = *rc::gen::inRange(0, 40);
    const int uniquePerSide = *rc::gen::inRange(0, 40);

    const std::string sharedBlock = makeLines("shared_", shared);
    CandidateFile file;
    file.path = path;
    file.content = sharedBlock + makeLines("cand_only_", uniquePerSide);
    file.referenceContent = sharedBlock + makeLines("ref_only_", uniquePerSide);

    const auto result = evaluateFile(file);

    // La decisione di blocco da similarità deve coincidere con la regola di
    // soglia applicata alla percentuale effettivamente calcolata.
    const bool blockedBySimilarity =
        isBlockedBySimilarity(result.similarityPercent);
    RC_ASSERT(blockedBySimilarity ==
              (result.similarityPercent >= kSimilarityBlockThresholdPercent));

    // Quando la similarità blocca, il messaggio identifica file e percentuale.
    if (blockedBySimilarity) {
        RC_ASSERT(result.blocked);
        const std::string msg = result.message();
        RC_ASSERT(msg.find(file.path) != std::string::npos);
        RC_ASSERT(msg.find('%') != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Req 27.4 (in combinato con 27.1) — regola delle >= 8 righe identiche
// consecutive. Generando lunghezze di run intorno al confine (7 / 8 / 9 e
// dintorni), immerse in molto contenuto disgiunto perché la similarità resti
// sotto soglia, il flag hasForbiddenIdenticalBlock scatta SSE run >= 8.
// ---------------------------------------------------------------------------
RC_GTEST_PROP(Property40, BloccoIdenticoSseRunMaggioreUgualeOtto, ()) {
    const int runLen = *rc::gen::inRange(0, 20);  // include 7 / 8 / 9

    std::string identical;
    for (int i = 0; i < runLen; ++i) {
        identical += "identical_run_line_" + std::to_string(i) + "\n";
    }

    CandidateFile file;
    file.path = "sdk/run.cpp";
    // Contesto disgiunto abbondante: isola la run identica dalla soglia di
    // similarità complessiva.
    file.content = makeLines("cand_disjoint_", 80) + identical;
    file.referenceContent = makeLines("ref_disjoint_", 80) + identical;

    const auto result = evaluateFile(file);

    const bool expectedForbidden =
        (static_cast<std::size_t>(runLen) >= kMaxIdenticalConsecutiveLines);
    RC_ASSERT(result.hasForbiddenIdenticalBlock == expectedForbidden);
    RC_ASSERT(result.longestIdenticalRun >= static_cast<std::size_t>(runLen));

    if (expectedForbidden) {
        RC_ASSERT(result.blocked);  // run >= 8 blocca (Req 27.1/27.4)
    }
}

// ---------------------------------------------------------------------------
// Req 27.4 — il gate è di SOLA LETTURA: non muta i propri input, qualunque sia
// la percentuale/run generata.
// ---------------------------------------------------------------------------
RC_GTEST_PROP(Property40, GateNonMutaGliInput, (std::string path)) {
    if (path.empty()) {
        path = "sdk/immutable.cpp";
    }
    const int shared = *rc::gen::inRange(0, 30);
    const int uniquePerSide = *rc::gen::inRange(0, 30);

    const std::string sharedBlock = makeLines("shared_", shared);
    CandidateFile file;
    file.path = path;
    file.content = sharedBlock + makeLines("cand_only_", uniquePerSide);
    file.referenceContent = sharedBlock + makeLines("ref_only_", uniquePerSide);
    file.isDerived = *rc::gen::arbitrary<bool>();

    const std::string pathBefore = file.path;
    const std::string contentBefore = file.content;
    const std::string refBefore = file.referenceContent;
    const std::string markerBefore = file.attributionMarker;
    const bool derivedBefore = file.isDerived;

    (void)evaluateFile(file);

    RC_ASSERT(file.path == pathBefore);
    RC_ASSERT(file.content == contentBefore);
    RC_ASSERT(file.referenceContent == refBefore);
    RC_ASSERT(file.attributionMarker == markerBefore);
    RC_ASSERT(file.isDerived == derivedBefore);
}

}  // namespace
