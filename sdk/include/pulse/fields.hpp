// pulse/fields.hpp — field injection type-safe dello SDK Pulse
// (Layer 5, Requisito 6).
//
// Questo header fornisce `PulseField<T, Key>`: il meccanismo con cui una mod
// associa a un'istanza di una classe del gioco fino a 256 campi addizionali
// tipizzati, ciascuno identificato da una CHIAVE ESPLICITA univoca (Req 6.1),
// con isolamento per-istanza (Req 6.2) e valore di DEFAULT tipizzato quando il
// campo non è ancora stato scritto (Req 6.3).
//
// Portata di questa attività (task 16.1): storage per-istanza
// (`instance -> storage`), get/set, default tipizzato e registro chiavi per
// classe (fino a 256 campi).
//
// Aggiunto in task 16.2 (Req 6.4): cleanup al distruttore. Ogni istanziazione
// `PulseField<T, Key, Class>` registra UNA VOLTA, nel registro per classe, un
// "cleaner" type-erased che sa rilasciare il proprio storage per una singola
// istanza. L'entry point a livello SDK `pulse::fields::releaseInstance<Class>(
// instance)` invoca tutti i cleaner registrati per quella classe su QUELLA
// sola istanza, rilasciando entro un'unica operazione TUTTO lo stato iniettato
// di quell'istanza, senza toccare lo stato delle altre istanze (Req 6.4). È
// questo il punto che l'hook del distruttore della classe del gioco — cablato
// dal loader (vedi `pulse_loader`/bindings) — deve chiamare passando il `this`
// dell'istanza in distruzione (vedi nota di cablaggio sotto `releaseInstance`).
//
// Aggiunto in task 16.5 (Req 6.5, 6.6): controlli di non-compilazione.
//   * Req 6.5 (tipo errato): la type-safety è garantita PER COSTRUZIONE — il
//     valore di un campo ha il tipo `T` dell'istanziazione `PulseField<T,...>`,
//     quindi `set`/`get`/`getRef` con un tipo non convertibile a/da `T` non
//     compila.
//   * Req 6.6 (conflitto chiave/tipo sulla stessa classe): un registro di tipi
//     a compile-time (`DeclaredFieldType<Class, Key>`, popolato dalla macro
//     `PULSE_FIELD_KEY`) consente di dichiarare il tipo canonico di una chiave
//     per una classe; `PulseField` contiene uno `static_assert` che fallisce la
//     compilazione se la stessa chiave viene poi dichiarata per la STESSA
//     classe con un tipo DIVERSO. Il meccanismo è opt-in e non-invasivo: in
//     assenza di registrazione nessun vincolo aggiuntivo è imposto.
//
// Modello di isolamento (Req 6.2): ogni istanziazione distinta
// `PulseField<T, Key, Class>` possiede una propria mappa statica
// `const void* (indirizzo istanza) -> T`. Lo stato di un'istanza è quindi
// indipendente da quello di ogni altra istanza e da ogni altro campo.
//
// Stack: C++20/23 (Requisito 26.1). Header-only, nessuna dipendenza esterna.
#ifndef PULSE_FIELDS_HPP
#define PULSE_FIELDS_HPP

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulse {

// ---------------------------------------------------------------------------
// FixedString — stringa letterale utilizzabile come parametro template
// non-type (NTTP), così la chiave del campo è parte del TIPO di PulseField.
// Questo rende la chiave esplicita (Req 6.1) e abilita, in una attività
// successiva (task 16.5), i controlli di conflitto a compile-time (Req 6.6).
// ---------------------------------------------------------------------------
template <std::size_t N>
struct FixedString {
    char value[N]{};

    constexpr FixedString(const char (&str)[N]) {  // NOLINT(google-explicit-constructor)
        for (std::size_t i = 0; i < N; ++i) {
            value[i] = str[i];
        }
    }

    // Vista sulla chiave senza il terminatore NUL.
    [[nodiscard]] constexpr std::string_view view() const {
        return std::string_view(value, N - 1);
    }

    [[nodiscard]] constexpr std::size_t size() const { return N - 1; }
};

namespace fields::detail {

// ---------------------------------------------------------------------------
// FieldRegistry — registro delle chiavi dei campi PER CLASSE del gioco.
//
// Tiene traccia delle chiavi distinte dichiarate per una data classe `Class`
// e impone la capacità massima di 256 campi per istanza/classe (Req 6.1).
// È istanziato una volta per ciascun tipo `Class` (statico header-only).
// ---------------------------------------------------------------------------
template <class Class>
struct FieldRegistry {
    // Numero massimo di campi tipizzati associabili a un'istanza (Req 6.1).
    static constexpr std::size_t kMaxFields = 256;

    // Cleaner type-erased per il cleanup al distruttore (Req 6.4): rilascia lo
    // storage di UN campo per UNA singola istanza (identificata dal suo
    // indirizzo). Restituisce true se esisteva uno stato da rilasciare.
    using Cleaner = bool (*)(const void* instance);

    // Insieme ordinato delle chiavi registrate (lookup eterogeneo su
    // string_view tramite comparatore trasparente).
    [[nodiscard]] static std::set<std::string, std::less<>>& keys() {
        static std::set<std::string, std::less<>> instance;
        return instance;
    }

    // Elenco dei cleaner registrati per questa classe: uno per ciascuna
    // istanziazione distinta `PulseField<T, Key, Class>` (Req 6.4).
    [[nodiscard]] static std::vector<Cleaner>& cleaners() {
        static std::vector<Cleaner> instance;
        return instance;
    }

    // Registra una chiave per questa classe. Idempotente: registrare due volte
    // la stessa chiave non la duplica. Restituisce false SOLO se la chiave è
    // nuova e la classe ha già raggiunto il limite di 256 campi (Req 6.1).
    static bool registerKey(std::string_view key) {
        auto& set = keys();
        if (set.find(key) != set.end()) {
            return true;  // già registrata
        }
        if (set.size() >= kMaxFields) {
            return false;  // capacità (256) raggiunta
        }
        set.emplace(key);
        return true;
    }

    // Registra un cleaner per questa classe (chiamato UNA VOLTA per ciascuna
    // istanziazione `PulseField<T, Key, Class>`, vedi PulseField). Ogni cleaner
    // rilascia lo storage del proprio campo per una singola istanza (Req 6.4).
    static void registerCleaner(Cleaner cleaner) {
        cleaners().push_back(cleaner);
    }

    // Numero di cleaner (campi con storage) registrati per questa classe.
    [[nodiscard]] static std::size_t cleanerCount() { return cleaners().size(); }

    // Rilascia, entro UNA SOLA operazione, tutto lo stato iniettato associato a
    // `instance` per QUESTA classe, invocando ogni cleaner registrato su quella
    // sola istanza. Non tocca lo stato delle altre istanze (Req 6.4): ogni
    // cleaner cancella solo la voce della mappa con chiave == indirizzo di
    // `instance`. Restituisce il numero di campi che avevano effettivamente
    // uno stato da rilasciare per quell'istanza.
    static std::size_t releaseInstance(const void* instance) {
        std::size_t released = 0;
        for (const Cleaner cleaner : cleaners()) {
            if (cleaner(instance)) {
                ++released;
            }
        }
        return released;
    }

    [[nodiscard]] static bool contains(std::string_view key) {
        const auto& set = keys();
        return set.find(key) != set.end();
    }

    // Numero di chiavi (campi distinti) registrate per questa classe.
    [[nodiscard]] static std::size_t fieldCount() { return keys().size(); }

    [[nodiscard]] static bool atCapacity() {
        return keys().size() >= kMaxFields;
    }

    // Azzera il registro (utile nei test per isolare i casi).
    static void reset() {
        keys().clear();
        cleaners().clear();
    }
};

}  // namespace fields::detail

// ---------------------------------------------------------------------------
// Registro di TIPI per (Class, Key) a compile-time (Req 6.6).
//
// `DeclaredFieldType<Class, Key>` è un punto di personalizzazione: per default
// NESSUN tipo canonico è dichiarato per una chiave (`declared == false`), così
// `PulseField` resta retro-compatibile e non-invasivo. La macro
// `PULSE_FIELD_KEY(Class, "chiave", Tipo)` (vedi sotto, a fine header) registra
// il tipo canonico di una chiave per una classe specializzando questo trait.
//
// Quando una chiave ha un tipo canonico dichiarato, ogni `PulseField<T, Key,
// Class>` con `T` diverso da quel tipo fallisce la compilazione tramite lo
// `static_assert` interno a `PulseField` (conflitto di dichiarazione, Req 6.6).
// Una seconda registrazione della STESSA chiave per la STESSA classe con un
// tipo diverso è di per sé un errore di compilazione (specializzazione esplicita
// in conflitto del medesimo trait).
namespace fields::detail {

template <class Class, FixedString Key>
struct DeclaredFieldType {
    static constexpr bool declared = false;
};

// True se `T` è compatibile con l'eventuale tipo canonico registrato per
// (Class, Key). Se nessun tipo canonico è stato registrato (`declared ==
// false`), ogni `T` è accettato — il vincolo è puramente opt-in (Req 6.6).
// La verifica del tipo canonico (`::type`) avviene SOLO nella specializzazione
// `Declared == true`, così il primary template (chiave non registrata) non
// richiede l'esistenza di `DeclaredFieldType<Class, Key>::type`.
template <class T, class Class, FixedString Key, bool Declared>
struct FieldTypeConsistentImpl {
    static constexpr bool value = true;  // chiave non registrata → nessun vincolo
};

template <class T, class Class, FixedString Key>
struct FieldTypeConsistentImpl<T, Class, Key, true> {
    static constexpr bool value =
        std::is_same_v<T, typename DeclaredFieldType<Class, Key>::type>;
};

template <class T, class Class, FixedString Key>
inline constexpr bool field_type_consistent_v =
    FieldTypeConsistentImpl<T, Class, Key,
                            DeclaredFieldType<Class, Key>::declared>::value;

}  // namespace fields::detail
//
//   T      tipo del valore associato all'istanza.
//   Key    chiave esplicita univoca del campo (FixedString, NTTP) (Req 6.1).
//   Class  classe del gioco a cui il campo è associato; determina il registro
//          chiavi per classe (Req 6.1/6.6). Default `void` per i campi non
//          legati a una classe specifica.
//
// Uso:
//   PulseField<int, "myMod/jumpCount", Player> jumpCount;
//   jumpCount.get(player);          // -> 0 (default tipizzato) finché non scritto (Req 6.3)
//   jumpCount.set(player, 3);       // memorizzato solo per `player` (Req 6.2)
// ---------------------------------------------------------------------------
template <class T, FixedString Key, class Class = void>
class PulseField {
public:
    using value_type = T;
    using class_type = Class;
    using registry = fields::detail::FieldRegistry<Class>;

    // Conflitto chiave/tipo sulla stessa classe (Req 6.6): se la chiave `Key`
    // ha un tipo canonico dichiarato per `Class` (via PULSE_FIELD_KEY) e `T`
    // non coincide con esso, la compilazione fallisce con questo messaggio.
    // In assenza di registrazione il vincolo non si applica (opt-in).
    static_assert(
        fields::detail::field_type_consistent_v<T, Class, Key>,
        "PulseField: conflitto di dichiarazione (Req 6.6) — la chiave è già "
        "dichiarata per questa classe del gioco con un tipo diverso.");

    // Numero massimo di campi associabili a un'istanza di `Class` (Req 6.1).
    static constexpr std::size_t kMaxFieldsPerInstance = registry::kMaxFields;

    // Chiave esplicita del campo (Req 6.1).
    [[nodiscard]] static constexpr std::string_view key() noexcept {
        return Key.view();
    }

    // Alla dichiarazione, registra la chiave nel registro per classe (Req 6.1)
    // e — una sola volta per questa istanziazione (T, Key, Class) — registra il
    // cleaner usato dal cleanup al distruttore (Req 6.4).
    PulseField() {
        registry::registerKey(key());
        ensureCleanerRegistered();
    }

    // Memorizza `value` SOLO per `instance` (isolamento per-istanza, Req 6.2).
    void set(const Class* instance, T value) {
        storage()[as_key(instance)] = std::move(value);
    }

    // Restituisce il valore di `instance`, oppure il DEFAULT tipizzato `T{}`
    // se mai assegnato (Req 6.3). Non muta lo stato (lettura pura) e non
    // tocca i valori delle altre istanze (Req 6.2).
    [[nodiscard]] T get(const Class* instance) const {
        const auto& map = storage();
        const auto it = map.find(as_key(instance));
        if (it == map.end()) {
            return T{};  // default tipizzato (Req 6.3)
        }
        return it->second;
    }

    // Riferimento modificabile al valore di `instance`. Se il campo non era
    // ancora assegnato, lo inizializza con il default tipizzato `T{}` (Req 6.3)
    // e ne restituisce il riferimento. Riguarda solo questa istanza (Req 6.2).
    [[nodiscard]] T& getRef(const Class* instance) {
        return storage()[as_key(instance)];
    }

    // True se a `instance` è già stato assegnato un valore per questo campo.
    [[nodiscard]] bool has(const Class* instance) const {
        const auto& map = storage();
        return map.find(as_key(instance)) != map.end();
    }

    // Rilascia lo stato di questo campo per `instance` (base del cleanup al
    // distruttore, task 16.2/Req 6.4), senza toccare le altre istanze (Req 6.2).
    // Restituisce true se esisteva un valore da rilasciare.
    bool clear(const Class* instance) {
        return storage().erase(as_key(instance)) > 0;
    }

    // Rilascia lo stato di questo campo per TUTTE le istanze.
    static void clearAll() { storage().clear(); }

    // Numero di istanze che hanno un valore memorizzato per questo campo
    // (introspezione/diagnostica e test).
    [[nodiscard]] static std::size_t liveInstanceCount() {
        return storage().size();
    }

private:
    // Chiave della mappa: l'INDIRIZZO dell'istanza (Req 6.2). Type-erased a
    // `const void*` per la sola identità; la type-safety del VALORE è garantita
    // dal tipo `T` dell'istanziazione.
    [[nodiscard]] static const void* as_key(const Class* instance) {
        return static_cast<const void*>(instance);
    }

    // Cleaner type-erased registrato nel registro per classe (Req 6.4): rilascia
    // lo storage di QUESTO campo per la sola istanza il cui indirizzo è `key`.
    // La chiave dello storage è già l'indirizzo dell'istanza (vedi as_key), così
    // il loader/hook del distruttore può passare direttamente il `this`.
    static bool clearByAddress(const void* key) {
        return storage().erase(key) > 0;
    }

    // Registra `clearByAddress` nel registro della classe ESATTAMENTE una volta
    // per questa istanziazione (T, Key, Class), indipendentemente da quanti
    // oggetti PulseField di questo tipo vengano costruiti.
    static void ensureCleanerRegistered() {
        static const bool registered = [] {
            registry::registerCleaner(&clearByAddress);
            return true;
        }();
        (void)registered;
    }

    // Storage per-istanza dedicato a questa istanziazione (T, Key, Class):
    // mappa `indirizzo istanza -> valore`. Static header-only → un'unica mappa
    // condivisa tra tutte le unità di traduzione per questa istanziazione.
    [[nodiscard]] static std::unordered_map<const void*, T>& storage() {
        static std::unordered_map<const void*, T> instance;
        return instance;
    }
};

// ---------------------------------------------------------------------------
// fields::releaseInstance<Class>(instance) — entry point del cleanup al
// distruttore (Req 6.4).
//
// Rilascia, entro un'UNICA operazione, TUTTO lo stato iniettato associato
// all'istanza `instance` di `Class`, invocando ogni `PulseField<..., Class>`
// registrato per quella classe sulla SOLA istanza indicata. Le altre istanze
// (e le loro voci nelle stesse mappe di campo) restano intatte (Req 6.2/6.4),
// perché ogni cleaner cancella esclusivamente la voce con chiave == indirizzo
// di `instance`. Restituisce il numero di campi che avevano effettivamente uno
// stato da rilasciare per quell'istanza (0 se nessun campo era stato scritto).
//
// Cablaggio (loader): l'hook installato sul distruttore della classe del gioco
// `Class` deve chiamare `pulse::fields::releaseInstance<Class>(this)` come
// parte della medesima operazione di distruzione, PRIMA di proseguire con il
// distruttore originale. Lo SDK è header-only e non conosce gli indirizzi del
// gioco: il binding del distruttore (loader/bindings) fornisce il punto di
// aggancio; questo entry point è la sola superficie che quell'hook richiama.
// Chiamare releaseInstance per un'istanza senza stato iniettato è sicuro
// (no-op che restituisce 0), perciò l'hook può invocarlo incondizionatamente.
namespace fields {

template <class Class>
inline std::size_t releaseInstance(const Class* instance) {
    return detail::FieldRegistry<Class>::releaseInstance(
        static_cast<const void*>(instance));
}

}  // namespace fields

}  // namespace pulse

