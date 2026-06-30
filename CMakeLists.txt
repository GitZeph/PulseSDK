# Pulse SDK / Pulse Loader — build root
# Requisito 26.2/26.3: compila loader e SDK su Windows, macOS e Linux.
# Requisito 26.4: in caso di fallimento interrompe la build con OS + causa,
#                 senza produrre artefatti parziali.
cmake_minimum_required(VERSION 3.21)

project(Pulse
    VERSION 0.1.0
    DESCRIPTION "Pulse SDK / Pulse Loader — mod loader e SDK originali per Geometry Dash"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Standard del linguaggio (Requisito 26.1): C++20 minimo, C++23 se disponibile.
# ---------------------------------------------------------------------------
if(NOT DEFINED PULSE_CXX_STANDARD)
    if("cxx_std_23" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
        set(PULSE_CXX_STANDARD 23)
    else()
        set(PULSE_CXX_STANDARD 20)
    endif()
endif()

set(CMAKE_CXX_STANDARD ${PULSE_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

message(STATUS "Pulse: standard C++ selezionato = C++${PULSE_CXX_STANDARD}")

# ---------------------------------------------------------------------------
# Helper di abort: interrompe la configurazione/build riportando OS + causa.
# Usare FATAL_ERROR garantisce che la generazione non prosegua e che non
# vengano prodotti artefatti parziali (Requisito 26.4).
# ---------------------------------------------------------------------------
function(pulse_abort cause)
    message(FATAL_ERROR
        "Pulse build interrotta su [${CMAKE_SYSTEM_NAME}]: ${cause}")
endfunction()

# ---------------------------------------------------------------------------
# Validazione del sistema operativo target (Requisito 26.2).
# Solo Windows, macOS e Linux sono supportati per la build host del MVP.
# ---------------------------------------------------------------------------
set(PULSE_SUPPORTED_HOST_OS "Windows" "Darwin" "Linux")
if(NOT CMAKE_SYSTEM_NAME IN_LIST PULSE_SUPPORTED_HOST_OS)
    pulse_abort("sistema operativo non supportato per la build host \
(attesi: Windows, macOS/Darwin, Linux)")
endif()

message(STATUS "Pulse: sistema operativo target = ${CMAKE_SYSTEM_NAME}")

# ---------------------------------------------------------------------------
# Niente build in-source: evita di sporcare l'albero sorgente con artefatti.
# ---------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    pulse_abort("build in-source non consentita; usa una cartella di build separata \
(es. cmake -S . -B build)")
endif()

# ---------------------------------------------------------------------------
# Opzioni di build.
# ---------------------------------------------------------------------------
option(PULSE_BUILD_TESTS "Compila i test di loader e SDK" OFF)

# ---------------------------------------------------------------------------
# Cross-compilazione (Requisiti 26.2/26.3).
# I toolchain file in cmake/toolchains/ coprono Android (NDK r26), iOS
# (ios.cmake + codesign) e il fat binary macOS universale. Vedi i commenti
# di ciascun file per l'invocazione. Su host macOS è disponibile anche una
# scorciatoia per il fat binary universale senza un toolchain file dedicato.
# ---------------------------------------------------------------------------
option(PULSE_MACOS_UNIVERSAL
    "Su macOS, produce artefatti fat binary universali (x86_64 + arm64)" OFF)

if(PULSE_MACOS_UNIVERSAL)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        pulse_abort("PULSE_MACOS_UNIVERSAL richiede un host macOS/Darwin \
(fat binary x86_64+arm64 non disponibile su questo OS)")
    endif()
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64"
        CACHE STRING "Architetture macOS universali (fat binary)" FORCE)
    message(STATUS "Pulse: fat binary macOS universale abilitato \
(${CMAKE_OSX_ARCHITECTURES}).")
endif()

# Output centralizzato degli artefatti.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---------------------------------------------------------------------------
# Sottoprogetti (Requisito 26.3): loader e SDK.
# ---------------------------------------------------------------------------
add_subdirectory(sdk)
add_subdirectory(loader)

# ---------------------------------------------------------------------------
# Pulse CLI (Requisito 8.2/8.4/8.5/8.6): integrazione di build/install del
# binario Rust `pulse`. Non ricompila i moduli C++; compila la CLI con cargo e
# ne aggiunge le regole di install per-componente (COMPONENT cli).
# ---------------------------------------------------------------------------
add_subdirectory(cli)

# ---------------------------------------------------------------------------
# Test (Requisiti 26.2, 26.3): GoogleTest + RapidCheck via CTest.
# Abilitati solo con -DPULSE_BUILD_TESTS=ON per non appesantire la build base.
# ---------------------------------------------------------------------------
if(PULSE_BUILD_TESTS)
    enable_testing()
    include(CTest)
    add_subdirectory(tests)
    message(STATUS "Pulse: test abilitati (ctest).")
endif()

message(STATUS "Pulse: configurazione completata (loader + sdk).")
