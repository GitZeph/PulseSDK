#!/usr/bin/env bash
#
# tools/extract_menulayer_init_offset.sh
#
# Helper di DOCUMENTAZIONE/OPERATIVO per la verifica manuale dell'offset reale di
# `MenuLayer::init` su Geometry Dash 2.2081 macOS arm64 (Prioritized_Target).
# Supporta il task 9.2 della spec pulse-gd-integration (Req 4.2, 5.1).
#
# IMPORTANTE
# ----------
# Questo script NON viene eseguito in CI e NON ha alcun binario di GD in questo
# repository: richiede il binario REALE di Geometry Dash sulla macchina macOS
# dell'User. Non scarica nulla, non modifica nulla, e' di sola lettura
# (otool/nm). Senza un percorso valido stampa la procedura ed esce.
#
# USO
#   tools/extract_menulayer_init_offset.sh /path/to/Geometry\ Dash.app/Contents/MacOS/Geometry\ Dash
#
# Al termine, riportare l'offset relativo verificato in
#   mod-index/bindings/2.2081/macos-arm64.pbind
# impostando `offset = 0x...` e `verified = true`.

set -euo pipefail

# Simbolo C++ mangled atteso per MenuLayer::init() (Itanium ABI). Se il binario
# e' strippato questo simbolo non comparira' nella symtab e si dovra' ricorrere
# al pattern-scan del prologo (passo 4).
MANGLED_SYMBOL='_ZN9MenuLayer4initEv'
DEMANGLED='MenuLayer::init'

print_procedure() {
    cat <<'EOF'
=====================================================================
 Procedura manuale — offset reale di MenuLayer::init (GD 2.2081 arm64)
=====================================================================

Strumenti macOS richiesti: otool, nm, c++filt, vtool (tutti in Xcode CLT).

1) CONFERMARE ARCH/IMMAGINE Mach-O (arm64):
     otool -hv "<gd_binary>"
     otool -l  "<gd_binary>" | grep -A4 LC_SEGMENT_64    # base __TEXT / vmaddr

2) RISOLVERE IL SIMBOLO dalla symbol table (se presente):
     nm -arch arm64 "<gd_binary>" | c++filt | grep 'MenuLayer::init'
     # oppure, per indirizzi e simboli grezzi:
     otool -arch arm64 -Iv "<gd_binary>" | grep -i MenuLayer

   L'indirizzo riportato e' un VM address. L'offset relativo all'immagine =
   vm_address_funzione - vm_base_immagine (vmaddr del primo LC_SEGMENT_64
   __TEXT, tipicamente 0x100000000 prima dello slide).

3) (SE STRIPPATO) PATTERN-SCAN DEL PROLOGO:
   Disassemblare e cercare la firma di byte del prologo calibrata su 2.2081:
     otool -arch arm64 -tv "<gd_binary>" | less
   Individuare il prologo della funzione (tipico arm64:
     stp x28, x27, [sp, #-0x..]!   /   stp x29, x30, [sp, #0x..]
     add x29, sp, #0x..            /   mov x.., x0  (this) ...).

4) VERIFICARE IL PROLOGO all'offset candidato:
     otool -arch arm64 -tv "<gd_binary>" | \
       awk -v a="<vm_address>" '$1 ~ a {print; c=8} c-->0'
   Confrontare con la firma attesa e fare un cross-check OSSERVATIVO con il
   riferimento concettuale Geode per 2.2081 (solo verifica visiva — NESSUN
   riuso di codice).

5) SCRIVERE IL .pbind:
   In mod-index/bindings/2.2081/macos-arm64.pbind impostare:
     offset   = 0x<offset_relativo_verificato>
     verified = true
   Solo dopo questo passo binding_verifier marchera' il binding come resolved
   (Req 4.3) e il loader installera' l'hook su MenuLayer::init (Req 9.1).
=====================================================================
EOF
}

main() {
    if [[ $# -lt 1 ]]; then
        echo "[info] Nessun binario fornito: stampo solo la procedura (dry-run)." >&2
        echo "[info] Uso: $0 <percorso eseguibile Mach-O di GD 2.2081 arm64>" >&2
        echo >&2
        print_procedure
        exit 0
    fi

    local bin="$1"
    if [[ ! -f "$bin" ]]; then
        echo "[errore] File non trovato: $bin" >&2
        echo "[info] Questo script richiede il binario REALE di Geometry Dash." >&2
        exit 1
    fi

    if ! command -v otool >/dev/null 2>&1 || ! command -v nm >/dev/null 2>&1; then
        echo "[errore] otool/nm non disponibili (installare Xcode Command Line Tools)." >&2
        exit 1
    fi

    echo "== Mach-O header =="
    otool -hv "$bin" || true

    echo
    echo "== Base immagine (__TEXT vmaddr) =="
    otool -l "$bin" | awk '/LC_SEGMENT_64/{seg=1} seg && /segname __TEXT/{t=1} t && /vmaddr/{print; t=0; seg=0}' || true

    echo
    echo "== Ricerca simbolo $DEMANGLED ($MANGLED_SYMBOL) =="
    if nm -arch arm64 "$bin" 2>/dev/null | grep -q "$MANGLED_SYMBOL"; then
        nm -arch arm64 "$bin" | grep "$MANGLED_SYMBOL" | c++filt
        echo
        echo "[ok] Simbolo presente nella symtab: leggere il VM address sopra e"
        echo "     sottrarre la base __TEXT per ottenere l'offset relativo."
    else
        echo "[warn] $MANGLED_SYMBOL non trovato nella symtab (binario strippato?)."
        echo "[warn] Procedere con il pattern-scan del prologo (passo 4)."
    fi

    echo
    print_procedure
    echo
    echo "[reminder] Aggiornare mod-index/bindings/2.2081/macos-arm64.pbind con"
    echo "           l'offset relativo verificato e verified = true."
}

main "$@"