// ---------------------------------------------------------------------------
// PULSE_FIELD_KEY(Class, KeyLiteral, T) — registra il TIPO canonico di una
// chiave per una classe del gioco (Req 6.6).
//
// Specializza `pulse::fields::detail::DeclaredFieldType<Class, Key>` così che
// ogni successivo `PulseField<U, KeyLiteral, Class>` con `U != T` fallisca la
// compilazione (static_assert in PulseField). Da usare a scope di namespace
// (tipicamente globale) PRIMA delle dichiarazioni dei campi corrispondenti.
//
// Esempio:
//   struct Player { /* ... */ };
//   PULSE_FIELD_KEY(Player, "myMod/score", int);   // tipo canonico = int
//   pulse::PulseField<int,   "myMod/score", Player> ok;      // compila
//   pulse::PulseField<float, "myMod/score", Player> bad;     // NON compila
//
// Registrare due volte la stessa (Class, chiave) con tipi diversi è a sua volta
// un errore di compilazione (specializzazioni esplicite in conflitto).
#define PULSE_FIELD_KEY(Class, KeyLiteral, T)                                  \
    template <>                                                                \
    struct pulse::fields::detail::DeclaredFieldType<Class,                     \
                                                    ::pulse::FixedString(      \
                                                        KeyLiteral)> {         \
        using type = T;                                                        \
        static constexpr bool declared = true;                                 \
    }

#endif  // PULSE_FIELDS_HPP