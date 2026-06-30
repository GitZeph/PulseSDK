// tests/log_test.cpp — unit test del sistema di logging e degli helper popup
// (task 21.1, Req 13.1, 13.2, 13.3, 13.4, 13.5, 13.6).
//
// Verifica:
//   * Level è l'insieme chiuso {Debug, Info, Warning, Error}; un log valido è
//     registrato con livello + identità della Mod ed è recuperabile per la
//     sessione (Req 13.1, 13.5);
//   * un livello di severità fuori dall'insieme chiuso (codice grezzo) è
//     rifiutato con indicazione di errore e NON registrato (Req 13.2);
//   * se la destinazione di archiviazione non è disponibile, la registrazione
//     fallisce preservando l'esecuzione della Mod chiamante e restituendo
//     un'indicazione di errore (Req 13.6);
//   * gli helper popup accettano titolo non vuoto ≤ 100 e corpo ≤ 1000 e
//     rifiutano (senza mostrare) titolo vuoto o contenuti oltre i limiti
//     (Req 13.3, 13.4).
//
// Header-only: include solo l'header pubblico dello SDK <pulse/log.hpp>.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <pulse/log.hpp>

namespace {

using pulse::Level;
using pulse::LogErrorCode;
using pulse::Logger;
using pulse::LogRecord;
using pulse::PopupContent;
using pulse::PopupErrorCode;
using pulse::SessionLogStore;

// Presenter di popup di prova: registra i contenuti effettivamente mostrati,
// così i test possono verificare che un popup non valido NON venga mostrato.
class RecordingPopupPresenter final : public pulse::IPopupPresenter {
public:
    void present(const PopupContent& content) override {
        shown.push_back(content);
    }
    std::vector<PopupContent> shown;
};

// --- Req 13.1 / 13.5: log valido registrato con livello + identità mod ------

TEST(Logger, RecordsValidMessageWithLevelAndModIdentity) {
    SessionLogStore store;
    Logger logger("modA", store);

    const auto res = logger.log(Level::Info, "avvio completato");

    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.code, LogErrorCode::None);
    ASSERT_EQ(store.size(), 1u);
    EXPECT_EQ(store.records()[0].level, Level::Info);
    EXPECT_EQ(store.records()[0].mod, "modA");
    EXPECT_EQ(store.records()[0].message, "avvio completato");
}

TEST(Logger, AcceptsEveryLevelOfTheClosedSet) {
    SessionLogStore store;
    Logger logger("modA", store);

    EXPECT_TRUE(logger.log(Level::Debug, "d").ok);
    EXPECT_TRUE(logger.log(Level::Info, "i").ok);
    EXPECT_TRUE(logger.log(Level::Warning, "w").ok);
    EXPECT_TRUE(logger.log(Level::Error, "e").ok);
    EXPECT_EQ(store.size(), 4u);
}

// Req 13.5: i record restano recuperabili per l'intera sessione e sono
// filtrabili per identità della Mod emittente.
TEST(Logger, MessagesAreRetrievableForTheSessionPerMod) {
    SessionLogStore store;
    Logger a("modA", store);
    Logger b("modB", store);

    a.log(Level::Info, "a1");
    b.log(Level::Warning, "b1");
    a.log(Level::Error, "a2");

    EXPECT_EQ(store.size(), 3u);
    const auto fromA = store.recordsForMod("modA");
    ASSERT_EQ(fromA.size(), 2u);
    EXPECT_EQ(fromA[0].message, "a1");
    EXPECT_EQ(fromA[1].message, "a2");
    EXPECT_EQ(store.recordsForMod("modB").size(), 1u);
}

// --- Req 13.2: livello fuori insieme rifiutato senza registrare -------------

TEST(Logger, RejectsInvalidLevelCodeWithoutRecording) {
    SessionLogStore store;
    Logger logger("modA", store);

    for (const int invalid : {-1, 4, 99, -100}) {
        const auto res = logger.log(invalid, "messaggio");
        EXPECT_FALSE(res.ok) << "codice " << invalid;
        EXPECT_EQ(res.code, LogErrorCode::InvalidLevel) << "codice " << invalid;
        EXPECT_FALSE(res.message.empty());
    }
    // Nessun messaggio non valido deve essere stato registrato.
    EXPECT_EQ(store.size(), 0u);
}

