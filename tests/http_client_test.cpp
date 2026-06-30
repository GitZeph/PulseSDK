// http_client_test.cpp — unit test di pulse::http::HttpClient (task 30.1).
//
// Copre le semantiche osservabili del Requisito 12 con un trasporto fittizio
// (FakeTransport) che NON apre alcuna connessione di rete reale:
//   * ogni metodo HTTP (GET/POST/PUT/DELETE/PATCH/HEAD) raggiunge il trasporto
//     e produce una risposta valida (Req 12.1, 12.2);
//   * corpo della richiesta oltre 10 MB rifiutato prima del trasporto (Req 12.1);
//   * corpo della risposta oltre 50 MB rifiutato (Req 12.2);
//   * stato fuori dall'intervallo 100..599 rifiutato; 100 e 599 accettati (Req 12.2);
//   * errore di timeout e di rete con categoria corretta, senza dati parziali (Req 12.4);
//   * limite di 16 richieste concorrenti per Mod: la 17ª è rifiutata (Req 12.5);
//   * gating del permesso "network": senza permesso la richiesta è negata e il
//     trasporto NON viene mai invocato (Req 12.6).
#include <pulse/http.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using pulse::http::Body;
using pulse::http::Headers;
using pulse::http::HttpClient;
using pulse::http::HttpError;
using pulse::http::HttpErrorCode;
using pulse::http::HttpMethod;
using pulse::http::HttpRequest;
using pulse::http::HttpResponse;
using pulse::http::HttpResult;
using pulse::http::IHttpTransport;
using pulse::http::kMaxConcurrentPerMod;
using pulse::http::kMaxRequestBodyBytes;
using pulse::http::kMaxResponseBodyBytes;
using pulse::http::ModId;
using pulse::http::SendResult;
using pulse::http::TransportResult;

namespace {

// ---------------------------------------------------------------------------
// FakeTransport — trasporto in-memory iniettabile, senza rete reale.
//
// In modalità `autoComplete` (default) invoca subito la callback con
// `nextResult`. In modalità differita accoda le callback affinché il test possa
// modellare richieste simultanee in volo e completarle a comando.
// ---------------------------------------------------------------------------
class FakeTransport : public IHttpTransport {
public:
    void perform(const HttpRequest& request, std::chrono::milliseconds timeout,
                 CompletionCallback onComplete) override {
        ++performCalls;
        receivedMethods.push_back(request.method);
        lastTimeout = timeout;
        if (autoComplete) {
            onComplete(nextResult);
        } else {
            pending.push_back(std::move(onComplete));
        }
    }

    // Completa tutte le richieste differite in coda con `result`.
    void completeAll(const TransportResult& result) {
        auto callbacks = std::move(pending);
        pending.clear();
        for (auto& cb : callbacks) {
            cb(result);
        }
    }

    int performCalls{0};
    bool autoComplete{true};
    TransportResult nextResult{TransportResult::success(HttpResponse{200, {}, Body{}})};
    std::vector<HttpMethod> receivedMethods;
    std::chrono::milliseconds lastTimeout{0};
    std::vector<CompletionCallback> pending;
};

// Predicato di permesso che concede sempre.
HttpClient::PermissionPredicate allowAll() {
    return [](const ModId&) { return true; };
}

// Predicato di permesso che nega sempre.
HttpClient::PermissionPredicate denyAll() {
    return [](const ModId&) { return false; };
}

HttpRequest makeRequest(HttpMethod method, ModId mod = "mod.alpha") {
    HttpRequest req;
    req.modId = std::move(mod);
    req.method = method;
    req.url = "https://example.test/resource";
    return req;
}

}  // namespace

// Req 12.1, 12.2: ogni metodo HTTP raggiunge il trasporto e produce una
// risposta valida consegnata via callback.
TEST(HttpClientTest, AllMethodsReachTransportAndSucceed) {
    const HttpMethod methods[] = {HttpMethod::GET,    HttpMethod::POST,
                                  HttpMethod::PUT,    HttpMethod::DELETE,
                                  HttpMethod::PATCH,  HttpMethod::HEAD};

    for (HttpMethod method : methods) {
        FakeTransport transport;
        transport.nextResult =
            TransportResult::success(HttpResponse{200, {}, Body{"corpo"}});
        HttpClient client(transport, allowAll());

        std::optional<HttpResult> outcome;
        SendResult sent = client.send(makeRequest(method),
                                      [&](HttpResult r) { outcome = std::move(r); });

        EXPECT_TRUE(sent.isAccepted());
        EXPECT_EQ(transport.performCalls, 1);
        ASSERT_EQ(transport.receivedMethods.size(), 1u);
        EXPECT_EQ(transport.receivedMethods.front(), method);
        ASSERT_TRUE(outcome.has_value());
        ASSERT_TRUE(outcome->isOk());
        EXPECT_EQ(outcome->response().status, 200);
        EXPECT_EQ(outcome->response().body, Body{"corpo"});
    }
}

