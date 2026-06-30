// pulse/async.hpp — header pubblico del sistema asincrono / task dello SDK
// Pulse (Layer 5, Requisito 11).
//
// Espone il `Task<T>` e il `TaskScheduler` ai Developer di mod. L'implementazione
// (header-only, template) vive sotto sdk/async/task.hpp ed è inclusa qui; questo
// file è il punto d'ingresso pubblico, raggiungibile via la include path di
// `pulse::sdk` come <pulse/async.hpp> (stesso schema di <pulse/events.hpp>).
//
// API principali:
//   * Task<T>::spawn(modId, work)   avvia un'operazione asincrona (Req 11.1, 11.5)
//   * Task<T>::then(onMain)         continuazione sul thread principale (Req 11.2/11.3)
//   * TaskScheduler::pumpMainThread()  tick di frame che esegue le continuazioni
//   * TaskScheduler::cancelMod(modId)  annulla i task di una mod disabilitata (Req 11.4)
//   * Result<T>                     esito ok(valore) / fail(errore con causa)
//
// Esempio:
//   auto spawned = pulse::Task<int>::spawn("myMod", [] { return 21 * 2; });
//   if (spawned) {
//       spawned.value().then([](pulse::TaskResult<int> r) {
//           if (r) { /* usa r.value() sul thread principale */ }
//           else  { /* gestisci r.error() */ }
//       });
//   }
//   // ... al frame successivo, sul thread principale:
//   pulse::TaskScheduler::global().pumpMainThread();
//
// Stack: C++20/23 (Requisito 26.1). Header-only, nessuna dipendenza esterna.
#ifndef PULSE_ASYNC_HPP
#define PULSE_ASYNC_HPP

// L'implementazione è collocata sotto sdk/async/ (codice del sistema asincrono)
// ed esposta da questo header pubblico tramite include relativo, così non
// dipende dalla presenza di sdk/async/ nella include path pubblica.
#include "../../async/task.hpp"

namespace pulse {

// Promuove i tipi pubblici del sistema asincrono nel namespace `pulse`, in modo
// che i Developer usino `pulse::Task`, `pulse::TaskScheduler`, ecc.
using async::ModId;
using async::Task;
using async::TaskError;
using async::TaskErrorCode;
using async::TaskScheduler;

// `Result` nel namespace async è generico; per i Developer lo esponiamo come
// `pulse::TaskResult<T>` per evitare ambiguità con altri Result dello SDK.
template <class T>
using TaskResult = async::Result<T>;

// Limite massimo di task simultanei per mod (Req 11.5).
using async::kMaxConcurrentTasksPerMod;

}  // namespace pulse

#endif  // PULSE_ASYNC_HPP
