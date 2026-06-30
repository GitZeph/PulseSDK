// tests/property29_http_concurrency_limit_test.cpp
// Feature: pulse-sdk, Property 29 — Limite di concorrenza delle richieste HTTP.
// Validates: Requisiti 12.5
//
// Property 29 (design.md / Req 12.5): per ogni Mod, l'HttpClient ammette al
// massimo `kMaxConcurrentPerMod` (= 16) richieste HTTP concorrenti. Mantenendo
// le richieste "in volo" con un trasporto fittizio che NON completa finché non
// viene rilasciato, l'invariante da verificare è:
//   (a) per ciascuna Mod le prime fino-a-16 richieste sono ACCETTATE e OGNI
//       richiesta oltre la 16ª è rifiutata con
//       HttpErrorCode::ConcurrencyLimitExceeded;
//   (b) activeRequests(mod) non supera mai 16 (mentre le richieste sono in volo);
//   (c) il limite è applicato in modo INDIPENDENTE per ogni Mod — una Mod al
//       limite non riduce la capacità di un'altra Mod (isolamento per-mod).
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si genera un numero modesto di Mod (≤ 3) e, per ciascuna, un numero di
//     richieste che spazia sia SOTTO sia SOPRA il limite di 16 (cap a ~20 per
//     mantenere il test veloce);
//   * il trasporto fittizio (`autoComplete = false`) accoda le callback di
//     completamento senza invocarle, così tutte le richieste accettate restano
//     contemporaneamente in volo e `activeRequests` riflette la concorrenza
//     reale;
//   * si conta, per ogni Mod, il numero di richieste accettate e si verificano
//     gli invarianti (a)/(b)/(c);
//   * TEARDOWN: si rilasciano le richieste in volo con `completeAll` e si
//     verifica che gli slot tornino liberi (drain), come nei test in
//     tests/http_client_test.cpp.

#include <pulse/http.hpp>

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

using pulse::http::Body;
using pulse::http::HttpClient;
using pulse::http::HttpErrorCode;
using pulse::http::HttpMethod;
using pulse::http::HttpRequest;
using pulse::http::HttpResponse;
using pulse::http::HttpResult;
using pulse::http::IHttpTransport;
using pulse::http::kMaxConcurrentPerMod;
using pulse::http::ModId;
using pulse::http::SendResult;
using pulse::http::TransportResult;

namespace {

// ---------------------------------------------------------------------------
// FakeTransport — trasporto in-memory iniettabile, senza rete reale.
//
// In modalità differita (`autoComplete = false`) accoda le callback di
// completamento affinché il test mantenga le richieste in volo simultanee, e le
// completi a comando con `completeAll`. Stesso schema di tests/http_client_test.cpp.
// ---------------------------------------------------------------------------
class FakeTransport : public IHttpTransport {
public:
    void perform(const HttpRequest& /*request*/, std::chrono::milliseconds /*timeout*/,
                 CompletionCallback onComplete) override {
        ++performCalls;
        if (autoComplete) {
            onComplete(TransportResult::success(HttpResponse{200, {}, Body{}}));
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
    bool autoComplete{false};
    std::vector<CompletionCallback> pending;
};

// Predicato di permesso "network" che concede sempre (gating non in esame qui).
HttpClient::PermissionPredicate allowAll() {
    return [](const ModId&) { return true; };
}

HttpRequest makeRequest(const ModId& mod) {
    HttpRequest req;
    req.modId = mod;
    req.method = HttpMethod::GET;
    req.url = "https://example.test/resource";
    return req;
}

}  // namespace

// Property 29: limite di concorrenza HTTP per-mod, applicato in isolamento.
RC_GTEST_PROP(Property29HttpConcurrency,
              LimitEnforcedIndependentlyPerMod,
              ()) {
    // Numero modesto di Mod (1..3) per mantenere il test rapido.
    const std::size_t numMods =
        *rc::gen::inRange<std::size_t>(1, 4).as("numero di mod");

    // Per ogni Mod, un numero di richieste che attraversa il limite: 1..20
    // (alcune sotto 16, altre sopra). Il cap a 20 mantiene il test rapido.
    std::vector<std::size_t> requested;
    requested.reserve(numMods);
    for (std::size_t i = 0; i < numMods; ++i) {
        requested.push_back(
            *rc::gen::inRange<std::size_t>(1, 21).as("richieste per mod"));
    }

    FakeTransport transport;
    transport.autoComplete = false;  // mantiene le richieste in volo.
    HttpClient client(transport, allowAll());

    std::vector<ModId> modIds;
    modIds.reserve(numMods);
    std::map<ModId, std::size_t> accepted;

    // Avvia, per ciascuna Mod, le richieste e verifica l'esito di ciascuna
    // rispetto al limite per-mod (a).
    for (std::size_t i = 0; i < numMods; ++i) {
        const ModId modId = "mod." + std::to_string(i);
        modIds.push_back(modId);

        std::size_t ok = 0;
        for (std::size_t k = 0; k < requested[i]; ++k) {
            SendResult sent = client.send(makeRequest(modId), [](HttpResult) {});
            if (k < kMaxConcurrentPerMod) {
                // (a) le prime fino-a-16 richieste DEVONO essere accettate.
                RC_ASSERT(sent.isAccepted());
                ++ok;
            } else {
                // (a) ogni richiesta oltre la 16ª DEVE essere rifiutata con
                //     ConcurrencyLimitExceeded.
                RC_ASSERT(!sent.isAccepted());
                RC_ASSERT(sent.error().code ==
                          HttpErrorCode::ConcurrencyLimitExceeded);
            }
        }
        accepted[modId] = ok;

        // (b) il conteggio in volo non supera mai il limite.
        RC_ASSERT(client.activeRequests(modId) <= kMaxConcurrentPerMod);
    }

    // (b)/(c) con TUTTE le Mod ancora in volo: ogni Mod ha esattamente
    // min(richieste, 16) richieste attive, indipendentemente dalle altre.
    // Questo dimostra l'isolamento per-mod: una Mod al limite non riduce la
    // capacità di un'altra.
    for (std::size_t i = 0; i < numMods; ++i) {
        const ModId& modId = modIds[i];
        const std::size_t expected =
            std::min<std::size_t>(requested[i], kMaxConcurrentPerMod);
        RC_ASSERT(accepted[modId] == expected);
        RC_ASSERT(client.activeRequests(modId) == expected);
        RC_ASSERT(client.activeRequests(modId) <= kMaxConcurrentPerMod);
    }

    // TEARDOWN: rilascia le richieste in volo e drena. Gli slot di concorrenza
    // si liberano per ogni Mod (mirroring del pattern di lifetime dei test
    // unitari in tests/http_client_test.cpp).
    transport.completeAll(TransportResult::success(HttpResponse{200, {}, Body{}}));
    for (const auto& modId : modIds) {
        RC_ASSERT(client.activeRequests(modId) == 0u);
    }
}
