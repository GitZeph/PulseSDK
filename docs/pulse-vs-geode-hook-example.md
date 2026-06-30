# Esempio di hooking: Pulse vs Geode affiancati

Questo documento soddisfa il **Requisito 5.5**: *il Pulse_SDK deve fornire almeno un esempio documentato che mostri affiancate la sintassi di hooking di Pulse e la sintassi equivalente di Geode per lo stesso hook.*

L'esempio usa lo **stesso hook** in entrambe le sintassi: l'override di `MenuLayer::init`, la funzione invocata da Geometry Dash quando costruisce il menu principale. In entrambi i casi l'obiettivo è identico: eseguire del codice della mod e poi invocare la funzione originale del gioco preservandone parametri e valore di ritorno.

La sintassi Pulse riportata di seguito è quella **realmente implementata** in [`sdk/include/pulse/hooks.hpp`](../sdk/include/pulse/hooks.hpp): la macro `PULSE_HOOK(Symbol, ReturnType, Params)`, la variante con priorità `PULSE_HOOK_PRIORITY(Symbol, Priority, ReturnType, Params)`, l'helper `callOriginal(...)` e la verifica della firma a compile-time tramite il concept `SignatureMatches`.

## Lo stesso hook nelle due sintassi

### Geode (`$modify`)

```cpp
#include <Geode/modify/MenuLayer.hpp>
using namespace geode::prelude;

// $modify genera una classe derivata da MenuLayer.
// L'override di init() ridefinisce il metodo del gioco.
class $modify(MyMenuLayer, MenuLayer) {
    bool init() {
        // Si chiama l'originale per nome qualificato della classe base.
        if (!MenuLayer::init()) return false;
        log::info("MenuLayer caricato");
        return true;
    }
};
```

Caratteristiche della forma Geode:

- l'aggancio passa per la generazione di una **classe derivata** (`$modify(MyMenuLayer, MenuLayer)`);
- l'originale si invoca scrivendo a mano `MenuLayer::init()`, ripetendo il nome della classe bersaglio;
- la corrispondenza tra la firma dell'override e quella reale della funzione del gioco non è verificata in modo uniforme a compile-time: incompatibilità di firma tendono a emergere a **link-time o a runtime**.

### Pulse (`PULSE_HOOK`)

```cpp
#include <pulse/hooks.hpp>

// PULSE_HOOK(Symbol, ReturnType, Params)
//  - Symbol      : riferimento simbolico alla funzione bersaglio (Req 5.1).
//                  Nessun indirizzo di memoria, nessuna firma grezza.
//  - ReturnType  : tipo di ritorno della firma del bersaglio.
//  - Params      : lista dei parametri tra parentesi.
// La firma dichiarata è verificata A COMPILE-TIME contro il binding (Req 5.2).
PULSE_HOOK(MenuLayer_init, bool, (MenuLayer* self)) {
    // callOriginal inoltra a perfezione gli argomenti al trampolino verso
    // l'originale, preservando parametri e valore di ritorno (Req 5.3).
    if (!callOriginal(self)) return false;
    Pulse::log().info("MenuLayer caricato");
    return true;
}
```

Lo stesso hook con **priorità di catena** esplicita (Requisito 3.2), per controllare l'ordine quando più mod agganciano la stessa funzione:

```cpp
#include <pulse/hooks.hpp>

// PULSE_HOOK_PRIORITY(Symbol, Priority, ReturnType, Params)
//  - Priority : intero 0..1000 (default 500). Valore più alto = eseguito prima.
PULSE_HOOK_PRIORITY(MenuLayer_init, 600, bool, (MenuLayer* self)) {
    if (!callOriginal(self)) return false;
    Pulse::log().info("MenuLayer caricato con priorità 600");
    return true;
}
```

> Nota: il corpo che segue la macro **è** il detour. Il nome `self` è un normale
> nome di parametro scelto dal Developer e non fa parte del tipo funzione: la
> verifica di compatibilità riguarda tipo di ritorno e tipi dei parametri, non i
> loro nomi.

## Come Pulse verifica la firma a compile-time

La macro espande, tra le altre cose, in uno `static_assert` basato sul concept `SignatureMatches`, definito in `hooks.hpp`:

```cpp
template <auto Symbol, class Declared>
concept SignatureMatches =
    !HasBindingTraits<Symbol> ||
    std::same_as<typename BindingTraits<Symbol>::Fn, Declared>;
```

Il layer dei bindings specializza `BindingTraits` per ogni simbolo conosciuto, esponendo la firma canonica della funzione bersaglio:

```cpp
template <>
struct pulse::hooks::BindingTraits<pulse::hooks::FixedString("MenuLayer_init")> {
    using Fn = bool(MenuLayer*);   // firma canonica del bersaglio
};
```

Con questa specializzazione presente, se il Developer dichiara una firma incompatibile la **compilazione fallisce** con un messaggio diagnostico (Requisito 5.2). Per esempio:

```cpp
// ERRORE A COMPILE-TIME: il binding di MenuLayer_init è bool(MenuLayer*),
// ma qui si dichiara int(MenuLayer*) (tipo di ritorno incompatibile).
PULSE_HOOK(MenuLayer_init, int, (MenuLayer* self)) {
    return 0;
}
// static_assert fallisce:
// "PULSE_HOOK(MenuLayer_init): la firma dichiarata è incompatibile con quella
//  del binding della funzione bersaglio (tipo di ritorno o parametri diversi)."
```

Se per un simbolo non esiste (ancora) una specializzazione di `BindingTraits`, il concept è permissivo a compile-time e la coerenza viene comunque verificata a load-time durante la risoluzione del simbolo (`resolve_all`, Requisito 5.4).

## Vantaggi ergonomici e di sicurezza di Pulse

| Aspetto | Geode (`$modify`) | Pulse (`PULSE_HOOK`) |
|---------|-------------------|----------------------|
| Riferimento al bersaglio | classe derivata generata dalla macro | riferimento simbolico (`Symbol`), nessun indirizzo o firma grezza (Req 5.1) |
| Verifica della firma | spesso emersa a link-time o runtime | `static_assert` a **compile-time** con messaggio diagnostico (Req 5.2) |
| Invocazione dell'originale | `MenuLayer::init()` scritto a mano | `callOriginal(...)` che preserva parametri e valore di ritorno (Req 5.3) |
| Simbolo non risolvibile | comportamento meno granulare | solo quell'hook è annullato e segnalato, gli altri proseguono (Req 5.4) |
| Priorità di catena | priorità presente, parità non sempre deterministica | `PULSE_HOOK_PRIORITY` con ordine deterministico per priorità + ordine di caricamento (Req 3.2, 3.3) |

In sintesi, la sintassi Pulse riduce il codice ripetitivo (niente nome della classe bersaglio ripetuto, niente indirizzi), sposta gli errori di firma dalla fase di link/runtime alla fase di **compilazione** e rende esplicito e sicuro l'aggancio alla funzione originale tramite `callOriginal`, che propaga in modo type-safe parametri e valore di ritorno della firma del bersaglio.

## Riferimenti

- Implementazione reale della macro: [`sdk/include/pulse/hooks.hpp`](../sdk/include/pulse/hooks.hpp)
- Tabella di confronto completa Pulse vs Geode: [`docs/pulse-vs-geode.md`](./pulse-vs-geode.md) (voce `IMP-01`, sintassi di hooking)
- Requisiti correlati: 5.1, 5.2, 5.3, 5.4, 5.5 (hooking dichiarativo), 3.2, 3.3 (priorità e ordine di catena)
