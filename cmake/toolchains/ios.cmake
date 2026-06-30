# ---------------------------------------------------------------------------
# Pulse SDK — Toolchain di cross-compilazione iOS (arm64)
# ---------------------------------------------------------------------------
# Target: iOS arm64 (Requisiti 26.2/26.3). Produce una `dylib` destinata a
# contesti jailbreak/sideload, con configurazione di code-signing.
#
# Uso:
#   cmake -S . -B build-ios \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ios.cmake \
#         -DPULSE_IOS_CODESIGN_IDENTITY="iPhone Developer" \
#         -GXcode
#
# Parametri configurabili:
#   PULSE_IOS_DEPLOYMENT_TARGET  versione minima di iOS (default 14.0)
#   PULSE_IOS_CODESIGN_IDENTITY  identità di firma; "-" per ad-hoc signing,
#                                stringa vuota per disabilitare la firma.
#
# Prerequisiti:
#   - Build su host macOS con Xcode/Command Line Tools installati.
#
# Requisito 26.4: la configurazione viene interrotta con OS (iOS) + causa se
# l'ambiente non è idoneo (host non macOS, SDK iOS assente), senza generare
# artefatti parziali.
# ---------------------------------------------------------------------------

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_PROCESSOR arm64)

# --- L'host deve essere macOS (richiesto da SDK e codesign) ---------------
if(NOT CMAKE_HOST_APPLE)
    message(FATAL_ERROR
        "Pulse build interrotta su [iOS]: la cross-compilazione iOS richiede \
un host macOS con Xcode; host corrente '${CMAKE_HOST_SYSTEM_NAME}'")
endif()

# --- Architettura e deployment target -------------------------------------
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Architetture iOS")
set(CMAKE_OSX_SYSROOT "iphoneos" CACHE STRING "SDK iOS")

if(NOT DEFINED PULSE_IOS_DEPLOYMENT_TARGET)
    set(PULSE_IOS_DEPLOYMENT_TARGET "14.0")
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET "${PULSE_IOS_DEPLOYMENT_TARGET}"
    CACHE STRING "Versione minima di iOS")

# --- Verifica disponibilità dell'SDK iOS (Requisito 26.4) -----------------
find_program(_PULSE_XCRUN xcrun)
if(NOT _PULSE_XCRUN)
    message(FATAL_ERROR
        "Pulse build interrotta su [iOS]: 'xcrun' non trovato; installa Xcode \
o i Command Line Tools per accedere all'SDK iOS")
endif()

execute_process(
    COMMAND "${_PULSE_XCRUN}" --sdk iphoneos --show-sdk-path
    OUTPUT_VARIABLE _PULSE_IOS_SDK_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _PULSE_IOS_SDK_RESULT
    ERROR_QUIET
)
if(NOT _PULSE_IOS_SDK_RESULT EQUAL 0 OR "${_PULSE_IOS_SDK_PATH}" STREQUAL "")
    message(FATAL_ERROR
        "Pulse build interrotta su [iOS]: SDK iphoneos non disponibile; verifica \
l'installazione di Xcode e l'accettazione della licenza (xcodebuild -license)")
endif()

# --- Code signing (Requisito 26.3: dylib firmata per sideload/jailbreak) --
if(NOT DEFINED PULSE_IOS_CODESIGN_IDENTITY)
    # Firma ad-hoc per build locali; sovrascrivibile con un'identità reale.
    set(PULSE_IOS_CODESIGN_IDENTITY "-")
endif()

set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "YES")
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${PULSE_IOS_CODESIGN_IDENTITY}")
if("${PULSE_IOS_CODESIGN_IDENTITY}" STREQUAL "")
    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO")
    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO")
endif()

# iOS non esegue test sull'host di build: cerca solo le librerie nel sysroot.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

message(STATUS "Pulse: cross-compilazione iOS arm64 \
(deployment ${CMAKE_OSX_DEPLOYMENT_TARGET}, codesign '${PULSE_IOS_CODESIGN_IDENTITY}')")
