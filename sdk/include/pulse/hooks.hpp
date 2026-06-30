// pulse/hooks.hpp — hooking dichiarativo dello SDK Pulse (Layer 5, Requisito 5).
//
// Questo header fornisce la macro `PULSE_HOOK`, cuore dell'API di hooking
// dichiarativo dello SDK. La macro:
//
//   * associa un metodo della mod a una funzione del gioco bersaglio usando
//     unicamente un RIFERIMENTO SIMBOLICO alla funzione (Requisito 5.1): il
//     Developer non scrive indirizzi di memoria né firme grezze;
//   * VERIFICA A COMPILE-TIME che la firma dichiarata sia compatibile con
//     quella esposta dal binding del simbolo, tramite il concept
//     `SignatureMatches` e uno `static_assert` (Requisito 5.2): una firma
//     incompatibile fa FALLIRE LA COMPILAZIONE con un messaggio diagnostico;
//   * espande in un DETOUR REGISTRATO presso un registro globale dello SDK,
//     pronto per essere installato dall'Hooking Engine del loader (Requisito 2.2);
//   * supporta una `priority` di catena (Requisito 3.2) tramite la variante
//     `PULSE_HOOK_PRIORITY`;
//   * fornisce `callOriginal(...)` che invoca la funzione originale del gioco
//     PRESERVANDO parametri e valore di ritorno della firma (Requisito 5.3).
//
// Risoluzione a load-time (Requisito 5.4): `resolve_all(resolver)` percorre il
// registro e marca ogni hook come risolto/irrisolto in base alla risoluzione
// del simbolo, SENZA lanciare eccezioni. Un hook il cui simbolo non è
// risolvibile viene annullato (marcato irrisolto e segnalato) mentre gli altri
// hook validi restano risolti e installabili.
//
// Modello di integrazione (header-only, zero-cost):
//   - Lo SDK è il layer inferiore: NON dipende dal loader. La macro pubblica
//     solo `symbol + detour + slot del trampolino + priority`. Il loader
//     risolve il simbolo tramite i bindings (vedi `resolve_all`), installa il
//     detour sul backend e scrive il trampolino restituito nello slot della
//     registrazione via `pulse::hooks::bind_trampoline`.
//   - La verifica della firma a compile-time si appoggia a `BindingTraits`,
//     che il layer dei bindings specializza per ciascun simbolo esponendo il
//     tipo canonico della funzione bersaglio (`Fn`). In assenza di una
//     specializzazione il controllo è permissivo (vedi `SignatureMatches`): la
//     coerenza viene comunque verificata a load-time.
//
// Stack: C++20/23 (Requisito 26.1). Header-only, nessuna dipendenza esterna.
#ifndef PULSE_HOOKS_HPP
#define PULSE_HOOKS_HPP

#include <concepts>
#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

// Delegazione cross-immagine (solo build POSIX FUORI dal Loader_Artifact).
// Quando una mod esterna `dlopen`'d (o un test/standalone) gira in un processo
// che ha caricato un Loader_Artifact, `register_hook` delega l'iscrizione al
// registro del Loader risolvendo `pulse_loader_register_hook` via dyld. Dentro
// l'artefatto questo header NON è incluso (registrazione locale diretta).
#if !defined(PULSE_LOADER_ARTIFACT) && (defined(__APPLE__) || defined(__unix__))
#include <dlfcn.h>
#include <string>
#endif

namespace pulse::hooks {

// ---------------------------------------------------------------------------
// FixedString — letterale di stringa utilizzabile come parametro template
// non-tipo (NTTP). Consente di indicizzare `BindingTraits` per nome simbolico
// del bersaglio in modo strutturale (C++20), così la macro può passare
// `#Symbol` direttamente al concept di verifica della firma.
// ---------------------------------------------------------------------------
template <std::size_t N>
struct FixedString {
    char value[N]{};

    constexpr FixedString(const char (&literal)[N]) {
        for (std::size_t i = 0; i < N; ++i) {
            value[i] = literal[i];
        }
    }

    // Vista senza il terminatore NUL (utile per diagnostica/confronti).
    [[nodiscard]] constexpr std::string_view view() const {
        return std::string_view{value, N - 1};
    }

