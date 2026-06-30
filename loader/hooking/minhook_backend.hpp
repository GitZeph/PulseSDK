// loader/hooking/minhook_backend.hpp — backend di hooking basato su MinHook
// (fork interno `pulse-minhook`) + Zydis, per Windows x86-64 (Requisito 2.2).
//
// Vedi design "Selezione del motore di hooking per piattaforma": su Windows x64
// Pulse usa MinHook (trampolino affidabile su x64, licenza BSD compatibile,
// footprint minimo) con Zydis come disassembler a lunghezza per relocare i
// prologhi non banali.
//
// Compilazione cross-platform:
//  - Su Windows con il backend abilitato (PULSE_HOOK_BACKEND_MINHOOK definito da
//    CMake), `MinHookBackend` implementa install/remove/readOriginal su MinHook.
//  - Su qualunque altra build host (es. macOS) la classe resta DEFINITA e
//    COMPILABILE, ma le primitive ritornano `HookErrorCode::Unsupported` e
//    `available()` ritorna false: l'interfaccia e il path non-Windows compilano.
//
// In entrambi i casi `make_minhook_backend()` restituisce un'istanza valida,
// così il codice chiamante non deve preoccuparsi del target a compile-time.
#ifndef PULSE_LOADER_HOOKING_MINHOOK_BACKEND_HPP
#define PULSE_LOADER_HOOKING_MINHOOK_BACKEND_HPP

#include <memory>

#include "hooking/hook_backend.hpp"

namespace pulse::hooking {

// Backend di hooking su MinHook + Zydis (Windows x64). Su piattaforme non
// supportate è uno stub che riporta `Unsupported`.
class MinHookBackend final : public IHookBackend {
public:
    MinHookBackend();
    ~MinHookBackend() override;

    MinHookBackend(const MinHookBackend&) = delete;
    MinHookBackend& operator=(const MinHookBackend&) = delete;

    Result<Trampoline> install(std::uintptr_t target, void* detour) override;
    Result<void> remove(std::uintptr_t target) override;
    Result<ByteSpan> readOriginal(std::uintptr_t target,
                                  std::size_t len) override;

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] bool available() const noexcept override;

private:
    // Stato di inizializzazione di MinHook (rilevante solo su Windows).
    bool initialized_{false};
};

// Factory: restituisce sempre un backend valido (mai nullptr). Su piattaforme
// senza MinHook l'istanza riporta `available() == false`.
[[nodiscard]] std::unique_ptr<IHookBackend> make_minhook_backend();

}  // namespace pulse::hooking

#endif  // PULSE_LOADER_HOOKING_MINHOOK_BACKEND_HPP
