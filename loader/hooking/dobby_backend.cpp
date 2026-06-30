// loader/hooking/dobby_backend.cpp — implementazione del backend Dobby.
//
// Il corpo reale (Dobby: DobbyHook / DobbyDestroy) è compilato solo quando il
// backend è abilitato per un target Apple tramite la macro
// PULSE_HOOK_BACKEND_DOBBY (definita da CMake quando l'opzione PULSE_ENABLE_DOBBY
// è ON e Dobby è reso disponibile via FetchContent). Su ogni altra build host —
// incluso il default macOS senza il fetch di Dobby — la classe compila come stub
// che riporta `HookErrorCode::Unsupported`, così loader/hooking resta
// compilabile su qualsiasi host (Requisito 26.3).
//
// Gestione PAC su arm64e (Requisito 2.2):
//  - Su Apple Silicon arm64e i puntatori a codice e dati sono firmati con
//    Pointer Authentication Code (PAC). Dobby gestisce internamente la firma e
//    la verifica dei puntatori durante l'installazione del trampolino, quindi
//    `DobbyHook` restituisce un `origin` chiamabile senza ulteriore firma da
//    parte nostra.
//  - Per leggere i byte originali del prologo (readOriginal) e per usare il
//    target come indirizzo di memoria dobbiamo invece operare sul puntatore
//    GREZZO (raw), strippando l'eventuale PAC: usiamo `ptrauth_strip` quando il
//    toolchain espone <ptrauth.h> (arm64e), altrimenti il puntatore è già
//    grezzo. Il trampolino restituito dall'engine resta quello firmato fornito
//    da Dobby, idoneo all'invocazione dell'originale.
#include "hooking/dobby_backend.hpp"

#include <memory>
#include <string>

// Abilita il path reale solo sui target Apple con Dobby disponibile.
#if defined(__APPLE__) && defined(PULSE_HOOK_BACKEND_DOBBY)
#define PULSE_DOBBY_ACTIVE 1
#else
#define PULSE_DOBBY_ACTIVE 0
#endif

#if PULSE_DOBBY_ACTIVE
#include <cstring>

#include <dobby.h>

// Pointer Authentication: disponibile sulla toolchain arm64e di Apple.
#if defined(__arm64e__) && __has_include(<ptrauth.h>)
#include <ptrauth.h>
#define PULSE_DOBBY_HAS_PTRAUTH 1
#else
#define PULSE_DOBBY_HAS_PTRAUTH 0
#endif
#endif  // PULSE_DOBBY_ACTIVE

