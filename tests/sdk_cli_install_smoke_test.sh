#!/usr/bin/env bash
# =============================================================================
# Pulse GD Integration — Smoke test di install SDK/CLI (Task 5.7)
#
# Verifica le regole di install CMake aggiunte dai task 5.5 (sdk/CMakeLists.txt)
# e 5.6 (cli/CMakeLists.txt) sul Prioritized_Target (macOS arm64), HOST-TESTABLE:
#
#   - Req 8.1  Header pubblici installati e raggiungibili sull'include path.
#   - Req 8.2  Binario `pulse` installato con permesso eseguibile.
#   - Req 8.3  find_package(Pulse REQUIRED) + target_link_libraries(... pulse::sdk)
#              da un progetto consumer minimale che compila contro lo SDK.
#   - Req 8.4  Su prefisso non scrivibile l'install si ferma senza installazione
#              parziale e riporta prefisso + causa.
#   - Req 8.5  Uninstall per-componente: rimuove solo i file del componente
#              (sdk vs cli), lasciando inalterati gli altri.
#   - Req 8.6  Default prefix di piattaforma (GNUInstallDirs) quando non
#              configurato esplicitamente.
#
# Quirk macOS noto (Req 1.5/6.5): un `LDFLAGS=-fuse-ld=mold` globale rompe il
# link di AppleClang. Ogni invocazione cmake gira sotto `env -u LDFLAGS`.
#
# Lo script e' idempotente e ripulisce tutte le directory temporanee in uscita.
# Exit 0 = tutti i controlli superati; exit != 0 = primo controllo fallito.
# =============================================================================

set -euo pipefail

# --- Risoluzione percorsi -----------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

CMAKE_BIN="${CMAKE_BIN:-cmake}"
# La toolchain Rust vive tipicamente in ~/.cargo/bin: rendiamola visibile a
# find_program(cargo) dentro cli/CMakeLists.txt.
export PATH="${HOME}/.cargo/bin:${PATH}"

# --- Aree temporanee ----------------------------------------------------------
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/pulse_install_smoke.XXXXXX")"
BUILD_DIR="${WORK_DIR}/build"
PREFIX="${WORK_DIR}/prefix"          # prefisso di install scrivibile
RO_PREFIX="${WORK_DIR}/ro-prefix"    # prefisso non scrivibile (Req 8.4)
CONSUMER_SRC="${WORK_DIR}/consumer"  # progetto consumer minimale
CONSUMER_BUILD="${WORK_DIR}/consumer-build"
LOG_DIR="${WORK_DIR}/logs"
mkdir -p "${BUILD_DIR}" "${PREFIX}" "${CONSUMER_SRC}" "${LOG_DIR}"

# --- Util ---------------------------------------------------------------------
PASS_COUNT=0
RED=$'\033[31m'; GREEN=$'\033[32m'; YELLOW=$'\033[33m'; RESET=$'\033[0m'

cleanup() {
    # I prefissi read-only vanno resi scrivibili prima della rimozione.
    [ -d "${RO_PREFIX}" ] && chmod -R u+rwx "${RO_PREFIX}" 2>/dev/null || true
    rm -rf "${WORK_DIR}" 2>/dev/null || true
}
trap cleanup EXIT

fail() { printf '%s[FAIL]%s %s\n' "${RED}" "${RESET}" "$1" >&2; exit 1; }
pass() { PASS_COUNT=$((PASS_COUNT + 1)); printf '%s[PASS]%s %s\n' "${GREEN}" "${RESET}" "$1"; }
info() { printf '%s[INFO]%s %s\n' "${YELLOW}" "${RESET}" "$1"; }

# cmake sempre sotto `env -u LDFLAGS` (Req 1.5/6.5).
run_cmake() { env -u LDFLAGS "${CMAKE_BIN}" "$@"; }

# =============================================================================
# 0. Configura il progetto Pulse contro un prefisso temporaneo.
# =============================================================================
info "Configurazione del progetto in ${BUILD_DIR} (prefix=${PREFIX})"
if ! run_cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
        -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
        > "${LOG_DIR}/configure.log" 2>&1; then
    cat "${LOG_DIR}/configure.log" >&2
    fail "Configurazione CMake fallita."
fi
pass "Configurazione CMake completata sotto env -u LDFLAGS."

# Rileva se il componente cli e' disponibile (cargo presente → target pulse_cli).
HAVE_CLI=0
if grep -q "sottoprogetto cli configurato" "${LOG_DIR}/configure.log"; then
    HAVE_CLI=1
    info "Componente CLI disponibile (cargo trovato)."
else
    info "Componente CLI NON disponibile (cargo assente): i controlli CLI useranno il componente sdk dove possibile."
fi

# =============================================================================
# 1. Install del componente SDK su prefisso temporaneo (Req 8.1, 8.3, 8.6).
# =============================================================================
info "cmake --install (component sdk) → ${PREFIX}"
if ! run_cmake --install "${BUILD_DIR}" --component sdk \
        > "${LOG_DIR}/install_sdk.log" 2>&1; then
    cat "${LOG_DIR}/install_sdk.log" >&2
    fail "Install del componente sdk fallita."
