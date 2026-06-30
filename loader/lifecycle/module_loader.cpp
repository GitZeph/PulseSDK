// loader/lifecycle/module_loader.cpp — seam per-piattaforma del Module_Loader
// (External Mod Loading, task 5.1).
//
// Questo file fornisce:
//   * le utility `to_string(ModuleLoadError)` e `make_module_error(...)`;
//   * gli stub delle piattaforme NON-Apple (Windows/Android/iOS) con
//     `available() == false` e primitive che riportano `Unsupported` (Req 11.4);
//   * la forma del seam reale macOS (`MacOsModuleLoader`): in QUESTO task la
//     logica `dlopen`/`dlsym`/`dlclose` con estrazione su file temporaneo è
//     LASCIATA AL TASK 5.2. Qui le primitive degradano in modo confinato a
//     `Unsupported`, così la build host compila e si comporta in modo
//     prevedibile finché 5.2 non cabla l'OS loader reale;
//   * `make_platform_module_loader()` per la selezione a compile-time (Req 11.2/11.4).
#include "lifecycle/module_loader.hpp"

#if defined(__APPLE__)
// L'OS loader reale (dlopen/dlsym/dlclose) e le primitive POSIX per il file
// temporaneo a permessi ristretti sono disponibili su tutte le piattaforme
// Apple, indipendentemente dal backend di hooking: il path reale viene quindi
// COMPILATO sia con Dobby ON sia OFF (default), mentre `available()` resta
// vincolato ad Apple+Dobby (vedi sotto). Così la build host macOS di default
// (Dobby OFF) compila comunque la forma reale del seam (task 5.2).
#include <dlfcn.h>     // dlopen, dlsym, dlclose, dlerror
#include <unistd.h>    // getpid
#include <cstdlib>     // std::getenv
#include <cstdio>      // std::snprintf
#include <fstream>     // std::ofstream
#include <random>      // std::random_device, std::mt19937_64
#include <system_error>
#endif

namespace pulse::lifecycle {

std::string_view to_string(ModuleLoadError error) noexcept {
    switch (error) {
        case ModuleLoadError::None:
            return "None";
        case ModuleLoadError::ExtractFailed:
            return "ExtractFailed";
        case ModuleLoadError::DlopenFailed:
            return "DlopenFailed";
        case ModuleLoadError::SymbolNotFound:
            return "SymbolNotFound";
        case ModuleLoadError::Unsupported:
            return "Unsupported";
    }
    return "Unknown";
}

pulse::hooking::HookError make_module_error(ModuleLoadError error, std::string message) {
    // La causa specifica del Module_Loader è mappata sulla categoria più vicina
    // dell'engine di hooking; il messaggio porta sempre la `ModuleLoadError`
    // testuale così la diagnostica resta puntuale (insieme chiuso del design).
    pulse::hooking::HookErrorCode code = pulse::hooking::HookErrorCode::BackendFailure;
    switch (error) {
        case ModuleLoadError::None:
            code = pulse::hooking::HookErrorCode::None;
            break;
        case ModuleLoadError::Unsupported:
            code = pulse::hooking::HookErrorCode::Unsupported;
            break;
        case ModuleLoadError::SymbolNotFound:
            code = pulse::hooking::HookErrorCode::InvalidArgument;
            break;
        case ModuleLoadError::ExtractFailed:
        case ModuleLoadError::DlopenFailed:
            code = pulse::hooking::HookErrorCode::BackendFailure;
            break;
    }

    std::string text = "[";
    text += to_string(error);
    text += "] ";
    text += std::move(message);
    return pulse::hooking::HookError{code, std::move(text)};
}

namespace {

// Errore comune per i loader non disponibili: nessun caricamento sulla
// piattaforma corrente (Req 11.4/11.5).
ModResult<ModuleHandle> unsupported_load(std::string_view who) {
    return ModResult<ModuleHandle>::err(make_module_error(
        ModuleLoadError::Unsupported,
        std::string(who) + ": caricamento non supportato su questa piattaforma"));
}

ModResult<void*> unsupported_resolve(std::string_view who) {
    return ModResult<void*>::err(make_module_error(
        ModuleLoadError::Unsupported,
        std::string(who) + ": risoluzione entry point non supportata su questa piattaforma"));
}

ModResult<void> unsupported_unload(std::string_view who) {
    return ModResult<void>::err(make_module_error(
        ModuleLoadError::Unsupported,
        std::string(who) + ": unload non supportato su questa piattaforma"));
}

#if defined(__APPLE__)

// Radice per-utente dei file temporanei della sessione: ~/Library/Caches/Pulse.
// Se `HOME` non è disponibile (ambiente atipico), si ricade su una radice
// temporanea, così l'estrazione resta confinata e prevedibile.
std::filesystem::path caches_root() {
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / "Library" / "Caches" / "Pulse";
    }
    return std::filesystem::temp_directory_path() / "Pulse";
}

// Genera un identificatore di sessione opaco e per-processo: pid + 64 bit
// casuali in esadecimale. Sufficiente a isolare sessioni concorrenti dello
// stesso utente sotto cartelle distinte.
std::string make_session_id() {
    std::random_device rd;
    std::mt19937_64 gen(static_cast<std::uint64_t>(rd()) ^
                        (static_cast<std::uint64_t>(::getpid()) << 32));
    const std::uint64_t r = gen();
    char buf[48];
    std::snprintf(buf, sizeof(buf), "session-%d-%016llx", static_cast<int>(::getpid()),
                  static_cast<unsigned long long>(r));
    return std::string(buf);
}

// Permessi ristretti: solo il proprietario (0700 per la dir, 0600 per il file).
using std::filesystem::perms;
using std::filesystem::perm_options;

#endif  // __APPLE__

}  // namespace

