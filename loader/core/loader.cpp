// loader/core/loader.cpp — unità di compilazione minimale del loader.
//
// Garantisce che il target `pulse_loader` produca un artefatto compilato
// (Requisito 26.3). La logica reale del LoaderCore arriva con le attività 2.x.
#include "pulse_loader/loader.hpp"

#include "pulse/version.hpp"

namespace pulse::loader {

// Verifiche statiche di coerenza dello stack al momento della build.
static_assert(host_os() == HostOs::Windows || host_os() == HostOs::MacOS ||
                  host_os() == HostOs::Linux,
              "Pulse Loader supporta solo Windows, macOS e Linux per la build host");

// Punto di ancoraggio che lega il loader alla versione dello SDK.
SdkVersion loader_sdk_version() noexcept {
    return sdk_version();
}

}  // namespace pulse::loader
