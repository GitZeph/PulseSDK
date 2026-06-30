/* pulse_loader/runtime_entry.h — entry point centralizzato esportato del
 * Loader_Artifact (Requisito 1.2).
 *
 * Quando il loader è compilato come Loader_Artifact dinamico
 * (`PULSE_BUILD_LOADER_ARTIFACT=ON` → macro `PULSE_LOADER_ARTIFACT=1`), il
 * target è costruito con visibilità nascosta di default (`-fvisibility=hidden`).
 * Questo header dichiara il SOLO simbolo C esportato del Loader_Artifact —
 * `pulse_loader_runtime_entry` — marcandolo con visibilità "default"
 * (o `dllexport` su Windows) così che resti risolvibile nell'artefatto.
 *
 * È il punto di ingresso unico e centralizzato del runtime Pulse: il
 * costruttore di early-load di piattaforma (vedi i bootstrap, attività di
 * Fase B) lo invoca una sola volta dopo aver ottenuto il controllo nel
 * processo di Geometry Dash. L'header usa il linkage C per garantire un nome
 * di simbolo stabile e non decorato.
 */
#ifndef PULSE_LOADER_RUNTIME_ENTRY_H
#define PULSE_LOADER_RUNTIME_ENTRY_H

/* Attributo di esportazione del simbolo. Sotto `-fvisibility=hidden` solo i
 * simboli marcati `visibility("default")` (o `dllexport` su Windows) finiscono
 * nella tabella dei simboli esportati dell'artefatto. */
#if defined(_WIN32)
#  define PULSE_LOADER_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#  define PULSE_LOADER_EXPORT __attribute__((visibility("default")))
#else
#  define PULSE_LOADER_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

/* Entry point centralizzato del runtime Pulse, esportato dal Loader_Artifact.
 *
 * Restituisce `true` se l'avvio del runtime è andato a buon fine. Eseguito
 * esattamente una volta per processo (l'idempotency guard del costruttore di
 * early-load è cablata in Fase B). Non propaga eccezioni e non termina il
 * processo: in caso di fallimento lascia partire Geometry Dash senza mod. */
PULSE_LOADER_EXPORT bool pulse_loader_runtime_entry(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PULSE_LOADER_RUNTIME_ENTRY_H */
