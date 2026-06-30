// loader/hooking/head_thunk_arm64.S — Head_Thunk reale arm64 (Layer 3,
// Hook_Chaining, Requisiti 4.3, 5.2, 4.5).
//
// Salto indiretto *signature-agnostic* (tail-branch) verso il puntatore mutabile
// `currentHead` di un Target_Address. L'unica `DobbyHook` del bersaglio punta a
// una copia di questo template; cambiare la testa della catena è una singola
// scrittura atomica su `currentHead`, senza seconda `DobbyHook` né riscrittura
// del prologo (Req 4.3, 5.2).
//
// Semantica (Req 4.3, 5.2):
//   - carica in x16 l'indirizzo della cella `currentHead` (literal embedded,
//     patchato a runtime da `make_head_thunk`);
//   - carica in x16 il valore di `currentHead` (== Chain_Head.detour, offset 0
//     della HeadCell);
//   - `br x16` cede il controllo SENZA costruire un nuovo frame, lasciando
//     INTATTI i registri argomento intero `x0..x7`, i registri vettoriali
//     `v0..v7` e lo stack (SP/LR), così qualunque firma del bersaglio viene
//     inoltrata in modo trasparente.
//
// È un *template* posizione-relativa: `make_head_thunk` ne copia i byte da
// [pulse_head_thunk_arm64_tpl, pulse_head_thunk_arm64_tpl_end) in memoria
// eseguibile e sovrascrive il literal a `pulse_head_thunk_arm64_tpl_patch` con
// `&cell.currentHead`. Il `ldr` literal è PC-relativo: copiando il blocco
// contiguo la distanza relativa è preservata.
//
// Logica originale Pulse (Requisito 27). Compilato solo dietro l'opzione CMake
// `PULSE_ENABLE_HEAD_THUNK_ASM` (default OFF, dietro l'artefatto Dobby); il ramo
// reale è verificato manualmente su GD reale in Fase E, non in CI.

#if defined(__aarch64__) || defined(__arm64__)

// Mach-O (Apple) antepone un underscore ai simboli C; ELF (Linux/Android) no.
#if defined(__APPLE__)
#define PULSE_SYM(name) _##name
#else
#define PULSE_SYM(name) name
#endif

    .text
    .p2align 4

    .globl PULSE_SYM(pulse_head_thunk_arm64_tpl)
    .globl PULSE_SYM(pulse_head_thunk_arm64_tpl_patch)
    .globl PULSE_SYM(pulse_head_thunk_arm64_tpl_end)

PULSE_SYM(pulse_head_thunk_arm64_tpl):
    ldr     x16, Lpulse_head_cell   // x16 = &cell.currentHead (literal patchato)
    ldr     x16, [x16]              // x16 = currentHead (== Chain_Head.detour)
    br      x16                     // tail-branch: x0..x7/v0..v7 e stack intatti

    .p2align 3
Lpulse_head_cell:
PULSE_SYM(pulse_head_thunk_arm64_tpl_patch):
    .quad   0                       // patchato con &cell.currentHead a runtime
PULSE_SYM(pulse_head_thunk_arm64_tpl_end):

// Niente stack eseguibile (hardening del linker su ELF).
#if defined(__linux__)
    .section .note.GNU-stack,"",%progbits
#endif

#endif  // __aarch64__
