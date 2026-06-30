// tests/hook_chain_registry_byte_invariance_test.cpp — unit test del vincolo di
// ammissione dell'Hook_Gate e dell'invarianza dei byte (task 7.1, Requisiti
// 10.2, 10.3, 10.4, 10.5).
//
// Questo test consolida la verifica del vincolo di ammissione della
// HookChainRegistry e la proprietà di invarianza dei byte:
//   * Req 10.2: la Registry ammette il PRIMO anello di ogni target SOLO via
//     Hook_Gate, cioè SOLO SE il binding è risolto/verificato e il backend
//     riporta `available() == true`; l'unica Underlying_Installation è creata
//     esclusivamente in quel caso.
//   * Req 10.3: un simbolo non risolto NON aggiunge alcun anello, NON effettua
//     alcuna install e produce una diagnostica che identifica il Mod_Id e il
//     simbolo.
//   * Req 10.5: un backend non disponibile (`available() == false`) NON effettua
//     alcuna install e produce una diagnostica che nomina il backend.
//   * Req 10.4 (cuore di questo task): quando NESSUN anello raggiunge lo stato
//     Enabled, l'eseguibile e gli asset della GD_Installation restano
//     byte-per-byte identici allo stato precedente all'esecuzione del loader.
//
// I "byte dell'eseguibile e degli asset" sono modellati dalla memoria simulata
// del FakeBackend (`liveBytes` per ogni Target_Address): se la Registry non
// ammette alcun anello, il FakeBackend non viene mai installato e i byte live
// restano identici al seed pre-loader. Per i target mai toccati la regione non
// viene nemmeno creata (assenza di qualunque mutazione).

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bindings/bindings.hpp"
#include "hooking/fake_backend.hpp"
#include "hooking/hook_chain_registry.hpp"
#include "hooking/rollback_store.hpp"
#include "lifecycle/hook_ownership.hpp"

namespace {

using pulse::hooking::ChainOpOutcome;
using pulse::hooking::ChainOpResult;
using pulse::hooking::FakeBackend;
using pulse::hooking::HookChainRegistry;
using pulse::hooking::IHookBackend;
using pulse::hooking::LinkSpec;
using pulse::hooking::RollbackStore;
using pulse::lifecycle::HookOwnershipLedger;
using pulse::loader::bindings::FunctionBinding;

using Bytes = FakeBackend::Bytes;

// Backend host senza un hooking backend reale: `available() == false` (Req
// 10.5). Non muta mai alcuna memoria simulata: ogni primitiva fallisce.
class UnavailableBackend final : public IHookBackend {
public:
    pulse::hooking::Result<pulse::hooking::Trampoline> install(std::uintptr_t,
                                                               void*) override {
        return pulse::hooking::Result<pulse::hooking::Trampoline>::err(
            pulse::hooking::HookErrorCode::Unsupported, "unavailable");
    }
    pulse::hooking::Result<void> remove(std::uintptr_t) override {
        return pulse::hooking::Result<void>::err(
            pulse::hooking::HookErrorCode::Unsupported, "unavailable");
    }
    pulse::hooking::Result<pulse::hooking::ByteSpan> readOriginal(
        std::uintptr_t, std::size_t) override {
        return pulse::hooking::Result<pulse::hooking::ByteSpan>::err(
            pulse::hooking::HookErrorCode::Unsupported, "unavailable");
    }
    [[nodiscard]] std::string_view name() const noexcept override {
        return "unavailable-backend";
    }
    [[nodiscard]] bool available() const noexcept override { return false; }
};

int gDetourTag = 0;
void* fakeDetour() { return static_cast<void*>(&gDetourTag); }

FunctionBinding resolvedBinding(std::string symbol, std::uintptr_t address) {
    FunctionBinding b;
    b.symbol = std::move(symbol);
    b.address = address;
    b.resolved = true;
    return b;
}

FunctionBinding unresolvedBinding(std::string symbol, std::uintptr_t address) {
    FunctionBinding b;
    b.symbol = std::move(symbol);
    b.address = address;
    b.resolved = false;
    return b;
}

LinkSpec makeLink(std::string owner, std::string symbol, void** slot) {
    LinkSpec link;
    link.owner = std::move(owner);
    link.symbol = std::move(symbol);
    link.priority = 500;
    link.loadOrder = 0;
    link.detour = fakeDetour();
    link.slot = slot;
    return link;
}

struct Fixture {
    FakeBackend backend;
    RollbackStore rollback{std::filesystem::temp_directory_path() /
                           "pulse_chain_byte_invariance_test.bin"};
    HookOwnershipLedger ledger;
    std::vector<std::string> events;

