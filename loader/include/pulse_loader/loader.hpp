// pulse_loader/loader.hpp — header interno del runtime del loader Pulse.
//
// Definisce l'identità di base del loader. I sottosistemi reali (bootstrap,
// detection, hooking, lifecycle) saranno aggiunti dalle attività successive.
#ifndef PULSE_LOADER_LOADER_HPP
#define PULSE_LOADER_LOADER_HPP

#include "pulse/version.hpp"

namespace pulse::loader {

// Sistema operativo host su cui il loader è stato compilato.
enum class HostOs {
    Windows,
    MacOS,
    Linux,
};

// Sistema operativo target rilevato a compile-time per questa build.
constexpr HostOs host_os() noexcept {
#if defined(_WIN32)
    return HostOs::Windows;
#elif defined(__APPLE__)
    return HostOs::MacOS;
#elif defined(__linux__)
    return HostOs::Linux;
#else
#error "Pulse Loader: sistema operativo non supportato"
#endif
}

// Restituisce la versione dello SDK con cui il loader è stato compilato.
// Definita in loader/core/loader.cpp: linkare contro questo simbolo valida
// che il target statico `pulse_loader` sia compilato e collegabile.
SdkVersion loader_sdk_version() noexcept;

}  // namespace pulse::loader

#endif  // PULSE_LOADER_LOADER_HPP
