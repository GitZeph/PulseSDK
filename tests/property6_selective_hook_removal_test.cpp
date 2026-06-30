// tests/property6_selective_hook_removal_test.cpp
// Feature: pulse-sdk, Property 6 — Rimozione selettiva degli hook.
// Validates: Requirements 2.4 (Requisiti 2.4)
//
// Property 6 (design.md / Req 2.4): per ogni insieme randomizzato di hook
// posseduti da più mod su più funzioni bersaglio, chiamare `remove(owner)`
// sull'Hooking Engine:
//   (a) rimuove SOLO gli hook di quella mod — `hooksForOwner(owner) == 0`
//       dopo la rimozione;
//   (b) lascia INTATTI tutti gli hook delle altre mod — i loro conteggi
//       `hooksForOwner(o)` restano invariati e il totale cala esattamente del
//       numero di hook posseduti dalla mod rimossa;
//   (c) ripristina il detour sul backend di una funzione SOLO quando la sua
//       catena resta vuota (cioè la mod rimossa era l'unica a possedere hook
//       su quella funzione); le funzioni ancora hookate da altre mod restano
//       installate sul backend.
//
// Strategia (RapidCheck, ≥100 iterazioni di default):
//   * si genera un numero di mod e di funzioni bersaglio, poi un insieme
//     randomizzato di hook (owner, target, priority); ogni hook riceve un
//     `loadOrder` univoco (l'indice) per un ordine totale deterministico;
//   * si installano tutti gli hook tramite un HookEngine sostenuto da un
//     FakeBackend in-memory (nessun fallimento iniettato: tutti riescono);
//   * si calcola lo stato atteso direttamente dall'insieme generato
//     (conteggi per mod, insieme di proprietari per funzione);
//   * si rimuove una mod scelta a caso e si verificano gli invarianti (a)–(c)
//     contro lo stato atteso.
//
// L'engine installa il detour sul backend UNA sola volta per funzione (al
// primo hook) e ripristina i byte originali solo quando l'ultima catena di
// quella funzione si svuota: la rimozione selettiva è quindi osservabile sia
// dall'introspezione dell'engine sia dallo stato del FakeBackend.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "hooking/fake_backend.hpp"
#include "hooking/hook_engine.hpp"