    HookChainRegistry makeRegistry() {
        return HookChainRegistry{backend, rollback, ledger,
                                 [this](std::string_view m) {
                                     events.emplace_back(m);
                                 }};
    }

    [[nodiscard]] bool diagnosedContaining(
        std::initializer_list<std::string_view> needles) const {
        for (const auto& e : events) {
            bool all = true;
            for (const auto needle : needles) {
                if (e.find(needle) == std::string::npos) {
                    all = false;
                    break;
                }
            }
            if (all) return true;
        }
        return false;
    }

    ~Fixture() {
        std::error_code ec;
        std::filesystem::remove(rollback.path(), ec);
    }
};

// ---------------------------------------------------------------------------
// Req 10.4 — invarianza dei byte: simbolo non risolto.
//
// Si modellano i byte "dell'eseguibile" del target seminandone il prologo nel
// FakeBackend (stato pre-loader). Un anello con binding non risolto è rifiutato:
// nessuna install, e i byte live restano byte-per-byte identici al seed.
// ---------------------------------------------------------------------------
TEST(HookChainRegistryByteInvariance, UnresolvedSymbolLeavesBytesUnchanged) {
    Fixture fx;
    constexpr std::uintptr_t kTarget = 0xA000;
    const Bytes pristine{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    fx.backend.seedOriginal(kTarget, pristine);

    auto registry = fx.makeRegistry();
    void* slot = nullptr;
    LinkSpec link = makeLink("com.pulse.alpha", "MenuLayer::init", &slot);

    ChainOpResult result = registry.insertLink(
        kTarget, unresolvedBinding("MenuLayer::init", kTarget), link);

    // Req 10.3: nessun anello, nessuna install, diagnostica con Mod_Id + simbolo.
    EXPECT_EQ(result.outcome, ChainOpOutcome::Rejected);
    EXPECT_TRUE(result.chainOrder.empty());
    EXPECT_FALSE(registry.hasInstall(kTarget));
    EXPECT_EQ(registry.installCount(), 0u);
    EXPECT_EQ(fx.backend.installedCount(), 0u);
    EXPECT_EQ(fx.backend.installAttempts(), 0u);  // il gate non tocca il backend
    EXPECT_TRUE(fx.rollback.empty());
    EXPECT_TRUE(fx.diagnosedContaining({"com.pulse.alpha", "MenuLayer::init"}));

    // Req 10.4: i byte modellati del target restano identici allo stato pristine.
    const auto live = fx.backend.liveBytes(kTarget);
    ASSERT_TRUE(live.has_value());
    EXPECT_EQ(*live, pristine);
    const auto original = fx.backend.snapshotOriginal(kTarget);
    ASSERT_TRUE(original.has_value());
    EXPECT_EQ(*original, pristine);
    EXPECT_FALSE(fx.backend.isInstalled(kTarget));
}

// ---------------------------------------------------------------------------
// Req 10.4/10.5 — invarianza dei byte: backend non disponibile.
//
// Con un backend `available() == false` nessuna install avviene; poiché
// l'UnavailableBackend non modella memoria, si verifica l'invarianza tramite
// l'assenza di qualunque install/anello e la diagnostica che nomina il backend.
// ---------------------------------------------------------------------------
TEST(HookChainRegistryByteInvariance, UnavailableBackendPerformsNoInstall) {
    UnavailableBackend backend;
    RollbackStore rollback{std::filesystem::temp_directory_path() /
                           "pulse_chain_byte_invariance_unavail.bin"};
    HookOwnershipLedger ledger;
    std::vector<std::string> events;
    HookChainRegistry registry{backend, rollback, ledger,
                               [&events](std::string_view m) {
                                   events.emplace_back(m);
                               }};

    constexpr std::uintptr_t kTarget = 0xB000;
    void* slot = nullptr;
    LinkSpec link = makeLink("com.pulse.alpha", "MenuLayer::init", &slot);

    ChainOpResult result = registry.insertLink(
        kTarget, resolvedBinding("MenuLayer::init", kTarget), link);

    EXPECT_EQ(result.outcome, ChainOpOutcome::Rejected);
    EXPECT_TRUE(result.chainOrder.empty());
    EXPECT_FALSE(registry.hasInstall(kTarget));
    EXPECT_EQ(registry.installCount(), 0u);
    EXPECT_TRUE(rollback.empty());
    // Lo slot del trampolino non è cablato: nessun anello ammesso.
    EXPECT_EQ(slot, nullptr);

    // La diagnostica nomina il backend (Req 10.5).
    bool namesBackend = false;
    for (const auto& e : events) {
        if (e.find("unavailable-backend") != std::string::npos) {
            namesBackend = true;
        }
    }
    EXPECT_TRUE(namesBackend);

    std::error_code ec;
    std::filesystem::remove(rollback.path(), ec);
}

// ---------------------------------------------------------------------------
// Req 10.4 — invarianza dei byte: install del backend fallita.
//
// Se la sola Underlying_Installation fallisce, la Registry non crea alcuna
// catena (nessuna install parziale) e la memoria simulata del target resta
// invariata byte-per-byte (il FakeBackend non muta lo stato su un install
// fallito).
// ---------------------------------------------------------------------------
TEST(HookChainRegistryByteInvariance, FailedInstallLeavesBytesUnchanged) {
    Fixture fx;
    constexpr std::uintptr_t kTarget = 0xC000;
    // Si semina l'intero prologo letto dalla Registry (kPrologueBytes == 16) così
    // la regione modellata non viene estesa dalla lettura: l'invarianza dei byte
    // è confrontata sull'intero prologo.
    const Bytes pristine{0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04,
                         0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C};
    fx.backend.seedOriginal(kTarget, pristine);
    fx.backend.failInstallAt(kTarget);  // forza il fallimento dell'unica install

    auto registry = fx.makeRegistry();
    void* slot = nullptr;
    LinkSpec link = makeLink("com.pulse.alpha", "MenuLayer::init", &slot);

    ChainOpResult result = registry.insertLink(
        kTarget, resolvedBinding("MenuLayer::init", kTarget), link);

    EXPECT_EQ(result.outcome, ChainOpOutcome::Rejected);
    EXPECT_TRUE(result.chainOrder.empty());
    EXPECT_FALSE(registry.hasInstall(kTarget));  // nessuna install parziale
    EXPECT_EQ(registry.installCount(), 0u);
    EXPECT_EQ(registry.chainSize(kTarget), 0u);  // nessuna catena
    EXPECT_EQ(fx.backend.installedCount(), 0u);

    // Req 10.4: i byte del target restano identici allo stato pristine.
    const auto live = fx.backend.liveBytes(kTarget);
    ASSERT_TRUE(live.has_value());
    EXPECT_EQ(*live, pristine);
    EXPECT_FALSE(fx.backend.isInstalled(kTarget));
}

// ---------------------------------------------------------------------------
// Req 10.4 — invarianza dei byte su più target quando NESSUN anello è ammesso.
//
// Si modellano più target (eseguibile + asset) col loro stato pristine; tutti
// gli anelli sono inammissibili (binding non risolto). Al termine: zero install,
// e ogni target resta byte-per-byte identico al seed pre-loader.
// ---------------------------------------------------------------------------
TEST(HookChainRegistryByteInvariance, NoLinkEnabledKeepsAllTargetsByteIdentical) {
    Fixture fx;
    const std::vector<std::pair<std::uintptr_t, Bytes>> targets{
        {0xD000, Bytes{0x90, 0x90, 0x90, 0x90}},          // "eseguibile"
        {0xD100, Bytes{0xAA, 0xBB, 0xCC, 0xDD, 0xEE}},    // "asset" 1
        {0xD200, Bytes{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB}},  // "asset" 2
    };
    for (const auto& [addr, bytes] : targets) {
        fx.backend.seedOriginal(addr, bytes);
    }

    auto registry = fx.makeRegistry();

    // Tutti gli anelli sono rifiutati: binding non risolto su ciascun target.
    for (const auto& [addr, bytes] : targets) {
        void* slot = nullptr;
        LinkSpec link = makeLink("com.pulse.alpha", "Sym::" + std::to_string(addr),
                                 &slot);
        ChainOpResult result = registry.insertLink(
            addr, unresolvedBinding(link.symbol, addr), link);
        EXPECT_EQ(result.outcome, ChainOpOutcome::Rejected);
        EXPECT_EQ(slot, nullptr);  // slot non cablato: anello non ammesso
    }

    // Nessuna Underlying_Installation in tutto il sistema (Req 10.4).
    EXPECT_EQ(registry.installCount(), 0u);
    EXPECT_EQ(fx.backend.installedCount(), 0u);
    EXPECT_TRUE(fx.rollback.empty());

    // Ogni target resta byte-per-byte identico al seed pre-loader (Req 10.4).
    for (const auto& [addr, bytes] : targets) {
        const auto live = fx.backend.liveBytes(addr);
        ASSERT_TRUE(live.has_value());
        EXPECT_EQ(*live, bytes);
        EXPECT_FALSE(fx.backend.isInstalled(addr));
    }
}

// ---------------------------------------------------------------------------
// Req 10.2 — l'ammissione del PRIMO anello avviene SOLO via Hook_Gate (binding
// risolto + backend available()). Caso ammissibile: l'unica install è creata e
// i byte live divergono dagli originali (la patch del prologo avviene una sola
// volta), mentre gli originali persistiti restano la verità per il rollback.
// Questo controllo positivo delimita l'invarianza: i byte cambiano SOLO quando
// un anello raggiunge Enabled.
// ---------------------------------------------------------------------------
TEST(HookChainRegistryByteInvariance, AdmittedLinkIsTheOnlyCauseOfByteChange) {
    Fixture fx;
    constexpr std::uintptr_t kTarget = 0xE000;
    // Prologo completo (kPrologueBytes == 16) così originali e byte persistiti
    // nel RollbackStore coincidono con lo stato pristine senza estensioni.
    const Bytes pristine{0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
                         0x11, 0x21, 0x31, 0x41, 0x51, 0x61, 0x71, 0x81};
    fx.backend.seedOriginal(kTarget, pristine);

    auto registry = fx.makeRegistry();
    void* slot = nullptr;
    LinkSpec link = makeLink("com.pulse.alpha", "MenuLayer::init", &slot);

    ChainOpResult result = registry.insertLink(
        kTarget, resolvedBinding("MenuLayer::init", kTarget), link);

    // Anello ammesso (Req 10.2): unica install creata.
    EXPECT_EQ(result.outcome, ChainOpOutcome::CreatedInstall);
    EXPECT_TRUE(registry.hasInstall(kTarget));
    EXPECT_EQ(fx.backend.installedCount(), 1u);
    EXPECT_EQ(fx.backend.installAttempts(), 1u);

    // I byte live divergono ora dagli originali (la patch del prologo è
    // avvenuta), ma gli originali sono preservati per il rollback byte-esatto.
    const auto live = fx.backend.liveBytes(kTarget);
    const auto original = fx.backend.snapshotOriginal(kTarget);
    ASSERT_TRUE(live.has_value());
    ASSERT_TRUE(original.has_value());
    EXPECT_NE(*live, pristine);       // l'unica install ha patchato il prologo
    EXPECT_EQ(*original, pristine);   // gli originali restano lo stato pristine

    // Il RollbackStore persiste i byte originali pristine (verità per il restore).
    ASSERT_EQ(fx.rollback.size(), 1u);
    EXPECT_EQ(fx.rollback.records().front().originalBytes, pristine);
}

}  // namespace
