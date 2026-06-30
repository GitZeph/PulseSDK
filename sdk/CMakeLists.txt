# Pulse SDK — API per i Developer (Layer 5)
# Header pubblici in sdk/include/pulse/ (Requisito 26.5 / struttura design).
#
# Lo SDK è una libreria con header pubblici. Per ora è realizzato come libreria
# INTERFACE perché espone principalmente header (macro PULSE_HOOK, field
# injection type-safe, ecc.); i sorgenti compilati verranno aggiunti dalle
# attività successive senza modificarne l'interfaccia pubblica.

set(PULSE_SDK_PUBLIC_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Sorgenti dello SDK (popolati dalle attività successive: hooks/, fields/, ...).
file(GLOB_RECURSE PULSE_SDK_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/hooks/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/fields/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/events/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/settings/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/storage/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/async/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/http/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/log/*.cpp"
)

# Layout di installazione standard di piattaforma (Req 8.6): GNUInstallDirs
# fornisce CMAKE_INSTALL_INCLUDEDIR / CMAKE_INSTALL_LIBDIR derivati dal prefix
# (CMAKE_INSTALL_PREFIX), che di default è quello di piattaforma quando non
# configurato (Req 8.6).
include(GNUInstallDirs)

# Gli include directory usano generator expression così l'EXPORT resta valido:
# durante la build puntano all'albero sorgente, una volta installati puntano
# all'include prefix (Req 8.1).
if(PULSE_SDK_SOURCES)
    add_library(pulse_sdk STATIC ${PULSE_SDK_SOURCES})
    target_include_directories(pulse_sdk PUBLIC
        $<BUILD_INTERFACE:${PULSE_SDK_PUBLIC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_compile_features(pulse_sdk PUBLIC cxx_std_${PULSE_CXX_STANDARD})
else()
    # Nessun sorgente ancora: esponi gli header pubblici come libreria INTERFACE.
    add_library(pulse_sdk INTERFACE)
    target_include_directories(pulse_sdk INTERFACE
        $<BUILD_INTERFACE:${PULSE_SDK_PUBLIC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_compile_features(pulse_sdk INTERFACE cxx_std_${PULSE_CXX_STANDARD})
endif()

add_library(pulse::sdk ALIAS pulse_sdk)

# Con NAMESPACE pulse:: l'EXPORT userebbe il nome del target (pulse::pulse_sdk).
# Impostiamo EXPORT_NAME=sdk così il consumer ottiene esattamente pulse::sdk,
# coerente con l'alias del build tree (Req 8.3).
set_target_properties(pulse_sdk PROPERTIES EXPORT_NAME sdk)

# ---------------------------------------------------------------------------
# Regole di installazione dello SDK (Requisiti 8.1, 8.3, 8.6).
#
# Un progetto mod consumer, configurato contro lo stesso prefix, può fare:
#     find_package(Pulse REQUIRED)
#     target_link_libraries(my_mod PRIVATE pulse::sdk)
# e raggiungere gli header pubblici sull'include path (Req 8.1, 8.3).
# ---------------------------------------------------------------------------

# Req 8.1: header pubblici sotto l'include prefix (preserva la struttura
# include/pulse/...). Componente "sdk" per disinstallazione/manifest per-componente.
install(DIRECTORY "${PULSE_SDK_PUBLIC_INCLUDE_DIR}/pulse"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT sdk)

# Esporta il target pulse_sdk nel set PulseTargets (INTERFACE o STATIC).
install(TARGETS pulse_sdk
        EXPORT PulseTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT sdk
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT sdk
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# Req 8.3: installa il file dei target esportati con namespace pulse:: così il
# consumer ottiene il target importato pulse::sdk.
install(EXPORT PulseTargets
        NAMESPACE pulse::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Pulse"
        COMPONENT sdk)

# Req 8.3: genera e installa PulseConfig.cmake per find_package(Pulse).
include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/PulseConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PulseConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Pulse")

# Versionamento del package config: consente a find_package(Pulse <ver>) di
# negoziare la compatibilità (allineato a PROJECT_VERSION).
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PulseConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/PulseConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/PulseConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Pulse"
        COMPONENT sdk)

message(STATUS "Pulse: sottoprogetto sdk configurato.")

# ---------------------------------------------------------------------------
# Compile-test del Build_Check della GD_API_Surface (task 15.1, Req 3.4, 10.1).
#
# Il `Build_Check` header-only della superficie è realizzato dagli `static_assert`
# generati in `sdk/include/pulse/gd/{types,bindings,hooks}.gen.hpp` (prodotti da
# `pulse surface generate` dal manifest seed `mod-index/surface/surface.toml`).
# Questo target è l'INFRASTRUTTURA che verifica quel Build_Check: compila uno
# snippet C++ che include i tre header generati e dichiara un hook con
# `PULSE_GD_HOOK(MenuLayer, init, …)` con firma conforme al Typed_Binding.
#
# Coerentemente con le altre estensioni di build (PULSE_BUILD_LOADER_ARTIFACT,
# PULSE_ENABLE_DOBBY, …), vive dietro un'opzione **default OFF**, così la build
# base e i test restano invariati. È una OBJECT library: compila lo snippet
# (verifica del Build_Check) senza richiedere link né `main`.
#
# Su host macOS configurare/buildare sotto `env -u LDFLAGS` (evita che un
# -fuse-ld=mold globale rompa il toolchain AppleClang).
# ---------------------------------------------------------------------------
option(PULSE_BUILD_SURFACE_COMPILE_TEST
       "Compila il compile-test del Build_Check degli header SDK generati della GD_API_Surface"
       OFF)

if(PULSE_BUILD_SURFACE_COMPILE_TEST)
    # Gli header generati devono esistere: sono prodotti da `pulse surface
    # generate`. Se mancano, interrompiamo con OS + causa azionabile (Req 26.4)
    # anziché fallire con un oscuro errore di #include mancante.
    set(_pulse_surface_gen_dir
        "${PULSE_SDK_PUBLIC_INCLUDE_DIR}/pulse/gd")
    foreach(_hdr types bindings hooks)
        if(NOT EXISTS "${_pulse_surface_gen_dir}/${_hdr}.gen.hpp")
            pulse_abort("PULSE_BUILD_SURFACE_COMPILE_TEST: header generato \
assente '${_pulse_surface_gen_dir}/${_hdr}.gen.hpp'. Rigenera l'API SDK con \
`pulse surface generate` dal manifest seed mod-index/surface/surface.toml.")
        endif()
    endforeach()

    add_library(pulse_surface_compile_test OBJECT
        "${CMAKE_CURRENT_SOURCE_DIR}/compile-test/surface_build_check.cpp")

    # Le usage requirements di pulse::sdk forniscono l'include path
    # (sdk/include) per `<pulse/hooks.hpp>` e per gli header generati
    # `<pulse/gd/*.gen.hpp>`. register_hook è header-only (inline), quindi la
    # sola compilazione dell'OBJECT basta a verificare il Build_Check.
    target_link_libraries(pulse_surface_compile_test PRIVATE pulse::sdk)
    target_compile_features(pulse_surface_compile_test
        PRIVATE cxx_std_${PULSE_CXX_STANDARD})

    message(STATUS "Pulse: compile-test del Build_Check della GD_API_Surface \
abilitato (target pulse_surface_compile_test). Su macOS buildare sotto \
`env -u LDFLAGS`.")
endif()