namespace pulse::hooking {

namespace {
constexpr std::string_view kBackendName = "dobby";

#if PULSE_DOBBY_ACTIVE
// Rimuove l'eventuale PAC dal puntatore a codice per ottenere l'indirizzo
// grezzo di memoria (lettura del prologo / chiamata a DobbyHook/Destroy).
// Su arm64e usa ptrauth_strip; altrove il puntatore è già grezzo.
void* strip_pac(std::uintptr_t target) noexcept {
    auto* ptr = reinterpret_cast<void*>(target);
#if PULSE_DOBBY_HAS_PTRAUTH
    ptr = ptrauth_strip(ptr, ptrauth_key_function_pointer);
#endif
    return ptr;
}
#endif  // PULSE_DOBBY_ACTIVE
}  // namespace

#if PULSE_DOBBY_ACTIVE

// ---------------------------------------------------------------------------
// Implementazione reale: macOS (x86-64 / arm64) e iOS (arm64) con Dobby.
// ---------------------------------------------------------------------------

DobbyBackend::DobbyBackend() {
    // Dobby non richiede un'inizializzazione globale esplicita: lo stato è
    // pronto non appena la libreria è linkata.
    initialized_ = true;
}

DobbyBackend::~DobbyBackend() = default;

Result<Trampoline> DobbyBackend::install(std::uintptr_t target, void* detour) {
    if (!initialized_) {
        return Result<Trampoline>::err(HookErrorCode::BackendFailure,
                                       "Dobby non inizializzato");
    }
    if (target == 0 || detour == nullptr) {
        return Result<Trampoline>::err(HookErrorCode::InvalidArgument,
                                       "target o detour nullo");
    }

    // Indirizzo grezzo (PAC strippato su arm64e) per l'API di Dobby.
    void* target_ptr = strip_pac(target);
    void* origin = nullptr;

    // DobbyHook installa il detour e popola `origin` con il trampolino verso la
    // funzione originale. Su arm64e Dobby gestisce internamente la firma PAC dei
    // puntatori, quindi `origin` è direttamente chiamabile (Req 2.2).
    // Firma API corrente: int DobbyHook(void* address, void* fake_func,
    // void** out_origin_func) — niente typedef `dobby_dummy_func_t`.
    const int status = DobbyHook(target_ptr, detour, &origin);
    if (status != 0) {
        return Result<Trampoline>::err(
            HookErrorCode::BackendFailure,
            "DobbyHook ha riportato un errore (codice " +
                std::to_string(status) + ")");
    }
    if (origin == nullptr) {
        // Senza trampolino l'engine non potrebbe invocare l'originale: annulla.
        DobbyDestroy(target_ptr);
        return Result<Trampoline>::err(
            HookErrorCode::BackendFailure,
            "DobbyHook non ha restituito il trampolino originale");
    }

    return Result<Trampoline>::ok(Trampoline{origin});
}

Result<void> DobbyBackend::remove(std::uintptr_t target) {
    if (!initialized_) {
        return Result<void>::err(HookErrorCode::BackendFailure,
                                 "Dobby non inizializzato");
    }
    if (target == 0) {
        return Result<void>::err(HookErrorCode::InvalidArgument, "target nullo");
    }

    // DobbyDestroy ripristina il codice originale della funzione (Req 2.4).
    const int status = DobbyDestroy(strip_pac(target));
    if (status != 0) {
        return Result<void>::err(
            HookErrorCode::BackendFailure,
            "DobbyDestroy ha riportato un errore (codice " +
                std::to_string(status) + ")");
    }

    return Result<void>::ok();
}

Result<ByteSpan> DobbyBackend::readOriginal(std::uintptr_t target,
                                            std::size_t len) {
    if (target == 0) {
        return Result<ByteSpan>::err(HookErrorCode::InvalidArgument,
                                     "target nullo");
    }
    if (len == 0) {
        return Result<ByteSpan>::ok(ByteSpan{});
    }

    // In-process: i byte del prologo si leggono direttamente dalla memoria del
    // codice. Va invocato PRIMA di install() per catturare i byte originali da
    // persistere nel RollbackStore (Req 18.1). Sull'arm64e usiamo il puntatore
    // grezzo (PAC strippato).
    const auto* src = reinterpret_cast<const std::uint8_t*>(strip_pac(target));
    std::vector<std::uint8_t> buffer(len);
    std::memcpy(buffer.data(), src, len);

    return Result<ByteSpan>::ok(ByteSpan{std::move(buffer)});
}

bool DobbyBackend::available() const noexcept { return initialized_; }

#else  // !PULSE_DOBBY_ACTIVE

// ---------------------------------------------------------------------------
// Stub: compila ovunque ma riporta `Unsupported`. Mantiene loader/hooking
// compilabile sull'host di default (es. macOS senza il fetch di Dobby) — Req 26.3.
// ---------------------------------------------------------------------------

namespace {
HookError unsupported() {
    return HookError{
        HookErrorCode::Unsupported,
        "Dobby è disponibile solo sui target Apple (macOS/iOS) con "
        "PULSE_ENABLE_DOBBY abilitato"};
}
}  // namespace

DobbyBackend::DobbyBackend() = default;
DobbyBackend::~DobbyBackend() = default;

Result<Trampoline> DobbyBackend::install(std::uintptr_t /*target*/,
                                         void* /*detour*/) {
    return Result<Trampoline>::err(unsupported());
}

Result<void> DobbyBackend::remove(std::uintptr_t /*target*/) {
    return Result<void>::err(unsupported());
}

Result<ByteSpan> DobbyBackend::readOriginal(std::uintptr_t /*target*/,
                                            std::size_t /*len*/) {
    return Result<ByteSpan>::err(unsupported());
}

bool DobbyBackend::available() const noexcept { return false; }

#endif  // PULSE_DOBBY_ACTIVE

// ---------------------------------------------------------------------------
// Comune a tutte le piattaforme.
// ---------------------------------------------------------------------------

std::string_view DobbyBackend::name() const noexcept { return kBackendName; }

std::unique_ptr<IHookBackend> make_dobby_backend() {
    return std::make_unique<DobbyBackend>();
}

}  // namespace pulse::hooking
