// pulse/pulse.hpp — header pubblico aggregatore dello SDK Pulse.
//
// Questo è il punto d'ingresso pubblico per i Developer di mod. Le singole
// API (PULSE_HOOK, PulseField, EventBus, settings, storage, async, http, log)
// verranno aggiunte dalle attività successive del piano di implementazione,
// ciascuna nel proprio header sotto pulse/.
//
// Stack: C++20/23 (Requisito 26.1).
#ifndef PULSE_PULSE_HPP
#define PULSE_PULSE_HPP

#include "pulse/version.hpp"

// Hooking dichiarativo: macro PULSE_HOOK minimale (single hook) con callOriginal
// (Requisiti 5.1, 5.3, 2.2).
#include "pulse/hooks.hpp"

#endif  // PULSE_PULSE_HPP
