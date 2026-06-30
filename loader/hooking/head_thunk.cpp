// loader/hooking/head_thunk.cpp — ramo REALE di `make_head_thunk` (Layer 3,
// Hook_Chaining, Requisiti 4.3, 5.2, 4.5).
//
// Questa unità di traduzione ha corpo SOLO quando l'opzione CMake
// `PULSE_ENABLE_HEAD_THUNK_ASM` è attiva (macro `PULSE_HEAD_THUNK_ASM`); in caso
// contrario `make_head_thunk` è l'host-stub `inline` di `head_thunk.hpp` e questo
// file è vuoto, così la build host statica (CI, default OFF) resta invariata e
// non c'è doppia definizione del simbolo.
//
// Quando attiva, copia il *template* assembly per-architettura
// (`head_thunk_arm64.S` / `head_thunk_x86_64.S`) in memoria eseguibile e patcha
// il puntatore embedded con `&cell.currentHead`, restituendo un Head_Thunk
// stabile signature-agnostic (tail-branch verso `currentHead`). Il ramo reale è
// verificato manualmente su GD reale in Fase E (non in CI / non da PBT).
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).

#include "head_thunk.hpp"

#if defined(PULSE_HEAD_THUNK_ASM)

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>  // sys_icache_invalidate
#include <pthread.h>                 // pthread_jit_write_protect_np (arm64e/JIT)
#endif

namespace pulse::hooking {

namespace {

// Simboli del template assembly per-architettura (definiti nei .S). Marcati
// `extern "C"` così non subiscono name mangling; sono usati solo come marcatori
// di indirizzo per delimitare/patchare il template.
extern "C" {
#if defined(__aarch64__) || defined(__arm64__)
extern const unsigned char pulse_head_thunk_arm64_tpl[];
extern const unsigned char pulse_head_thunk_arm64_tpl_patch[];
extern const unsigned char pulse_head_thunk_arm64_tpl_end[];
#elif defined(__x86_64__)
extern const unsigned char pulse_head_thunk_x86_64_tpl[];
extern const unsigned char pulse_head_thunk_x86_64_tpl_patch[];
extern const unsigned char pulse_head_thunk_x86_64_tpl_end[];
#endif
}

#if defined(__aarch64__) || defined(__arm64__)
constexpr const unsigned char* kTplBegin = pulse_head_thunk_arm64_tpl;
constexpr const unsigned char* kTplPatch = pulse_head_thunk_arm64_tpl_patch;
constexpr const unsigned char* kTplEnd = pulse_head_thunk_arm64_tpl_end;
#elif defined(__x86_64__)
constexpr const unsigned char* kTplBegin = pulse_head_thunk_x86_64_tpl;
constexpr const unsigned char* kTplPatch = pulse_head_thunk_x86_64_tpl_patch;
constexpr const unsigned char* kTplEnd = pulse_head_thunk_x86_64_tpl_end;
#else
#error "PULSE_HEAD_THUNK_ASM abilitato su un'architettura senza sorgente Head_Thunk (atteso arm64 o x86-64)"
#endif

}  // namespace

// ---------------------------------------------------------------------------
// make_head_thunk (ramo reale) — alloca un Head_Thunk eseguibile per `cell`.
//
// 1) copia [kTplBegin, kTplEnd) in una pagina eseguibile;
// 2) patcha il literal embedded (a `kTplPatch`) con `&cell.currentHead` così il
//    thunk legge a runtime la testa corrente della catena (Req 4.3, 5.2);
// 3) rende la regione R+X e invalida la I-cache (arm64).
//
// Restituisce il puntatore d'ingresso al thunk (da passare a `DobbyHook` come
// detour dell'unica Underlying_Installation), oppure `nullptr` in caso di
// fallimento dell'allocazione (il chiamante tratta il fallimento come gate
// negato — nessuna install parziale).
// ---------------------------------------------------------------------------
[[nodiscard]] void* make_head_thunk(HeadCell& cell) noexcept {
    const std::size_t tplSize = static_cast<std::size_t>(kTplEnd - kTplBegin);
    const std::size_t patchOff = static_cast<std::size_t>(kTplPatch - kTplBegin);

    const long pageSizeRaw = ::sysconf(_SC_PAGESIZE);
    const std::size_t pageSize =
        pageSizeRaw > 0 ? static_cast<std::size_t>(pageSizeRaw) : std::size_t{4096};
    const std::size_t allocSize = ((tplSize + pageSize - 1) / pageSize) * pageSize;

    // Pagina W+X mappata privata e anonima. Su arm64 Apple (hardened runtime)
    // MAP_JIT richiede l'entitlement com.apple.security.cs.allow-jit e il toggle
    // pthread_jit_write_protect_np: la verifica end-to-end è in Fase E sul
    // bundle reale di GD.
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && defined(MAP_JIT)
    flags |= MAP_JIT;
#endif
    void* mem = ::mmap(nullptr, allocSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                       flags, -1, 0);
    if (mem == MAP_FAILED) {
        return nullptr;
    }

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    pthread_jit_write_protect_np(0);  // abilita la scrittura sulla pagina JIT
#endif

    // Copia il template e patcha il puntatore alla cella (offset 0 == currentHead).
    std::memcpy(mem, kTplBegin, tplSize);
    void* cellPtr = static_cast<void*>(&cell.currentHead);
    std::memcpy(static_cast<unsigned char*>(mem) + patchOff, &cellPtr, sizeof(cellPtr));

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    pthread_jit_write_protect_np(1);  // ripristina la protezione in esecuzione
#endif

#if defined(__APPLE__)
    sys_icache_invalidate(mem, tplSize);  // coerenza I-cache dopo la scrittura
#elif defined(__GNUC__)
    __builtin___clear_cache(static_cast<char*>(mem),
                            static_cast<char*>(mem) + tplSize);
#endif

    return mem;
}

}  // namespace pulse::hooking

#endif  // PULSE_HEAD_THUNK_ASM
