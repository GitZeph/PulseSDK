// loader/lifecycle/module_loader.hpp — seam per-piattaforma per il caricamento
// di un Mod_Module nel processo (External Mod Loading, task 5.1).
//
// Questo header definisce il seam astratto `IModuleLoader` seguendo lo stesso
// pattern di `IPlatformBootstrap`: interfaccia astratta + query di
// disponibilità (`available()`) + una sola implementazione reale come primo
// deliverable (macOS arm64, fornita dal task 5.2) e gli stub delle altre
// piattaforme (Windows/Android/iOS) con `available() == false`.
//
// Requisiti coperti da questo task (5.1):
//   * Req 11.1 — interfaccia astratta che dichiara le operazioni di
//     caricamento di un Mod_Module e di risoluzione del suo entry point
//     esportato, più una query di disponibilità che riporta `true` solo quando
//     la piattaforma corrente fornisce un'implementazione reale.
//   * Req 11.2 — esattamente un'implementazione reale come primo deliverable
//     (macOS arm64): qui ne viene definito il tipo `MacOsModuleLoader`, la cui
//     logica `dlopen`/`dlsym`/`dlclose` reale è riempita dal task 5.2.
//   * Req 11.4 — Windows/Android/iOS stanno dietro lo stesso seam con
//     `available() == false`, non essendo il primo deliverable.
//
// Riuso dei tipi del codebase (firme che combaciano col design §3):
//   * `pulse::hooking::Result<T>`  — esito di una primitiva (no eccezioni).
//   * `pulse::lifecycle::ModId`    — identità della mod proprietaria.
//   * `pulse::package::Bytes`      — byte del Mod_Module (`code/module.pulsebin`
//                                     già verificato dal Mod_Manifest_Validator).
//
// Oltre al seam, questo header fornisce un `FakeModuleLoader` host-testabile
// (header-only, come `FakeBackend`) che modella un registro di simboli e i byte
// delle funzioni esportate, così discovery/validazione/ciclo di vita possono
// essere testati in CI senza l'OS loader reale.
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).
#ifndef PULSE_LOADER_LIFECYCLE_MODULE_LOADER_HPP
#define PULSE_LOADER_LIFECYCLE_MODULE_LOADER_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hooking/hook_backend.hpp"            // pulse::hooking::Result, HookError
#include "lifecycle/dependency_resolver.hpp"   // pulse::lifecycle::ModId
#include "package/pulse_package.hpp"           // pulse::package::Bytes

namespace pulse::lifecycle {

// Byte del Mod_Module caricabile (`code/module.pulsebin`). Riuso del tipo
// dell'archivio di package, già verificato a monte.
using Bytes = pulse::package::Bytes;

// Esito di una primitiva del Module_Loader: riuso di `Result<T>` dell'engine di
// hooking (stile minimale, nessuna eccezione nelle primitive del seam).
template <class T>
using ModResult = pulse::hooking::Result<T>;

// ---------------------------------------------------------------------------
// ModuleHandle — handle opaco di un Mod_Module caricato nel processo. Incapsula
// il `void*` restituito dall'OS loader (`dlopen` su macOS) e il file temporaneo
// da cui il modulo è stato caricato (per il cleanup in `unload`).
// ---------------------------------------------------------------------------
struct ModuleHandle {
    void* native{nullptr};                // handle nativo dell'OS loader
    std::filesystem::path extractedPath;  // file temp da cui è stato caricato
    [[nodiscard]] bool valid() const noexcept { return native != nullptr; }
};

// ---------------------------------------------------------------------------
// ModuleLoadError — insieme chiuso delle cause di fallimento del Module_Loader.
//   * None          — nessun errore.
//   * ExtractFailed — impossibile estrarre i byte su file temporaneo.
//   * DlopenFailed  — il caricamento del modulo (`dlopen`/equivalente) fallisce.
//   * SymbolNotFound— l'entry point dichiarato non è esportato/risolvibile.
//   * Unsupported   — la piattaforma corrente non fornisce un loader reale.
// ---------------------------------------------------------------------------
enum class ModuleLoadError { None, ExtractFailed, DlopenFailed, SymbolNotFound, Unsupported };

// Rappresentazione testuale stabile di una causa (per diagnostica e messaggi).
[[nodiscard]] std::string_view to_string(ModuleLoadError error) noexcept;

// Costruisce un `HookError` (categoria + messaggio) a partire da una causa del
// Module_Loader, così le primitive che restituiscono `ModResult<T>` possono
// veicolare la causa specifica nel messaggio mantenendo l'API `Result<T>`.
[[nodiscard]] pulse::hooking::HookError make_module_error(ModuleLoadError error,
                                                          std::string message);

// ---------------------------------------------------------------------------
// IModuleLoader — seam astratto per il caricamento di un Mod_Module (Req 11.1).
// ---------------------------------------------------------------------------
class IModuleLoader {
public:
    virtual ~IModuleLoader() = default;

