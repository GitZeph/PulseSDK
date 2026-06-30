// loader/hooking/fake_backend.hpp — backend di hooking in-memory per i test.
//
// FakeBackend implementa IHookBackend (install/remove/readOriginal) interamente
// in memoria, senza patchare codice macchina reale. Modella una "memoria"
// simulata per ogni funzione bersaglio (byte originali + byte correnti) e
// consente di iniettare fallimenti deterministici.
//
// Scopo (task 4.2, Req 2.2): fornire un doppio di test deterministico per i
// property test su catena di hook e rollback. In particolare permette di:
//   * verificare il round-trip dei byte (readOriginal restituisce sempre il
//     prologo originale, anche dopo l'installazione);
//   * verificare il ripristino esatto su remove (rollback);
//   * forzare il fallimento di install per testare l'atomicità e il retry
//     dell'Hooking Engine (Req 2.5) senza dipendere da un backend reale.
//
// È header-only e privo di dipendenze esterne: i test possono usarlo senza
// linkare un backend di piattaforma (MinHook/Dobby/ShadowHook). Implementa
// l'interfaccia canonica `pulse::hooking::IHookBackend`.
#ifndef PULSE_LOADER_HOOKING_FAKE_BACKEND_HPP
#define PULSE_LOADER_HOOKING_FAKE_BACKEND_HPP

#include "hooking/hook_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulse::hooking {

// Backend in-memory deterministico per i test dell'Hooking Engine.
//
// Non è thread-safe: i test lo usano da un singolo thread. La generazione dei
// byte "originali" non seminati è deterministica rispetto all'indirizzo, così
// le proprietà sono ripetibili tra le esecuzioni.
class FakeBackend final : public IHookBackend {
public:
    // Byte grezzo del codice simulato.
    using Byte = std::uint8_t;
    // Buffer di byte di proprietà (rappresentazione interna della memoria).
    using Bytes = std::vector<Byte>;

    // Lunghezza di default del prologo simulato quando non è stato seminato
    // esplicitamente. Sufficiente a coprire i casi d'uso dei test.
    static constexpr std::size_t kDefaultPrologueLen = 16;

    FakeBackend() = default;

    // ---------------------------------------------------------------------
    // Configurazione della memoria simulata.
    // ---------------------------------------------------------------------

    // Semina i byte originali di un bersaglio. Reimposta lo stato del
    // bersaglio a "non installato" con i byte correnti pari agli originali.
    void seedOriginal(std::uintptr_t target, Bytes bytes) {
        Region region;
        region.original = std::move(bytes);
        region.live = region.original;
        region.installed = false;
        region.detour = nullptr;
        regions_[target] = std::move(region);
    }

    // ---------------------------------------------------------------------
    // Iniezione di fallimenti (deterministica).
    // ---------------------------------------------------------------------

    // Fa fallire ogni install sul bersaglio indicato finché non viene pulito.
    void failInstallAt(std::uintptr_t target,
                       HookErrorCode code = HookErrorCode::BackendFailure,
                       std::string message = "fake: install forzato a fallire") {
        installFailures_[target] = HookError{code, std::move(message)};
    }

    // Accoda un fallimento applicato al prossimo install (qualunque bersaglio).
    // Chiamate ripetute accodano più fallimenti consecutivi.
    void failNextInstall(HookErrorCode code = HookErrorCode::BackendFailure,
                         std::string message = "fake: install forzato a fallire") {
        queuedInstallFailures_.push_back(HookError{code, std::move(message)});
    }

    // Fa fallire tutti gli install finché disabilitato.
    void failAllInstalls(bool enabled,
                         HookErrorCode code = HookErrorCode::BackendFailure,
                         std::string message = "fake: tutti gli install falliscono") {
        if (enabled) {
            failAllInstalls_ = HookError{code, std::move(message)};
        } else {
            failAllInstalls_.reset();
        }
    }

    // Fa fallire ogni remove sul bersaglio indicato finché non viene pulito.
    void failRemoveAt(std::uintptr_t target,
                      HookErrorCode code = HookErrorCode::BackendFailure,
                      std::string message = "fake: remove forzato a fallire") {
        removeFailures_[target] = HookError{code, std::move(message)};
    }

