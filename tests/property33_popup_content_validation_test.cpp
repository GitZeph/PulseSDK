// tests/property33_popup_content_validation_test.cpp
// Feature: pulse-sdk, Property 33 — Validazione del contenuto dei popup.
// Validates: Requirements 13.4 (Requisito 13.4)
//
// Property 33 (design.md / Req 13.4): per ogni coppia (titolo, corpo), la
// visualizzazione del popup è consentita SE E SOLO SE il titolo è non vuoto E
// di lunghezza ≤ 100 (kMaxPopupTitleLength) E il corpo è di lunghezza ≤ 1000
// (kMaxPopupBodyLength); altrimenti la richiesta è rifiutata con l'errore
// specifico e — per showPopup — il popup NON viene mai mostrato.
//
// Oracolo (rispecchia la precedenza effettiva in validatePopup):
//   ok == (!title.empty() && title.size()<=100 && body.size()<=1000)
//   quando rifiutato, il code segue la PRIMA regola violata:
//     titolo vuoto          -> EmptyTitle
//     altrimenti titolo>100 -> TitleTooLong
//     altrimenti corpo>1000 -> BodyTooLong
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si generano titolo e corpo con lunghezze campionate ATTORNO ai confini
//     (titolo: 0,1,99,100,101 + arbitrario; corpo: 0,999,1000,1001 +
//     arbitrario), così i casi limite below/at/above i limiti sono coperti di
//     frequente, oltre a lunghezze arbitrarie;
//   * si confronta validatePopup contro l'oracolo (ok + code);
//   * si invoca showPopup con un IPopupPresenter "registratore" e si verifica
//     che un popup rifiutato non venga MAI presentato e uno accettato venga
//     presentato ESATTAMENTE una volta, con titolo/corpo esatti.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "pulse/log.hpp"

namespace {

using pulse::IPopupPresenter;
using pulse::PopupContent;
using pulse::PopupErrorCode;
using pulse::PopupResult;
using pulse::kMaxPopupBodyLength;
using pulse::kMaxPopupTitleLength;
using pulse::showPopup;
using pulse::validatePopup;

// IPopupPresenter "registratore": cattura ogni present() per poter asserire
// quante volte (e con quale contenuto) il popup è stato mostrato.
class RecordingPopupPresenter final : public IPopupPresenter {
public:
    void present(const PopupContent& content) override {
        presented_.push_back(content);
    }

    [[nodiscard]] std::size_t count() const noexcept { return presented_.size(); }
    [[nodiscard]] const std::vector<PopupContent>& presented() const noexcept {
        return presented_;
    }

private:
    std::vector<PopupContent> presented_;
};

// Oracolo della precedenza degli errori — rispecchia validatePopup (Req 13.4).
PopupErrorCode expectedCode(const std::string& title, const std::string& body) {
    if (title.empty()) {
        return PopupErrorCode::EmptyTitle;
    }
    if (title.size() > kMaxPopupTitleLength) {
        return PopupErrorCode::TitleTooLong;
    }
    if (body.size() > kMaxPopupBodyLength) {
        return PopupErrorCode::BodyTooLong;
    }
    return PopupErrorCode::None;
}

// Generatore di una stringa di lunghezza esatta `len` con byte arbitrari
// (binary-safe: può contenere NUL incorporati). La lunghezza, non il contenuto,
// è ciò che governa la validazione.
rc::Gen<std::string> genStringOfLength(std::size_t len) {
    return rc::gen::container<std::string>(len, rc::gen::arbitrary<char>());
}

// Genera una stringa la cui lunghezza è campionata attorno ai confini indicati
// (più una coda di lunghezze arbitrarie), per esercitare i casi below/at/above.
rc::Gen<std::string> genAroundBoundaries(const std::vector<std::size_t>& boundaries) {
    return rc::gen::mapcat(
        rc::gen::weightedOneOf<std::size_t>({
            // Peso maggiore alle lunghezze di confine.
            {6, rc::gen::elementOf(boundaries)},
            // Coda: lunghezze arbitrarie ma contenute, per non esplodere.
            {1, rc::gen::inRange<std::size_t>(0, 1200)},
        }),
        [](std::size_t len) { return genStringOfLength(len); });
}

// --- Property 33 — la validazione del popup vale sse e solo se l'oracolo -----
// Feature: pulse-sdk, Property 33. Validates: Requirements 13.4.
RC_GTEST_PROP(Property33PopupContentValidation,
              AcceptsIffTitleNonEmptyAndWithinLimits,
              ()) {
    // Confini del titolo: 0 (vuoto), 1 (minimo valido), 99/100 (al limite),
    // 101 (oltre). Confini del corpo: 0, 999/1000 (al limite), 1001 (oltre).
    const std::string title =
        *genAroundBoundaries({0, 1, 99, 100, 101}).as("titolo");
    const std::string body =
        *genAroundBoundaries({0, 999, 1000, 1001}).as("corpo");

    const bool oracleOk = !title.empty() &&
                          title.size() <= kMaxPopupTitleLength &&
                          body.size() <= kMaxPopupBodyLength;
    const PopupErrorCode oracleCode = expectedCode(title, body);

    // (1) validatePopup deve concordare con l'oracolo su ok e code.
    const PopupResult validation = validatePopup(title, body);
    RC_ASSERT(validation.ok == oracleOk);
    RC_ASSERT(validation.code == oracleCode);
    if (oracleOk) {
        RC_ASSERT(validation.code == PopupErrorCode::None);
    } else {
        RC_ASSERT(validation.code != PopupErrorCode::None);
    }

    // (2) showPopup deve restituire lo stesso esito di validatePopup e
    // presentare il popup ESATTAMENTE una volta sse accettato, mai se rifiutato.
    RecordingPopupPresenter presenter;
    const PopupResult shown = showPopup(title, body, presenter);
    RC_ASSERT(shown.ok == oracleOk);
    RC_ASSERT(shown.code == oracleCode);

    if (oracleOk) {
        // Accettato: presentato una sola volta, con titolo/corpo esatti.
        RC_ASSERT(presenter.count() == std::size_t{1});
        RC_ASSERT(presenter.presented().front().title == title);
        RC_ASSERT(presenter.presented().front().body == body);
    } else {
        // Rifiutato: il presenter non è MAI invocato (Req 13.4).
        RC_ASSERT(presenter.count() == std::size_t{0});
    }
}

}  // namespace
