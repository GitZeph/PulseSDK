# ---------------------------------------------------------------------------
# Pulse SDK — Toolchain di cross-compilazione Android (NDK r26)
# ---------------------------------------------------------------------------
# Target: Android arm64-v8a (default) e armeabi-v7a (Requisiti 26.2/26.3).
# Produce un `.so` per ABI tramite il toolchain ufficiale del NDK.
#
# Uso:
#   cmake -S . -B build-android-arm64 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/android-ndk.cmake \
#         -DANDROID_ABI=arm64-v8a
#
#   cmake -S . -B build-android-armv7 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/android-ndk.cmake \
#         -DANDROID_ABI=armeabi-v7a
#
# Prerequisiti:
#   - NDK r26 installato; la variabile d'ambiente ANDROID_NDK_ROOT (oppure
#     ANDROID_NDK_HOME) deve puntare alla radice del NDK.
#
# Requisito 26.4: se il NDK non è configurato/trovato, la configurazione viene
# interrotta con un messaggio che indica l'OS target (Android) e la causa,
# senza generare artefatti parziali.
# ---------------------------------------------------------------------------

# Sistema operativo target di questa cross-compilazione.
set(CMAKE_SYSTEM_NAME Android)

# --- ABI configurabile (default arm64-v8a) -------------------------------
if(NOT DEFINED ANDROID_ABI)
    set(ANDROID_ABI "arm64-v8a" CACHE STRING "ABI Android target")
endif()

set(_PULSE_ANDROID_SUPPORTED_ABIS "arm64-v8a" "armeabi-v7a")
if(NOT ANDROID_ABI IN_LIST _PULSE_ANDROID_SUPPORTED_ABIS)
    message(FATAL_ERROR
        "Pulse build interrotta su [Android]: ANDROID_ABI='${ANDROID_ABI}' non \
supportato (attesi: arm64-v8a, armeabi-v7a)")
endif()

# --- Livello API / platform configurabile --------------------------------
if(NOT DEFINED ANDROID_PLATFORM)
    # API 24 (Android 7.0): coerente con il supporto 32/64-bit del Requisito 1.1.
    set(ANDROID_PLATFORM "android-24" CACHE STRING "Livello API Android target")
endif()

# STL di default per le build C++ Pulse.
if(NOT DEFINED ANDROID_STL)
    set(ANDROID_STL "c++_shared" CACHE STRING "STL Android")
endif()

# --- Individuazione della radice del NDK (Requisito 26.4) -----------------
set(_PULSE_NDK_ROOT "")
if(DEFINED ENV{ANDROID_NDK_ROOT} AND NOT "$ENV{ANDROID_NDK_ROOT}" STREQUAL "")
    set(_PULSE_NDK_ROOT "$ENV{ANDROID_NDK_ROOT}")
elseif(DEFINED ENV{ANDROID_NDK_HOME} AND NOT "$ENV{ANDROID_NDK_HOME}" STREQUAL "")
    set(_PULSE_NDK_ROOT "$ENV{ANDROID_NDK_HOME}")
elseif(DEFINED ANDROID_NDK AND NOT "${ANDROID_NDK}" STREQUAL "")
    set(_PULSE_NDK_ROOT "${ANDROID_NDK}")
endif()

if("${_PULSE_NDK_ROOT}" STREQUAL "")
    message(FATAL_ERROR
        "Pulse build interrotta su [Android]: radice del NDK non configurata; \
imposta ANDROID_NDK_ROOT (o ANDROID_NDK_HOME) sulla cartella del NDK r26")
endif()

if(NOT IS_DIRECTORY "${_PULSE_NDK_ROOT}")
    message(FATAL_ERROR
        "Pulse build interrotta su [Android]: radice del NDK non trovata in \
'${_PULSE_NDK_ROOT}'; verifica l'installazione del NDK r26")
endif()

set(_PULSE_NDK_TOOLCHAIN "${_PULSE_NDK_ROOT}/build/cmake/android.toolchain.cmake")
if(NOT EXISTS "${_PULSE_NDK_TOOLCHAIN}")
    message(FATAL_ERROR
        "Pulse build interrotta su [Android]: toolchain del NDK non trovato in \
'${_PULSE_NDK_TOOLCHAIN}'; l'installazione del NDK r26 sembra incompleta")
endif()

set(ANDROID_NDK "${_PULSE_NDK_ROOT}" CACHE PATH "Radice del NDK Android")

# Delega al toolchain ufficiale del NDK, che imposta compilatori, sysroot e flag.
include("${_PULSE_NDK_TOOLCHAIN}")

message(STATUS "Pulse: cross-compilazione Android ABI=${ANDROID_ABI} \
platform=${ANDROID_PLATFORM} (NDK: ${_PULSE_NDK_ROOT})")