    // True SOLO se la piattaforma corrente fornisce un'implementazione reale
    // (Req 11.1, 11.4). macOS arm64 (con Dobby) → true; Windows/Android/iOS →
    // false.
    [[nodiscard]] virtual bool available() const noexcept = 0;

    // Nome del loader (per logging/diagnostica, es. "pulse-module-macos").
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    // Carica i byte del Mod_Module nel processo e restituisce un handle
    // (Req 5.1). `owner` è il Mod_Id proprietario (diagnostica/nome del file
    // temporaneo); `moduleImage` sono i byte già verificati di
    // `code/module.pulsebin`.
    [[nodiscard]] virtual ModResult<ModuleHandle> load(const ModId& owner,
                                                       const Bytes& moduleImage) = 0;

    // Risolve l'entry point esportato dichiarato nel Mod_Manifest (Req 5.2/5.3).
    // Simbolo assente/non risolvibile → errore `SymbolNotFound`.
    [[nodiscard]] virtual ModResult<void*> resolveEntryPoint(const ModuleHandle& handle,
                                                             std::string_view symbol) = 0;

    // Scarica il Mod_Module dopo la rimozione degli hook (Req 8.7).
    [[nodiscard]] virtual ModResult<void> unload(ModuleHandle& handle) = 0;
};

// ---------------------------------------------------------------------------
// MacOsModuleLoader — UNICA implementazione reale del primo deliverable
// (macOS arm64, Req 11.2/11.3). In QUESTO task (5.1) è definita la sola forma
// del seam: la logica reale `dlopen(RTLD_NOW|RTLD_LOCAL)` / `dlsym` / `dlclose`
// con estrazione su file temporaneo per-sessione è implementata dal task 5.2.
//
// Finché la logica reale non è cablata, `available()` riporta `true` solo
// quando il backend Dobby è abilitato sulla piattaforma Apple (coerenza
// artefatto/backend, vedi CMake `PULSE_ENABLE_DOBBY`); altrove riporta `false`
// e le primitive degradano in modo confinato a `Unsupported`.
// ---------------------------------------------------------------------------
class MacOsModuleLoader final : public IModuleLoader {
public:
    [[nodiscard]] bool available() const noexcept override;
    [[nodiscard]] std::string_view name() const noexcept override { return "pulse-module-macos"; }
    [[nodiscard]] ModResult<ModuleHandle> load(const ModId& owner,
                                               const Bytes& moduleImage) override;
    [[nodiscard]] ModResult<void*> resolveEntryPoint(const ModuleHandle& handle,
                                                     std::string_view symbol) override;
    [[nodiscard]] ModResult<void> unload(ModuleHandle& handle) override;

private:
    // Identificatore di sessione (stabile per istanza del loader): tutti i file
    // temporanei di questa sessione vivono sotto la stessa cartella isolata
    // `~/Library/Caches/Pulse/<sessione>/`. Calcolato pigramente alla prima
    // estrazione così istanze del loader mai usate non toccano il filesystem.
    std::string sessionId_{};
};

// ---------------------------------------------------------------------------
// Stub delle piattaforme NON-Apple (Req 11.4): stesso seam, `available()`
// sempre `false`, primitive che riportano `Unsupported`. Non sono il primo
// deliverable.
// ---------------------------------------------------------------------------
class WindowsModuleLoader final : public IModuleLoader {
public:
    [[nodiscard]] bool available() const noexcept override { return false; }
    [[nodiscard]] std::string_view name() const noexcept override { return "pulse-module-windows"; }
    [[nodiscard]] ModResult<ModuleHandle> load(const ModId& owner,
                                               const Bytes& moduleImage) override;
    [[nodiscard]] ModResult<void*> resolveEntryPoint(const ModuleHandle& handle,
                                                     std::string_view symbol) override;
    [[nodiscard]] ModResult<void> unload(ModuleHandle& handle) override;
};

class AndroidModuleLoader final : public IModuleLoader {
public:
    [[nodiscard]] bool available() const noexcept override { return false; }
    [[nodiscard]] std::string_view name() const noexcept override { return "pulse-module-android"; }
    [[nodiscard]] ModResult<ModuleHandle> load(const ModId& owner,
                                               const Bytes& moduleImage) override;
    [[nodiscard]] ModResult<void*> resolveEntryPoint(const ModuleHandle& handle,
                                                     std::string_view symbol) override;
    [[nodiscard]] ModResult<void> unload(ModuleHandle& handle) override;
};

class IOsModuleLoader final : public IModuleLoader {
public:
    [[nodiscard]] bool available() const noexcept override { return false; }
    [[nodiscard]] std::string_view name() const noexcept override { return "pulse-module-ios"; }
    [[nodiscard]] ModResult<ModuleHandle> load(const ModId& owner,
                                               const Bytes& moduleImage) override;
    [[nodiscard]] ModResult<void*> resolveEntryPoint(const ModuleHandle& handle,
                                                     std::string_view symbol) override;
    [[nodiscard]] ModResult<void> unload(ModuleHandle& handle) override;
};

// ---------------------------------------------------------------------------
// make_platform_module_loader — selezione a compile-time del Module_Loader del
// target corrente (Req 11.2/11.4). macOS → MacOsModuleLoader (reale per primo);
// Windows/Android → stub; ogni altro host → MacOsModuleLoader, che fuori da
// Apple riporta `available() == false`.
// ---------------------------------------------------------------------------
[[nodiscard]] std::unique_ptr<IModuleLoader> make_platform_module_loader();

// ---------------------------------------------------------------------------
// FakeModuleLoader — implementazione host-testabile (header-only, come
// `FakeBackend`). Modella un registro di simboli e i byte delle funzioni
// esportate da un Mod_Module, senza alcun `dlopen` reale: i test programmano
// quali simboli un'immagine espone (e i relativi byte) e il fake li risolve in
// indirizzi stabili e non nulli. Consente di esercitare il caricamento, la
// risoluzione dell'entry point (anche il caso `SymbolNotFound`, Req 5.3) e lo
// scaricamento in CI.
// ---------------------------------------------------------------------------
class FakeModuleLoader final : public IModuleLoader {
public:
    // Un simbolo esportato dal modulo: nome + byte della funzione (modello
    // host-testabile della memoria della funzione). L'indirizzo risolto è
    // l'indirizzo (stabile, non nullo) del buffer dei byte.
    struct ExportedSymbol {
        std::string name;
        std::vector<std::uint8_t> bytes{0x00};  // ≥1 byte → indirizzo non nullo
    };

