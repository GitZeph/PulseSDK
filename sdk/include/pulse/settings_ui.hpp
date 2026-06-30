// pulse/settings_ui.hpp — header pubblico dell'UI auto-generata delle
// impostazioni dello SDK Pulse (Layer 5, Requisiti 9.2, 9.3, 9.5, 10.3, 10.6).
//
// Questo è il punto d'ingresso pubblico del task 31.2. Include sia la
// dichiarazione tipizzata delle impostazioni (<pulse/settings.hpp>, task 19.1)
// sia lo storage per-mod (<pulse/storage.hpp>, task 20.1), e l'implementazione
// header-only del controller (sotto sdk/settings/settings_ui.hpp). I tipi
// pubblici sono promossi nel namespace `pulse` per l'uso ergonomico da parte
// dei Developer.
//
// API principali:
//   * pulse::SettingsUiController(registry, storage, clock, confirm, sink)
//   * controller.generateControls()      -> un controllo per setting (Req 9.2)
//   * controller.edit(name, value)        -> persiste/rifiuta l'edit (Req 9.3/9.5)
//   * controller.currentValue(name)       -> valore persistito o default (Req 10.3)
//   * controller.deletePersistedData()    -> elimina su conferma User (Req 10.6)
//
// Stack: C++20/23 (Requisito 26.1). Header-only, nessuna dipendenza esterna.
#ifndef PULSE_SETTINGS_UI_HPP
#define PULSE_SETTINGS_UI_HPP

#include <pulse/settings.hpp>
#include <pulse/storage.hpp>

// L'implementazione è collocata sotto sdk/settings/ ed esposta da questo header
// pubblico tramite include relativo, così non dipende dalla presenza di
// sdk/settings/ nella include path pubblica.
#include "../../settings/settings_ui.hpp"

namespace pulse {

// Promuove i tipi pubblici dell'UI delle impostazioni nel namespace `pulse`.
using settings::ControlDescriptor;
using settings::ControlKind;
using settings::EditResult;
using settings::RemovalResult;
using settings::SettingsUiController;
using settings::controlKindFor;

}  // namespace pulse

#endif  // PULSE_SETTINGS_UI_HPP