namespace {

using pulse::hooking::FakeBackend;
using pulse::hooking::HookEngine;
using pulse::hooking::HookRequest;
using pulse::hooking::ModId;

int g_detour = 0;
void* const kDetour = &g_detour;

// Descrittore di un hook generato: indice mod proprietaria, indice funzione
// bersaglio e priorità grezza. Il loadOrder è assegnato in modo univoco dal
// test (indice di inserimento) per un ordine totale deterministico (Req 3.3).
struct HookSpec {
    int ownerIdx;
    int targetIdx;
    int priority;
};

// Identità mod stabile a partire dall'indice.
ModId ownerName(int idx) { return "mod" + std::to_string(idx); }

// Indirizzo di funzione bersaglio stabile, non nullo e distinto per indice.
std::uintptr_t targetAddr(int idx) {
    return static_cast<std::uintptr_t>(0x1000 + idx * 0x40);
}

// --- Property 6 — rimozione selettiva degli hook --------------------------
// Feature: pulse-sdk, Property 6. Validates: Requirements 2.4.
RC_GTEST_PROP(Property6SelectiveHookRemoval,
              RemoveOwnerRemovesOnlyThatModAndRestoresEmptiedTargets,
              ()) {
    // Numero di mod e di funzioni bersaglio coinvolte.
    const int numOwners = *rc::gen::inRange(1, 6).as("numero di mod");
    const int numTargets = *rc::gen::inRange(1, 6).as("numero di funzioni");

    // Insieme randomizzato di hook vincolato ai domini sopra. La dimensione è
    // libera (anche vuota) per esercitare il caso degenere.
    const auto specs = *rc::gen::container<std::vector<HookSpec>>(
        rc::gen::construct<HookSpec>(
            rc::gen::inRange(0, numOwners),
            rc::gen::inRange(0, numTargets),
            rc::gen::inRange(0, 1001)))
                            .as("insieme di hook (owner, target, priority)");

    // Mod da rimuovere, scelta a caso tra quelle possibili.
    const int removeIdx = *rc::gen::inRange(0, numOwners).as("mod da rimuovere");

    FakeBackend backend;
    HookEngine engine{backend};

    // Stato atteso ricavato dall'insieme generato.
    std::map<int, std::size_t> ownerCount;          // hook per mod
    std::map<int, std::set<int>> targetOwners;      // proprietari per funzione

    // Installa tutti gli hook; con backend senza fallimenti ognuno riesce.
    std::uint64_t loadOrder = 0;
    for (const HookSpec& s : specs) {
        HookRequest req;
        req.owner = ownerName(s.ownerIdx);
        req.functionName = "fn" + std::to_string(s.targetIdx);
        req.target = targetAddr(s.targetIdx);
        req.detour = kDetour;
        req.priority = s.priority;
        req.loadOrder = loadOrder++;

        const auto outcome = engine.install(req);
        RC_ASSERT(outcome.installed);

        ownerCount[s.ownerIdx] += 1;
        targetOwners[s.targetIdx].insert(s.ownerIdx);
    }

    // Sanity: lo stato installato riflette esattamente l'insieme generato.
    RC_ASSERT(engine.totalHooks() == specs.size());
    RC_ASSERT(engine.installedTargets() == targetOwners.size());
    for (const auto& [ownerIdx, count] : ownerCount) {
        RC_ASSERT(engine.hooksForOwner(ownerName(ownerIdx)) == count);
    }

    // Conteggi attesi per la rimozione della mod scelta.
    const std::size_t removedCount =
        ownerCount.count(removeIdx) ? ownerCount[removeIdx] : 0u;

    std::size_t expectedRestored = 0;
    for (const auto& [targetIdx, owners] : targetOwners) {
        if (owners.count(removeIdx) && owners.size() == 1) {
            // L'unica proprietaria era la mod rimossa: la catena si svuota e il
            // detour viene ripristinato (Req 2.4).
            ++expectedRestored;
        }
    }

    // --- Rimozione selettiva ---------------------------------------------
    const auto outcome = engine.remove(ownerName(removeIdx));

    // Nessun fallimento di ripristino del backend (nessuno iniettato).
    RC_ASSERT(outcome.ok());

    // (a) Sono rimossi esattamente gli hook della mod rimossa.
    RC_ASSERT(outcome.hooksRemoved == removedCount);
    RC_ASSERT(engine.hooksForOwner(ownerName(removeIdx)) == 0u);

    // (c) Sono ripristinate solo le funzioni la cui catena è rimasta vuota.
    RC_ASSERT(outcome.targetsRestored == expectedRestored);

    // (b) Gli hook delle altre mod restano intatti, conteggi invariati.
    for (const auto& [ownerIdx, count] : ownerCount) {
        if (ownerIdx == removeIdx) continue;
        RC_ASSERT(engine.hooksForOwner(ownerName(ownerIdx)) == count);
    }

    // Il totale cala esattamente del numero di hook della mod rimossa.
    RC_ASSERT(engine.totalHooks() == specs.size() - removedCount);

    // (c) Stato per-funzione: una funzione resta hookata sse esisteva almeno
    // una proprietaria diversa dalla mod rimossa; altrimenti è ripristinata sul
    // backend. Engine e FakeBackend devono concordare.
    for (const auto& [targetIdx, owners] : targetOwners) {
        std::set<int> remaining = owners;
        remaining.erase(removeIdx);
        const bool stillHooked = !remaining.empty();
        const std::uintptr_t addr = targetAddr(targetIdx);

        RC_ASSERT(engine.isTargetInstalled(addr) == stillHooked);
        RC_ASSERT(backend.isInstalled(addr) == stillHooked);

        if (!stillHooked) {
            // La funzione ripristinata ha i byte live pari agli originali.
            const auto live = backend.liveBytes(addr);
            const auto orig = backend.snapshotOriginal(addr);
            RC_ASSERT(live.has_value());
            RC_ASSERT(orig.has_value());
            RC_ASSERT(*live == *orig);
        }
    }
}

}  // namespace