    // Specifica del modulo prodotto quando una data immagine viene caricata.
    struct ModuleSpec {
        std::vector<ExportedSymbol> exports;             // simboli esportati
        bool failLoad{false};                            // simula load fallita
        ModuleLoadError loadError{ModuleLoadError::DlopenFailed};
    };

    // Disponibilità simulata (default: disponibile per i test host).
    void setAvailable(bool value) noexcept { available_ = value; }

    // Programma il modulo prodotto al caricamento dell'immagine `image`. Più
    // immagini distinte possono essere programmate; un'immagine non programmata
    // viene caricata con successo ma senza simboli esportati.
    void program(Bytes image, ModuleSpec spec) {
        programmed_.push_back({std::move(image), std::move(spec)});
    }

    [[nodiscard]] bool available() const noexcept override { return available_; }
    [[nodiscard]] std::string_view name() const noexcept override { return "pulse-module-fake"; }

    // Numero di invocazioni di `load()` ricevute (host-test: verifica che il
    // re-enable NON esegua un nuovo `dlopen`, Req 7.5).
    [[nodiscard]] std::size_t loadCount() const noexcept { return loadCount_; }
    // Numero di invocazioni di `unload()` ricevute (host-test: verifica che il
    // disable NON scarichi il Mod_Module, Req 7.7).
    [[nodiscard]] std::size_t unloadCount() const noexcept { return unloadCount_; }

