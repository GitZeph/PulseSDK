// pulse/log.hpp — header pubblico del sistema di logging e degli helper popup
// dello SDK Pulse (Layer 5, Requisito 13).
//
// Espone ai Developer di mod il `Logger` e gli helper di notifica popup.
// L'implementazione (header-only) vive sotto sdk/log/log.hpp ed è inclusa qui
// con un percorso relativo; questo file è il punto d'ingresso pubblico,
// raggiungibile via la include path di `pulse::sdk` come <pulse/log.hpp>
// (stesso schema di <pulse/events.hpp>).
//
// API principali:
//   * Level{Debug, Info, Warning, Error}     insieme chiuso dei livelli (Req 13.1)
//   * SessionLogStore                         sink di sessione recuperabile (Req 13.5)
//   * Logger::log(Level, msg)                 registra un messaggio valido (Req 13.1, 13.5)
//   * Logger::log(int code, msg)              rifiuta un livello fuori insieme (Req 13.2)
//   * showPopup / validatePopup               validazione del contenuto popup (Req 13.3, 13.4)
//
// Esempio:
//   pulse::SessionLogStore store;                       // sink di sessione
//   pulse::Logger logger("myMod", store);               // identità della mod
//   logger.log(pulse::Level::Info, "avvio");            // (Req 13.1, 13.5)
//   auto res = logger.log(7, "livello errato");          // res.ok == false (Req 13.2)
//
//   struct ConsolePopup : pulse::IPopupPresenter {
//       void present(const pulse::PopupContent&) override { /* mostra UI */ }
//   } presenter;
//   pulse::showPopup("Titolo", "Corpo", presenter);      // (Req 13.3, 13.4)
//
// Stack: C++20/23 (Requisito 26.1). Header-only, nessuna dipendenza esterna.
#ifndef PULSE_LOG_HPP
#define PULSE_LOG_HPP

// L'implementazione è collocata sotto sdk/log/ (codice del sistema di logging)
// ed esposta da questo header pubblico tramite include relativo, così non
// dipende dalla presenza di sdk/log/ nella include path pubblica.
#include "../../log/log.hpp"

namespace pulse {

// Promuove i tipi pubblici del modulo di logging nel namespace `pulse`, così i
// Developer usano `pulse::Logger`, `pulse::Level`, `pulse::SessionLogStore`, ecc.
using log::ILogSink;
using log::IPopupPresenter;
using log::Level;
using log::Logger;
using log::LogErrorCode;
using log::LogRecord;
using log::LogResult;
using log::ModId;
using log::PopupContent;
using log::PopupErrorCode;
using log::PopupResult;
using log::SessionLogStore;

using log::kMaxPopupBodyLength;
using log::kMaxPopupTitleLength;
using log::levelFromCode;
using log::showPopup;
using log::validatePopup;

}  // namespace pulse

#endif  // PULSE_LOG_HPP
