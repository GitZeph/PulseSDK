# ---------------------------------------------------------------------------
# Pulse SDK — Toolchain per fat binary macOS universale (x86_64 + arm64)
# ---------------------------------------------------------------------------
# Target: macOS universale (Requisito 26.3). Produce una `dylib` fat con
# entrambe le architetture x86_64 e arm64 (Apple Silicon) in un singolo
# artefatto, usando Apple clang.
#
# Uso:
#   cmake -S . -B build-macos-universal \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/macos-universal.cmake
#
# In alternativa, sull'host macOS è disponibile l'opzione del CMake root:
#   cmake -S . -B build -DPULSE_MACOS_UNIVERSAL=ON
#
# Requisito 26.4: se invocato su un host non-macOS la configurazione viene
# interrotta con OS + causa, senza generare artefatti parziali.
# ---------------------------------------------------------------------------

set(CMAKE_SYSTEM_NAME Darwin)

if(NOT CMAKE_HOST_APPLE)
    message(FATAL_ERROR
        "Pulse build interrotta su [Darwin]: il fat binary macOS universale \
richiede un host macOS con Apple clang; host corrente '${CMAKE_HOST_SYSTEM_NAME}'")
endif()

# Entrambe le architetture in un unico artefatto universale.
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64"
    CACHE STRING "Architetture macOS universali (fat binary)")

if(NOT DEFINED PULSE_MACOS_DEPLOYMENT_TARGET)
    set(PULSE_MACOS_DEPLOYMENT_TARGET "11.0")
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET "${PULSE_MACOS_DEPLOYMENT_TARGET}"
    CACHE STRING "Versione minima di macOS")

message(STATUS "Pulse: build macOS universale fat binary \
(${CMAKE_OSX_ARCHITECTURES}, deployment ${CMAKE_OSX_DEPLOYMENT_TARGET})")