// Req 12.3: il client inoltra al trasporto il timeout di 30 s (default).
TEST(HttpClientTest, ForwardsDefaultThirtySecondTimeout) {
    FakeTransport transport;
    HttpClient client(transport, allowAll());

    client.send(makeRequest(HttpMethod::GET), [](HttpResult) {});

    EXPECT_EQ(client.timeout(), std::chrono::seconds(30));
    EXPECT_EQ(transport.lastTimeout, std::chrono::seconds(30));
}

// Req 12.1: un corpo della richiesta oltre 10 MB è rifiutato PRIMA di qualsiasi
// effetto di trasporto.
TEST(HttpClientTest, RejectsRequestBodyOverTenMegabytes) {
    FakeTransport transport;
    HttpClient client(transport, allowAll());

    HttpRequest req = makeRequest(HttpMethod::POST);
    req.body = Body(kMaxRequestBodyBytes + 1, 'x');

    bool callbackInvoked = false;
    SendResult sent =
        client.send(req, [&](HttpResult) { callbackInvoked = true; });

    ASSERT_FALSE(sent.isAccepted());
    EXPECT_EQ(sent.error().code, HttpErrorCode::RequestTooLarge);
    EXPECT_EQ(transport.performCalls, 0);  // nessun effetto di trasporto.
    EXPECT_FALSE(callbackInvoked);
}

// Req 12.1: un corpo della richiesta esattamente a 10 MB è accettato (limite).
TEST(HttpClientTest, AcceptsRequestBodyExactlyTenMegabytes) {
    FakeTransport transport;
    HttpClient client(transport, allowAll());

    HttpRequest req = makeRequest(HttpMethod::POST);
    req.body = Body(kMaxRequestBodyBytes, 'x');

    SendResult sent = client.send(req, [](HttpResult) {});

    EXPECT_TRUE(sent.isAccepted());
    EXPECT_EQ(transport.performCalls, 1);
}

// Req 12.2: un corpo della risposta oltre 50 MB è rifiutato senza dati parziali.
TEST(HttpClientTest, RejectsResponseBodyOverFiftyMegabytes) {
    FakeTransport transport;
    transport.nextResult = TransportResult::success(
        HttpResponse{200, {}, Body(kMaxResponseBodyBytes + 1, 'y')});
    HttpClient client(transport, allowAll());

    std::optional<HttpResult> outcome;
    client.send(makeRequest(HttpMethod::GET),
                [&](HttpResult r) { outcome = std::move(r); });

    ASSERT_TRUE(outcome.has_value());
    ASSERT_FALSE(outcome->isOk());
    EXPECT_EQ(outcome->error().code, HttpErrorCode::ResponseTooLarge);
}

// Req 12.2: stato fuori dall'intervallo 100..599 rifiutato; i limiti 100 e 599
// sono accettati.
TEST(HttpClientTest, ValidatesStatusRange) {
    struct Case {
        int status;
        bool valid;
    };
    const Case cases[] = {{99, false},  {100, true}, {200, true},
                          {599, true},  {600, false}, {0, false}};

    for (const Case& c : cases) {
        FakeTransport transport;
        transport.nextResult =
            TransportResult::success(HttpResponse{c.status, {}, Body{}});
        HttpClient client(transport, allowAll());

        std::optional<HttpResult> outcome;
        client.send(makeRequest(HttpMethod::GET),
                    [&](HttpResult r) { outcome = std::move(r); });

        ASSERT_TRUE(outcome.has_value());
        if (c.valid) {
            EXPECT_TRUE(outcome->isOk()) << "status " << c.status;
            EXPECT_EQ(outcome->response().status, c.status);
        } else {
            ASSERT_FALSE(outcome->isOk()) << "status " << c.status;
            EXPECT_EQ(outcome->error().code, HttpErrorCode::InvalidStatus);
        }
    }
}

// Req 12.4: un timeout produce un errore con categoria Timeout e nessun dato
// parziale di risposta.
TEST(HttpClientTest, TimeoutErrorHasTimeoutCategory) {
    FakeTransport transport;
    transport.nextResult = TransportResult::timeoutError("timeout dopo 30 s");
    HttpClient client(transport, allowAll());

    std::optional<HttpResult> outcome;
    client.send(makeRequest(HttpMethod::GET),
                [&](HttpResult r) { outcome = std::move(r); });

    ASSERT_TRUE(outcome.has_value());
    ASSERT_FALSE(outcome->isOk());
    EXPECT_EQ(outcome->error().code, HttpErrorCode::Timeout);
    EXPECT_FALSE(outcome->error().message.empty());
}

