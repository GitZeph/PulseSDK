// loader/hooking/head_thunk.hpp — Head_Thunk stabile per-Target_Address
// (Layer 3 — Hooking Engine, Hook_Chaining, Requisiti 1.1, 1.5, 3.1, 4.3, 5.2,
// 4.5).
//
// L'unica `DobbyHook` di un Target_Address NON punta direttamente al detour
// della testa della catena, bensì a un **Head_Thunk stabile** posseduto dal
// loader: un piccolo trampolino *signature-agnostic* che esegue un salto
// indiretto verso un puntatore mutabile `currentHead`, senza toccare i registri
// degli argomenti né costruire un nuovo frame (tail-branch). Inserire o
// rimuovere il Chain_Head diventa così una **singola scrittura atomica** su
// `currentHead` — nessuna seconda `DobbyHook`, nessuna riscrittura del prologo,
// nessuna mutazione del Rollback_Store (Req 4.3, 4.5, 4.6, 5.2).
//
// Due livelli di realtà (vedi design / `docs/phase-e-real-gd-verification.md`):
//   * HOST (questo header): il "salto indiretto" è **stubbato**. `make_head_thunk`
//     restituisce un puntatore opaco che l'arnia di test host usa per leggere
//     `currentHead` e invocarlo come puntatore a funzione di firma di prova,
//     così la correttezza del relinking della HookChainRegistry è verificabile
//     in CI senza un backend reale.
//   * REALE (Fase E): i sorgenti assembly per-architettura
//     (`head_thunk_arm64.S` / `head_thunk_x86_64.S`, task 7.4) realizzano il
//     ramo reale del salto indiretto verso `currentHead`; `make_head_thunk` li
//     userà dietro l'opzione CMake (default OFF). Coperto manualmente su GD
//     reale, non da PBT.
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).
// Header-only host-stub: nessuna dipendenza dal backend reale.
#ifndef PULSE_LOADER_HOOKING_HEAD_THUNK_HPP
#define PULSE_LOADER_HOOKING_HEAD_THUNK_HPP

#include <atomic>

