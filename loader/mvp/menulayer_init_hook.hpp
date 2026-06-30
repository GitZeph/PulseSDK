// loader/mvp/menulayer_init_hook.hpp — hook dimostrativo MVP su MenuLayer::init.
//
// Questo modulo dichiara il detour dimostrativo del MVP (design → "Hook
// dimostrativo su MenuLayer::init + logging"). Il detour è generato dalla macro
// `PULSE_HOOK` dello SDK (single-hook MVP) e, una volta cablato dall'Hooking
// Engine, esegue PRIMA il proprio codice (con logging) e POI invoca l'originale
// del gioco tramite `callOriginal`, preservandone parametri e valore di ritorno
// (Requisiti 2.2, 5.3). L'esecuzione del detour e dell'originale viene loggata
// (criterio di completamento osservabile dell'MVP).
//
// Cross-host: il `MenuLayer` reale del gioco non è disponibile sull'host di
// build/test (macOS/Linux); qui si usa un segnaposto tipizzato che modella solo
// lo stato osservabile necessario a dimostrare che l'originale è stato eseguito.
#ifndef PULSE_LOADER_MVP_MENULAYER_INIT_HOOK_HPP
#define PULSE_LOADER_MVP_MENULAYER_INIT_HOOK_HPP

#include <functional>
#include <string_view>

namespace pulse::loader::mvp {

// Segnaposto del game object `MenuLayer` per il demo MVP. Il layout reale del
// gioco è diverso: qui rappresentiamo solo un flag osservabile che l'originale
// imposta, così i test possono verificare che l'originale sia stato invocato.
struct MenuLayer {
    bool initialized = false;
};

// Sink di log del demo, usato dal detour per registrare la propria esecuzione
// e quella dell'originale. Iniettabile per i test; se non impostato i messaggi
// vengono ignorati.
using DemoLogSink = std::function<void(std::string_view)>;

// Imposta (o azzera) il sink di log del demo.
void set_demo_log_sink(DemoLogSink sink);

// Registra un messaggio del demo sul sink corrente, se presente.
void demo_log(std::string_view message);

// Identità della demo mod come sorgente diagnostica (Req 9.2). Il detour e il
// cablaggio (DemoMod) usano questo identificatore così ogni log della prova
// end-to-end identifica la demo mod e l'hook come sorgente.
inline constexpr std::string_view kDemoModId = "pulse.demo";

// Simbolo del binding del gioco risolto dal Bindings System (Req 20.2).
inline constexpr std::string_view kMenuLayerInitBindingSymbol = "MenuLayer::init";

// Simbolo con cui il detour è registrato presso il registro dello SDK. La macro
// `PULSE_HOOK` usa un identificatore C++ (privo di `::`), quindi differisce dal
// simbolo del binding: la riconciliazione avviene al cablaggio (MvpLoader).
inline constexpr std::string_view kMenuLayerInitRegistrationSymbol = "MenuLayer_init";

// Forza il link dell'unità di traduzione del detour quando il loader è una
// libreria statica, garantendo l'esecuzione dell'inizializzatore statico che
// registra il detour presso il registro dello SDK. Restituisce il simbolo di
// registrazione del detour dimostrativo.
std::string_view ensure_menulayer_init_hook_registered();

}  // namespace pulse::loader::mvp

#endif  // PULSE_LOADER_MVP_MENULAYER_INIT_HOOK_HPP