// Req 12.4: un errore di rete produce un errore con categoria Network.
TEST(HttpClientTest, NetworkErrorHasNetworkCategory) {
    FakeTransport transport;
    transport.nextResult = TransportResult::networkError("connessione rifiutata");
    HttpClient client(transport, allowAll());

    std::optional<HttpResult> outcome;
    client.send(makeRequest(HttpMethod::GET),
                [&](HttpResult r) { outcome = std::move(r); });

    ASSERT_TRUE(outcome.has_value());
    ASSERT_FALSE(outcome->isOk());
    EXPECT_EQ(outcome->error().code, HttpErrorCode::Network);
    EXPECT_FALSE(outcome->error().message.empty());
}

// Req 12.5: con 16 richieste concorrenti in volo per una Mod, la 17ª è rifiutata
// con errore di superamento del limite; al completamento gli slot si liberano.
TEST(HttpClientTest, RejectsSeventeenthConcurrentRequestPerMod) {
    FakeTransport transport;
    transport.autoComplete = false;  // mantiene le richieste in volo.
    HttpClient client(transport, allowAll());

    // Avvia 16 richieste: tutte accettate e in volo.
    for (std::size_t i = 0; i < kMaxConcurrentPerMod; ++i) {
        SendResult sent =
            client.send(makeRequest(HttpMethod::GET), [](HttpResult) {});
        EXPECT_TRUE(sent.isAccepted()) << "richiesta " << i;
    }
    EXPECT_EQ(client.activeRequests("mod.alpha"), kMaxConcurrentPerMod);
    EXPECT_EQ(transport.performCalls, static_cast<int>(kMaxConcurrentPerMod));

    // La 17ª richiesta concorrente è rifiutata.
    SendResult seventeenth =
        client.send(makeRequest(HttpMethod::GET), [](HttpResult) {});
    ASSERT_FALSE(seventeenth.isAccepted());
    EXPECT_EQ(seventeenth.error().code, HttpErrorCode::ConcurrencyLimitExceeded);
    // Nessuna nuova invocazione del trasporto per la richiesta rifiutata.
    EXPECT_EQ(transport.performCalls, static_cast<int>(kMaxConcurrentPerMod));

    // Completando le richieste in volo, gli slot si liberano e si può inviare
    // di nuovo.
    transport.completeAll(TransportResult::success(HttpResponse{200, {}, Body{}}));
    EXPECT_EQ(client.activeRequests("mod.alpha"), 0u);

    SendResult afterRelease =
        client.send(makeRequest(HttpMethod::GET), [](HttpResult) {});
    EXPECT_TRUE(afterRelease.isAccepted());
}

// Req 12.5: il limite di concorrenza è per-Mod; Mod distinte non si influenzano.
TEST(HttpClientTest, ConcurrencyLimitIsPerMod) {
    FakeTransport transport;
    transport.autoComplete = false;
    HttpClient client(transport, allowAll());

    for (std::size_t i = 0; i < kMaxConcurrentPerMod; ++i) {
        client.send(makeRequest(HttpMethod::GET, "mod.alpha"), [](HttpResult) {});
    }
    // mod.alpha è al limite, ma mod.beta parte da zero.
    SendResult betaSend =
        client.send(makeRequest(HttpMethod::GET, "mod.beta"), [](HttpResult) {});
    EXPECT_TRUE(betaSend.isAccepted());
    EXPECT_EQ(client.activeRequests("mod.beta"), 1u);
}

// Req 12.6: senza permesso "network" la richiesta è negata PRIMA di qualsiasi
// effetto di trasporto: il trasporto non viene mai invocato.
TEST(HttpClientTest, DeniesWithoutNetworkPermissionAndNeverCallsTransport) {
    FakeTransport transport;
    HttpClient client(transport, denyAll());

    bool callbackInvoked = false;
    SendResult sent = client.send(makeRequest(HttpMethod::GET),
                                  [&](HttpResult) { callbackInvoked = true; });

    ASSERT_FALSE(sent.isAccepted());
    EXPECT_EQ(sent.error().code, HttpErrorCode::PermissionDenied);
    EXPECT_EQ(transport.performCalls, 0);  // nessuna connessione aperta.
    EXPECT_FALSE(callbackInvoked);
    EXPECT_EQ(client.activeRequests("mod.alpha"), 0u);  // nessuno slot prenotato.
}

// Req 12.6: un predicato di permesso vuoto nega ogni richiesta.
TEST(HttpClientTest, EmptyPermissionPredicateDeniesAll) {
    FakeTransport transport;
    HttpClient client(transport, HttpClient::PermissionPredicate{});

    SendResult sent = client.send(makeRequest(HttpMethod::GET), [](HttpResult) {});

    ASSERT_FALSE(sent.isAccepted());
    EXPECT_EQ(sent.error().code, HttpErrorCode::PermissionDenied);
    EXPECT_EQ(transport.performCalls, 0);
}