    // Fa fallire ogni readOriginal sul bersaglio indicato finché non pulito.
    void failReadAt(std::uintptr_t target,
                    HookErrorCode code = HookErrorCode::BackendFailure,
                    std::string message = "fake: readOriginal forzato a fallire") {
        readFailures_[target] = HookError{code, std::move(message)};
    }

    // Rimuove tutti i fallimenti iniettati (per-bersaglio, accodati, globali).
    void clearInjectedFailures() {
        installFailures_.clear();
        removeFailures_.clear();
        readFailures_.clear();
        queuedInstallFailures_.clear();
        failAllInstalls_.reset();
    }

    // ---------------------------------------------------------------------
    // Introspezione per i test.
    // ---------------------------------------------------------------------

    [[nodiscard]] bool isInstalled(std::uintptr_t target) const {
        const auto it = regions_.find(target);
        return it != regions_.end() && it->second.installed;
    }

    // Numero di hook attualmente installati.
    [[nodiscard]] std::size_t installedCount() const {
        std::size_t count = 0;
        for (const auto& [addr, region] : regions_) {
            if (region.installed) ++count;
        }
        return count;
    }

    // Numero totale di tentativi di install ricevuti (riusciti o falliti).
    [[nodiscard]] std::size_t installAttempts() const noexcept {
        return installAttempts_;
    }

    // Byte correnti ("live") del bersaglio, se esiste una regione.
    [[nodiscard]] std::optional<Bytes> liveBytes(std::uintptr_t target) const {
        const auto it = regions_.find(target);
        if (it == regions_.end()) return std::nullopt;
        return it->second.live;
    }

    // Snapshot dei byte originali del bersaglio, se esiste una regione.
    [[nodiscard]] std::optional<Bytes> snapshotOriginal(std::uintptr_t target) const {
        const auto it = regions_.find(target);
        if (it == regions_.end()) return std::nullopt;
        return it->second.original;
    }

    // ---------------------------------------------------------------------
    // IHookBackend.
    // ---------------------------------------------------------------------

    Result<Trampoline> install(std::uintptr_t target, void* detour) override {
        ++installAttempts_;

        if (target == 0) {
            return Result<Trampoline>::err(
                HookErrorCode::InvalidArgument, "fake: indirizzo bersaglio nullo");
        }

        // Fallimenti iniettati: nessuna mutazione dello stato della memoria
        // simulata (atomicità del singolo tentativo di install, Req 2.5).
        if (failAllInstalls_) {
            return Result<Trampoline>::err(*failAllInstalls_);
        }
        if (!queuedInstallFailures_.empty()) {
            HookError failure = std::move(queuedInstallFailures_.front());
            queuedInstallFailures_.pop_front();
            return Result<Trampoline>::err(std::move(failure));
        }
        if (const auto it = installFailures_.find(target); it != installFailures_.end()) {
            return Result<Trampoline>::err(it->second);
        }

        Region& region = getOrCreateRegion(target);
        if (region.installed) {
            return Result<Trampoline>::err(
                HookErrorCode::AlreadyHooked, "fake: hook già installato sul bersaglio");
        }

        // Simula la patch del prologo: i byte "live" diventano uno stub di
        // detour derivato dagli originali (garantito diverso); gli originali
        // restano intatti per il rollback. La lunghezza del prologo seminato
        // viene preservata.
        region.live = makeDetourStub(region.original);
        region.installed = true;
        region.detour = detour;

        // Il trampolino "esegue" il prologo originale: nel fake è un indirizzo
        // sintetico stabile e non nullo per il bersaglio.
        void* tramp = reinterpret_cast<void*>(makeTrampolineAddress(target));
        return Result<Trampoline>::ok(Trampoline{tramp});
    }

    Result<void> remove(std::uintptr_t target) override {
        if (const auto it = removeFailures_.find(target); it != removeFailures_.end()) {
            return Result<void>::err(it->second);
        }

        const auto it = regions_.find(target);
        if (it == regions_.end() || !it->second.installed) {
            return Result<void>::err(
                HookErrorCode::NotHooked, "fake: nessun hook installato sul bersaglio");
        }

        // Ripristino esatto dei byte originali (rollback, Req 18.4).
        Region& region = it->second;
        region.live = region.original;
        region.installed = false;
        region.detour = nullptr;
        return Result<void>::ok();
    }