// ---------------------------------------------------------------------------
// MacOsModuleLoader — forma del seam reale. La logica `dlopen`/`dlsym`/`dlclose`
// è cablata dal task 5.2; qui le primitive degradano a `Unsupported`.
// ---------------------------------------------------------------------------
bool MacOsModuleLoader::available() const noexcept {
#if defined(__APPLE__) && defined(PULSE_HOOK_BACKEND_DOBBY)
    // Coerenza artefatto/backend: il Module_Loader reale è dichiarato disponibile
    // solo sulla piattaforma Apple quando il backend Dobby è abilitato (Req 11.3),
    // perché un artefatto caricabile da GD senza un backend di hooking reale non
    // potrebbe comunque installare alcun hook.
    return true;
#else
    return false;
#endif
}

#if defined(__APPLE__)

// ---------------------------------------------------------------------------
// Implementazione reale macOS (Req 5.1/5.2/11.3/8.7). Compilata su tutte le
// piattaforme Apple (anche con Dobby OFF), mentre `available()` resta gated su
// Apple+Dobby. Il comportamento `dlopen` reale è verificato in Fase E.
// ---------------------------------------------------------------------------
ModResult<ModuleHandle> MacOsModuleLoader::load(const ModId& owner, const Bytes& moduleImage) {
    // (1) Estrazione su file temporaneo per-sessione con permessi ristretti.
    //     I byte sono GIÀ verificati dal Mod_Manifest_Validator (Req 2.5): qui
    //     ci si limita a materializzarli su disco perché `dlopen` su macOS
    //     richiede un percorso su filesystem.
    if (sessionId_.empty()) {
        sessionId_ = make_session_id();
    }

    std::error_code ec;
    const std::filesystem::path sessionDir = caches_root() / sessionId_;
    std::filesystem::create_directories(sessionDir, ec);
    if (ec) {
        return ModResult<ModuleHandle>::err(make_module_error(
            ModuleLoadError::ExtractFailed,
            std::string(name()) + ": impossibile creare la cartella di sessione '" +
                sessionDir.string() + "': " + ec.message()));
    }
    // Dir 0700: accessibile solo al proprietario.
    std::filesystem::permissions(sessionDir, perms::owner_all, perm_options::replace, ec);
    // (errore non fatale: il fallimento di chmod non impedisce l'estrazione)

    const std::filesystem::path modulePath = sessionDir / (owner + ".dylib");
    {
        std::ofstream out(modulePath, std::ios::binary | std::ios::trunc);
        if (!out) {
            return ModResult<ModuleHandle>::err(make_module_error(
                ModuleLoadError::ExtractFailed,
                std::string(name()) + ": impossibile aprire in scrittura '" +
                    modulePath.string() + "' per la mod '" + owner + "'"));
        }
        if (!moduleImage.empty()) {
            out.write(reinterpret_cast<const char*>(moduleImage.data()),
                      static_cast<std::streamsize>(moduleImage.size()));
        }
        if (!out) {
            std::error_code rmEc;
            std::filesystem::remove(modulePath, rmEc);
            return ModResult<ModuleHandle>::err(make_module_error(
                ModuleLoadError::ExtractFailed,
                std::string(name()) + ": scrittura dei byte del modulo fallita per '" +
                    owner + "'"));
        }
    }
    // File 0600: lettura/scrittura solo per il proprietario.
    std::filesystem::permissions(modulePath, perms::owner_read | perms::owner_write,
                                 perm_options::replace, ec);

    // (2) dlopen(path, RTLD_NOW | RTLD_LOCAL): RTLD_LOCAL isola i simboli della
    //     mod, RTLD_NOW forza la risoluzione immediata così un modulo con
    //     simboli mancanti fallisce QUI in modo confinato (Req 6.1).
    ::dlerror();  // pulisce eventuali errori pendenti
    void* native = ::dlopen(modulePath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (native == nullptr) {
        const char* err = ::dlerror();
        std::error_code rmEc;
        std::filesystem::remove(modulePath, rmEc);
        return ModResult<ModuleHandle>::err(make_module_error(
            ModuleLoadError::DlopenFailed,
            std::string(name()) + ": dlopen fallito per '" + owner + "': " +
                (err != nullptr ? err : "errore sconosciuto")));
    }

    ModuleHandle handle;
    handle.native = native;
    handle.extractedPath = modulePath;
    return ModResult<ModuleHandle>::ok(handle);
}

ModResult<void*> MacOsModuleLoader::resolveEntryPoint(const ModuleHandle& handle,
                                                      std::string_view symbol) {
    if (!handle.valid()) {
        return ModResult<void*>::err(make_module_error(
            ModuleLoadError::DlopenFailed,
            std::string(name()) + ": handle non valido nella risoluzione di '" +
                std::string(symbol) + "'"));
    }
    // `dlsym` può legittimamente restituire nullptr per un simbolo il cui valore
    // è nullo: si discrimina l'assenza pulendo e ri-leggendo `dlerror()`.
    ::dlerror();
    void* sym = ::dlsym(handle.native, std::string(symbol).c_str());
    const char* err = ::dlerror();
    if (sym == nullptr && err != nullptr) {
        return ModResult<void*>::err(make_module_error(
            ModuleLoadError::SymbolNotFound,
            std::string(name()) + ": simbolo '" + std::string(symbol) +
                "' non esportato dal modulo: " + err));
    }
    return ModResult<void*>::ok(sym);
}

ModResult<void> MacOsModuleLoader::unload(ModuleHandle& handle) {
    if (!handle.valid()) {
        return ModResult<void>::err(make_module_error(
            ModuleLoadError::DlopenFailed,
            std::string(name()) + ": unload di un handle non valido"));
    }
    const int rc = ::dlclose(handle.native);
    handle.native = nullptr;

    // Rimozione del file temporaneo della sessione (cleanup, Req 8.7).
    std::error_code rmEc;
    if (!handle.extractedPath.empty()) {
        std::filesystem::remove(handle.extractedPath, rmEc);
    }

    if (rc != 0) {
        const char* err = ::dlerror();
        return ModResult<void>::err(make_module_error(
            ModuleLoadError::DlopenFailed,
            std::string(name()) + ": dlclose ha riportato un errore: " +
                (err != nullptr ? err : "errore sconosciuto")));
    }
    return ModResult<void>::ok();
}

#else  // !__APPLE__

// ---------------------------------------------------------------------------
// Host non-Apple (es. Linux CI): nessun OS loader reale. Le primitive degradano
// a `Unsupported` in modo confinato, così la build resta verde fuori da Apple.
// ---------------------------------------------------------------------------
ModResult<ModuleHandle> MacOsModuleLoader::load(const ModId& owner, const Bytes& moduleImage) {
    (void)owner;
    (void)moduleImage;
    return unsupported_load(name());
}

ModResult<void*> MacOsModuleLoader::resolveEntryPoint(const ModuleHandle& handle,
                                                      std::string_view symbol) {
    (void)handle;
    (void)symbol;
    return unsupported_resolve(name());
}

ModResult<void> MacOsModuleLoader::unload(ModuleHandle& handle) {
    (void)handle;
    return unsupported_unload(name());
}

#endif  // __APPLE__

// ---------------------------------------------------------------------------
// Stub Windows (Req 11.4).
// ---------------------------------------------------------------------------
ModResult<ModuleHandle> WindowsModuleLoader::load(const ModId&, const Bytes&) {
    return unsupported_load(name());
}
ModResult<void*> WindowsModuleLoader::resolveEntryPoint(const ModuleHandle&, std::string_view) {
    return unsupported_resolve(name());
}
ModResult<void> WindowsModuleLoader::unload(ModuleHandle&) {
    return unsupported_unload(name());
}

// ---------------------------------------------------------------------------
// Stub Android (Req 11.4).
// ---------------------------------------------------------------------------
ModResult<ModuleHandle> AndroidModuleLoader::load(const ModId&, const Bytes&) {
    return unsupported_load(name());
}
ModResult<void*> AndroidModuleLoader::resolveEntryPoint(const ModuleHandle&, std::string_view) {
    return unsupported_resolve(name());
}
ModResult<void> AndroidModuleLoader::unload(ModuleHandle&) {
    return unsupported_unload(name());
}

// ---------------------------------------------------------------------------
// Stub iOS (Req 11.4).
// ---------------------------------------------------------------------------
ModResult<ModuleHandle> IOsModuleLoader::load(const ModId&, const Bytes&) {
    return unsupported_load(name());
}
ModResult<void*> IOsModuleLoader::resolveEntryPoint(const ModuleHandle&, std::string_view) {
    return unsupported_resolve(name());
}
ModResult<void> IOsModuleLoader::unload(ModuleHandle&) {
    return unsupported_unload(name());
}

// ---------------------------------------------------------------------------
// Selezione a compile-time del Module_Loader del target corrente (Req 11.2/11.4).
// ---------------------------------------------------------------------------
std::unique_ptr<IModuleLoader> make_platform_module_loader() {
#if defined(_WIN32)
    return std::make_unique<WindowsModuleLoader>();
#elif defined(__ANDROID__)
    return std::make_unique<AndroidModuleLoader>();
#elif defined(__APPLE__)
    // macOS arm64 è l'unica implementazione reale del primo deliverable; la
    // logica `dlopen` reale è cablata dal task 5.2. (iOS resta dietro lo stub
    // dedicato, selezionabile dai consumatori che ne hanno bisogno.)
    return std::make_unique<MacOsModuleLoader>();
#else
    // Host non bersaglio (es. Linux CI): nessun loader reale. Restituiamo il
    // loader macOS che, fuori da Apple, riporta `available() == false`.
    return std::make_unique<MacOsModuleLoader>();
#endif
}

}  // namespace pulse::lifecycle