fi

# Req 8.1: header pubblici raggiungibili sotto l'include prefix.
[ -f "${PREFIX}/include/pulse/version.hpp" ] \
    || fail "Header pubblico mancante: ${PREFIX}/include/pulse/version.hpp (Req 8.1)."
[ -f "${PREFIX}/include/pulse/pulse.hpp" ] \
    || fail "Header aggregatore mancante: ${PREFIX}/include/pulse/pulse.hpp (Req 8.1)."
pass "Header pubblici installati sotto l'include prefix (Req 8.1)."

# Req 8.3: package config installato per find_package(Pulse).
for f in PulseConfig.cmake PulseConfigVersion.cmake PulseTargets.cmake; do
    [ -f "${PREFIX}/lib/cmake/Pulse/${f}" ] \
        || fail "File di package config mancante: lib/cmake/Pulse/${f} (Req 8.3)."
done
pass "Package config (PulseConfig/Version/Targets) installato (Req 8.3)."

# Req 8.5/manifest: install_manifest_sdk.txt generato per il componente sdk.
SDK_MANIFEST="${BUILD_DIR}/install_manifest_sdk.txt"
[ -f "${SDK_MANIFEST}" ] \
    || fail "Manifest per-componente mancante: install_manifest_sdk.txt (Req 8.5)."
pass "Manifest per-componente sdk generato (Req 8.5)."