namespace pulse::hooking {

// ---------------------------------------------------------------------------
// HeadCell — stato per-Target_Address letto dall'Head_Thunk.
//
// `currentHead` è il puntatore mutabile alla testa corrente della catena
// (== Chain_Head.detour). La scrittura è atomica con semantica *release* e la
// lettura dal thunk osserva sempre un puntatore mai "lacerato" (Req 9.1, 9.2),
// così una dispatch in volo non vede mai uno stato intermedio.
// ---------------------------------------------------------------------------
struct HeadCell {
    std::atomic<void*> currentHead{nullptr};  // == Chain_Head.detour (Req 4.3, 5.2)
};

// ---------------------------------------------------------------------------
// make_head_thunk — alloca/ottiene un Head_Thunk eseguibile associato a `cell`.
//
// Il thunk:
//   arm64 : carica `cell.currentHead` in x16 e `br x16` (x0..x7/v0..v7 intatti);
//   x86-64: `jmp *currentHead` (rdi,rsi,rdx,rcx,r8,r9 e xmm0..xmm7 intatti).
// Restituisce un puntatore funzione opaco da passare a `DobbyHook` come detour
// dell'unica Underlying_Installation. NON costruisce un frame: inoltra qualunque
// firma in modo trasparente (tail-branch).
//
// Due rami selezionati a compile-time dall'opzione CMake `PULSE_ENABLE_HEAD_THUNK_ASM`
// (default OFF, dietro l'artefatto Dobby), che definisce la macro
// `PULSE_HEAD_THUNK_ASM`:
//
//   * RAMO REALE (`PULSE_HEAD_THUNK_ASM` definita, task 7.4 / Fase E): la
//     definizione vive in `head_thunk.cpp` e copia il template assembly
//     per-architettura (`head_thunk_arm64.S` / `head_thunk_x86_64.S`) in memoria
//     eseguibile, patchando il puntatore alla `HeadCell`; restituisce un thunk
//     che fa un salto indiretto signature-agnostic verso `currentHead`. Coperto
//     manualmente su GD reale in Fase E, non da PBT.
//
//   * HOST-STUB (default, opzione OFF): restituisce un puntatore opaco a `cell`
//     che l'arnia di test host risolve leggendo `cell.currentHead` e invocandolo
//     come puntatore a funzione di firma di prova; il relinking della catena
//     resta verificabile in CI senza un backend reale.
#if defined(PULSE_HEAD_THUNK_ASM)
// Ramo reale: definito in head_thunk.cpp (non-inline, dietro l'opzione CMake).
[[nodiscard]] void* make_head_thunk(HeadCell& cell) noexcept;
#else
[[nodiscard]] inline void* make_head_thunk(HeadCell& cell) noexcept {
    // HOST-STUB: espone la `HeadCell` come "thunk" opaco. L'arnia di test legge
    // `currentHead` da questo puntatore e lo invoca.
    return static_cast<void*>(&cell);
}
#endif

// ---------------------------------------------------------------------------
// set_head — aggiorna atomicamente la testa corrente (cambio di Chain_Head).
//
// Nessuna re-installazione, nessuna riscrittura del prologo, nessuna mutazione
// del Rollback_Store: il cambio di testa è una singola scrittura *release*
// (Req 4.3, 5.2).
// ---------------------------------------------------------------------------
inline void set_head(HeadCell& cell, void* headDetour) noexcept {
    cell.currentHead.store(headDetour, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// store_slot / load_slot — accesso atomico ai Trampoline_Slot (`void**`).
//
// I Trampoline_Slot (`pulse_original`) sono semplici `void**` esterni di
// proprietà delle registrazioni SDK, non `std::atomic`. La HookChainRegistry,
// quando ri-cabla i vicini al cambio di catena (insert/remove/relink/teardown),
// scrive il puntatore al successivo anello (o al Real_Trampoline per la coda)
// con uno **store atomico a semantica release**, e la dispatch in volo legge lo
// slot con un **load a semantica acquire**: così una dispatch che inoltra lungo
// la catena non osserva mai un puntatore *lacerato* (Req 9.1). `std::atomic_ref`
// (C++20) impone la semantica atomica sulla memoria esterna senza cambiarne il
// tipo né l'ABI dello slot, coerentemente con la scrittura *release* di
// `currentHead` (`set_head`).
//
// NOTA sul modello di concorrenza (Requisito 9): il design è pragmatico e
// onesto, senza schema lock-free. La garanzia atomica qui copre il solo
// **non-lacceramento** del puntatore (Req 9.1). L'effetto-da-invocazione-
// successiva (Req 9.2) e l'assenza di stato parzialmente modificato (Req 9.3)
// poggiano sull'assunzione di **serializzazione single-thread di early-load**
// (vedi `hook_ownership.hpp`): i caricamenti delle mod — e quindi ogni mutazione
// strutturale della catena — sono serializzati sul singolo thread di early-load
// (ordine `LoadPlan.order`) e protetti dallo `std::mutex` strutturale della
// HookChainRegistry, mentre Geometry Dash invoca gli hook sul Game_Thread. Una
// dispatch in volo prosegue sulla vista che ha già caricato (la testa è letta
// una volta all'ingresso via Head_Thunk; gli slot inoltrano lungo la catena
// esistente) e una mutazione ha effetto a partire da una invocazione
// successiva, senza essere alterata. NON si tenta una sincronizzazione più
// forte (RCU/hazard pointer): non corrisponde al livello di concorrenza reale
// del runtime.
inline void store_slot(void** slot, void* value) noexcept {
    std::atomic_ref<void*>(*slot).store(value, std::memory_order_release);
}

[[nodiscard]] inline void* load_slot(void** slot) noexcept {
    return std::atomic_ref<void*>(*slot).load(std::memory_order_acquire);
}

}  // namespace pulse::hooking

#endif  // PULSE_LOADER_HOOKING_HEAD_THUNK_HPP
