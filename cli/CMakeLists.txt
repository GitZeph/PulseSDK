# Pulse CLI — integrazione di build/install del binario `pulse` (Rust).
#
# La CLI è un tool standalone Rust (crate `pulse-cli`, binario `pulse`), non
# linkato nel processo di Geometry Dash. Questo file NON ricompila i moduli
# C++: si limita a (1) compilare il binario `pulse` con cargo a build time e
# (2) installarlo sotto il prefix di installazione con permesso eseguibile.
#
# Requisiti coperti:
#   8.2 — installa l'eseguibile `pulse` sotto un prefix configurabile, con
#         permesso eseguibile impostato.
#   8.4 — se il prefix non è scrivibile, l'install si ferma SENZA installazione
#         parziale e riporta prefix + causa.
#   8.5 — install per-componente (COMPONENT cli) → `cmake --install --component cli`
#         genera `install_manifest_cli.txt`, simmetrico a `install_manifest_sdk.txt`
#         del componente sdk; l'uninstall per-componente rimuove solo i propri file.
#   8.6 — usa il default prefix di piattaforma quando CMAKE_INSTALL_PREFIX non è
#         configurato (via GNUInstallDirs).

# Layout di installazione standard di piattaforma (Req 8.6): GNUInstallDirs
# deriva CMAKE_INSTALL_BINDIR dal prefix (CMAKE_INSTALL_PREFIX), che di default
# è quello di piattaforma quando non configurato esplicitamente.
include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Toolchain Rust: serve `cargo` per compilare il binario `pulse`.
# ---------------------------------------------------------------------------
find_program(PULSE_CARGO_EXECUTABLE cargo)

set(PULSE_CLI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
# Target dir dedicato dentro l'albero di build CMake (non sporca cli/target).
set(PULSE_CLI_CARGO_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cargo-target")
set(PULSE_CLI_BINARY
    "${PULSE_CLI_CARGO_TARGET_DIR}/release/pulse${CMAKE_EXECUTABLE_SUFFIX}")

if(NOT PULSE_CARGO_EXECUTABLE)
    # Senza cargo non possiamo produrre il binario: avvisiamo ma non abortiamo
    # la configurazione dell'intero progetto (loader/sdk restano buildabili).
    message(WARNING
        "Pulse: `cargo` non trovato nel PATH; il binario `pulse` (componente "
        "cli) non sarà compilato né installato. Installa la toolchain Rust per "
        "abilitare l'install della CLI.")
    return()
endif()

# ---------------------------------------------------------------------------
# Build del binario `pulse` (cargo build --release) a build time.
#
# Su host macOS un eventuale `LDFLAGS=-fuse-ld=mold` globale rompe il link di
# AppleClang/host linker: la build cargo gira con LDFLAGS rimosso dall'ambiente
# (coerente con Req 1.5/6.5; `env -u LDFLAGS`).
# ---------------------------------------------------------------------------
set(_pulse_cargo_build
    ${PULSE_CARGO_EXECUTABLE} build --release
        --manifest-path "${PULSE_CLI_SOURCE_DIR}/Cargo.toml"
        --target-dir "${PULSE_CLI_CARGO_TARGET_DIR}")

if(APPLE)
    set(_pulse_cargo_build
        ${CMAKE_COMMAND} -E env --unset=LDFLAGS ${_pulse_cargo_build})
endif()

add_custom_command(
    OUTPUT "${PULSE_CLI_BINARY}"
    COMMAND ${_pulse_cargo_build}
    WORKING_DIRECTORY "${PULSE_CLI_SOURCE_DIR}"
    COMMENT "Pulse: compilazione del binario `pulse` (cargo build --release)"
    VERBATIM)

# `ALL` così una normale `cmake --build` produce il binario, rendendolo
# disponibile per `cmake --install`.
add_custom_target(pulse_cli ALL DEPENDS "${PULSE_CLI_BINARY}")

# ---------------------------------------------------------------------------
# Guard di scrivibilità del prefix (Req 8.4).
#
# Questo blocco install(CODE) è dichiarato PRIMA dell'install(PROGRAMS): se il
# prefix non è scrivibile, message(FATAL_ERROR) interrompe l'install prima che
# qualunque file venga copiato → nessuna installazione parziale, ed emette un
# errore che identifica sia il prefix sia la causa.
#
# Tagghiamo il blocco con COMPONENT cli così gira solo durante l'install del
# componente cli (coerente con l'install per-componente, Req 8.5).
# ---------------------------------------------------------------------------
install(CODE [==[
    set(_pulse_prefix "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
    set(_pulse_bindir "${_pulse_prefix}/${CMAKE_INSTALL_BINDIR}")
    message(STATUS "Pulse(cli): verifica scrivibilità del prefix '${_pulse_prefix}'")

    # Tenta di creare l'albero del bindir di destinazione (idempotente).
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_pulse_bindir}"
        RESULT_VARIABLE _pulse_mk_res
        ERROR_VARIABLE  _pulse_mk_err)
    if(NOT _pulse_mk_res EQUAL 0)
        message(FATAL_ERROR
            "Pulse(cli): impossibile installare nel prefix '${_pulse_prefix}': "
            "creazione di '${_pulse_bindir}' fallita (causa: ${_pulse_mk_err}). "
            "Installazione interrotta senza modifiche.")
    endif()

    # Probe di scrittura effettiva: rileva un prefix non scrivibile (permessi).
    set(_pulse_probe "${_pulse_bindir}/.pulse_write_probe")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E touch "${_pulse_probe}"
        RESULT_VARIABLE _pulse_touch_res
        ERROR_VARIABLE  _pulse_touch_err)
    if(NOT _pulse_touch_res EQUAL 0)
        message(FATAL_ERROR
            "Pulse(cli): il prefix di installazione non è scrivibile: "
            "'${_pulse_prefix}' (causa: ${_pulse_touch_err}). "
            "Installazione interrotta senza modifiche.")
    endif()
    file(REMOVE "${_pulse_probe}")
]==] COMPONENT cli)

# ---------------------------------------------------------------------------
# Install del binario `pulse` (Req 8.2).
#
# install(PROGRAMS) imposta il bit eseguibile (OWNER/GROUP/WORLD execute) sul
# file installato. Componente cli per il manifest/uninstall per-componente.
# ---------------------------------------------------------------------------
install(PROGRAMS "${PULSE_CLI_BINARY}"
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
        COMPONENT cli)

message(STATUS "Pulse: sottoprogetto cli configurato (binario `pulse`).")
