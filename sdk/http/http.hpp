// sdk/http/http.hpp — implementazione (header-only) del client HTTP dello SDK
// Pulse (Layer 5, Requisito 12).
//
// Questo header fornisce `HttpClient`: l'API con cui una Mod effettua richieste
// HTTP asincrone gated dal permesso "network" del proprio Manifest. Le
// semantiche osservabili implementate qui (tutte validate dagli unit test e dal
// property test P29) sono:
//
//   * Metodi GET, POST, PUT, DELETE, PATCH, HEAD (Req 12.1).
//   * Corpo della richiesta di dimensione massima 10 MB: una richiesta più
//     grande è RIFIUTATA prima di qualsiasi effetto di trasporto (Req 12.1).
//   * Risposta con codice di stato numerico nell'intervallo 100..599, insieme
//     di intestazioni e corpo fino a 50 MB (Req 12.2). Uno stato fuori
//     intervallo o un corpo oltre 50 MB producono un errore senza dati
//     parziali.
//   * Timeout di 30 secondi applicato alla richiesta in corso (Req 12.3); il
//     valore è iniettabile per i test e inoltrato al trasporto.
//   * Errore con categoria {network, timeout} e descrizione testuale leggibile,
//     senza alcun dato parziale di risposta (Req 12.4).
//   * Limite di 16 richieste concorrenti per Mod: la 17ª richiesta concorrente
//     è rifiutata con un errore di superamento del limite (Req 12.5).
//   * Gating sul permesso "network": senza permesso la richiesta è NEGATA prima
//     di aprire qualsiasi connessione, ossia prima di toccare il trasporto
//     (Req 12.6).
//
// Per restare auto-contenuto e testabile senza rete reale, `HttpClient`:
//   * astrae il trasporto effettivo dietro l'interfaccia iniettabile
//     `IHttpTransport` (i test usano un trasporto fittizio, nessuna rete);
//   * riceve il controllo del permesso come predicato iniettabile
//     `std::function<bool(const ModId&)>` (NON dipende dal Sandbox del Layer 6,
//     che è un task successivo);
//   * NON dipende da `pulse::async::Task` (task concorrente): l'esito finale è
//     consegnato tramite una callback auto-contenuta.
//
// Stack: C++20/23 (Requisito 26.1). Header-only, nessuna dipendenza esterna.
#ifndef PULSE_HTTP_HTTP_HPP
#define PULSE_HTTP_HTTP_HPP

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pulse::http {

// ---------------------------------------------------------------------------
// Tipi di base.
// ---------------------------------------------------------------------------

// Identità della Mod che effettua la richiesta: serve per il gating del
// permesso (Req 12.6) e per il conteggio della concorrenza per-Mod (Req 12.5).
using ModId = std::string;

// Metodi HTTP supportati (Req 12.1).
enum class HttpMethod { GET, POST, PUT, DELETE, PATCH, HEAD };

// Insieme di intestazioni: coppie nome/valore. Una lista di coppie (anziché una
// mappa) preserva l'ordine e ammette intestazioni ripetute, comuni in HTTP.
using Headers = std::vector<std::pair<std::string, std::string>>;

// Corpo della richiesta o della risposta. Contenitore di byte binary-safe.
using Body = std::string;

// Limiti osservabili del Requisito 12.
inline constexpr std::size_t kMaxRequestBodyBytes = 10u * 1024u * 1024u;   // 10 MB (Req 12.1)
inline constexpr std::size_t kMaxResponseBodyBytes = 50u * 1024u * 1024u;  // 50 MB (Req 12.2)
inline constexpr std::size_t kMaxConcurrentPerMod = 16u;                   // (Req 12.5)
inline constexpr int kMinStatus = 100;                                     // (Req 12.2)
inline constexpr int kMaxStatus = 599;                                     // (Req 12.2)
inline constexpr std::chrono::milliseconds kDefaultTimeout{30'000};        // 30 s (Req 12.3)

// ---------------------------------------------------------------------------
// Richiesta e risposta.
// ---------------------------------------------------------------------------

// Richiesta HTTP avviata da una Mod.
struct HttpRequest {
    ModId modId;                       // identità della Mod chiamante (gating + concorrenza).
    HttpMethod method{HttpMethod::GET};
    std::string url;
    Headers headers;
    Body body;
};

// Risposta HTTP. `status` è garantito nell'intervallo 100..599 quando consegnato
// con esito positivo; il corpo non supera i 50 MB (Req 12.2).
struct HttpResponse {
    int status{0};
    Headers headers;
    Body body;
};

// ---------------------------------------------------------------------------
// Errori.
// ---------------------------------------------------------------------------

// Categoria della causa di un errore HTTP. {Network, Timeout} sono le categorie
// del Requisito 12.4; le restanti coprono i rifiuti locali (permesso,
// concorrenza, validazione di dimensione/stato) imposti prima o dopo il
// trasporto.
enum class HttpErrorCode {
    Network,                  // errore di rete (Req 12.4).
    Timeout,                  // superamento del timeout di 30 s (Req 12.3, 12.4).
    PermissionDenied,         // permesso "network" assente (Req 12.6).
    ConcurrencyLimitExceeded, // oltre 16 richieste concorrenti per Mod (Req 12.5).
    RequestTooLarge,          // corpo della richiesta oltre 10 MB (Req 12.1).
    ResponseTooLarge,         // corpo della risposta oltre 50 MB (Req 12.2).
    InvalidStatus,            // stato fuori dall'intervallo 100..599 (Req 12.2).
};

// Oggetto di errore: categoria della causa + descrizione leggibile (Req 12.4).
struct HttpError {
    HttpErrorCode code{HttpErrorCode::Network};
    std::string message;
};

// ---------------------------------------------------------------------------
// HttpResult — esito finale di una richiesta accettata, in stile Result.
//
// Contiene una `HttpResponse` valida oppure un `HttpError`. In caso di errore
// NON espone alcun dato parziale di risposta (Req 12.4).
// ---------------------------------------------------------------------------
class HttpResult {
public:
    [[nodiscard]] static HttpResult ok(HttpResponse response) {
        return HttpResult{true, std::move(response), {}};
    }
    [[nodiscard]] static HttpResult fail(HttpError error) {
        return HttpResult{false, {}, std::move(error)};
    }

    [[nodiscard]] bool isOk() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    // Precondizione: isOk(). Risposta consegnata alla Mod.
    [[nodiscard]] const HttpResponse& response() const noexcept { return response_; }

    // Precondizione: !isOk(). Descrive la causa del fallimento.
    [[nodiscard]] const HttpError& error() const noexcept { return error_; }

private:
    HttpResult(bool ok, HttpResponse response, HttpError error)
        : ok_(ok), response_(std::move(response)), error_(std::move(error)) {}

    bool ok_;
    HttpResponse response_;
    HttpError error_;
};

// ---------------------------------------------------------------------------
// SendResult — esito immediato della chiamata a `send()`.
//
// Indica se la richiesta è stata ACCETTATA e affidata al trasporto, oppure
// RIFIUTATA in modo sincrono prima di qualsiasi effetto di trasporto. I rifiuti
// sincroni sono: permesso assente (Req 12.6), corpo della richiesta oltre 10 MB
// (Req 12.1) e superamento del limite di concorrenza (Req 12.5). In caso di
// rifiuto la callback dell'esito finale NON viene invocata.
// ---------------------------------------------------------------------------
class SendResult {
public:
    [[nodiscard]] static SendResult accepted() { return SendResult{true, {}}; }
    [[nodiscard]] static SendResult rejected(HttpError error) {
        return SendResult{false, std::move(error)};
    }

    [[nodiscard]] bool isAccepted() const noexcept { return accepted_; }
    explicit operator bool() const noexcept { return accepted_; }

    // Precondizione: !isAccepted(). Descrive il motivo del rifiuto.
    [[nodiscard]] const HttpError& error() const noexcept { return error_; }

private:
    SendResult(bool accepted, HttpError error)
        : accepted_(accepted), error_(std::move(error)) {}

    bool accepted_;
    HttpError error_;
};

// ---------------------------------------------------------------------------
// TransportResult — esito che il trasporto restituisce al client.
// ---------------------------------------------------------------------------
struct TransportResult {
    bool ok{false};            // true => `response` valida; false => errore di trasporto.
    HttpResponse response;     // valida sse ok.
    // In caso di errore di trasporto la categoria DEVE essere Network o Timeout
    // (le uniche categorie che il trasporto può segnalare, Req 12.4).
    HttpErrorCode errorCode{HttpErrorCode::Network};
    std::string errorMessage;

    [[nodiscard]] static TransportResult success(HttpResponse response) {
        TransportResult r;
        r.ok = true;
        r.response = std::move(response);
        return r;
    }
    [[nodiscard]] static TransportResult networkError(std::string message) {
        TransportResult r;
        r.ok = false;
        r.errorCode = HttpErrorCode::Network;
        r.errorMessage = std::move(message);
        return r;
    }
    [[nodiscard]] static TransportResult timeoutError(std::string message) {
        TransportResult r;
        r.ok = false;
        r.errorCode = HttpErrorCode::Timeout;
        r.errorMessage = std::move(message);
        return r;
    }
};

// ---------------------------------------------------------------------------
// IHttpTransport — interfaccia astratta del trasporto di rete effettivo.
//
// È iniettabile così i test usano un trasporto fittizio (nessuna rete reale).
// L'implementazione concreta applica il `timeout` fornito e segnala l'esito
// invocando `onComplete` (eventualmente in modo differito, per modellare
// richieste in volo simultanee).
// ---------------------------------------------------------------------------
class IHttpTransport {
public:
    using CompletionCallback = std::function<void(TransportResult)>;

    virtual ~IHttpTransport() = default;

    // Avvia il trasporto della richiesta con il timeout indicato e invoca
    // `onComplete` al termine. Può completare in modo sincrono o differito.
    virtual void perform(const HttpRequest& request,
                         std::chrono::milliseconds timeout,
                         CompletionCallback onComplete) = 0;
};

// ---------------------------------------------------------------------------
// HttpClient — client HTTP gated per-Mod (Req 12).
// ---------------------------------------------------------------------------
class HttpClient {
public:
    using ResponseCallback = std::function<void(HttpResult)>;
    using PermissionPredicate = std::function<bool(const ModId&)>;

    // Costruisce il client.
    //   * `transport`            trasporto effettivo (iniettato; non posseduto).
    //   * `hasNetworkPermission` predicato di gating del permesso "network"
    //                            (Req 12.6). Se vuoto, ogni richiesta è negata.
    //   * `maxConcurrentPerMod`  limite di concorrenza per Mod (default 16,
    //                            Req 12.5); iniettabile per i test.
    //   * `timeout`              timeout della richiesta (default 30 s, Req 12.3).
    HttpClient(IHttpTransport& transport,
               PermissionPredicate hasNetworkPermission,
               std::size_t maxConcurrentPerMod = kMaxConcurrentPerMod,
               std::chrono::milliseconds timeout = kDefaultTimeout)
        : transport_(transport),
          hasNetworkPermission_(std::move(hasNetworkPermission)),
          maxConcurrentPerMod_(maxConcurrentPerMod),
          timeout_(timeout) {}

    [[nodiscard]] std::chrono::milliseconds timeout() const noexcept { return timeout_; }
    [[nodiscard]] std::size_t maxConcurrentPerMod() const noexcept {
        return maxConcurrentPerMod_;
    }

    // Numero di richieste attualmente in volo per `modId`.
    [[nodiscard]] std::size_t activeRequests(const ModId& modId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = active_.find(modId);
        return it == active_.end() ? 0u : it->second;
    }

    // Avvia una richiesta HTTP. Restituisce l'esito IMMEDIATO (accettata o
    // rifiutata sincronamente). Se accettata, l'esito finale è consegnato a
    // `onComplete`. L'ordine dei controlli garantisce che il permesso sia
    // verificato PRIMA di qualsiasi effetto di trasporto (Req 12.6).
    SendResult send(const HttpRequest& request, ResponseCallback onComplete) {
        // 1. Gating del permesso "network" PRIMA di ogni effetto di trasporto
        //    (Req 12.6): nega senza aprire alcuna connessione.
        if (!hasNetworkPermission_ || !hasNetworkPermission_(request.modId)) {
            return SendResult::rejected(HttpError{
                HttpErrorCode::PermissionDenied,
                "richiesta negata: permesso \"network\" non dichiarato nel "
                "Manifest della Mod"});
        }

        // 2. Corpo della richiesta entro 10 MB (Req 12.1): rifiuta prima del
        //    trasporto, senza alcuna connessione.
        if (request.body.size() > kMaxRequestBodyBytes) {
            return SendResult::rejected(HttpError{
                HttpErrorCode::RequestTooLarge,
                "richiesta rifiutata: corpo della richiesta oltre il limite di "
                "10 MB"});
        }

        // 3. Limite di concorrenza per Mod (Req 12.5): la 17ª richiesta
        //    concorrente è rifiutata. La prenotazione dello slot è atomica.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::size_t& count = active_[request.modId];
            if (count >= maxConcurrentPerMod_) {
                return SendResult::rejected(HttpError{
                    HttpErrorCode::ConcurrencyLimitExceeded,
                    "richiesta rifiutata: superato il limite di 16 richieste "
                    "HTTP concorrenti per Mod"});
            }
            ++count;  // prenota lo slot di concorrenza.
        }

        // 4. Affida la richiesta al trasporto con il timeout configurato
        //    (Req 12.3). La validazione della risposta avviene al completamento.
        const ModId mod = request.modId;
        transport_.perform(
            request, timeout_,
            [this, mod, cb = std::move(onComplete)](TransportResult tr) {
                releaseSlot(mod);  // libera lo slot di concorrenza.

                if (!tr.ok) {
                    // Errore di rete o timeout: nessun dato parziale (Req 12.4).
                    cb(HttpResult::fail(
                        HttpError{tr.errorCode, std::move(tr.errorMessage)}));
                    return;
                }

                // Validazione dello stato: intervallo 100..599 (Req 12.2).
                if (tr.response.status < kMinStatus ||
                    tr.response.status > kMaxStatus) {
                    cb(HttpResult::fail(HttpError{
                        HttpErrorCode::InvalidStatus,
                        "risposta rifiutata: codice di stato fuori "
                        "dall'intervallo 100..599"}));
                    return;
                }

                // Corpo della risposta entro 50 MB (Req 12.2).
                if (tr.response.body.size() > kMaxResponseBodyBytes) {
                    cb(HttpResult::fail(HttpError{
                        HttpErrorCode::ResponseTooLarge,
                        "risposta rifiutata: corpo della risposta oltre il "
                        "limite di 50 MB"}));
                    return;
                }

                cb(HttpResult::ok(std::move(tr.response)));
            });

        return SendResult::accepted();
    }

private:
    // Decrementa il contatore di concorrenza per `modId`, senza scendere sotto 0.
    void releaseSlot(const ModId& modId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = active_.find(modId);
        if (it != active_.end() && it->second > 0) {
            --it->second;
        }
    }

    IHttpTransport& transport_;
    PermissionPredicate hasNetworkPermission_;
    std::size_t maxConcurrentPerMod_;
    std::chrono::milliseconds timeout_;

    mutable std::mutex mutex_;
    // Richieste in volo per Mod (protetto da `mutex_`).
    std::map<ModId, std::size_t, std::less<>> active_;
};

}  // namespace pulse::http

#endif  // PULSE_HTTP_HTTP_HPP