    [[nodiscard]] ModResult<ModuleHandle> load(const ModId& owner,
                                               const Bytes& moduleImage) override {
        ++loadCount_;
        if (!available_) {
            return ModResult<ModuleHandle>::err(
                make_module_error(ModuleLoadError::Unsupported,
                                  "FakeModuleLoader non disponibile"));
        }

        const ModuleSpec* spec = findSpec(moduleImage);
        if (spec != nullptr && spec->failLoad) {
            return ModResult<ModuleHandle>::err(
                make_module_error(spec->loadError,
                                  "FakeModuleLoader: caricamento simulato fallito per '" +
                                      owner + "'"));
        }

        auto record = std::make_unique<LoadedModule>();
        record->owner = owner;
        record->loaded = true;
        if (spec != nullptr) {
            for (const auto& sym : spec->exports) {
                record->symbols.emplace(sym.name, sym.bytes);
            }
        }

        ModuleHandle handle;
        handle.native = record.get();
        handle.extractedPath =
            std::filesystem::path("/fake/pulse/") / (owner + ".dylib");
        modules_.push_back(std::move(record));
        return ModResult<ModuleHandle>::ok(handle);
    }

    [[nodiscard]] ModResult<void*> resolveEntryPoint(const ModuleHandle& handle,
                                                     std::string_view symbol) override {
        auto* record = asRecord(handle);
        if (record == nullptr || !record->loaded) {
            return ModResult<void*>::err(
                make_module_error(ModuleLoadError::DlopenFailed,
                                  "FakeModuleLoader: handle non valido o modulo scaricato"));
        }
        auto it = record->symbols.find(std::string(symbol));
        if (it == record->symbols.end()) {
            return ModResult<void*>::err(
                make_module_error(ModuleLoadError::SymbolNotFound,
                                  "FakeModuleLoader: simbolo '" + std::string(symbol) +
                                      "' non esportato da '" + record->owner + "'"));
        }
        return ModResult<void*>::ok(static_cast<void*>(it->second.data()));
    }

    [[nodiscard]] ModResult<void> unload(ModuleHandle& handle) override {
        ++unloadCount_;
        auto* record = asRecord(handle);
        if (record == nullptr) {
            return ModResult<void>::err(
                make_module_error(ModuleLoadError::DlopenFailed,
                                  "FakeModuleLoader: unload di un handle non valido"));
        }
        record->loaded = false;
        handle.native = nullptr;
        return ModResult<void>::ok();
    }

private:
    // Record interno di un modulo caricato: registro simbolo → byte funzione.
    struct LoadedModule {
        ModId owner;
        std::unordered_map<std::string, std::vector<std::uint8_t>> symbols;
        bool loaded{false};
    };

    [[nodiscard]] LoadedModule* asRecord(const ModuleHandle& handle) noexcept {
        return static_cast<LoadedModule*>(handle.native);
    }

    [[nodiscard]] const ModuleSpec* findSpec(const Bytes& image) const noexcept {
        for (const auto& [bytes, spec] : programmed_) {
            if (bytes == image) {
                return &spec;
            }
        }
        return nullptr;
    }

    bool available_{true};
    std::vector<std::pair<Bytes, ModuleSpec>> programmed_{};
    std::vector<std::unique_ptr<LoadedModule>> modules_{};
    std::size_t loadCount_{0};
    std::size_t unloadCount_{0};
};

}  // namespace pulse::lifecycle

#endif  // PULSE_LOADER_LIFECYCLE_MODULE_LOADER_HPP
