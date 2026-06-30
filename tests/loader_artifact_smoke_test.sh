#!/usr/bin/env bash
# =============================================================================
# loader_artifact_smoke_test.sh — Build/smoke test del Loader_Artifact
#
# Feature: pulse-gd-integration — Task 1.3 (build/smoke test, NON PBT).
# Verifica le proprieta' OSSERVABILI del Requisito 1 sul Loader_Artifact, senza
# Geometry Dash reale, usando solo configurazioni CMake e l'ispezione dei
# binari prodotti (nm/otool):
#
#   1. PULSE_BUILD_LOADER_ARTIFACT=ON produce ESATTAMENTE UN modulo dinamico
#      (.dylib su macOS) e NESSUNA libreria statica          -> Req 1.1, 1.6
#   2. il modulo dinamico ESPORTA il simbolo C
#      `pulse_loader_runtime_entry` (verificato con nm/otool) -> Req 1.2
#   3. la configurazione di default (OFF) produce SOLO la libreria statica e
#      NESSUN modulo dinamico: le due uscite sono mutuamente esclusive
#                                                             -> Req 1.1, 1.3
#   4. su un Target_Platform non supportato la build si INTERROMPE con un
#      errore che identifica l'OS e la causa e NON lascia alcun artefatto su
#      disco                                                  -> Req 1.4
#   5. tutte le build girano sotto `env -u LDFLAGS`, cosi' un
#      `-fuse-ld=mold` globale non rompe il link AppleClang    -> Req 1.5
#
# Il test e' OPZIONALE (sotto-attivita' contrassegnata con `*`) ed e' pensato
# per la matrice CI macOS del Prioritized_Target (macOS arm64). Usa cartelle di
# build temporanee che vengono ripulite all'uscita.
#
# Uso:   tests/loader_artifact_smoke_test.sh
# Exit:  0 = tutte le asserzioni passate, !=0 = almeno una fallita.
# =============================================================================
set -u -o pipefail

# --- Risoluzione dei percorsi ------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# --- Stato e helper di reporting --------------------------------------------
FAILURES=0
PASSES=0
TMP_DIRS=()

pass() { printf '  \033[32mPASS\033[0m  %s\n' "$1"; PASSES=$((PASSES + 1)); }
fail() { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; FAILURES=$((FAILURES + 1)); }
info() { printf '\n== %s ==\n' "$1"; }

cleanup() {
    for d in "${TMP_DIRS[@]:-}"; do
        [ -n "${d:-}" ] && [ -d "$d" ] && rm -rf "$d"
    done
}
trap cleanup EXIT

mktmp() {
    local d
    d="$(mktemp -d "/tmp/pulse_artifact_smoke.XXXXXX")"
    TMP_DIRS+=("$d")
    printf '%s' "$d"
}

# --- Determinazione del formato del modulo dinamico per host ----------------
HOST_OS="$(uname -s)"
case "$HOST_OS" in
    Darwin) DYN_NAME="libpulse_loader.dylib" ; STATIC_NAME="libpulse_loader.a" ;;
    Linux)  DYN_NAME="libpulse_loader.so"    ; STATIC_NAME="libpulse_loader.a" ;;
    MINGW*|MSYS*|CYGWIN*) DYN_NAME="pulse_loader.dll" ; STATIC_NAME="libpulse_loader.a" ;;
    *)      DYN_NAME="libpulse_loader.dylib" ; STATIC_NAME="libpulse_loader.a" ;;
esac

# Conta i file che corrispondono ESATTAMENTE al nome dato sotto una build dir.
count_named() {
    local dir="$1" name="$2"
    find "$dir" -type f -name "$name" 2>/dev/null | wc -l | tr -d ' '
}

