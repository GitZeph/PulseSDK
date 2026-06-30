// loader/lifecycle/dependency_resolver.hpp — Layer 4 (Mod Lifecycle &
// Dependency Resolver, Requisito 4.1, 4.2, 4.3).
//
// Il `DependencyResolver` calcola, a partire dai manifest delle mod installate,
// un `LoadPlan` composto da:
//   * `order`    — l'ordine di caricamento topologico in cui ogni mod è
//                  caricata DOPO tutte le mod da cui dipende (Requisito 4.1);
//   * `excluded` — l'elenco delle mod escluse con il motivo dell'esclusione:
//                  dipendenza mancante o di versione incompatibile (Req 4.2)
//                  oppure coinvolgimento in un ciclo di dipendenze (Req 4.3).
//
// Algoritmo (Kahn topological sort):
//   1. Si pre-escludono le mod con dipendenze mancanti o di versione
//      incompatibile (Req 4.2). L'esclusione è propagata: una mod che dipende
//      (anche transitivamente) da una mod esclusa viene a sua volta esclusa,
//      perché la dipendenza richiesta non sarà caricata.
//   2. Sul sottografo delle mod superstiti si esegue Kahn: si parte dai nodi
//      con in-degree 0, si emette un ordine e si decrementano gli in-degree.
//      Il tie-break sui nodi pronti è deterministico (ordine lessicografico di
//      `ModId`) così l'ordine prodotto è ripetibile (coerente con le attese di
//      determinismo del loadOrder, IMP-02 / Req 3.3).
//   3. I nodi che non vengono mai emessi appartengono a uno o più cicli: sono
//      tutti esclusi e riportati come membri del ciclo (Req 4.3).
//
// Nota di integrazione: il tipo di input (`ResolverManifest`, `Dependency`) è
// definito qui in forma leggera per la sola risoluzione delle dipendenze. Il
// modello completo del Manifest e il parser TOML sono realizzati separatamente
// (task 13.1); la riconciliazione verso un unico `Manifest` avverrà al
// cablaggio, mappando i campi `id`/`version`/`dependencies` senza modificare
// l'interfaccia pubblica di questo modulo.
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).
#ifndef PULSE_LOADER_LIFECYCLE_DEPENDENCY_RESOLVER_HPP
#define PULSE_LOADER_LIFECYCLE_DEPENDENCY_RESOLVER_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::lifecycle {

// ---------------------------------------------------------------------------
// ModId — identità della mod dichiarata nel Manifest (Requisito 16.1).
// Modellato come stringa, coerente con `pulse::hooking::ModId`. Definito qui in
// forma minimale per la risoluzione delle dipendenze.
// ---------------------------------------------------------------------------
using ModId = std::string;

// ---------------------------------------------------------------------------
// SemVer — versione semantica (major.minor.patch) usata per i vincoli di
// versione delle dipendenze. Confronto campo-per-campo (ordine totale).
// Il modello completo del versionamento sarà definito dal Manifest (task 13.1);
// qui basta un confronto sufficiente a valutare i vincoli minimi/range.
// ---------------------------------------------------------------------------
struct SemVer {
    std::uint32_t major{0};
    std::uint32_t minor{0};
    std::uint32_t patch{0};

    friend constexpr bool operator==(const SemVer&, const SemVer&) = default;

    // Ordine totale lessicografico su (major, minor, patch).
    [[nodiscard]] constexpr bool operator<(const SemVer& other) const noexcept {
        if (major != other.major) return major < other.major;
        if (minor != other.minor) return minor < other.minor;
        return patch < other.patch;
    }
    [[nodiscard]] constexpr bool operator<=(const SemVer& o) const noexcept { return *this < o || *this == o; }
    [[nodiscard]] constexpr bool operator>(const SemVer& o) const noexcept { return o < *this; }
    [[nodiscard]] constexpr bool operator>=(const SemVer& o) const noexcept { return o < *this || *this == o; }
};

// ---------------------------------------------------------------------------
// VersionConstraint — vincolo di versione su una dipendenza.
//
// Modello minimale ma sufficiente per la risoluzione: una versione installata
// `v` soddisfa il vincolo se `min <= v` e (se presente un upper bound) `v` è
// nell'intervallo. Il default (min = 0.0.0, nessun upper bound) accetta
// qualsiasi versione. La semantica completa dei range (caret/tilde) sarà
// definita dal parser del Manifest (task 13.1) e mappata su questo tipo.
// ---------------------------------------------------------------------------
struct VersionConstraint {
    SemVer min{};                          // versione minima richiesta (inclusiva)
    std::optional<SemVer> maxExclusive{};  // upper bound esclusivo, se presente

