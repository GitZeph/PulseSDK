// loader/bootstrap/android_bootstrap.hpp — bootstrap Android (Req 1.1, 1.2).
//
// Implementazione di `IPlatformBootstrap` per Android basata sul precaricamento
// di una libreria nativa (`.so`). La tecnica primaria è:
//
//   1. Un wrapper APK ripacchettizzato di Geometry Dash dichiara la libreria
//      del loader tra quelle native e la carica via `System.loadLibrary(...)`
//      (oppure tramite una voce `android:name`/`<meta-data>` letta dal codice
//      di avvio dell'app).
//   2. Quando il linker dinamico mappa la `.so`, il runtime Java invoca il
//      costruttore JNI standard `JNI_OnLoad(JavaVM*, void*)`. Questo accade
//      DURANTE l'inizializzazione dell'`Application`/`Activity`, ovvero PRIMA
//      che Cocos2d-x costruisca l'`AppDelegate` e mandi in scena la prima
//      schermata del gioco (Req 1.2).
//   3. `JNI_OnLoad` ottiene così il controllo early-load e avvia il runtime
//      centralizzato del loader tramite un unico entry point (Req 1.4).
//
// Geometry Dash su Android è distribuito in due ABI: `arm64-v8a` (64-bit) e
// `armeabi-v7a` (32-bit). Entrambe sono richieste dal Req 1.1. Questa stessa
// classe rappresenta entrambe le varianti: `platform()` restituisce il valore
// `Platform` corretto in base all'ABI con cui l'unità è stata compilata
// (`__aarch64__` → AndroidArm64, altrimenti AndroidArmV7). La build produce un
// artefatto `.so` per ciascuna ABI tramite il toolchain file dell'NDK.
//
// La dichiarazione della classe è platform-agnostic, così l'intero progetto
// configura e compila anche su host non-Android (Windows/macOS/Linux). Il
// codice che usa le API JNI/NDK è isolato nel .cpp dietro
// `#if defined(__ANDROID__)`. Sull'host non-Android `inject()` ritorna un
// fallimento diagnostico `UnsupportedHost` senza side effect.
#ifndef PULSE_LOADER_BOOTSTRAP_ANDROID_BOOTSTRAP_HPP
#define PULSE_LOADER_BOOTSTRAP_ANDROID_BOOTSTRAP_HPP

#include <functional>

#include "bootstrap/platform_bootstrap.hpp"

namespace pulse::bootstrap {

// Entry point centralizzato del runtime invocato dal bootstrap una volta
// ottenuto il controllo (Req 1.4). Restituisce `true` se il runtime è stato
// avviato correttamente. È iniettabile per consentire i test e per disaccoppiare
// il bootstrap dal Loader Core.
using RuntimeEntryPoint = std::function<bool()>;

// Determina a compile-time la `Platform` Android corrispondente all'ABI con
// cui questa unità di traduzione viene compilata. Su `arm64-v8a` il compilatore
// definisce `__aarch64__`; su `armeabi-v7a` (ARM 32-bit) no. Definita anche su
// host non-Android per mantenere `platform()` totale: in quel caso il valore
// non è operativo ma identifica comunque l'ABI target nominale (arm64).
constexpr Platform android_abi_platform() noexcept {
#if defined(__aarch64__)
    return Platform::AndroidArm64;
#else
    return Platform::AndroidArmV7;
#endif
}

class AndroidBootstrap final : public IPlatformBootstrap {
public:
    AndroidBootstrap() = default;

    // Costruisce il bootstrap con un entry point del runtime esplicito.
    // Se non fornito, `inject()` considera l'aggancio early-load riuscito senza
    // avviare alcun runtime (utile finché il Loader Core non è cablato).
    explicit AndroidBootstrap(RuntimeEntryPoint runtime_entry)
        : runtime_entry_(std::move(runtime_entry)) {}

    // Esegue il bootstrap early-load guidato da `JNI_OnLoad`. Su Android:
    // verifica di non essere già iniettato e invoca l'entry point del runtime
    // prima della scena iniziale (Req 1.2, 1.4). Su host non-Android ritorna un
    // fallimento diagnostico (`UnsupportedHost`) senza side effect, così la
    // build cross-platform resta compilabile e testabile.
    BootstrapResult inject() override;

    // Piattaforma servita: determinata dall'ABI di compilazione (Req 1.1).
    // arm64-v8a → AndroidArm64; armeabi-v7a → AndroidArmV7.
    Platform platform() const override { return android_abi_platform(); }

private:
    RuntimeEntryPoint runtime_entry_{};
    bool              injected_{false};
};

}  // namespace pulse::bootstrap

#endif  // PULSE_LOADER_BOOTSTRAP_ANDROID_BOOTSTRAP_HPP
