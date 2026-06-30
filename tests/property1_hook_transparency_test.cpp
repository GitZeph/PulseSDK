// tests/property1_hook_transparency_test.cpp
// Feature: pulse-sdk, Property 1 — Trasparenza dell'hook (call-original).
// Validates: Requirements 2.2, 5.3 (Requisiti 2.2, 5.3)
//
// Property 1 (design.md): "Per ogni funzione bersaglio e per ogni tupla di
// argomenti validi, l'esecuzione attraverso un hook che si limita a invocare
// callOriginal deve produrre lo stesso valore di ritorno e gli stessi effetti
// osservabili della chiamata diretta alla funzione originale."
//
// Strategia (model-based, ≥100 iterazioni RapidCheck di default):
//   * MODELLO  = invocazione DIRETTA della funzione originale del gioco;
//   * SISTEMA  = invocazione ATTRAVERSO il detour generato da PULSE_HOOK, il
//                cui corpo si limita a `return callOriginal(args...)`.
// Per ogni tupla di argomenti generata casualmente, si confrontano il valore
// di ritorno e gli effetti osservabili (mutazione di stato, conteggio delle
// invocazioni dell'originale) tra MODELLO e SISTEMA: devono coincidere.
//
// Il passo di INSTALLAZIONE è modellato con `pulse::hooking::FakeBackend`
// (loader/hooking/fake_backend.hpp): si verifica che install() abbia successo
// e poi — come fa l'Hooking Engine reale dopo install() — si cabla il
// trampolino verso l'originale con `pulse::hooks::bind_trampoline`, così che
// `callOriginal` invochi effettivamente la funzione originale.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstdint>
#include <string_view>

#include <pulse/hooks.hpp>

#include "hooking/fake_backend.hpp"

namespace {

// ---------------------------------------------------------------------------
// Funzione bersaglio 1 — firma aritmetica con effetti osservabili.
// Registra gli ultimi argomenti e il numero di invocazioni: così la
// trasparenza può essere verificata non solo sul valore di ritorno ma anche
// sugli effetti collaterali (Req 5.3 "preservando i parametri").
// ---------------------------------------------------------------------------
struct AddObservation {
    long long lastA = 0;
    long long lastB = 0;
    int calls = 0;
};
AddObservation g_add_obs;

// `int` in ingresso, `long long` in uscita: evita l'overflow con segno (UB)
// pur esercitando l'intero dominio di `int` generato da RapidCheck.
long long original_add(int a, int b) {
    g_add_obs.lastA = a;
    g_add_obs.lastB = b;
    ++g_add_obs.calls;
    return static_cast<long long>(a) + static_cast<long long>(b);
}

// ---------------------------------------------------------------------------
// Funzione bersaglio 2 — firma "metodo" con puntatore `self` mutato.
// Verifica che la trasparenza valga anche per gli effetti su un'istanza
// passata per puntatore (caso tipico di un hook su un metodo del gioco).
// ---------------------------------------------------------------------------
struct Accumulator {
    long long total = 0;
    int touches = 0;
};
long long original_accumulate(Accumulator* self, int delta) {
    self->total += static_cast<long long>(delta);
    ++self->touches;
    return self->total;
}

}  // namespace

// ---------------------------------------------------------------------------
// Hook TRASPARENTI dichiarati con PULSE_HOOK: il corpo si limita a inoltrare
// a callOriginal e a propagarne il valore di ritorno, senza alterare
// parametri né effetti. È la forma minimale che la Property 1 richiede.
// ---------------------------------------------------------------------------
PULSE_HOOK(prop1_add, long long, (int a, int b)) {
    return callOriginal(a, b);
}

PULSE_HOOK(prop1_accumulate, long long, (Accumulator* self, int delta)) {
    return callOriginal(self, delta);
}

namespace {

using pulse::hooking::FakeBackend;

// Modella il passo di installazione dell'Hooking Engine: install() sul
// FakeBackend deve avere successo, poi si cabla il trampolino verso
// l'originale reale (come fa l'engine dopo install()), così che callOriginal
// invochi la funzione originale. Restituisce il detour tipizzato registrato.
template <class FnPtr, class DetourFn>
DetourFn model_install_and_bind(std::string_view symbol, std::uintptr_t target,
                                FnPtr original) {
    FakeBackend backend;
    auto installed = backend.install(target, reinterpret_cast<void*>(original));
    RC_ASSERT(installed.has_value());      // l'install del backend riesce
    RC_ASSERT(backend.isInstalled(target));

    // L'engine cabla il trampolino verso l'originale: callOriginal lo invoca.
    RC_ASSERT(pulse::hooks::bind_trampoline(
        symbol, reinterpret_cast<void*>(original)));

    const pulse::hooks::HookRegistration* reg = pulse::hooks::find(symbol);
    RC_ASSERT(reg != nullptr);
    // Confronto su bool: evita che RapidCheck tenti di stampare un `void*`.
    const bool detour_bound = reg->detour != nullptr;
    RC_ASSERT(detour_bound);
    return reinterpret_cast<DetourFn>(reg->detour);
}

// --- Property 1 / firma aritmetica ----------------------------------------
// Feature: pulse-sdk, Property 1. Validates: Requirements 2.2, 5.3.
RC_GTEST_PROP(Property1HookTransparency,
              AddThroughHookEqualsDirectOriginal,
              (int a, int b)) {
    auto detour = model_install_and_bind<long long (*)(int, int),
                                         long long (*)(int, int)>(
        "prop1_add", 0x4000, &original_add);

    // MODELLO: chiamata diretta all'originale; cattura ritorno + effetti.
    g_add_obs = AddObservation{};
    const long long direct = original_add(a, b);
    const AddObservation direct_obs = g_add_obs;

    // SISTEMA: chiamata attraverso l'hook trasparente.
    g_add_obs = AddObservation{};
    const long long via_hook = detour(a, b);
    const AddObservation hook_obs = g_add_obs;

    RC_ASSERT(via_hook == direct);                  // stesso valore di ritorno
    RC_ASSERT(hook_obs.lastA == direct_obs.lastA);  // stessi parametri visti
    RC_ASSERT(hook_obs.lastB == direct_obs.lastB);
    RC_ASSERT(hook_obs.calls == direct_obs.calls);  // originale invocato 1 volta
    RC_ASSERT(hook_obs.calls == 1);
}

// --- Property 1 / firma "metodo" con self mutato --------------------------
// Feature: pulse-sdk, Property 1. Validates: Requirements 2.2, 5.3.
RC_GTEST_PROP(Property1HookTransparency,
              AccumulateThroughHookEqualsDirectOriginal,
              (int initial, int delta)) {
    auto detour =
        model_install_and_bind<long long (*)(Accumulator*, int),
                               long long (*)(Accumulator*, int)>(
            "prop1_accumulate", 0x5000, &original_accumulate);

    // MODELLO: diretto sull'originale.
    Accumulator direct_self{static_cast<long long>(initial), 0};
    const long long direct = original_accumulate(&direct_self, delta);

    // SISTEMA: attraverso l'hook trasparente, con un'istanza equivalente.
    Accumulator hook_self{static_cast<long long>(initial), 0};
    const long long via_hook = detour(&hook_self, delta);

    RC_ASSERT(via_hook == direct);                       // stesso ritorno
    RC_ASSERT(hook_self.total == direct_self.total);     // stesso effetto su self
    RC_ASSERT(hook_self.touches == direct_self.touches); // originale invocato 1 volta
    RC_ASSERT(hook_self.touches == 1);
}

}  // namespace