    // Vincolo che accetta qualsiasi versione (>= 0.0.0).
    [[nodiscard]] static VersionConstraint any() noexcept { return VersionConstraint{}; }

    // Vincolo ">= v".
    [[nodiscard]] static VersionConstraint atLeast(SemVer v) noexcept {
        return VersionConstraint{v, std::nullopt};
    }

    // Vincolo "[lo, hi)".
    [[nodiscard]] static VersionConstraint range(SemVer lo, SemVer hi) noexcept {
        return VersionConstraint{lo, hi};
    }

    // Verifica se la versione `v` soddisfa il vincolo.
    [[nodiscard]] bool satisfiedBy(const SemVer& v) const noexcept {
        if (v < min) return false;
        if (maxExclusive.has_value() && !(v < *maxExclusive)) return false;
        return true;
    }
};

// ---------------------------------------------------------------------------
// Dependency — una dipendenza dichiarata: id della mod richiesta + vincolo di
// versione che la versione installata di quella mod deve soddisfare.
// ---------------------------------------------------------------------------
struct Dependency {
    ModId id;
    VersionConstraint versionConstraint{};
};

// ---------------------------------------------------------------------------
// ResolverManifest — input leggero per la risoluzione delle dipendenze.
// Sottoinsieme del Manifest completo (task 13.1) con i soli campi rilevanti
// per il calcolo dell'ordine di caricamento.
// ---------------------------------------------------------------------------
struct ResolverManifest {
    ModId id;
    SemVer version{};
    std::vector<Dependency> dependencies;
};

// ---------------------------------------------------------------------------
// Exclusion — motivo per cui una mod è stata esclusa dal caricamento.
// ---------------------------------------------------------------------------
struct ExclusionReason {
    enum class Kind {
        MissingDependency,       // dipendenza non installata (Req 4.2)
        IncompatibleDependency,  // dipendenza installata ma di versione incompatibile (Req 4.2)
        DependencyExcluded,      // dipende (anche transitivamente) da una mod esclusa (Req 4.2)
        DependencyCycle,         // coinvolta in un ciclo di dipendenze (Req 4.3)
    };

    Kind kind{Kind::MissingDependency};
    // Identifica la dipendenza problematica (Missing/Incompatible/Excluded) o,
    // per i cicli, è vuota (i membri del ciclo sono elencati in `cycle`).
    ModId dependency;
    // Versione richiesta vs trovata, popolata per IncompatibleDependency a fini
    // diagnostici (Req 4.2). Per gli altri casi rimane di default.
    std::optional<SemVer> foundVersion{};
    // Per DependencyCycle: elenco delle mod che formano il ciclo (Req 4.3),
    // ordinato deterministicamente per id.
    std::vector<ModId> cycle;
};

struct Exclusion {
    ModId mod;              // mod esclusa
    ExclusionReason reason; // motivo dell'esclusione
};

// ---------------------------------------------------------------------------
// LoadPlan — risultato della risoluzione (Requisito 4.1, 4.2, 4.3).
// ---------------------------------------------------------------------------
struct LoadPlan {
    std::vector<ModId> order;       // ordine topologico: dipendenze prima (Req 4.1)
    std::vector<Exclusion> excluded; // mod escluse con motivo (Req 4.2, 4.3)
};

// ---------------------------------------------------------------------------
// DependencyResolver — calcola il LoadPlan dai manifest installati.
// ---------------------------------------------------------------------------
class DependencyResolver {
public:
    // Kahn topological sort + rilevamento cicli + check vincoli di versione.
    //   * `order` contiene le mod caricabili, ognuna dopo le sue dipendenze;
    //   * `excluded` contiene le mod con dipendenze mancanti/incompatibili
    //     (anche transitivamente) o coinvolte in cicli, con la relativa causa.
    // L'ordine è deterministico (tie-break lessicografico su ModId).
    [[nodiscard]] LoadPlan resolve(const std::vector<ResolverManifest>& installed) const;
};

}  // namespace pulse::lifecycle

#endif  // PULSE_LOADER_LIFECYCLE_DEPENDENCY_RESOLVER_HPP
