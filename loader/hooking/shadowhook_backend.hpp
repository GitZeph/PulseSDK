// loader/hooking/shadowhook_backend.hpp — backend di hooking basato su
// ShadowHook (ByteDance) per Android arm64-v8a e armeabi-v7a (Requisito 2.2).
//
// Vedi design "Selezione del motore di hooking per piattaforma": su Android
// Pulse usa ShadowHook, specializzato su inline hook arm64/thumb/arm32, con
// gestione robusta della cache d'istruzioni (I-cache flush dopo la patch) e dei
// thread. Lo stesso backend copre sia il 64-bit (arm64-v8a) sia il 32-bit
// (armeabi-v7a, modalità thumb/arm32) richiesto dal Requisito 1.1.
//
// ShadowHook astrae le differenze di set d'istruzioni: rileva automaticamente
// se il bersaglio è codice ARM (arm32), Thumb o AArch64 in base all'indirizzo e
// allo stato del processore, reloca il prologo nel set corretto e invalida la
// cache d'istruzioni del bersaglio e del trampolino. Pulse non deve quindi
// gestire manualmente thumb-bit né `__builtin___clear_cache`.
//
// Compilazione cross-platform:
//  - Su Android con il backend abilitato (PULSE_HOOK_BACKEND_SHADOWHOOK definito
//    da CMake) `ShadowHookBackend` implementa install/remove/readOriginal su
//    ShadowHook.
//  - Su qualunque altra build host (es. macOS) la classe resta DEFINITA e
//    COMPILABILE, ma le primitive ritornano `HookErrorCode::Unsupported` e
//    `available()` ritorna false: l'interfaccia e il path non-Android compilano.
//
// In entrambi i casi `make_shadowhook_backend()` restituisce un'istanza valida,
// così il codice chiamante non deve preoccuparsi del target a compile-time.
#ifndef PULSE_LOADER_HOOKING_SHADOWHOOK_BACKEND_HPP
#define PULSE_LOADER_HOOKING_SHADOWHOOK_BACKEND_HPP

#include <cstdint>
#include <map>
#include <memory>

#include "hooking/hook_backend.hpp"

namespace pulse::hooking {

// Backend di hooking su ShadowHook (Android arm64/armv7). Su piattaforme non
// supportate è uno stub che riporta `Unsupported`.
class ShadowHookBackend final : public IHookBackend {
public:
    ShadowHookBackend();
    ~ShadowHookBackend() override;

    ShadowHookBackend(const ShadowHookBackend&) = delete;
    ShadowHookBackend& operator=(const ShadowHookBackend&) = delete;

    Result<Trampoline> install(std::uintptr_t target, void* detour) override;
    Result<void> remove(std::uintptr_t target) override;
    Result<ByteSpan> readOriginal(std::uintptr_t target,
                                  std::size_t len) override;

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] bool available() const noexcept override;

private:
    // Stato di inizializzazione di ShadowHook (rilevante solo su Android).
    bool initialized_{false};

    // ShadowHook identifica un hook tramite uno "stub" opaco restituito da
    // shadowhook_hook_sym_addr; serve per shadowhook_unhook. Mappiamo
    // l'indirizzo bersaglio allo stub per implementare remove(target).
    std::map<std::uintptr_t, void*> stubs_{};
};

// Factory: restituisce sempre un backend valido (mai nullptr). Su piattaforme
// senza ShadowHook l'istanza riporta `available() == false`.
[[nodiscard]] std::unique_ptr<IHookBackend> make_shadowhook_backend();

}  // namespace pulse::hooking

#endif  // PULSE_LOADER_HOOKING_SHADOWHOOK_BACKEND_HPP
