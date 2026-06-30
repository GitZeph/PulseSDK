// loader/hooking/registry_grouping.hpp — raggruppamento del registro SDK per
// Target_Address risolto (Hook_Chaining, task 7.2, Requisito 10.1).
//
// Il punto critico del wiring (design §5, "Gestione del registro SDK"):
// `pulse::hooks::find(symbol)` e `bind_trampoline(symbol, …)` toccano SOLO la
// PRIMA registrazione per quel simbolo, quindi sono insufficienti per il
// chaining, dove più registrazioni dello stesso simbolo (più mod che hookano la
// stessa funzione) devono coesistere su un'unica Underlying_Installation.
//
// Questo modulo fornisce la logica HOST-TESTABILE che, dato il registro dello
// SDK già risolto (`resolve_all(resolver)` ha marcato `resolved`/`target` di
// ciascuna registrazione), **itera l'intero registro** e **raggruppa per
// Target_Address risolto** le registrazioni `resolved`, producendo per ogni
// target la sequenza ordinata di `LinkSpec` (Chain_Order: priority DESC,
// loadOrder ASC) pronta per `HookChainRegistry::insertLink`. Le registrazioni
// non risolte non producono alcun anello (invariante "zero hook su indirizzi
// non risolti").
//
// Il cablaggio dentro `PULSE_LOADER_ARTIFACT` (`resolve_all` + chiamata a
// `insertLink` per ogni anello dei gruppi) è il task 7.5; questo modulo isola
// la sola logica di raggruppamento/ordinamento, verificabile in CI senza il
// runtime dell'artefatto.
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).
#ifndef PULSE_LOADER_HOOKING_REGISTRY_GROUPING_HPP
#define PULSE_LOADER_HOOKING_REGISTRY_GROUPING_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <pulse/hooks.hpp>

#include "hooking/hook_chain.hpp"            // pulse::hooking::ModId
#include "hooking/hook_chain_registry.hpp"   // pulse::hooking::LinkSpec

namespace pulse::hooking {

// ---------------------------------------------------------------------------
// TargetGroup — un gruppo di Hook_Link che condividono lo stesso Target_Address
// risolto, già ordinati nel Chain_Order (priority DESC, loadOrder ASC).
//
//   * `target` — Target_Address risolto (l'indirizzo della funzione bersaglio,
//     ribasato sullo slide ASLR dal resolver di `resolve_all`);
//   * `links`  — gli anelli del target, in Chain_Order, ciascuno con `detour`
//     e `slot` (Trampoline_Slot `pulse_original`) della registrazione SDK.
//
// I gruppi sono restituiti nell'ordine di PRIMA apparizione del target nel
// registro (deterministico), così l'output è ripetibile a parità di registro.
// ---------------------------------------------------------------------------
struct TargetGroup {
    std::uintptr_t        target{0};
    std::vector<LinkSpec> links;
};

// Risolve il Mod_Id proprietario della registrazione all'indice `registryIndex`
// del registro globale. Tipicamente cablato su
// `HookOwnershipLedger::ownerOfIndex` (finestre di epoca). Può essere vuoto
// (nullptr): in tal caso l'owner di ciascun `LinkSpec` resta una stringa vuota
// (il raggruppamento/ordinamento per Target_Address non dipende dall'owner).
using OwnerResolver = std::function<ModId(std::size_t registryIndex)>;

// ---------------------------------------------------------------------------
// group_registry_by_target — raggruppa per Target_Address risolto (Req 10.1).
//
// Itera l'INTERO `registry` (non `find`, che restituisce solo il primo match):
//   * salta ogni registrazione non risolta (`!resolved` o `target == nullptr`):
//     non produce alcun anello (zero hook su indirizzi non risolti);
//   * per ogni registrazione risolta costruisce un `LinkSpec` con
//       owner      = ownerOf(index) (o stringa vuota se `ownerOf` è nullo),
//       symbol     = registration.symbol,
//       priority   = registration.priority (clamp applicato a valle da HookChain),
//       loadOrder  = index nel registro (ordine di inserzione / epoca),
//       detour     = registration.detour,
//       slot       = registration.trampoline (Trampoline_Slot pulse_original);
//   * raggruppa i `LinkSpec` per Target_Address risolto e ordina ciascun gruppo
//     nel Chain_Order (priority DESC con clamp [0,1000], loadOrder ASC),
//     coerente con `HookChain::precedes`.
//
// Restituisce i gruppi nell'ordine di prima apparizione del target nel registro.
// Funzione pura: non muta `registry` né alcuno stato globale.
[[nodiscard]] std::vector<TargetGroup> group_registry_by_target(
    const std::vector<pulse::hooks::HookRegistration>& registry,
    const OwnerResolver& ownerOf = nullptr);

}  // namespace pulse::hooking

#endif  // PULSE_LOADER_HOOKING_REGISTRY_GROUPING_HPP