    // Necessario perché il tipo sia "strutturale" e quindi usabile come NTTP.
    friend constexpr bool operator==(const FixedString&,
                                     const FixedString&) = default;
};

// ---------------------------------------------------------------------------
// BindingTraits — tratto specializzato dal layer dei bindings per esporre la
// firma canonica della funzione bersaglio di un simbolo.
//
// Il template primario è VUOTO (nessun membro `Fn`): per i simboli privi di
// binding noto a compile-time il controllo della firma è demandato alla
// risoluzione a load-time. Il layer dei bindings fornisce una specializzazione
// esplicita per ogni simbolo conosciuto:
//
//   template <>
//   struct pulse::hooks::BindingTraits<pulse::hooks::FixedString("MenuLayer_init")> {
//       using Fn = bool(MenuLayer*);   // firma canonica del bersaglio
//   };
// ---------------------------------------------------------------------------
template <FixedString Symbol>
struct BindingTraits {};

// Vero se esiste una specializzazione di BindingTraits<Symbol> che dichiara il
// tipo della funzione bersaglio `Fn`.
template <FixedString Symbol>
concept HasBindingTraits = requires { typename BindingTraits<Symbol>::Fn; };

// ---------------------------------------------------------------------------
// SignatureMatches — vero se la firma dichiarata `Declared` è compatibile con
// quella del binding del simbolo `Symbol` (Requisito 5.2).
//
// Semantica:
//   * se `Symbol` ha un binding noto (BindingTraits<Symbol>::Fn esiste): vale
//     SOLO se la firma dichiarata coincide esattamente con quella del binding
//     (tipo di ritorno + numero/tipi dei parametri). I nomi dei parametri non
//     fanno parte del tipo funzione, quindi sono ignorati;
//   * se `Symbol` non ha un binding noto a compile-time: il concept è
//     soddisfatto (controllo permissivo) e la coerenza viene verificata a
//     load-time da `resolve_all` (Requisito 5.4).
// ---------------------------------------------------------------------------
template <auto Symbol, class Declared>
concept SignatureMatches =
    !HasBindingTraits<Symbol> ||
    std::same_as<typename BindingTraits<Symbol>::Fn, Declared>;

// ---------------------------------------------------------------------------
// HookRegistration — descrittore di un hook dichiarato con PULSE_HOOK.
//
// `symbol`     riferimento simbolico alla funzione bersaglio (Req 5.1);
// `detour`     puntatore al detour generato dalla macro (Req 2.2);
// `trampoline` slot in cui l'Hooking Engine scrive il trampolino verso
//              l'originale; `callOriginal` lo invoca preservando firma,
//              parametri e valore di ritorno (Req 5.3);
// `priority`   priorità di catena (0..1000, default 500, Req 3.2);
// `resolved`   stato di risoluzione del simbolo a load-time (Req 5.4):
//              true = simbolo risolto e installabile; false = irrisolto e
//              quindi annullato (gli altri hook proseguono);
// `target`     indirizzo risolto della funzione bersaglio (nullptr se irrisolto).
// ---------------------------------------------------------------------------
struct HookRegistration {
    std::string_view symbol;
    void* detour{nullptr};
    void** trampoline{nullptr};
    int priority{500};
    bool resolved{false};
    void* target{nullptr};
};

// Registro globale dei detour dichiarati.
// Singleton header-only: l'`inline function` garantisce un'unica istanza tra
// tutte le unità di traduzione (C++17+), senza richiedere un file sorgente.
[[nodiscard]] inline std::vector<HookRegistration>& registry() {
    static std::vector<HookRegistration> instance;
    return instance;
}

// Registra un detour presso il registro globale. Invocata dalla macro al
// caricamento del modulo (inizializzazione statica). Restituisce la
// registrazione creata. `priority` ha default 500 (Req 3.2) per preservare la
// forma minimale `PULSE_HOOK(Symbol, ReturnType, Params)`.
//
// Il comportamento è adattivo a compile/run-time in due rami:
#if defined(PULSE_LOADER_ARTIFACT)
// Dentro il Loader_Artifact: registra SEMPRE nel registro di QUESTA immagine
// (il registro del Loader). Nessuna delega: eviterebbe la ricorsione con
// l'entry point esportato `pulse_loader_register_hook`, che a sua volta chiama
// `register_hook` (vedi loader/core/mod_registrar.cpp).
inline HookRegistration register_hook(std::string_view symbol, void* detour,
                                      void** trampoline, int priority = 500) {
    HookRegistration registration{symbol, detour, trampoline, priority,
                                  /*resolved=*/false, /*target=*/nullptr};
    registry().push_back(registration);
    return registration;
}
#else
// Fuori dal Loader (una mod `.dylib`, o un test/standalone host): se il processo
// ha caricato un Loader_Artifact che esporta `pulse_loader_register_hook`,
// delega a quello così l'hook atterra nel registro del LOADER (condiviso oltre
// il confine del `dlopen`). Altrimenti ricade sul registro locale di questa
// immagine (test host, nessun loader presente) — identico al comportamento
// precedente.
inline HookRegistration register_hook(std::string_view symbol, void* detour,
                                      void** trampoline, int priority = 500) {
#if defined(__APPLE__) || defined(__unix__)
    using LoaderRegFn = void (*)(const char*, void*, void**, int);
    // Risolto una volta per immagine; RTLD_DEFAULT cerca nell'intero processo.
    static LoaderRegFn loaderReg = reinterpret_cast<LoaderRegFn>(
        ::dlsym(RTLD_DEFAULT, "pulse_loader_register_hook"));
    if (loaderReg != nullptr) {
        std::string sym(symbol);  // garantisce NUL-terminazione; il loader copia
        loaderReg(sym.c_str(), detour, trampoline, priority);
        // Descrittore di solo valore (la registrazione autorevole vive nel
        // registro del Loader); ai chiamanti serve solo un valore.
        return HookRegistration{symbol, detour, trampoline, priority,
                                /*resolved=*/false, /*target=*/nullptr};
    }
#endif
    HookRegistration registration{symbol, detour, trampoline, priority,
                                  /*resolved=*/false, /*target=*/nullptr};
    registry().push_back(registration);
    return registration;
}
#endif

// Cerca la registrazione di un simbolo (corrispondenza esatta). Restituisce
// nullptr se nessun hook è stato dichiarato per quel simbolo.
[[nodiscard]] inline const HookRegistration* find(std::string_view symbol) {
    for (const auto& registration : registry()) {
        if (registration.symbol == symbol) {
            return &registration;
        }
    }
    return nullptr;
}

// Cabla il trampolino dell'originale per un hook registrato: l'Hooking Engine
// la invoca dopo `install()` passando l'indirizzo del trampolino. Dopo questa
// chiamata `callOriginal(...)` del detour invoca l'originale del gioco.
// Restituisce false se il simbolo non è registrato o lo slot è nullo.
inline bool bind_trampoline(std::string_view symbol, void* trampoline) {
    for (auto& registration : registry()) {
        if (registration.symbol == symbol) {
            if (registration.trampoline == nullptr) {
                return false;
            }
            *registration.trampoline = trampoline;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// ResolutionReport — esito di una passata di risoluzione a load-time.
//
// `resolved`   simboli risolti con successo (hook installabili);
// `unresolved` simboli NON risolvibili: i relativi hook sono stati annullati
//              (marcati irrisolti) e qui segnalati, senza interrompere gli
//              altri hook validi (Requisito 5.4).
// ---------------------------------------------------------------------------
struct ResolutionReport {
    std::vector<std::string_view> resolved;
    std::vector<std::string_view> unresolved;

    [[nodiscard]] bool all_resolved() const { return unresolved.empty(); }
    [[nodiscard]] std::size_t resolved_count() const { return resolved.size(); }
    [[nodiscard]] std::size_t unresolved_count() const {
        return unresolved.size();
    }
};

// Percorre il registro e risolve ogni simbolo tramite `resolve`, un invocabile
// `void*(std::string_view symbol)` che restituisce l'indirizzo della funzione
// bersaglio o nullptr se il simbolo non è risolvibile.
//
// Per ogni hook:
//   * se risolto (indirizzo non nullo): viene marcato `resolved = true` e
//     `target` aggiornato → l'hook resta valido e installabile;
//   * se irrisolto (nullptr): l'hook è ANNULLATO (marcato `resolved = false`,
//     `target = nullptr`) e segnalato in `unresolved`, MA la passata PROSEGUE
//     con gli altri hook senza lanciare eccezioni (Requisito 5.4).
//
// Restituisce un report con l'elenco dei simboli risolti e irrisolti.
template <class Resolver>
inline ResolutionReport resolve_all(Resolver&& resolve) {
    ResolutionReport report;
    for (auto& registration : registry()) {
        void* target = resolve(registration.symbol);
        if (target != nullptr) {
            registration.resolved = true;
            registration.target = target;
            report.resolved.push_back(registration.symbol);
        } else {
            // Annulla SOLO questo hook e prosegui con gli altri (Req 5.4).
            registration.resolved = false;
            registration.target = nullptr;
            report.unresolved.push_back(registration.symbol);
        }
    }
    return report;
}

// Numero di hook attualmente registrati (introspezione/diagnostica).
[[nodiscard]] inline std::size_t count() { return registry().size(); }

}  // namespace pulse::hooks

// ---------------------------------------------------------------------------
// PULSE_HOOK / PULSE_HOOK_PRIORITY — macro di dichiarazione di un hook.
//
// Uso (priorità di default 500):
//   PULSE_HOOK(MenuLayer_init, bool, (MenuLayer* self)) {
//       if (!callOriginal(self)) return false;   // preserva ritorno (Req 5.3)
//       // ... logica del detour ...
//       return true;
//   }
//
// Uso con priorità esplicita di catena (Req 3.2):
//   PULSE_HOOK_PRIORITY(MenuLayer_init, 600, bool, (MenuLayer* self)) { ... }
//
// Parametri della macro:
//   Symbol      identificatore simbolico della funzione bersaglio (Req 5.1).
//               Diventa anche il nome stringa registrato (es. "MenuLayer_init").
//   Priority    priorità di catena 0..1000 (solo PULSE_HOOK_PRIORITY).
//   ReturnType  tipo di ritorno della firma del bersaglio.
//   Params      lista dei parametri tra parentesi, es. (MenuLayer* self).
//
// L'espansione genera, in un namespace dedicato `pulse_hook_<Symbol>`:
//   - `PulseFn`        : il tipo funzione dichiarato (`ReturnType Params`);
//   - uno `static_assert(SignatureMatches<#Symbol, PulseFn>)` che fa fallire la
//     compilazione con diagnostica se la firma è incompatibile col binding
//     (Requisito 5.2);
//   - `pulse_original` : slot del trampolino verso l'originale (PulseFn*);
//   - `callOriginal`   : inoltro perfetto al trampolino (preserva tutto);
//   - `pulse_detour`   : il detour, il cui corpo è scritto dal Developer dopo
//                        la macro; ha visibilità diretta su `callOriginal`;
//   - `pulse_registration` : registra symbol+detour+slot+priority al caricamento.
// ---------------------------------------------------------------------------

// La registrazione del detour avviene tramite l'inizializzatore statico della
// variabile `pulse_registration`. Quella variabile non è ODR-usata da altro
// codice: in una build ottimizzata (es. il Loader_Artifact `.dylib` con
// dead-strip) il linker la eliminerebbe, e con essa il suo inizializzatore, NON
// eseguendo mai `register_hook` (registro vuoto → `find` ritorna nullptr). Per
// renderla a prova di dead-strip la marchiamo `used` (idioma standard per gli
// oggetti auto-registranti). Solo GCC/Clang: altrove (MSVC) la macro è vuota.
#if defined(__GNUC__) || defined(__clang__)
#define PULSE_HOOK_USED __attribute__((used))
#else
#define PULSE_HOOK_USED
#endif

#define PULSE_HOOK_PRIORITY(Symbol, Priority, ReturnType, Params)               \
    namespace pulse_hook_##Symbol {                                             \
        /* Tipo della funzione bersaglio (i nomi dei parametri sono ignorati). */ \
        using PulseFn = ReturnType Params;                                      \
        /* Verifica della firma a compile-time contro il binding (Req 5.2). */  \
        static_assert(                                                          \
            ::pulse::hooks::SignatureMatches<                                   \
                ::pulse::hooks::FixedString(#Symbol), PulseFn>,                 \
            "PULSE_HOOK(" #Symbol                                               \
            "): la firma dichiarata è incompatibile con quella del binding "    \
            "della funzione bersaglio (tipo di ritorno o parametri diversi).");  \
        /* Slot del trampolino: cablato dall'Hooking Engine dopo install(). */  \
        inline PulseFn* pulse_original = nullptr;                               \
        /* Dichiarazione anticipata del detour; il corpo segue la macro. */     \
        ReturnType pulse_detour Params;                                         \
        /* Invoca l'originale preservando parametri e valore di ritorno. */     \
        template <class... PulseArgs>                                           \
        inline ReturnType callOriginal(PulseArgs&&... pulse_args) {             \
            return pulse_original(std::forward<PulseArgs>(pulse_args)...);      \
        }                                                                       \
        /* Registrazione del detour al caricamento del modulo. `used` impedisce */ \
        /* il dead-strip dell'inizializzatore statico (vedi nota sopra). */     \
        PULSE_HOOK_USED                                                         \
        inline const ::pulse::hooks::HookRegistration pulse_registration =      \
            ::pulse::hooks::register_hook(                                      \
                #Symbol, reinterpret_cast<void*>(&pulse_detour),                \
                reinterpret_cast<void**>(&pulse_original), (Priority));         \
    }                                                                           \
    ReturnType pulse_hook_##Symbol::pulse_detour Params

// Forma minimale: priorità di catena di default (500, Req 3.2).
#define PULSE_HOOK(Symbol, ReturnType, Params)                                  \
    PULSE_HOOK_PRIORITY(Symbol, 500, ReturnType, Params)

#endif  // PULSE_HOOKS_HPP
