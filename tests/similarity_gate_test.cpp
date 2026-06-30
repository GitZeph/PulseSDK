// =============================================================================
// Pulse — Unit test del gate di similarità del codice (task 39.3)
//
// Verifica i comportamenti osservabili di `tools/similarity_gate.hpp`
// (Requisiti 27.1, 27.2, 27.3, 27.4, 27.5 / Proprietà 40):
//   - similarità < 30%  => build NON bloccata;
//   - similarità >= 30% => build bloccata, messaggio con file + percentuale;
//   - blocco identico di >= 8 righe consecutive => segnalato e bloccato;
//   - frammento derivato senza attribuzione => segnalato; con attribuzione OK;
//   - il gate è di SOLA LETTURA: non muta gli input.
// =============================================================================
#include "tools/similarity_gate.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using namespace pulse::quality;

// Costruisce un sorgente sintetico di N righe distinte con un prefisso dato.
std::string makeLines(const std::string& prefix, int count) {
    std::string out;
    for (int i = 0; i < count; ++i) {
        out += prefix + std::to_string(i) + "\n";
    }
    return out;
}

// ---------------------------------------------------------------------------
// Req 27.4 / Proprietà 40: similarità sotto soglia => build NON bloccata.
// File completamente disgiunti hanno similarità 0%.
// ---------------------------------------------------------------------------
TEST(SimilarityGate, BelowThresholdPasses) {
    CandidateFile file;
    file.path = "sdk/original.cpp";
    file.content = makeLines("pulse_unique_line_", 20);
    file.referenceContent = makeLines("geode_unrelated_line_", 20);

    const auto result = evaluateFile(file);

    EXPECT_LT(result.similarityPercent, kSimilarityBlockThresholdPercent);
    EXPECT_FALSE(result.blocked);
    EXPECT_FALSE(result.hasForbiddenIdenticalBlock);
}