TEST(Logger, AcceptsValidLevelCodes) {
    SessionLogStore store;
    Logger logger("modA", store);

    for (const int code : {0, 1, 2, 3}) {
        EXPECT_TRUE(logger.log(code, "ok").ok) << "codice " << code;
    }
    EXPECT_EQ(store.size(), 4u);
    EXPECT_EQ(store.records()[0].level, Level::Debug);
    EXPECT_EQ(store.records()[3].level, Level::Error);
}

TEST(Logger, LevelFromCodeMapsClosedSetOnly) {
    EXPECT_EQ(pulse::levelFromCode(0), Level::Debug);
    EXPECT_EQ(pulse::levelFromCode(3), Level::Error);
    EXPECT_FALSE(pulse::levelFromCode(4).has_value());
    EXPECT_FALSE(pulse::levelFromCode(-1).has_value());
}

// --- Req 13.6: destinazione non disponibile preserva l'esecuzione -----------

TEST(Logger, ReturnsErrorWhenSinkUnavailableButPreservesExecution) {
    SessionLogStore store;
    Logger logger("modA", store);

    store.setAvailable(false);  // destinazione non disponibile

    // L'esecuzione prosegue (nessuna eccezione) e si ottiene un errore.
    bool ok = true;
    LogErrorCode code = LogErrorCode::None;
    bool emptyMessage = true;
    EXPECT_NO_THROW({
        const auto res = logger.log(Level::Error, "destinazione giù");
        ok = res.ok;
        code = res.code;
        emptyMessage = res.message.empty();
    });

    EXPECT_FALSE(ok);
    EXPECT_EQ(code, LogErrorCode::SinkUnavailable);
    EXPECT_FALSE(emptyMessage);
    EXPECT_EQ(store.size(), 0u);  // nulla registrato

    // Quando la destinazione torna disponibile, il logging riprende.
    store.setAvailable(true);
    EXPECT_TRUE(logger.log(Level::Info, "ripristino").ok);
    EXPECT_EQ(store.size(), 1u);
}

// --- Req 13.3 / 13.4: validazione del contenuto dei popup -------------------

TEST(Popup, AcceptsNonEmptyTitleWithinLimitsAndShows) {
    RecordingPopupPresenter presenter;

    const auto res = pulse::showPopup("Titolo", "Corpo del messaggio", presenter);

    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.code, PopupErrorCode::None);
    ASSERT_EQ(presenter.shown.size(), 1u);
    EXPECT_EQ(presenter.shown[0].title, "Titolo");
    EXPECT_EQ(presenter.shown[0].body, "Corpo del messaggio");
}

TEST(Popup, AcceptsBoundaryLengths) {
    RecordingPopupPresenter presenter;

    const std::string title(pulse::kMaxPopupTitleLength, 't');   // esattamente 100
    const std::string body(pulse::kMaxPopupBodyLength, 'b');     // esattamente 1000

    EXPECT_TRUE(pulse::showPopup(title, body, presenter).ok);
    EXPECT_EQ(presenter.shown.size(), 1u);
}

TEST(Popup, RejectsEmptyTitleWithoutShowing) {
    RecordingPopupPresenter presenter;

    const auto res = pulse::showPopup("", "corpo valido", presenter);

    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.code, PopupErrorCode::EmptyTitle);
    EXPECT_TRUE(presenter.shown.empty());  // nessun popup mostrato
}

TEST(Popup, RejectsTitleOverLimitWithoutShowing) {
    RecordingPopupPresenter presenter;
    const std::string title(pulse::kMaxPopupTitleLength + 1, 't');  // 101

    const auto res = pulse::showPopup(title, "corpo", presenter);

    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.code, PopupErrorCode::TitleTooLong);
    EXPECT_TRUE(presenter.shown.empty());
}

TEST(Popup, RejectsBodyOverLimitWithoutShowing) {
    RecordingPopupPresenter presenter;
    const std::string body(pulse::kMaxPopupBodyLength + 1, 'b');  // 1001

    const auto res = pulse::showPopup("Titolo", body, presenter);

    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.code, PopupErrorCode::BodyTooLong);
    EXPECT_TRUE(presenter.shown.empty());
}

TEST(Popup, ValidateMatchesShowDecision) {
    EXPECT_TRUE(pulse::validatePopup("Titolo", "Corpo").ok);
    EXPECT_FALSE(pulse::validatePopup("", "Corpo").ok);
}

}  // namespace
