// loader/hooking/head_thunk_x86_64.S — Head_Thunk reale x86-64 (Layer 3,
// Hook_Chaining, Requisiti 4.3, 5.2, 4.5).
//
// Salto indiretto *signature-agnostic* (tail-branch) verso il puntatore mutabile
// `currentHead` di un Target_Address. L'unica `DobbyHook` del bersaglio punta a
// una copia di questo template; cambiare la testa della catena è una singola
// scrittura atomica su `currentHead`, senza seconda `DobbyHook` né riscrittura
// del prologo (Req 4.3, 5.2).
//
// Semantica (Req 4.3, 5.2):
//   - `movabs` carica in %rax l'indirizzo della cella `currentHead` (immediato a
//     64 bit, patchato a runtime da `make_head_thunk`);
//   - `jmp *(%rax)` è un salto di coda indiretto verso `currentHead`
//     (== Chain_Head.detour, offset 0 della HeadCell): NON costruisce un nuovo
//     frame e NON altera i registri argomento `rdi, rsi, rdx, rcx, r8, r9`, i
//     vettoriali `xmm0..xmm7` né lo stack, così qualunque firma del bersaglio
//     viene inoltrata in modo trasparente. (%rax è caller-saved e non è un
//     registro argomento nella SysV ABI: clobberarlo è conforme.)
//
// È un *template*: `make_head_thunk` ne copia i byte da
// [pulse_head_thunk_x86_64_tpl, pulse_head_thunk_x86_64_tpl_end) in memoria
// eseguibile e sovrascrive l'immediato a 64 bit a
// `pulse_head_thunk_x86_64_tpl_patch` con `&cell.currentHead`.
//
// Logica originale Pulse (Requisito 27). Compilato solo dietro l'opzione CMake
// `PULSE_ENABLE_HEAD_THUNK_ASM` (default OFF, dietro l'artefatto Dobby); il ramo
// reale è verificato manualmente su GD reale in Fase E, non in CI.

#if defined(__x86_64__) || defined(_M_X64)

// Mach-O (Apple) antepone un underscore ai simboli C; ELF (Linux) no.
#if defined(__APPLE__)
#define PULSE_SYM(name) _##name
#else
#define PULSE_SYM(name) name
#endif

    .text
    .p2align 4

    .globl PULSE_SYM(pulse_head_thunk_x86_64_tpl)
    .globl PULSE_SYM(pulse_head_thunk_x86_64_tpl_patch)
    .globl PULSE_SYM(pulse_head_thunk_x86_64_tpl_end)

PULSE_SYM(pulse_head_thunk_x86_64_tpl):
    movabsq $0, %rax                // %rax = &cell.currentHead (imm64 patchato)
    // L'immediato a 64 bit occupa gli ultimi 8 byte dell'istruzione `movabs`.
    .set PULSE_SYM(pulse_head_thunk_x86_64_tpl_patch), . - 8
    jmpq    *(%rax)                 // jmp *currentHead: arg regs/xmm/stack intatti
PULSE_SYM(pulse_head_thunk_x86_64_tpl_end):

// Niente stack eseguibile (hardening del linker su ELF).
#if defined(__linux__)
    .section .note.GNU-stack,"",%progbits
#endif

#endif  // __x86_64__
