// pulse/http.hpp — header pubblico del client HTTP dello SDK Pulse
// (Layer 5, Requisito 12).
//
// Espone `HttpClient` ai Developer di mod. L'implementazione (header-only) vive
// sotto sdk/http/http.hpp ed è inclusa qui tramite include relativo, così non
// dipende dalla presenza di sdk/http/ nella include path pubblica. Questo file
// è il punto d'ingresso pubblico, raggiungibile come <pulse/http.hpp>.
//
// API principali:
//   * HttpClient::send(request, onComplete)  avvia una richiesta HTTP asincrona
//                                            (Req 12.1) e ne consegna l'esito
//                                            via callback.
//   * HttpMethod{GET, POST, PUT, DELETE, PATCH, HEAD}                 (Req 12.1)
//   * HttpRequest / HttpResponse / Headers / Body                     (Req 12.2)
//   * HttpError + HttpErrorCode{Network, Timeout, ...}                (Req 12.4)
//   * SendResult (esito immediato) / HttpResult (esito finale)
//   * IHttpTransport (trasporto iniettabile, per il test senza rete reale)
//
// Il permesso "network" del Manifest è verificato tramite un predicato
// iniettabile prima di qualsiasi effetto di rete (Req 12.6); il timeout di 30 s
// (Req 12.3) e il limite di 16 richieste concorrenti per Mod (Req 12.5) sono
// applicati dal client.
//
// Stack: C++20/23 (Requisito 26.1). Header-only, nessuna dipendenza esterna.
#ifndef PULSE_HTTP_HPP
#define PULSE_HTTP_HPP

// L'implementazione è collocata sotto sdk/http/ (codice del client HTTP) ed
// esposta da questo header pubblico tramite include relativo.
#include "../../http/http.hpp"

namespace pulse {

// Promuove i tipi pubblici del client HTTP nel namespace `pulse`, così i
// Developer usino `pulse::HttpClient`, `pulse::HttpMethod`, ecc.
using http::Body;
using http::Headers;
using http::HttpClient;
using http::HttpError;
using http::HttpErrorCode;
using http::HttpMethod;
using http::HttpRequest;
using http::HttpResponse;
using http::HttpResult;
using http::IHttpTransport;
using http::ModId;
using http::SendResult;
using http::TransportResult;

}  // namespace pulse

#endif  // PULSE_HTTP_HPP