# =============================================================================
# Test 1 + 2 — Loader_Artifact ON: un solo modulo dinamico, nessuna statica,
#              simbolo esportato. (Req 1.1, 1.2, 1.5, 1.6)
# =============================================================================
test_artifact_on() {
    info "Test 1/2 — Loader_Artifact ON (un solo .dylib + simbolo esportato)"

    # Il modulo dinamico e' supportato solo su target Apple/Windows/Android. Sul
    # Prioritized_Target (macOS) eseguiamo il percorso completo; su altri host
    # (es. Linux) la build dinamica abortirebbe per design — comportamento gia'
    # coperto dal Test 4 — quindi saltiamo le asserzioni positive.
    if [ "$HOST_OS" != "Darwin" ]; then
        printf '  SKIP  host %s: build dinamica non supportata su questo host (vedi Test 4)\n' "$HOST_OS"
        return
    fi

    local b; b="$(mktmp)"
    # `env -u LDFLAGS`: un `-fuse-ld=mold` globale romperebbe il link AppleClang
    # (Req 1.5). PULSE_ENABLE_DOBBY=OFF mantiene il test focalizzato sul Req 1
    # (artefatto/simbolo) ed evita un fetch di rete del backend.
    if ! env -u LDFLAGS cmake -S "$REPO_ROOT" -B "$b" \
            -DPULSE_BUILD_LOADER_ARTIFACT=ON -DPULSE_ENABLE_DOBBY=OFF >"$b/configure.log" 2>&1; then
        fail "configurazione artefatto ON fallita (vedi $b/configure.log)"
        tail -15 "$b/configure.log"
        return
    fi
    if ! env -u LDFLAGS cmake --build "$b" --target pulse_loader >"$b/build.log" 2>&1; then
        fail "build artefatto ON fallita (vedi $b/build.log)"
        tail -20 "$b/build.log"
        return
    fi
    pass "configura+builda sotto \`env -u LDFLAGS\` (Req 1.5)"

    # Req 1.1/1.6: esattamente un modulo dinamico del loader.
    local n_dyn; n_dyn="$(count_named "$b" "$DYN_NAME")"
    if [ "$n_dyn" -eq 1 ]; then
        pass "prodotto esattamente un modulo dinamico ($DYN_NAME) (Req 1.1, 1.6)"
    else
        fail "atteso 1 $DYN_NAME, trovati $n_dyn (Req 1.1, 1.6)"
    fi

    # Req 1.1: nessuna libreria statica del loader in configurazione artefatto.
    local n_static; n_static="$(count_named "$b" "$STATIC_NAME")"
    if [ "$n_static" -eq 0 ]; then
        pass "nessuna libreria statica del loader con artefatto ON (esclusivita', Req 1.1)"
    else
        fail "trovata libreria statica $STATIC_NAME con artefatto ON (Req 1.1)"
    fi

    # Req 1.2: il simbolo `pulse_loader_runtime_entry` e' esportato.
    local dyn_path; dyn_path="$(find "$b" -type f -name "$DYN_NAME" 2>/dev/null | head -1)"
    if [ -z "$dyn_path" ]; then
        fail "impossibile localizzare il modulo dinamico per l'ispezione dei simboli (Req 1.2)"
        return
    fi
    # nm: simboli globali definiti; il leading underscore Mach-O e' tollerato.
    if nm -gU "$dyn_path" 2>/dev/null | grep -q "pulse_loader_runtime_entry" \
       || otool -l "$dyn_path" 2>/dev/null | grep -q "pulse_loader_runtime_entry"; then
        pass "simbolo 'pulse_loader_runtime_entry' esportato (nm/otool) (Req 1.2)"
    else
        fail "simbolo 'pulse_loader_runtime_entry' NON esportato nel modulo dinamico (Req 1.2)"
    fi
}

