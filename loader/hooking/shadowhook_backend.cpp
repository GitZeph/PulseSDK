// loader/hooking/shadowhook_backend.cpp — implementazione del backend ShadowHook.
//
// Il corpo reale (ShadowHook) è compilato solo quando il backend è abilitato per
// il target Android tramite la macro PULSE_HOOK_BACKEND_SHADOWHOOK (definita da
// CMake via FetchContent guardato su ANDROID + opzione PULSE_ENABLE_SHADOWHOOK).
// Su ogni altra build host la classe compila come stub che riporta
// `HookErrorCode::Unsupported`, così loader/hooking resta compilabile su
// macOS/Linux/Windows (Requisito 26.3).
#include "hooking/shadowhook_backend.hpp"

#include <memory>
#include <string>

// Abilita il path reale solo su Android con ShadowHook disponibile.
#if defined(__ANDROID__) && defined(PULSE_HOOK_BACKEND_SHADOWHOOK)
#define PULSE_SHADOWHOOK_ACTIVE 1
#else
#define PULSE_SHADOWHOOK_ACTIVE 0
#endif

#if PULSE_SHADOWHOOK_ACTIVE
#include <cstring>

#include <shadowhook.h>
#endif

namespace pulse::hooking {

namespace {
constexpr std::string_view kBackendName = "pulse-shadowhook";
}  // namespace

#if PULSE_SHADOWHOOK_ACTIVE

// ---------------------------------------------------------------------------
// Implementazione reale: Android arm64-v8a / armeabi-v7a con ShadowHook.
//
// ShadowHook gestisce internamente:
//  - rilevamento del set d'istruzioni del bersaglio (AArch64 / ARM32 / Thumb)
//    in base all'indirizzo e al thumb-bit, relocando il prologo nel set giusto;
//  - invalidazione della cache d'istruzioni (I-cache) del bersaglio e del
//    trampolino dopo la scrittura della patch, indispensabile su ARM per
//    rendere visibile il codice modificato alle pipeline degli altri core;
//  - sospensione/ispezione sicura dei thread che potrebbero trovarsi nel
//    prologo durante la patch.
// Pulse quindi non manipola manualmente né il thumb-bit né __clear_cache.
// ---------------------------------------------------------------------------

namespace {
// Converte l'errno corrente di ShadowHook in una HookError strutturata.
HookError shadowhook_error(HookErrorCode code, const char* prefix) {
    const int err = shadowhook_get_errno();
    const char* msg = shadowhook_to_errmsg(err);
    std::string text(prefix);
    if (msg != nullptr) {
        text += ": ";
        text += msg;
    }
    return HookError{code, std::move(text)};
}
}  // namespace

ShadowHookBackend::ShadowHookBackend() {
    // SHADOWHOOK_MODE_UNIQUE: un hook per indirizzo bersaglio, coerente con la
    // primitiva install/remove di Pulse (la catena multi-hook vive nel codice
    // Pulse, non nel backend — Requisito 27). `false` = non-debuggable.
    const int rc = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
    initialized_ = (rc == 0);
}

ShadowHookBackend::~ShadowHookBackend() {
    // Rilascia eventuali hook ancora attivi per non lasciare patch nel codice.
    for (const auto& [target, stub] : stubs_) {
        if (stub != nullptr) {
            shadowhook_unhook(stub);
        }
    }
    stubs_.clear();
}

Result<Trampoline> ShadowHookBackend::install(std::uintptr_t target,
                                              void* detour) {
    if (!initialized_) {
        return Result<Trampoline>::err(HookErrorCode::BackendFailure,
                                       "ShadowHook non inizializzato");
    }
    if (target == 0 || detour == nullptr) {
        return Result<Trampoline>::err(HookErrorCode::InvalidArgument,
                                       "target o detour nullo");
    }
    if (stubs_.find(target) != stubs_.end()) {
        return Result<Trampoline>::err(HookErrorCode::AlreadyHooked,
                                       "funzione bersaglio già hookata");
    }

    void* original = nullptr;
    // shadowhook_hook_sym_addr installa l'inline hook gestendo set d'istruzioni
    // (thumb/arm32/arm64) e I-cache; restituisce uno stub opaco (per unhook) e
    // popola `original` con il trampolino all'originale (Req 2.2).
    void* stub = shadowhook_hook_sym_addr(reinterpret_cast<void*>(target),
                                          detour, &original);
    if (stub == nullptr) {
        const int err = shadowhook_get_errno();
        HookErrorCode code = HookErrorCode::BackendFailure;
        // SHADOWHOOK_ERRNO_HOOK_DUP: bersaglio già hookato.
        if (err == SHADOWHOOK_ERRNO_HOOK_DUP) {
            code = HookErrorCode::AlreadyHooked;
        }
        return Result<Trampoline>::err(
            shadowhook_error(code, "shadowhook_hook_sym_addr"));
    }

    stubs_.emplace(target, stub);
    return Result<Trampoline>::ok(Trampoline{original});
}

Result<void> ShadowHookBackend::remove(std::uintptr_t target) {
    if (!initialized_) {
        return Result<void>::err(HookErrorCode::BackendFailure,
                                 "ShadowHook non inizializzato");
    }
    if (target == 0) {
        return Result<void>::err(HookErrorCode::InvalidArgument, "target nullo");
    }

    const auto it = stubs_.find(target);
    if (it == stubs_.end()) {
        return Result<void>::err(HookErrorCode::NotHooked,
                                 "nessun hook installato sul target");
    }

    // shadowhook_unhook ripristina il prologo originale e invalida la I-cache.
    const int rc = shadowhook_unhook(it->second);
    if (rc != 0) {
        return Result<void>::err(
            shadowhook_error(HookErrorCode::BackendFailure, "shadowhook_unhook"));
    }

    stubs_.erase(it);
    return Result<void>::ok();
}

Result<ByteSpan> ShadowHookBackend::readOriginal(std::uintptr_t target,
                                                 std::size_t len) {
    if (target == 0) {
        return Result<ByteSpan>::err(HookErrorCode::InvalidArgument,
                                     "target nullo");
    }
    if (len == 0) {
        return Result<ByteSpan>::ok(ByteSpan{});
    }

    // Legge i byte correnti del prologo dal nostro stesso spazio d'indirizzi.
    // Va invocato PRIMA di install() per catturare i byte originali da
    // persistere nel RollbackStore (Req 18.1). Su ARM/Thumb l'indirizzo del
    // simbolo può avere il thumb-bit (bit 0) impostato: lo azzeriamo per
    // ottenere l'indirizzo lineare reale dei byte di codice.
    const std::uintptr_t linear = target & ~static_cast<std::uintptr_t>(1);

    std::vector<std::uint8_t> buffer(len);
    std::memcpy(buffer.data(), reinterpret_cast<const void*>(linear), len);

    return Result<ByteSpan>::ok(ByteSpan{std::move(buffer)});
}

bool ShadowHookBackend::available() const noexcept { return initialized_; }

#else  // !PULSE_SHADOWHOOK_ACTIVE

// ---------------------------------------------------------------------------
// Stub non-Android: compila ovunque ma riporta `Unsupported`.
// Mantiene loader/hooking compilabile sulla build host macOS/Linux/Windows
// (Requisito 26.3).
// ---------------------------------------------------------------------------

namespace {
HookError unsupported() {
    return HookError{HookErrorCode::Unsupported,
                     "ShadowHook è disponibile solo su Android arm64/armv7"};
}
}  // namespace

ShadowHookBackend::ShadowHookBackend() = default;
ShadowHookBackend::~ShadowHookBackend() = default;

Result<Trampoline> ShadowHookBackend::install(std::uintptr_t /*target*/,
                                              void* /*detour*/) {
    return Result<Trampoline>::err(unsupported());
}

Result<void> ShadowHookBackend::remove(std::uintptr_t /*target*/) {
    return Result<void>::err(unsupported());
}

Result<ByteSpan> ShadowHookBackend::readOriginal(std::uintptr_t /*target*/,
                                                 std::size_t /*len*/) {
    return Result<ByteSpan>::err(unsupported());
}

bool ShadowHookBackend::available() const noexcept { return false; }

#endif  // PULSE_SHADOWHOOK_ACTIVE

// ---------------------------------------------------------------------------
// Comune a tutte le piattaforme.
// ---------------------------------------------------------------------------

std::string_view ShadowHookBackend::name() const noexcept { return kBackendName; }

std::unique_ptr<IHookBackend> make_shadowhook_backend() {
    return std::make_unique<ShadowHookBackend>();
}

}  // namespace pulse::hooking