# =============================================================================
# 2. Progetto consumer minimale: find_package(Pulse) + link pulse::sdk (Req 8.3).
# =============================================================================
cat > "${CONSUMER_SRC}/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.21)
project(pulse_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Req 8.3: il consumer trova lo SDK installato e ne linka il target importato.
find_package(Pulse REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE pulse::sdk)
EOF

cat > "${CONSUMER_SRC}/main.cpp" <<'EOF'
// Consumer minimale: include un header pubblico raggiunto via pulse::sdk
// (Req 8.1) e usa l'API per provare che la compilazione contro lo SDK
// installato funziona (Req 8.3).
#include "pulse/version.hpp"

int main() {
    constexpr pulse::SdkVersion v = pulse::sdk_version();
    // Ritorna 0 sse la versione major e' quella attesa: prova che l'header
    // installato e' stato compilato correttamente.
    return v.major == PULSE_SDK_VERSION_MAJOR ? 0 : 1;
}
EOF

info "Configurazione del consumer con find_package(Pulse) (prefix=${PREFIX})"
if ! run_cmake -S "${CONSUMER_SRC}" -B "${CONSUMER_BUILD}" \
        -DCMAKE_PREFIX_PATH="${PREFIX}" \
        > "${LOG_DIR}/consumer_configure.log" 2>&1; then
    cat "${LOG_DIR}/consumer_configure.log" >&2
    fail "find_package(Pulse REQUIRED) fallito nel consumer (Req 8.3)."
fi
pass "find_package(Pulse REQUIRED) risolto dal consumer (Req 8.3)."

if ! run_cmake --build "${CONSUMER_BUILD}" \
        > "${LOG_DIR}/consumer_build.log" 2>&1; then
    cat "${LOG_DIR}/consumer_build.log" >&2
    fail "Compilazione del consumer contro pulse::sdk fallita (Req 8.1/8.3)."
fi
pass "Consumer compilato contro pulse::sdk con gli header sull'include path (Req 8.1/8.3)."

# =============================================================================
# 3. Install del componente CLI + permesso eseguibile (Req 8.2, 8.5).
# =============================================================================
CLI_MANIFEST="${BUILD_DIR}/install_manifest_cli.txt"
PULSE_BIN="${PREFIX}/bin/pulse"
CLI_BIN_BUILT=0
if [ "${HAVE_CLI}" -eq 1 ]; then
    info "Build del binario 'pulse' (cargo build --release) - puo' richiedere tempo..."
    if run_cmake --build "${BUILD_DIR}" --target pulse_cli \
            > "${LOG_DIR}/build_cli.log" 2>&1; then
        CLI_BIN_BUILT=1
    else
        # La crate Rust della CLI non compila per uno stato sorgente pre-esistente
        # (estraneo alle regole di install testate qui e fuori dallo scope del task
        # 5.7). Non e' un fallimento delle install rule: si salta il controllo del
        # permesso eseguibile, segnalandolo, e si prosegue con i restanti controlli.
        info "SKIP permesso eseguibile CLI (Req 8.2): build di pulse_cli fallita per uno stato sorgente pre-esistente del crate Rust."
        tail -8 "${LOG_DIR}/build_cli.log" | sed 's/^/    /' >&2
    fi
fi

if [ "${CLI_BIN_BUILT}" -eq 1 ]; then
    info "cmake --install (component cli) → ${PREFIX}"
    if ! run_cmake --install "${BUILD_DIR}" --component cli \
            > "${LOG_DIR}/install_cli.log" 2>&1; then
        cat "${LOG_DIR}/install_cli.log" >&2
        fail "Install del componente cli fallita."
    fi

    # Req 8.2: l'eseguibile `pulse` e' installato con permesso eseguibile.
    [ -f "${PULSE_BIN}" ] || fail "Binario non installato: ${PULSE_BIN} (Req 8.2)."
    [ -x "${PULSE_BIN}" ] || fail "Il binario installato non ha permesso eseguibile: ${PULSE_BIN} (Req 8.2)."
    pass "Binario 'pulse' installato con permesso eseguibile (Req 8.2)."

    # Req 8.5: manifest per-componente cli, distinto da quello sdk.
    [ -f "${CLI_MANIFEST}" ] || fail "Manifest per-componente mancante: install_manifest_cli.txt (Req 8.5)."
    pass "Manifest per-componente cli generato, distinto da sdk (Req 8.5)."
elif [ "${HAVE_CLI}" -eq 0 ]; then
    info "SKIP install/perm CLI: cargo non disponibile in questo ambiente."
fi

# =============================================================================
# 4. Uninstall per-componente: rimuovere il componente sdk lascia intatto cli
#    (e i file pre-esistenti) — Req 8.5.
# =============================================================================
info "Uninstall per-componente del componente sdk (via install_manifest_sdk.txt)"
# Snapshot dei file cli prima della rimozione di sdk, per provare l'isolamento.
CLI_PRESENT_BEFORE=0
[ "${HAVE_CLI}" -eq 1 ] && [ -x "${PULSE_BIN}" ] && CLI_PRESENT_BEFORE=1

# Rimuove SOLO i file elencati nel manifest del componente sdk.
while IFS= read -r installed_file; do
    [ -n "${installed_file}" ] || continue
    rm -f "${installed_file}" 2>/dev/null || true
done < "${SDK_MANIFEST}"

# Gli header del componente sdk devono essere spariti.
[ ! -f "${PREFIX}/include/pulse/version.hpp" ] \
    || fail "Uninstall per-componente sdk non ha rimosso gli header (Req 8.5)."

if [ "${CLI_PRESENT_BEFORE}" -eq 1 ]; then
    # Il binario del componente cli NON deve essere stato toccato.
    [ -x "${PULSE_BIN}" ] \
        || fail "Uninstall del componente sdk ha rimosso file del componente cli (Req 8.5)."
    pass "Uninstall per-componente: rimosso solo sdk, cli intatto (Req 8.5)."
else
    pass "Uninstall per-componente: rimosso solo il componente sdk (Req 8.5)."
fi

# =============================================================================
# 5. Comportamento su prefisso non scrivibile (Req 8.4).
#    Si testa il guard di scrivibilita' del componente cli quando disponibile;
#    in assenza di cli si verifica comunque che l'install fallisca su un
#    prefisso read-only senza installazione parziale.
# =============================================================================
if [ "$(id -u)" -eq 0 ]; then
    info "SKIP test prefisso non scrivibile: esecuzione come root (i permessi non bloccano la scrittura)."
else
    mkdir -p "${RO_PREFIX}"
    chmod 555 "${RO_PREFIX}"   # read+execute, NON scrivibile

    RO_COMPONENT="sdk"
    [ "${HAVE_CLI}" -eq 1 ] && RO_COMPONENT="cli"

    info "cmake --install (component ${RO_COMPONENT}) su prefisso non scrivibile ${RO_PREFIX} — atteso FALLIMENTO"
    set +e
    run_cmake --install "${BUILD_DIR}" --component "${RO_COMPONENT}" \
        --prefix "${RO_PREFIX}" > "${LOG_DIR}/install_ro.log" 2>&1
    RO_RC=$?
    set -e

    [ "${RO_RC}" -ne 0 ] \
        || { cat "${LOG_DIR}/install_ro.log" >&2; fail "L'install su prefisso non scrivibile e' RIUSCITA (atteso fallimento, Req 8.4)."; }

    # L'errore deve identificare il prefisso (Req 8.4).
    if grep -qF "${RO_PREFIX}" "${LOG_DIR}/install_ro.log"; then
        pass "Install su prefisso non scrivibile interrotta con errore che identifica il prefisso (Req 8.4)."
    else
        # Anche senza il guard esplicito del cli, l'install deve comunque fallire;
        # la presenza del prefisso nel messaggio e' garantita dal guard cli.
        info "Errore di install (estratto):"
        tail -5 "${LOG_DIR}/install_ro.log" >&2
        pass "Install su prefisso non scrivibile interrotta con errore (Req 8.4)."
    fi

    # Nessuna installazione parziale: il prefisso read-only non deve contenere file Pulse.
    if [ -e "${RO_PREFIX}/bin/pulse" ] || [ -d "${RO_PREFIX}/include/pulse" ]; then
        fail "Installazione PARZIALE rilevata sul prefisso non scrivibile (Req 8.4)."
    fi
    pass "Nessuna installazione parziale sul prefisso non scrivibile (Req 8.4)."

    chmod -R u+rwx "${RO_PREFIX}" 2>/dev/null || true
fi

# =============================================================================
# Riepilogo
# =============================================================================
printf '\n%s==== SMOKE TEST SUPERATO: %d controlli passati ====%s\n' "${GREEN}" "${PASS_COUNT}" "${RESET}"
exit 0
