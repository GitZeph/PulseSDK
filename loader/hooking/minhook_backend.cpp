// loader/hooking/minhook_backend.cpp — implementazione del backend MinHook.
//
// Il corpo reale (MinHook + Zydis) è compilato solo quando il backend è
// abilitato per il target Windows x64 tramite la macro PULSE_HOOK_BACKEND_MINHOOK
// (definita da CMake via FetchContent guardato su WIN32). Su ogni altra build
// host la classe compila come stub che riporta `HookErrorCode::Unsupported`,
// così loader/hooking resta compilabile su macOS/Linux (Requisito 26.3).
#include "hooking/minhook_backend.hpp"

#include <memory>
#include <string>

// Abilita il path reale solo su Windows x64 con MinHook disponibile.
#if defined(_WIN32) && defined(PULSE_HOOK_BACKEND_MINHOOK)
#define PULSE_MINHOOK_ACTIVE 1
#else
#define PULSE_MINHOOK_ACTIVE 0
#endif

#if PULSE_MINHOOK_ACTIVE
#include <cstring>

#include <MinHook.h>
#include <windows.h>
#endif

namespace pulse::hooking {

namespace {
constexpr std::string_view kBackendName = "pulse-minhook";
}  // namespace

#if PULSE_MINHOOK_ACTIVE

// ---------------------------------------------------------------------------
// Implementazione reale: Windows x86-64 con MinHook + Zydis.
// ---------------------------------------------------------------------------

MinHookBackend::MinHookBackend() {
    // MH_Initialize è idempotente a livello di processo; tracciamo lo stato
    // per chiamare MH_Uninitialize nel distruttore.
    const MH_STATUS status = MH_Initialize();
    initialized_ = (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED);
}

MinHookBackend::~MinHookBackend() {
    if (initialized_) {
        MH_Uninitialize();
    }
}

Result<Trampoline> MinHookBackend::install(std::uintptr_t target, void* detour) {
    if (!initialized_) {
        return Result<Trampoline>::err(HookErrorCode::BackendFailure,
                                       "MinHook non inizializzato");
    }
    if (target == 0 || detour == nullptr) {
        return Result<Trampoline>::err(HookErrorCode::InvalidArgument,
                                       "target o detour nullo");
    }

    auto* target_ptr = reinterpret_cast<LPVOID>(target);
    void* trampoline = nullptr;

    // MH_CreateHook reloca il prologo (con il disassembler a lunghezza del fork
    // pulse-minhook/Zydis) e prepara il trampolino verso l'originale (Req 2.2).
    MH_STATUS status = MH_CreateHook(target_ptr, detour, &trampoline);
    if (status != MH_OK) {
        HookErrorCode code = HookErrorCode::BackendFailure;
        if (status == MH_ERROR_ALREADY_CREATED) {
            code = HookErrorCode::AlreadyHooked;
        } else if (status == MH_ERROR_NOT_EXECUTABLE) {
            code = HookErrorCode::NotExecutable;
        } else if (status == MH_ERROR_UNSUPPORTED_FUNCTION) {
            code = HookErrorCode::UnsupportedPrologue;
        } else if (status == MH_ERROR_MEMORY_PROTECT) {
            code = HookErrorCode::MemoryProtection;
        }
        return Result<Trampoline>::err(
            code, std::string("MH_CreateHook: ") + MH_StatusToString(status));
    }

    status = MH_EnableHook(target_ptr);
    if (status != MH_OK) {
        // Annulla la creazione parziale per non lasciare stato sporco (Req 2.5).
        MH_RemoveHook(target_ptr);
        return Result<Trampoline>::err(
            HookErrorCode::BackendFailure,
            std::string("MH_EnableHook: ") + MH_StatusToString(status));
    }

    return Result<Trampoline>::ok(Trampoline{trampoline});
}

Result<void> MinHookBackend::remove(std::uintptr_t target) {
    if (!initialized_) {
        return Result<void>::err(HookErrorCode::BackendFailure,
                                 "MinHook non inizializzato");
    }
    if (target == 0) {
        return Result<void>::err(HookErrorCode::InvalidArgument, "target nullo");
    }

    auto* target_ptr = reinterpret_cast<LPVOID>(target);

    MH_STATUS status = MH_DisableHook(target_ptr);
    if (status == MH_ERROR_NOT_CREATED) {
        return Result<void>::err(HookErrorCode::NotHooked,
                                 "nessun hook creato sul target");
    }
    if (status != MH_OK) {
        return Result<void>::err(
            HookErrorCode::BackendFailure,
            std::string("MH_DisableHook: ") + MH_StatusToString(status));
    }

    status = MH_RemoveHook(target_ptr);
    if (status != MH_OK) {
        return Result<void>::err(
            HookErrorCode::BackendFailure,
            std::string("MH_RemoveHook: ") + MH_StatusToString(status));
    }

    return Result<void>::ok();
}

Result<ByteSpan> MinHookBackend::readOriginal(std::uintptr_t target,
                                              std::size_t len) {
    if (target == 0) {
        return Result<ByteSpan>::err(HookErrorCode::InvalidArgument,
                                     "target nullo");
    }
    if (len == 0) {
        return Result<ByteSpan>::ok(ByteSpan{});
    }

    // Legge i byte correnti del prologo. Va invocato PRIMA di install() per
    // catturare i byte originali da persistere nel RollbackStore (Req 18.1).
    std::vector<std::uint8_t> buffer(len);
    SIZE_T read = 0;
    const BOOL ok = ReadProcessMemory(GetCurrentProcess(),
                                      reinterpret_cast<LPCVOID>(target),
                                      buffer.data(), len, &read);
    if (!ok || read != len) {
        return Result<ByteSpan>::err(HookErrorCode::MemoryProtection,
                                     "lettura del prologo originale fallita");
    }

    return Result<ByteSpan>::ok(ByteSpan{std::move(buffer)});
}

bool MinHookBackend::available() const noexcept { return initialized_; }

#else  // !PULSE_MINHOOK_ACTIVE

// ---------------------------------------------------------------------------
// Stub non-Windows: compila ovunque ma riporta `Unsupported`.
// Mantiene loader/hooking compilabile sulla build host macOS/Linux (Req 26.3).
// ---------------------------------------------------------------------------

namespace {
HookError unsupported() {
    return HookError{HookErrorCode::Unsupported,
                     "MinHook è disponibile solo su Windows x86-64"};
}
}  // namespace

MinHookBackend::MinHookBackend() = default;
MinHookBackend::~MinHookBackend() = default;

Result<Trampoline> MinHookBackend::install(std::uintptr_t /*target*/,
                                           void* /*detour*/) {
    return Result<Trampoline>::err(unsupported());
}

Result<void> MinHookBackend::remove(std::uintptr_t /*target*/) {
    return Result<void>::err(unsupported());
}

Result<ByteSpan> MinHookBackend::readOriginal(std::uintptr_t /*target*/,
                                              std::size_t /*len*/) {
    return Result<ByteSpan>::err(unsupported());
}

bool MinHookBackend::available() const noexcept { return false; }

#endif  // PULSE_MINHOOK_ACTIVE

// ---------------------------------------------------------------------------
// Comune a tutte le piattaforme.
// ---------------------------------------------------------------------------

std::string_view MinHookBackend::name() const noexcept { return kBackendName; }

std::unique_ptr<IHookBackend> make_minhook_backend() {
    return std::make_unique<MinHookBackend>();
}

}  // namespace pulse::hooking