# =============================================================================
# Test 3 — Default (OFF): solo libreria statica, nessun modulo dinamico.
#          (esclusivita' mutua / default statico — Req 1.1, 1.3)
# =============================================================================
test_default_static() {
    info "Test 3 — Default OFF (solo libreria statica)"
    local b; b="$(mktmp)"
    if ! env -u LDFLAGS cmake -S "$REPO_ROOT" -B "$b" >"$b/configure.log" 2>&1; then
        fail "configurazione default fallita (vedi $b/configure.log)"
        tail -15 "$b/configure.log"
        return
    fi
    if ! env -u LDFLAGS cmake --build "$b" --target pulse_loader >"$b/build.log" 2>&1; then
        fail "build default fallita (vedi $b/build.log)"
        tail -20 "$b/build.log"
        return
    fi

    local n_static; n_static="$(count_named "$b" "$STATIC_NAME")"
    local n_dyn;    n_dyn="$(count_named "$b" "$DYN_NAME")"
    if [ "$n_static" -eq 1 ]; then
        pass "default produce la libreria statica ($STATIC_NAME) (Req 1.3)"
    else
        fail "atteso 1 $STATIC_NAME in default, trovati $n_static (Req 1.3)"
    fi
    if [ "$n_dyn" -eq 0 ]; then
        pass "default NON produce un modulo dinamico (esclusivita', Req 1.1)"
    else
        fail "default ha prodotto $n_dyn moduli dinamici ($DYN_NAME) (Req 1.1)"
    fi
}

# =============================================================================
# Test 4 — Target non supportato: la build si interrompe con OS + causa e non
#          lascia artefatti su disco. (Req 1.4)
#
# `Linux` e' un host OS supportato (passa il guard di root) ma NON e' tra
# {APPLE, WIN32, ANDROID}: con l'artefatto ON entra nel ramo non supportato di
# loader/CMakeLists.txt e chiama `pulse_abort`. Su un host non-Linux questo
# equivale a una cross-configurazione; su un host Linux e' la configurazione
# nativa (Linux non produce un Loader_Artifact). In entrambi i casi esercita lo
# STESSO guard reale del progetto.
# =============================================================================
test_unsupported_abort() {
    info "Test 4 — Target non supportato: abort + nessun artefatto (Req 1.4)"
    local b; b="$(mktmp)"
    local log="$b/configure.log"

    if env -u LDFLAGS cmake -S "$REPO_ROOT" -B "$b" \
            -DPULSE_BUILD_LOADER_ARTIFACT=ON -DCMAKE_SYSTEM_NAME=Linux >"$log" 2>&1; then
        fail "la configurazione su target non supportato NON e' fallita (Req 1.4)"
        return
    fi
    pass "la configurazione su target non supportato si interrompe (Req 1.4)"

    # L'errore deve identificare l'OS e la causa di target non supportato.
    if grep -qi "Linux" "$log" && grep -qiE "non supportato|not supported|PULSE_BUILD_LOADER_ARTIFACT" "$log"; then
        pass "l'errore identifica l'OS e la causa di target non supportato (Req 1.4)"
    else
        fail "l'errore non identifica chiaramente OS + causa (vedi $log) (Req 1.4)"
        tail -15 "$log"
    fi

    # Nessun artefatto (statico o dinamico) deve restare su disco.
    local n_left; n_left="$(find "$b" -type f \( -name 'libpulse_loader.*' -o -name 'pulse_loader.dll' \) 2>/dev/null | wc -l | tr -d ' ')"
    if [ "$n_left" -eq 0 ]; then
        pass "nessun artefatto del loader lasciato su disco dopo l'abort (Req 1.4)"
    else
        fail "trovati $n_left artefatti residui dopo l'abort (Req 1.4)"
    fi
}

# =============================================================================
# Esecuzione
# =============================================================================
printf 'Pulse GD Integration — Loader_Artifact build/smoke test\n'
printf 'Host: %s | modulo dinamico atteso: %s\n' "$HOST_OS" "$DYN_NAME"
printf 'LDFLAGS (ambiente corrente): %s\n' "${LDFLAGS:-<non impostato>}"

test_artifact_on
test_default_static
test_unsupported_abort

info "Riepilogo"
printf '  PASS: %d   FAIL: %d\n' "$PASSES" "$FAILURES"
if [ "$FAILURES" -ne 0 ]; then
    printf '\033[31mSMOKE TEST FALLITO\033[0m\n'
    exit 1
fi
printf '\033[32mSMOKE TEST SUPERATO\033[0m\n'
exit 0