// ---------------------------------------------------------------------------
// Req 27.3/27.4 / Proprietà 40: similarità >= 30% => bloccata, con file e
// percentuale nel messaggio.
// ---------------------------------------------------------------------------
TEST(SimilarityGate, AtOrAboveThresholdBlocksWithFileAndPercent) {
    // 10 righe condivise + 0 esclusive da entrambi i lati => Jaccard 100%.
    const std::string shared = makeLines("shared_logic_", 10);
    CandidateFile file;
    file.path = "sdk/copied.cpp";
    file.content = shared;
    file.referenceContent = shared;

    const auto result = evaluateFile(file);

    EXPECT_GE(result.similarityPercent, kSimilarityBlockThresholdPercent);
    EXPECT_TRUE(result.blocked);
    // Il messaggio identifica file e percentuale (Req 27.4).
    const std::string msg = result.message();
    EXPECT_NE(msg.find("sdk/copied.cpp"), std::string::npos);
    EXPECT_NE(msg.find("%"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Confine esatto della soglia: la decisione precalcolata è "blocked sse >= 30".
// Guida la regola con percentuali arbitrarie (anticipa il property test 39.4).
// ---------------------------------------------------------------------------
TEST(SimilarityGate, ThresholdBoundaryDecision) {
    EXPECT_FALSE(isBlockedBySimilarity(0.0));
    EXPECT_FALSE(isBlockedBySimilarity(29.99));
    EXPECT_TRUE(isBlockedBySimilarity(30.0));   // confine incluso
    EXPECT_TRUE(isBlockedBySimilarity(30.01));
    EXPECT_TRUE(isBlockedBySimilarity(100.0));
}

// ---------------------------------------------------------------------------
// Req 27.1: blocco identico di >= 8 righe consecutive => segnalato e bloccato,
// anche se la similarità complessiva resta sotto soglia.
// ---------------------------------------------------------------------------
TEST(SimilarityGate, IdenticalEightLineBlockIsFlaggedAndBlocks) {
    // 8 righe identiche consecutive immerse in molto contenuto disgiunto, così
    // l'indice di Jaccard resta basso ma la run identica scatta il blocco.
    std::string identicalBlock;
    for (int i = 0; i < 8; ++i) {
        identicalBlock += "identical_block_line_" + std::to_string(i) + "\n";
    }

    CandidateFile file;
    file.path = "sdk/fragment.cpp";
    file.content = makeLines("cand_only_", 60) + identicalBlock;
    file.referenceContent = makeLines("ref_only_", 60) + identicalBlock;

    const auto result = evaluateFile(file);

    EXPECT_GE(result.longestIdenticalRun, kMaxIdenticalConsecutiveLines);
    EXPECT_TRUE(result.hasForbiddenIdenticalBlock);
    EXPECT_TRUE(result.blocked);
    EXPECT_LT(result.similarityPercent, kSimilarityBlockThresholdPercent)
        << "lo scenario isola il caso del blocco identico dalla soglia";
}

// ---------------------------------------------------------------------------
// Controprova: 7 righe identiche consecutive NON costituiscono violazione.
// ---------------------------------------------------------------------------
TEST(SimilarityGate, SevenIdenticalLinesDoNotTriggerBlock) {
    std::string block;
    for (int i = 0; i < 7; ++i) {
        block += "ident_" + std::to_string(i) + "\n";
    }
    CandidateFile file;
    file.path = "sdk/ok_fragment.cpp";
    file.content = makeLines("cand_", 60) + block;
    file.referenceContent = makeLines("ref_", 60) + block;

    const auto result = evaluateFile(file);

    EXPECT_LT(result.longestIdenticalRun, kMaxIdenticalConsecutiveLines);
    EXPECT_FALSE(result.hasForbiddenIdenticalBlock);
    EXPECT_FALSE(result.blocked);
}

// ---------------------------------------------------------------------------
// Req 27.2/27.5: frammento derivato senza attribuzione => segnalato/bloccato.
// ---------------------------------------------------------------------------
TEST(SimilarityGate, DerivedFragmentWithoutAttributionIsFlagged) {
    CandidateFile file;
    file.path = "sdk/derived.cpp";
    file.content = makeLines("derived_impl_", 20);  // nessun marcatore SPDX
    file.referenceContent = makeLines("geode_unrelated_", 20);
    file.isDerived = true;

    const auto result = evaluateFile(file);

    EXPECT_TRUE(result.isDerivedFragment);
    EXPECT_TRUE(result.missingAttribution);
    EXPECT_TRUE(result.blocked);
    EXPECT_NE(result.message().find("attribuzione"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Req 27.2/27.5: stesso frammento derivato CON attribuzione => passa (a parità
// di bassa similarità e nessun blocco identico).
// ---------------------------------------------------------------------------
TEST(SimilarityGate, DerivedFragmentWithAttributionPasses) {
    CandidateFile file;
    file.path = "sdk/derived_ok.cpp";
    file.content = "// SPDX-License-Identifier: MIT\n" +
                   makeLines("derived_impl_", 20);
    file.referenceContent = makeLines("geode_unrelated_", 20);
    file.isDerived = true;

    const auto result = evaluateFile(file);

    EXPECT_TRUE(result.isDerivedFragment);
    EXPECT_FALSE(result.missingAttribution);
    EXPECT_FALSE(result.blocked);
}

// ---------------------------------------------------------------------------
// Req 27.4: il gate è di SOLA LETTURA — non muta il contenuto degli input.
// ---------------------------------------------------------------------------
TEST(SimilarityGate, GateDoesNotMutateInputs) {
    CandidateFile file;
    file.path = "sdk/immutable.cpp";
    file.content = makeLines("line_", 30);
    file.referenceContent = makeLines("line_", 30);  // identici => bloccato
    file.isDerived = true;
    file.attributionMarker = "SPDX-License-Identifier";

    const std::string pathBefore = file.path;
    const std::string contentBefore = file.content;
    const std::string refBefore = file.referenceContent;
    const std::string markerBefore = file.attributionMarker;

    const auto result = evaluateFile(file);
    EXPECT_TRUE(result.blocked);  // scenario che esercita tutti i rami

    EXPECT_EQ(file.path, pathBefore);
    EXPECT_EQ(file.content, contentBefore);
    EXPECT_EQ(file.referenceContent, refBefore);
    EXPECT_EQ(file.attributionMarker, markerBefore);
}

// ---------------------------------------------------------------------------
// Req 27.3: report aggregato sull'intera build candidata — bloccata se almeno
// un file è bloccato; i file bloccati sono osservabili.
// ---------------------------------------------------------------------------
TEST(SimilarityGate, AggregateReportBlocksOnAnyFile) {
    const std::string shared = makeLines("dup_", 12);

    CandidateFile ok;
    ok.path = "sdk/clean.cpp";
    ok.content = makeLines("clean_a_", 20);
    ok.referenceContent = makeLines("clean_b_", 20);

    CandidateFile bad;
    bad.path = "sdk/dirty.cpp";
    bad.content = shared;
    bad.referenceContent = shared;

    const auto report = runGate({ok, bad});

    EXPECT_TRUE(report.buildBlocked());
    ASSERT_EQ(report.blockedFiles().size(), 1u);
    EXPECT_EQ(report.blockedFiles().front().filePath, "sdk/dirty.cpp");
    EXPECT_NE(report.summary().find("sdk/dirty.cpp"), std::string::npos);
}

}  // namespace
