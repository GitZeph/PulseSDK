// loader/hooking/hook_backend.hpp — interfaccia platform-agnostic del backend
// di hooking di Pulse (Layer 3 — Hooking Engine, Requisito 2.2).
//
// Questo header definisce l'astrazione `IHookBackend` e i tipi di supporto
// (`Trampoline`, `ByteSpan`, `Result`) usati dall'engine di hooking. Il backend
// concreto viene selezionato a compile-time per target (MinHook su Windows x64,
// Dobby su macOS/iOS, ShadowHook su Android — vedi design "Selezione del motore
// di hooking per piattaforma").
//
// L'interfaccia espone SOLO tre primitive — install / remove / readOriginal —
// così la logica di catena, priorità, conflitti e rollback resta nel codice
// originale Pulse (Requisito 27), indipendente dal backend.
//
// Stack: C++20/23 (Requisito 26.1). Header-only, nessuna dipendenza dal backend
// concreto: i file che includono questo header compilano su ogni piattaforma host.
#ifndef PULSE_LOADER_HOOKING_HOOK_BACKEND_HPP
#define PULSE_LOADER_HOOKING_HOOK_BACKEND_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulse::hooking {

// ---------------------------------------------------------------------------
// Errori del backend di hooking.
// ---------------------------------------------------------------------------

// Categoria della causa di un fallimento di una primitiva del backend.
// L'engine usa la categoria per decidere retry/rollback (Requisiti 2.5, 18).
enum class HookErrorCode {
    None,                 // nessun errore (stato di default)
    Unsupported,          // backend non disponibile sulla piattaforma corrente
    InvalidArgument,      // argomento non valido (es. target o detour nullo)
    AlreadyHooked,        // la funzione bersaglio risulta già hookata
    NotHooked,            // remove richiesto su una funzione non hookata
    NotExecutable,        // l'indirizzo bersaglio non è codice eseguibile
    UnsupportedPrologue,  // prologo non relocabile dal disassembler (Zydis)
    MemoryProtection,     // impossibile cambiare la protezione di memoria
    BackendFailure,       // errore generico riportato dal backend sottostante
};

// Descrizione strutturata di un errore: categoria + messaggio leggibile.
struct HookError {
    HookErrorCode code{HookErrorCode::None};
    std::string message;
};

// ---------------------------------------------------------------------------
// Result<T> — esito di una primitiva: valore in caso di successo, oppure errore.
// Stile minimale (no eccezioni nelle primitive del backend, Requisito 2.5).
// ---------------------------------------------------------------------------

template <class T>
class Result {
public:
    static Result ok(T value) {
        Result r;
        r.value_ = std::move(value);
        return r;
    }
    static Result err(HookError error) {
        Result r;
        r.error_ = std::move(error);
        return r;
    }
    static Result err(HookErrorCode code, std::string message) {
        return err(HookError{code, std::move(message)});
    }

    [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() & { return *value_; }
    const T& value() const& { return *value_; }
    T&& value() && { return std::move(*value_); }

    [[nodiscard]] const HookError& error() const noexcept { return error_; }

private:
    std::optional<T> value_{};
    HookError error_{};
};

// Specializzazione per operazioni senza valore di ritorno (remove).
template <>
class Result<void> {
public:
    static Result ok() {
        Result r;
        r.ok_ = true;
        return r;
    }
    static Result err(HookError error) {
        Result r;
        r.ok_ = false;
        r.error_ = std::move(error);
        return r;
    }
    static Result err(HookErrorCode code, std::string message) {
        return err(HookError{code, std::move(message)});
    }

    [[nodiscard]] bool has_value() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    [[nodiscard]] const HookError& error() const noexcept { return error_; }

private:
    bool ok_{false};
    HookError error_{};
};

// ---------------------------------------------------------------------------
// Trampoline — puntatore alla funzione che richiama il codice originale.
// Restituito da install(); l'engine lo usa per invocare l'originale (Req 2.2).
// ---------------------------------------------------------------------------

class Trampoline {
public:
    Trampoline() = default;
    explicit Trampoline(void* address) noexcept : address_(address) {}

    [[nodiscard]] void* address() const noexcept { return address_; }
    [[nodiscard]] bool valid() const noexcept { return address_ != nullptr; }

    // Reinterpreta il trampolino come puntatore a funzione tipizzato per
    // invocare l'originale preservando firma, parametri e valore di ritorno.
    template <class Fn>
    [[nodiscard]] Fn as() const noexcept {
        return reinterpret_cast<Fn>(address_);
    }

private:
    void* address_{nullptr};
};

// ---------------------------------------------------------------------------
// ByteSpan — sequenza di byte di proprietà (es. il prologo originale letto da
// readOriginal, usato per il rollback persistente, Requisito 18.1).
// ---------------------------------------------------------------------------

class ByteSpan {
public:
    ByteSpan() = default;
    explicit ByteSpan(std::vector<std::uint8_t> bytes) noexcept
        : bytes_(std::move(bytes)) {}

    [[nodiscard]] const std::uint8_t* data() const noexcept { return bytes_.data(); }
    [[nodiscard]] std::uint8_t* data() noexcept { return bytes_.data(); }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] bool empty() const noexcept { return bytes_.empty(); }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]] auto begin() const noexcept { return bytes_.begin(); }
    [[nodiscard]] auto end() const noexcept { return bytes_.end(); }

private:
    std::vector<std::uint8_t> bytes_{};
};

// ---------------------------------------------------------------------------
// IHookBackend — astrazione del motore di hooking per piattaforma (Req 2.2).
// ---------------------------------------------------------------------------

class IHookBackend {
public:
    virtual ~IHookBackend() = default;

    // Installa un detour sulla funzione a `target`, restituendo un trampolino
    // verso l'originale (Req 2.2). I prologhi non banali vengono relocati dal
    // disassembler a lunghezza. La politica di retry (max 3) vive a monte,
    // nell'engine (Req 2.5).
    virtual Result<Trampoline> install(std::uintptr_t target, void* detour) = 0;

    // Rimuove l'hook installato su `target`, ripristinando il codice originale
    // della funzione (Req 2.4).
    virtual Result<void> remove(std::uintptr_t target) = 0;

    // Legge i `len` byte originali del prologo a `target` prima di qualsiasi
    // modifica, per il rollback persistente (Req 18.1).
    virtual Result<ByteSpan> readOriginal(std::uintptr_t target,
                                          std::size_t len) = 0;

    // Nome del backend (per logging/diagnostica, es. "pulse-minhook").
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    // True se il backend è operativo sulla piattaforma/architettura corrente.
    // Su build host non supportate (es. macOS per MinHook) ritorna false.
    [[nodiscard]] virtual bool available() const noexcept = 0;
};

}  // namespace pulse::hooking

#endif  // PULSE_LOADER_HOOKING_HOOK_BACKEND_HPP
