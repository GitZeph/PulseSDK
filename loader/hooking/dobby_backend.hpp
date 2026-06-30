// loader/hooking/dobby_backend.hpp — backend di hooking basato su Dobby, per
// macOS (x86-64 / arm64) e iOS (arm64) — Requisiti 2.1, 2.2.
//
// Vedi design "Selezione del motore di hooking per piattaforma": sui target
// Apple Pulse usa Dobby, l'unico backend che copre uniformemente x86-64 e
// arm64 (Apple Silicon) con un'unica API, con gestione PAC (Pointer
// Authentication) su arm64e — coerente tra macOS arm64 e iOS arm64, riducendo
// il codice specifico per piattaforma.
//
// Compilazione cross-platform (stesso schema di MinHookBackend):
//  - Su un target Apple con il backend abilitato (PULSE_HOOK_BACKEND_DOBBY
//    definito da CMake quando l'opzione PULSE_ENABLE_DOBBY è ON e Dobby è
//    disponibile via FetchContent), `DobbyBackend` implementa
//    install/remove/readOriginal su Dobby (DobbyHook / DobbyDestroy).
//  - Su qualunque altra build host (incluso il default macOS senza il fetch di
//    Dobby) la classe resta DEFINITA e COMPILABILE, ma le primitive ritornano
//    `HookErrorCode::Unsupported` e `available()` ritorna false.
//
// In entrambi i casi `make_dobby_backend()` restituisce sempre un'istanza
// valida (mai nullptr), così il chiamante non deve conoscere il target a
// compile-time.
//
// Stack: C++20/23 (Requisito 26.1). Dipende solo da hook_backend.hpp.
#ifndef PULSE_LOADER_HOOKING_DOBBY_BACKEND_HPP
#define PULSE_LOADER_HOOKING_DOBBY_BACKEND_HPP

#include <memory>

#include "hooking/hook_backend.hpp"

namespace pulse::hooking {

// Backend di hooking su Dobby (macOS x86-64/arm64, iOS arm64), con gestione PAC
// su arm64e. Su piattaforme non supportate è uno stub che riporta `Unsupported`.
class DobbyBackend final : public IHookBackend {
public:
    DobbyBackend();
    ~DobbyBackend() override;

    DobbyBackend(const DobbyBackend&) = delete;
    DobbyBackend& operator=(const DobbyBackend&) = delete;

    Result<Trampoline> install(std::uintptr_t target, void* detour) override;
    Result<void> remove(std::uintptr_t target) override;
    Result<ByteSpan> readOriginal(std::uintptr_t target,
                                  std::size_t len) override;

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] bool available() const noexcept override;

private:
    // True quando il backend reale Dobby è compilato e operativo.
    bool initialized_{false};
};

// Factory: restituisce sempre un backend valido (mai nullptr). Sui target senza
// Dobby l'istanza riporta `available() == false`.
[[nodiscard]] std::unique_ptr<IHookBackend> make_dobby_backend();

}  // namespace pulse::hooking

#endif  // PULSE_LOADER_HOOKING_DOBBY_BACKEND_HPP