    Result<ByteSpan> readOriginal(std::uintptr_t target, std::size_t len) override {
        if (const auto it = readFailures_.find(target); it != readFailures_.end()) {
            return Result<ByteSpan>::err(it->second);
        }
        if (target == 0) {
            return Result<ByteSpan>::err(
                HookErrorCode::InvalidArgument, "fake: indirizzo bersaglio nullo");
        }

        // Restituisce sempre i byte ORIGINALI (mai i byte patchati), così il
        // round-trip di lettura è invariante rispetto all'installazione.
        const Region& region = ensureReadable(target, len);
        Bytes out(region.original.begin(),
                  region.original.begin() + static_cast<std::ptrdiff_t>(len));
        return Result<ByteSpan>::ok(ByteSpan{std::move(out)});
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "fake-backend";
    }

    // Il fake è sempre operativo: è pensato per girare su qualunque host di test.
    [[nodiscard]] bool available() const noexcept override { return true; }

private:
    struct Region {
        Bytes original;          // verità per il rollback (mai mutata da install)
        Bytes live;              // byte correnti (riflettono install/remove)
        bool installed = false;
        void* detour = nullptr;
    };

    // Genera in modo deterministico un byte di prologo "originale" per un dato
    // indirizzo e offset. Stabile tra le esecuzioni: i property test sono
    // così riproducibili.
    static Byte syntheticByte(std::uintptr_t target, std::size_t offset) noexcept {
        const std::uint64_t mixed =
            (static_cast<std::uint64_t>(target) + offset) * 0x9E3779B1ULL + 0x7FULL;
        return static_cast<Byte>((mixed >> 24) & 0xFFULL);
    }

    // Stub di detour derivato dagli originali: garantito diverso byte-per-byte
    // (b ^ 0xFF != b per ogni b), così i byte "live" patchati non coincidono
    // mai con quelli originali.
    static Bytes makeDetourStub(const Bytes& original) {
        Bytes stub(original.size());
        for (std::size_t i = 0; i < original.size(); ++i) {
            stub[i] = static_cast<Byte>(original[i] ^ 0xFFu);
        }
        return stub;
    }

    static std::uintptr_t makeTrampolineAddress(std::uintptr_t target) noexcept {
        // Indirizzo sintetico stabile, distinto dal bersaglio e non nullo.
        return target ^ 0xA5A5A5A5ULL;
    }

    // Restituisce la regione del bersaglio, creandola con un prologo sintetico
    // di lunghezza di default se non esiste ancora. Non altera mai una regione
    // già esistente (seminata o creata in precedenza).
    Region& getOrCreateRegion(std::uintptr_t target) {
        const auto it = regions_.find(target);
        if (it != regions_.end()) return it->second;

        Region region;
        region.original.resize(kDefaultPrologueLen);
        for (std::size_t i = 0; i < kDefaultPrologueLen; ++i) {
            region.original[i] = syntheticByte(target, i);
        }
        region.live = region.original;
        return regions_.emplace(target, std::move(region)).first->second;
    }

    // Garantisce che il bersaglio disponga di almeno `len` byte originali,
    // estendendo deterministicamente la regione se la lettura li richiede.
    Region& ensureReadable(std::uintptr_t target, std::size_t len) {
        Region& region = getOrCreateRegion(target);
        if (region.original.size() < len) {
            const std::size_t start = region.original.size();
            region.original.resize(len);
            region.live.resize(len);
            for (std::size_t i = start; i < len; ++i) {
                region.original[i] = syntheticByte(target, i);
                region.live[i] = region.installed
                                     ? static_cast<Byte>(region.original[i] ^ 0xFFu)
                                     : region.original[i];
            }
        }
        return region;
    }

    std::unordered_map<std::uintptr_t, Region> regions_{};
    std::unordered_map<std::uintptr_t, HookError> installFailures_{};
    std::unordered_map<std::uintptr_t, HookError> removeFailures_{};
    std::unordered_map<std::uintptr_t, HookError> readFailures_{};
    std::deque<HookError> queuedInstallFailures_{};
    std::optional<HookError> failAllInstalls_{};
    std::size_t installAttempts_ = 0;
};

}  // namespace pulse::hooking

#endif  // PULSE_LOADER_HOOKING_FAKE_BACKEND_HPP
