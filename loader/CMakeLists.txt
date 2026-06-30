# Pulse Loader — runtime nel processo di Geometry Dash (Layer 0-4, 6)
# Sottocartelle: bootstrap/ core/ bindings/ hooking/ lifecycle/
#
# Per il MVP il loader è compilato come libreria statica. I moduli reali
# (bootstrap per piattaforma, detection, hooking engine, lifecycle) verranno
# aggiunti dalle attività successive; questo file definisce il target e ne
# garantisce la compilazione su Windows/macOS/Linux (Requisito 26.3).

file(GLOB_RECURSE PULSE_LOADER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/bootstrap/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bindings/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/hooking/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/lifecycle/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/mvp/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/surface/*.cpp"
)

# ---------------------------------------------------------------------------
# Loader_Artifact dinamico vs libreria statica (Requisito 1).
#
# Di default il loader è una libreria STATICA: i test host continuano a linkare
# `pulse::loader` senza produrre alcun artefatto caricabile (Req 1.3). Con
# `PULSE_BUILD_LOADER_ARTIFACT=ON` il loader diventa invece il Loader_Artifact
# dinamico (MODULE) caricabile da Geometry Dash (`.dylib`/`.dll`/`.so`).
#
# Le due uscite sono MUTUAMENTE ESCLUSIVE (Req 1.1): l'`if/else` produce una
# sola libreria, mai entrambe nella stessa configurazione.
# ---------------------------------------------------------------------------
option(PULSE_BUILD_LOADER_ARTIFACT
       "Produce il Loader_Artifact dinamico (.dylib/.dll/.so) anziche' la lib statica" OFF)

if(PULSE_BUILD_LOADER_ARTIFACT)
    # Validazione del target PRIMA di creare qualunque cosa (Req 1.4): su un
    # target non supportato la configurazione si interrompe con OS + causa via
    # `pulse_abort` (FATAL_ERROR), quindi la generazione non prosegue e NON
    # viene prodotto alcun artefatto su disco.
    if(NOT (APPLE OR WIN32 OR ANDROID))
        pulse_abort("PULSE_BUILD_LOADER_ARTIFACT non supportato su questo target \
(attesi: macOS/.dylib, Windows/.dll, Android/.so)")
    endif()

    # SHARED: vera libreria dinamica (MH_DYLIB). Necessaria perché Geometry Dash
    # la carichi tramite un load command `LC_LOAD_DYLIB` (early-load): dyld NON
    # può linkare un MODULE/bundle (MH_BUNDLE) come dipendenza — fallisce con
    # "cannot link against bundle". Il MODULE sarebbe caricabile solo via dlopen.
    add_library(pulse_loader SHARED ${PULSE_LOADER_SOURCES})

    # Formato del file dell'artefatto per piattaforma (Req 1.1, 1.6):
    #   macOS/iOS  -> libpulse_loader.dylib
    #   Windows    -> pulse_loader.dll
    #   Android    -> libpulse_loader.so
    if(APPLE)
        set_target_properties(pulse_loader PROPERTIES
            PREFIX "lib" OUTPUT_NAME "pulse_loader" SUFFIX ".dylib")
        # Install name = @loader_path/libpulse_loader.dylib, identico al path del
        # load command `LC_LOAD_DYLIB` iniettato dall'Installer nell'eseguibile
        # di GD: dyld risolve la dylib accanto all'eseguibile. Forziamo questo id
        # già nell'artefatto del build tree (non solo all'install).
        set_target_properties(pulse_loader PROPERTIES
            BUILD_WITH_INSTALL_NAME_DIR ON
            INSTALL_NAME_DIR "@loader_path")
    elseif(WIN32)
        set_target_properties(pulse_loader PROPERTIES
            PREFIX "" OUTPUT_NAME "pulse_loader" SUFFIX ".dll")
    elseif(ANDROID)
        set_target_properties(pulse_loader PROPERTIES
            PREFIX "lib" OUTPUT_NAME "pulse_loader" SUFFIX ".so")
    endif()

    # Visibilità nascosta di default: viene esportato SOLO il simbolo C
    # `pulse_loader_runtime_entry` (marcato `visibility("default")` /
    # `dllexport` nell'header pubblico), come richiesto dal Req 1.2.
    set_target_properties(pulse_loader PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)

    # Attiva il path artefatto nei sorgenti (entry point esportato + costruttore
    # di early-load nei bootstrap) e linka l'API pubblica dello SDK.
    target_compile_definitions(pulse_loader PRIVATE PULSE_LOADER_ARTIFACT=1)
    target_link_libraries(pulse_loader PRIVATE pulse::sdk)

    # -----------------------------------------------------------------------
    # Embedding del Binding_Set_File verificato a COMPILE TIME.
    #
    # Il runtime entry (loader/core/runtime_entry.cpp) non deve fare alcun
    # lookup su file dopo l'iniezione in Geometry Dash: incorporiamo qui il
    # contenuto di `mod-index/bindings/2.2081/macos-arm64.pbind` in un header
    # generato (`pulse_embedded_pbind.hpp`) che espone
    # `pulse::loader::kEmbeddedPbind`. Se il `.pbind` è assente al configure
    # time, incorporiamo una stringa vuota: il flusso di runtime fallisce in
    # modo fail-open (zero hook). Questo blocco è ESCLUSIVAMENTE nel ramo
    # PULSE_BUILD_LOADER_ARTIFACT, così la build statica host resta invariata.
    # -----------------------------------------------------------------------
    set(_pulse_pbind_file
        "${CMAKE_SOURCE_DIR}/mod-index/bindings/2.2081/macos-arm64.pbind")
    if(EXISTS "${_pulse_pbind_file}")
        file(READ "${_pulse_pbind_file}" PULSE_EMBEDDED_PBIND_CONTENT)
        message(STATUS "Pulse: incorporo il .pbind verificato da ${_pulse_pbind_file}")
    else()
        set(PULSE_EMBEDDED_PBIND_CONTENT "")
        message(WARNING "Pulse: .pbind ${_pulse_pbind_file} assente; incorporo "
                        "stringa vuota (runtime fail-open, zero hook).")
    endif()

    set(_pulse_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/pulse_embedded_pbind.hpp.in"
        "${_pulse_generated_dir}/pulse_embedded_pbind.hpp"
        @ONLY)
    target_include_directories(pulse_loader PRIVATE "${_pulse_generated_dir}")

    message(STATUS "Pulse: Loader_Artifact dinamico abilitato \
(MODULE, simbolo esportato 'pulse_loader_runtime_entry').")
    message(STATUS "Pulse: su host macOS configurare/buildare sotto `env -u LDFLAGS` \
(evita che un -fuse-ld=mold globale rompa il link AppleClang — Req 1.5).")
else()
    add_library(pulse_loader STATIC ${PULSE_LOADER_SOURCES})
endif()

add_library(pulse::loader ALIAS pulse_loader)

target_include_directories(pulse_loader
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(pulse_loader PUBLIC cxx_std_${PULSE_CXX_STANDARD})

# Il loader consuma le API pubbliche dello SDK.
target_link_libraries(pulse_loader PUBLIC pulse::sdk)

message(STATUS "Pulse: sottoprogetto loader configurato.")

# ---------------------------------------------------------------------------
# Backend di hooking MinHook + Zydis (Task 4.1, Requisito 2.2).
# MinHook è specifico per Windows x86-64: l'integrazione via FetchContent è
# GUARDATA su WIN32 così la build host macOS/Linux configura e compila comunque
# (l'interfaccia IHookBackend e lo stub non-Windows compilano sempre).
# ---------------------------------------------------------------------------
if(WIN32)
    include(FetchContent)

    # Fork interno con relocazione dei prologhi via Zydis (override consentito).
    set(PULSE_MINHOOK_REPOSITORY "https://github.com/TsudaKageyu/minhook.git"
        CACHE STRING "Repository del fork pulse-minhook")
    set(PULSE_MINHOOK_TAG "master" CACHE STRING "Tag/branch di pulse-minhook")

    FetchContent_Declare(
        pulse_minhook
        GIT_REPOSITORY ${PULSE_MINHOOK_REPOSITORY}
        GIT_TAG ${PULSE_MINHOOK_TAG}
    )
    FetchContent_MakeAvailable(pulse_minhook)

    # Zydis: disassembler a lunghezza per relocare i prologhi non banali.
    set(PULSE_ZYDIS_TAG "v4.1.0" CACHE STRING "Tag di Zydis")
    FetchContent_Declare(
        zydis
        GIT_REPOSITORY "https://github.com/zyantific/zydis.git"
        GIT_TAG ${PULSE_ZYDIS_TAG}
    )
    FetchContent_MakeAvailable(zydis)

    target_compile_definitions(pulse_loader PRIVATE PULSE_HOOK_BACKEND_MINHOOK=1)
    target_link_libraries(pulse_loader PRIVATE minhook Zydis)

    message(STATUS "Pulse: backend di hooking MinHook + Zydis abilitato (Windows x64).")
else()
    message(STATUS "Pulse: MinHook non disponibile su questa piattaforma host; "
                   "loader/hooking usa lo stub non-Windows (compila ma Unsupported).")
endif()

# ---------------------------------------------------------------------------
# Backend di hooking Dobby (Task 24.1, Requisiti 2.1, 2.2).
# Dobby copre i target Apple: macOS x86-64/arm64 e iOS arm64, con gestione PAC
# su arm64e (vedi design "Selezione del motore di hooking per piattaforma").
#
# L'integrazione via FetchContent è gestita da un'opzione PULSE_ENABLE_DOBBY.
# Quando l'opzione è ON su un target Apple, Dobby viene reso disponibile e la
# macro PULSE_HOOK_BACKEND_DOBBY abilita il path reale (DobbyHook/DobbyDestroy),
# così `DobbyBackend::available()` riporta `true` (Req 3.1). Su host non-Apple
# l'opzione è ignorata e resta lo stub `Unsupported` (`available()==false`).
#
# COERENZA ARTEFATTO/BACKEND (Req 3.1): quando il Loader_Artifact dinamico è ON
# su un target Apple, Dobby viene attivato DI DEFAULT — un artefatto caricabile
# da Geometry Dash senza un backend di hooking reale non potrebbe installare
# alcun hook. L'User può comunque forzare esplicitamente `PULSE_ENABLE_DOBBY=OFF`
# (caso degradato segnalato con un WARNING di coerenza più sotto — Req 3.8).
# Fuori da questo scenario il default resta OFF, così la build host macOS
# configura/compila SENZA scaricare Dobby (path stub attivo).
# ---------------------------------------------------------------------------
if(PULSE_BUILD_LOADER_ARTIFACT AND APPLE)
    set(_pulse_dobby_default ON)
else()
    set(_pulse_dobby_default OFF)
endif()

option(PULSE_ENABLE_DOBBY
       "Abilita il backend di hooking Dobby (target Apple: macOS/iOS)"
       ${_pulse_dobby_default})

# Coerenza Loader_Artifact macOS <-> Dobby (Req 3.8): se l'artefatto è ON su
# Apple ma l'User ha forzato Dobby OFF, l'artefatto risultante avrà un backend
# che riporta `available()==false` e `HookGate` bloccherà ogni installazione di
# hook. Lo segnaliamo esplicitamente come configurazione degradata.
if(PULSE_BUILD_LOADER_ARTIFACT AND APPLE AND NOT PULSE_ENABLE_DOBBY)
    message(WARNING "Loader_Artifact macOS senza Dobby: il backend riportera' "
                    "available()==false e gli hook saranno bloccati (Req 3.8).")
endif()

if(PULSE_ENABLE_DOBBY AND APPLE)
    include(FetchContent)

    set(PULSE_DOBBY_REPOSITORY "https://github.com/jmpews/Dobby.git"
        CACHE STRING "Repository di Dobby")
    set(PULSE_DOBBY_TAG "master" CACHE STRING "Tag/branch di Dobby")

    # Build minimale di Dobby (solo libreria statica, niente esempi/test).
    set(DOBBY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(DOBBY_BUILD_TEST OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        dobby
        GIT_REPOSITORY ${PULSE_DOBBY_REPOSITORY}
        GIT_TAG ${PULSE_DOBBY_TAG}
    )
    FetchContent_MakeAvailable(dobby)

    target_compile_definitions(pulse_loader PRIVATE PULSE_HOOK_BACKEND_DOBBY=1)
    # Link STATICO di Dobby (target `dobby_static`, non `dobby` shared): così il
    # codice di Dobby è incorporato in libpulse_loader.dylib e l'artefatto resta
    # autosufficiente. Linkare il target shared introdurrebbe una dipendenza
    # `@rpath/libdobby.dylib` che dyld non risolverebbe una volta iniettato in
    # Geometry Dash (Library not loaded), oltre a richiedere copia/firma di un
    # secondo dylib nel bundle.
    target_link_libraries(pulse_loader PRIVATE dobby_static)

    message(STATUS "Pulse: backend di hooking Dobby abilitato (target Apple, PAC su arm64e).")
else()
    if(APPLE)
        message(STATUS "Pulse: Dobby disabilitato (PULSE_ENABLE_DOBBY=OFF); "
                       "loader/hooking usa lo stub (compila ma Unsupported).")
    endif()
endif()

# ---------------------------------------------------------------------------
# Head_Thunk reale per-architettura (Task 7.4, Requisiti 4.3, 5.2, 4.5).
#
# I sorgenti assembly `head_thunk_arm64.S` / `head_thunk_x86_64.S` realizzano il
# ramo REALE dell'Head_Thunk: un salto indiretto signature-agnostic (tail-branch)
# verso `currentHead`. Sono *signature-agnostic* e non sono inclusi nel GLOB dei
# `.cpp`: vengono aggiunti SOLO quando l'opzione e' ON, e solo il sorgente che
# corrisponde all'architettura del target viene compilato (un .S della ENA
# sbagliata non assemblerebbe).
#
# L'opzione e' default OFF (il ramo reale e' coperto manualmente in Fase E su GD
# reale): in CI / build host resta attivo l'host-stub `inline` di
# `head_thunk.hpp`, cosi' il relinking della catena resta verificabile senza un
# backend reale. Concettualmente vive DIETRO l'artefatto Dobby (l'unica
# `DobbyHook` punta a una copia di questo thunk): se attivata fuori da quel
# contesto lo segnaliamo con un WARNING ma il compile-smoke procede comunque.
#
# Compile-smoke: configurare/buildare con `-DPULSE_ENABLE_HEAD_THUNK_ASM=ON`
# sotto `env -u LDFLAGS` su macOS.
# ---------------------------------------------------------------------------
option(PULSE_ENABLE_HEAD_THUNK_ASM
       "Compila i sorgenti assembly reali dell'Head_Thunk per-architettura (dietro l'artefatto Dobby)"
       OFF)

if(PULSE_ENABLE_HEAD_THUNK_ASM)
    # Abilita il linguaggio ASM (clang assembla i `.S` con preprocessore C).
    # Se il compilatore CXX e' un launcher (es. ccache invocato come
    # `ccache clang++`), rispecchiamo lo stesso comando per l'ASM: senza questo,
    # l'auto-detection puo' eleggere il solo launcher come "assemblatore" e
    # passargli flag che non comprende (es. `ccache: invalid option -- D`).
    # Una `-DCMAKE_ASM_COMPILER=...` esplicita dell'utente ha comunque la
    # precedenza.
    if(NOT CMAKE_ASM_COMPILER)
        set(CMAKE_ASM_COMPILER "${CMAKE_CXX_COMPILER}")
        if(CMAKE_CXX_COMPILER_ARG1)
            set(CMAKE_ASM_COMPILER_ARG1 "${CMAKE_CXX_COMPILER_ARG1}")
        endif()
    endif()
    enable_language(ASM)

    # Seleziona il sorgente per l'architettura del target. CMAKE_SYSTEM_PROCESSOR
    # vale arm64/aarch64 su Apple Silicon e x86_64/AMD64 su Intel.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
        set(_pulse_head_thunk_asm
            "${CMAKE_CURRENT_SOURCE_DIR}/hooking/head_thunk_arm64.S")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
        set(_pulse_head_thunk_asm
            "${CMAKE_CURRENT_SOURCE_DIR}/hooking/head_thunk_x86_64.S")
    else()
        pulse_abort("PULSE_ENABLE_HEAD_THUNK_ASM: architettura non supportata \
'${CMAKE_SYSTEM_PROCESSOR}' (attesi arm64 o x86-64)")
    endif()

    target_sources(pulse_loader PRIVATE "${_pulse_head_thunk_asm}")
    # Macro che seleziona il ramo reale di `make_head_thunk` (head_thunk.cpp)
    # anziche' l'host-stub `inline` di head_thunk.hpp.
    target_compile_definitions(pulse_loader PRIVATE PULSE_HEAD_THUNK_ASM=1)

    if(NOT (PULSE_BUILD_LOADER_ARTIFACT AND PULSE_ENABLE_DOBBY))
        message(WARNING "PULSE_ENABLE_HEAD_THUNK_ASM e' pensato dietro l'artefatto "
                        "Dobby (PULSE_BUILD_LOADER_ARTIFACT + PULSE_ENABLE_DOBBY); "
                        "attivo qui solo per compile-smoke del ramo reale.")
    endif()

    message(STATUS "Pulse: Head_Thunk asm reale abilitato (${_pulse_head_thunk_asm}).")
else()
    message(STATUS "Pulse: Head_Thunk asm reale disabilitato "
                   "(PULSE_ENABLE_HEAD_THUNK_ASM=OFF); attivo l'host-stub di head_thunk.hpp.")
endif()

# ---------------------------------------------------------------------------
# Backend di hooking ShadowHook (Task 24.2, Requisiti 2.1, 2.2).
# ShadowHook (ByteDance) è specifico per Android arm64-v8a / armeabi-v7a:
# l'integrazione via FetchContent è GUARDATA su ANDROID e dietro l'opzione
# PULSE_ENABLE_SHADOWHOOK (default OFF), così la build host macOS/Linux/Windows
# configura e compila comunque usando lo stub non-Android (l'interfaccia
# IHookBackend e lo stub compilano sempre, senza scaricare ShadowHook).
# ---------------------------------------------------------------------------
option(PULSE_ENABLE_SHADOWHOOK
    "Abilita il backend di hooking ShadowHook (solo target Android arm64/armv7)"
    OFF)

if(ANDROID AND PULSE_ENABLE_SHADOWHOOK)
    include(FetchContent)

    # ShadowHook di ByteDance (gestione inline hook thumb/arm32/arm64 + I-cache).
    set(PULSE_SHADOWHOOK_REPOSITORY "https://github.com/bytedance/android-inline-hook.git"
        CACHE STRING "Repository di ShadowHook (android-inline-hook)")
    set(PULSE_SHADOWHOOK_TAG "main" CACHE STRING "Tag/branch di ShadowHook")

    FetchContent_Declare(
        pulse_shadowhook
        GIT_REPOSITORY ${PULSE_SHADOWHOOK_REPOSITORY}
        GIT_TAG ${PULSE_SHADOWHOOK_TAG}
    )
    FetchContent_MakeAvailable(pulse_shadowhook)

    target_compile_definitions(pulse_loader PRIVATE PULSE_HOOK_BACKEND_SHADOWHOOK=1)
    target_link_libraries(pulse_loader PRIVATE shadowhook)

    message(STATUS "Pulse: backend di hooking ShadowHook abilitato (Android arm64/armv7).")
else()
    message(STATUS "Pulse: ShadowHook non abilitato su questa configurazione; "
                   "loader/hooking usa lo stub non-Android (compila ma Unsupported).")
endif()
