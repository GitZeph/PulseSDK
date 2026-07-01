//! Compilazione nativa reale per `pulse build` (Requisito 6).
//!
//! [`NativeCompiler`] implementa il trait [`Compiler`] (definito in
//! [`crate::builder`]) invocando una toolchain C++ reale (`clang++`, `cl.exe`,
//! NDK clang) per produrre un **singolo modulo dinamico** caricabile dal
//! Pulse_Loader, nel formato nativo della piattaforma di destinazione
//! (`.dylib` su macOS/iOS, `.dll` su Windows, `.so` su Android/Linux). Il trait
//! [`Compiler`] resta il *seam di test*: i test continuano a iniettare
//! compilatori fittizi tramite [`crate::builder::build_with`].
//!
//! Mappa dei requisiti:
//!   - 6.1/6.2: i sorgenti nativi vengono compilati in un unico modulo dinamico
//!     impacchettato sotto `code/<platform>.<ext>` con un nome di file
//!     appropriato alla piattaforma;
//!   - 6.3: in caso di fallimento della toolchain si restituisce
//!     `Err(Vec<String>)` con le diagnostiche, che la build propaga come
//!     [`crate::builder::BuildError::CompilationFailed`] senza produrre alcun
//!     `.pulse`;
//!   - 6.4: se nessuna toolchain C++ è disponibile si restituisce un errore che
//!     identifica la toolchain mancante (di nuovo nessun pacchetto);
//!   - 6.5: su host macOS la toolchain viene invocata con la variabile
//!     `LDFLAGS` rimossa dall'ambiente del processo figlio (quirk
//!     `-fuse-ld=mold`);
//!   - 6.6: per le mod di tipo *script* i sorgenti vengono impacchettati senza
//!     invocare la toolchain nativa.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicU64, Ordering};

use crate::builder::{CompileOutput, CompiledArtifact, Compiler};
use crate::manifest::{Manifest, ModType};

// ---------------------------------------------------------------------------
// Piattaforma di destinazione.
// ---------------------------------------------------------------------------

/// Target di compilazione `(sistema operativo, architettura)` supportato.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TargetPlatform {
    MacosArm64,
    MacosX64,
    WindowsX64,
    AndroidArm64,
    AndroidArmv7,
    IosArm64,
    LinuxX64,
    LinuxArm64,
}

impl TargetPlatform {
    /// Deriva il target dall'host su cui gira la CLI.
    pub fn host() -> Self {
        match (std::env::consts::OS, std::env::consts::ARCH) {
            ("macos", "aarch64") => TargetPlatform::MacosArm64,
            ("macos", _) => TargetPlatform::MacosX64,
            ("windows", _) => TargetPlatform::WindowsX64,
            ("ios", _) => TargetPlatform::IosArm64,
            ("android", "aarch64") => TargetPlatform::AndroidArm64,
            ("android", _) => TargetPlatform::AndroidArmv7,
            ("linux", "aarch64") => TargetPlatform::LinuxArm64,
            // Fallback ragionevole per host non esplicitamente mappati.
            _ => TargetPlatform::LinuxX64,
        }
    }

    /// Nome del file del modulo dinamico per la piattaforma: `<platform>.<ext>`
    /// con l'estensione nativa (`.dylib`/`.dll`/`.so`) — usato sotto `code/`
    /// (Req 6.2).
    pub fn module_file_name(self) -> &'static str {
        match self {
            TargetPlatform::MacosArm64 => "macos-arm64.dylib",
            TargetPlatform::MacosX64 => "macos-x64.dylib",
            TargetPlatform::WindowsX64 => "windows-x64.dll",
            TargetPlatform::AndroidArm64 => "android-arm64.so",
            TargetPlatform::AndroidArmv7 => "android-armv7.so",
            TargetPlatform::IosArm64 => "ios-arm64.dylib",
            TargetPlatform::LinuxX64 => "linux-x64.so",
            TargetPlatform::LinuxArm64 => "linux-arm64.so",
        }
    }
}

// ---------------------------------------------------------------------------
// Toolchain discovery (Req 6.4).
// ---------------------------------------------------------------------------

/// Tipo di toolchain individuata, che determina la sintassi degli argomenti.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ToolchainKind {
    /// `clang++`/`g++`/NDK clang — argomenti stile GCC/Clang.
    Clang,
    /// `cl.exe`/`clang-cl` — argomenti stile MSVC.
    Msvc,
}

/// Una toolchain C++ individuata: percorso dell'eseguibile + tipo.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Toolchain {
    pub program: PathBuf,
    pub kind: ToolchainKind,
}

/// Individua una toolchain C++ adatta al `target` (Req 6.4).
///
/// Ordine di ricerca:
///   1. la variabile d'ambiente `CXX`, se impostata e risolvibile;
///   2. i candidati appropriati alla piattaforma cercati nel `PATH`
///      (`clang++`/`cl`/NDK clang...).
///
/// Restituisce `None` se nessun compilatore è disponibile, così che il
/// chiamante possa segnalare la toolchain mancante senza produrre pacchetti.
pub fn discover_toolchain(target: TargetPlatform) -> Option<Toolchain> {
    // 1. Override esplicito via CXX.
    if let Ok(cxx) = std::env::var("CXX") {
        let cxx = cxx.trim().to_string();
        if !cxx.is_empty() {
            if let Some(program) = resolve_program(&cxx) {
                return Some(Toolchain {
                    program,
                    kind: kind_for_name(&cxx),
                });
            }
        }
    }

    // 2. Candidati per piattaforma.
    let candidates: &[&str] = match target {
        TargetPlatform::WindowsX64 => &["clang++", "clang-cl", "cl"],
        TargetPlatform::AndroidArm64 | TargetPlatform::AndroidArmv7 => {
            // NDK clang esposto nel PATH come clang++ dopo l'attivazione del
            // toolchain file dell'NDK.
            &["clang++"]
        }
        _ => &["clang++", "c++", "g++"],
    };

    for name in candidates {
        if let Some(program) = find_in_path(name) {
            return Some(Toolchain {
                program,
                kind: kind_for_name(name),
            });
        }
    }

    None
}

/// Determina il tipo di toolchain dal nome dell'eseguibile.
fn kind_for_name(name: &str) -> ToolchainKind {
    let lower = name.to_ascii_lowercase();
    let stem = Path::new(&lower)
        .file_name()
        .and_then(|s| s.to_str())
        .unwrap_or(&lower);
    if stem == "cl" || stem == "cl.exe" || stem == "clang-cl" || stem == "clang-cl.exe" {
        ToolchainKind::Msvc
    } else {
        ToolchainKind::Clang
    }
}

/// Risolve un valore `CXX`: se contiene un separatore di percorso lo tratta
/// come path esplicito, altrimenti lo cerca nel `PATH`.
fn resolve_program(value: &str) -> Option<PathBuf> {
    let p = Path::new(value);
    if value.contains('/') || value.contains('\\') {
        if p.is_file() {
            return Some(p.to_path_buf());
        }
        return None;
    }
    find_in_path(value)
}

/// Cerca un eseguibile nelle directory del `PATH`. Su Windows considera anche
/// le estensioni eseguibili comuni se il nome non ne ha già una.
fn find_in_path(name: &str) -> Option<PathBuf> {
    let path_var = std::env::var_os("PATH")?;
    let has_ext = Path::new(name).extension().is_some();
    let win_exts = ["exe", "bat", "cmd"];

    for dir in std::env::split_paths(&path_var) {
        // Nome così com'è.
        let direct = dir.join(name);
        if direct.is_file() {
            return Some(direct);
        }
        // Su Windows prova le estensioni eseguibili note.
        if cfg!(windows) && !has_ext {
            for ext in win_exts {
                let candidate = dir.join(format!("{name}.{ext}"));
                if candidate.is_file() {
                    return Some(candidate);
                }
            }
        }
    }
    None
}

// ---------------------------------------------------------------------------
// Percorso degli header dello SDK.
// ---------------------------------------------------------------------------

/// Directory degli header pubblici dello SDK passata a `-I`/`/I`.
///
/// È sovrascrivibile via la variabile d'ambiente `PULSE_SDK_INCLUDE` (utile per
/// una CLI installata); in assenza, si usa il percorso relativo al sorgente
/// della CLI (`<repo>/sdk/include`).
pub fn sdk_include_dir() -> PathBuf {
    if let Ok(p) = std::env::var("PULSE_SDK_INCLUDE") {
        let p = p.trim();
        if !p.is_empty() {
            return PathBuf::from(p);
        }
    }
    PathBuf::from(concat!(env!("CARGO_MANIFEST_DIR"), "/../sdk/include"))
}

// ---------------------------------------------------------------------------
// NativeCompiler.
// ---------------------------------------------------------------------------

/// Compilatore nativo reale: invoca la toolchain C++ per produrre il modulo
/// dinamico della mod (Req 6.1–6.6).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct NativeCompiler {
    /// Piattaforma di destinazione della compilazione.
    pub target: TargetPlatform,
}

impl NativeCompiler {
    /// Crea un compilatore per la piattaforma indicata.
    pub fn new(target: TargetPlatform) -> Self {
        Self { target }
    }

    /// Crea un compilatore per l'host corrente.
    pub fn for_host() -> Self {
        Self::new(TargetPlatform::host())
    }
}

impl Compiler for NativeCompiler {
    fn compile(
        &self,
        project_dir: &Path,
        manifest: &Manifest,
    ) -> Result<CompileOutput, Vec<String>> {
        // Req 6.6: le mod di tipo script non invocano la toolchain nativa.
        if manifest.mod_info.mod_type == ModType::Script {
            return package_script_sources(project_dir);
        }
        self.compile_native(project_dir)
    }
}

impl NativeCompiler {
    /// Compila i sorgenti nativi in un singolo modulo dinamico (Req 6.1–6.5).
    fn compile_native(&self, project_dir: &Path) -> Result<CompileOutput, Vec<String>> {
        // Req 6.4: nessuna toolchain disponibile → errore che la identifica,
        // nessun pacchetto.
        let toolchain = discover_toolchain(self.target).ok_or_else(|| {
            vec![format!(
                "nessuna toolchain C++ trovata: impossibile compilare la mod nativa \
                 (attesi nel PATH: clang++, c++/g++ o cl.exe, oppure imposta la \
                 variabile d'ambiente CXX)"
            )]
        })?;

        let sources = collect_native_sources(project_dir)?;
        if sources.is_empty() {
            return Err(vec![format!(
                "nessun sorgente C/C++ trovato in '{}': impossibile compilare il \
                 modulo nativo",
                project_dir.join("src").display()
            )]);
        }

        // Directory di output temporanea isolata (rimossa al termine).
        let tmp = TempDir::new("pulse-native");
        fs::create_dir_all(tmp.path()).map_err(|e| {
            vec![format!(
                "impossibile creare la directory temporanea '{}': {e}",
                tmp.path().display()
            )]
        })?;

        let out_name = self.target.module_file_name();
        let out_path = tmp.path().join(out_name);

        let mut cmd = self.build_command(&toolchain, &sources, &out_path);

        // Req 6.5: su host macOS rimuovi LDFLAGS dall'ambiente del figlio così
        // che un eventuale `-fuse-ld=mold` globale non rompa il link AppleClang.
        if cfg!(target_os = "macos") {
            cmd.env_remove("LDFLAGS");
        }

        let output = cmd.output().map_err(|e| {
            vec![format!(
                "impossibile eseguire la toolchain '{}': {e}",
                toolchain.program.display()
            )]
        })?;

        if !output.status.success() {
            // Req 6.3: fallimento di compilazione → diagnostiche, nessun pacchetto.
            return Err(parse_diagnostics(&output.stderr, &output.stdout));
        }

        // Req 6.1/6.2: leggi l'unico modulo dinamico prodotto.
        let bytes = fs::read(&out_path).map_err(|e| {
            vec![format!(
                "la toolchain è terminata con successo ma il modulo '{}' non è \
                 leggibile: {e}",
                out_path.display()
            )]
        })?;

        Ok(CompileOutput {
            artifacts: vec![CompiledArtifact {
                file_name: out_name.to_string(),
                bytes,
            }],
        })
    }

    /// Costruisce il comando della toolchain in base al suo tipo.
    fn build_command(
        &self,
        toolchain: &Toolchain,
        sources: &[PathBuf],
        out_path: &Path,
    ) -> Command {
        let include = sdk_include_dir();
        let mut cmd = Command::new(&toolchain.program);
        match toolchain.kind {
            ToolchainKind::Clang => {
                // Req 6.1: singolo modulo dinamico, -I sugli header SDK.
                cmd.args(["-std=c++20", "-shared", "-fPIC"]);
                cmd.arg(format!("-I{}", include.display()));
                for s in sources {
                    cmd.arg(s);
                }
                cmd.arg("-o").arg(out_path);
            }
            ToolchainKind::Msvc => {
                // Equivalenti MSVC: /LD produce una DLL.
                cmd.args(["/nologo", "/std:c++20", "/EHsc", "/LD"]);
                cmd.arg(format!("/I{}", include.display()));
                for s in sources {
                    cmd.arg(s);
                }
                cmd.arg(format!("/Fe:{}", out_path.display()));
            }
        }
        cmd
    }
}

// ---------------------------------------------------------------------------
// Helper.
// ---------------------------------------------------------------------------

/// Impacchetta i sorgenti di una mod script senza invocare la toolchain (Req 6.6).
fn package_script_sources(project_dir: &Path) -> Result<CompileOutput, Vec<String>> {
    let src_dir = project_dir.join("src");
    let files = collect_files(&src_dir)?;
    if files.is_empty() {
        return Err(vec![format!(
            "nessun sorgente script trovato in '{}': impossibile impacchettare la mod",
            src_dir.display()
        )]);
    }
    Ok(CompileOutput {
        artifacts: files
            .into_iter()
            .map(|(file_name, bytes)| CompiledArtifact { file_name, bytes })
            .collect(),
    })
}

/// Estensioni dei sorgenti C/C++ compilabili.
const NATIVE_SOURCE_EXTS: [&str; 6] = ["cpp", "cc", "cxx", "c", "mm", "m"];

/// Raccoglie ricorsivamente i sorgenti C/C++ sotto `src/`, ordinati per percorso.
fn collect_native_sources(project_dir: &Path) -> Result<Vec<PathBuf>, Vec<String>> {
    let src_dir = project_dir.join("src");
    if !src_dir.is_dir() {
        return Ok(Vec::new());
    }
    let mut out: Vec<PathBuf> = Vec::new();
    let mut stack = vec![src_dir];
    while let Some(current) = stack.pop() {
        let entries = fs::read_dir(&current).map_err(|e| {
            vec![format!(
                "impossibile leggere la directory '{}': {e}",
                current.display()
            )]
        })?;
        for entry in entries {
            let entry = entry.map_err(|e| {
                vec![format!(
                    "voce di directory non leggibile in '{}': {e}",
                    current.display()
                )]
            })?;
            let path = entry.path();
            if path.is_dir() {
                stack.push(path);
            } else if path.is_file() {
                let is_source = path
                    .extension()
                    .and_then(|e| e.to_str())
                    .map(|e| NATIVE_SOURCE_EXTS.contains(&e.to_ascii_lowercase().as_str()))
                    .unwrap_or(false);
                if is_source {
                    out.push(path);
                }
            }
        }
    }
    out.sort();
    Ok(out)
}

/// Raccoglie ricorsivamente i file sotto `dir`, ordinati per percorso relativo,
/// restituendo coppie `(percorso_relativo, byte)`.
fn collect_files(dir: &Path) -> Result<Vec<(String, Vec<u8>)>, Vec<String>> {
    let mut out: Vec<(String, Vec<u8>)> = Vec::new();
    if !dir.is_dir() {
        return Ok(out);
    }
    let mut stack = vec![dir.to_path_buf()];
    while let Some(current) = stack.pop() {
        let entries = fs::read_dir(&current).map_err(|e| {
            vec![format!(
                "impossibile leggere la directory '{}': {e}",
                current.display()
            )]
        })?;
        for entry in entries {
            let entry = entry.map_err(|e| {
                vec![format!(
                    "voce di directory non leggibile in '{}': {e}",
                    current.display()
                )]
            })?;
            let path = entry.path();
            if path.is_dir() {
                stack.push(path);
            } else if path.is_file() {
                let bytes = fs::read(&path).map_err(|e| {
                    vec![format!(
                        "impossibile leggere il sorgente '{}': {e}",
                        path.display()
                    )]
                })?;
                let rel = path
                    .strip_prefix(dir)
                    .map(|p| p.to_path_buf())
                    .unwrap_or_else(|_| path.clone());
                out.push((rel.to_string_lossy().replace('\\', "/"), bytes));
            }
        }
    }
    out.sort_by(|a, b| a.0.cmp(&b.0));
    Ok(out)
}

/// Estrae le diagnostiche della toolchain in righe non vuote (Req 6.3).
fn parse_diagnostics(stderr: &[u8], stdout: &[u8]) -> Vec<String> {
    let mut lines: Vec<String> = Vec::new();
    for raw in [stderr, stdout] {
        let text = String::from_utf8_lossy(raw);
        for line in text.lines() {
            let trimmed = line.trim_end();
            if !trimmed.trim().is_empty() {
                lines.push(trimmed.to_string());
            }
        }
    }
    if lines.is_empty() {
        lines.push(
            "la toolchain è terminata con un codice di errore senza diagnostiche".to_string(),
        );
    }
    lines
}

// ---------------------------------------------------------------------------
// Directory temporanea con pulizia automatica.
// ---------------------------------------------------------------------------

/// Directory temporanea che si elimina automaticamente al `Drop`.
struct TempDir {
    path: PathBuf,
}

impl TempDir {
    fn new(tag: &str) -> Self {
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let n = COUNTER.fetch_add(1, Ordering::Relaxed);
        let pid = std::process::id();
        let path = std::env::temp_dir().join(format!("{tag}-{pid}-{n}"));
        let _ = fs::remove_dir_all(&path);
        TempDir { path }
    }

    fn path(&self) -> &Path {
        &self.path
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::manifest::{EntryPoint, ModInfo, Permissions, SemVer};

    /// Directory temporanea isolata per i test.
    struct TestProject {
        dir: TempDir,
    }

    impl TestProject {
        fn new(tag: &str) -> Self {
            let dir = TempDir::new(&format!("pulse-native-test-{tag}"));
            fs::create_dir_all(dir.path()).unwrap();
            TestProject { dir }
        }

        fn path(&self) -> &Path {
            self.dir.path()
        }

        fn write_source(&self, rel: &str, contents: &str) {
            let path = self.path().join("src").join(rel);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, contents).unwrap();
        }
    }

    fn manifest_of(mod_type: ModType) -> Manifest {
        Manifest {
            schema_version: 1,
            mod_info: ModInfo {
                id: "com.example.native".to_string(),
                version: SemVer::new(0, 1, 0),
                name: "Native".to_string(),
                mod_type,
            },
            entry_points: vec![EntryPoint {
                kind: "init".to_string(),
                symbol: "pulse_mod_init".to_string(),
            }],
            dependencies: Vec::new(),
            permissions: Permissions::default(),
            settings: Vec::new(),
            offsets: Vec::new(),
        }
    }

    /// Ogni piattaforma ha un nome di modulo con l'estensione nativa attesa.
    #[test]
    fn module_file_names_use_native_extensions() {
        assert_eq!(TargetPlatform::MacosArm64.module_file_name(), "macos-arm64.dylib");
        assert_eq!(TargetPlatform::WindowsX64.module_file_name(), "windows-x64.dll");
        assert_eq!(TargetPlatform::AndroidArm64.module_file_name(), "android-arm64.so");
        assert!(TargetPlatform::host().module_file_name().contains('.'));
    }

    /// Req 6.6 — una mod script viene impacchettata senza invocare la toolchain.
    #[test]
    fn script_mod_packages_sources_without_toolchain() {
        let proj = TestProject::new("script");
        proj.write_source("mod.lua", "-- lua mod\nprint('hello')\n");
        proj.write_source("util/helper.lua", "-- helper\n");

        let compiler = NativeCompiler::for_host();
        let out = compiler
            .compile(proj.path(), &manifest_of(ModType::Script))
            .expect("lo script deve impacchettarsi senza toolchain");

        let names: Vec<&str> = out.artifacts.iter().map(|a| a.file_name.as_str()).collect();
        assert!(names.contains(&"mod.lua"), "artefatti: {names:?}");
        assert!(names.contains(&"util/helper.lua"), "artefatti: {names:?}");
    }

    /// Una mod script senza sorgenti produce un errore (nessun pacchetto).
    #[test]
    fn script_mod_without_sources_errors() {
        let proj = TestProject::new("script-empty");
        fs::create_dir_all(proj.path().join("src")).unwrap();

        let compiler = NativeCompiler::for_host();
        let err = compiler
            .compile(proj.path(), &manifest_of(ModType::Script))
            .unwrap_err();
        assert!(!err.is_empty());
        assert!(err[0].contains("nessun sorgente script"));
    }

    /// La compilazione nativa senza sorgenti C/C++ fallisce con un errore.
    ///
    /// Esito deterministico indipendentemente dalla presenza della toolchain:
    /// toolchain assente → errore "toolchain mancante"; toolchain presente →
    /// errore "nessun sorgente". In entrambi i casi è un `Err`.
    #[test]
    fn native_compile_without_sources_errors() {
        let proj = TestProject::new("native-empty");
        fs::create_dir_all(proj.path().join("src")).unwrap();

        let compiler = NativeCompiler::for_host();
        let res = compiler.compile(proj.path(), &manifest_of(ModType::Native));
        assert!(res.is_err(), "atteso un errore di compilazione");
    }

    /// Happy-path della toolchain reale: se un compilatore è disponibile,
    /// la compilazione nativa produce un singolo modulo dinamico non vuoto con
    /// il nome di file appropriato alla piattaforma (Req 6.1, 6.2).
    ///
    /// Se nessuna toolchain è presente (es. CI minimale), il test viene saltato.
    #[test]
    fn native_compile_produces_single_module_when_toolchain_available() {
        let target = TargetPlatform::host();
        if discover_toolchain(target).is_none() {
            eprintln!("toolchain C++ assente: test saltato");
            return;
        }

        let proj = TestProject::new("native-ok");
        // Sorgente minimale autosufficiente (nessun header SDK necessario per la
        // verifica del percorso della toolchain).
        proj.write_source("mod.cpp", "extern \"C\" void pulse_mod_init() {}\n");

        let compiler = NativeCompiler::new(target);
        let out = compiler
            .compile(proj.path(), &manifest_of(ModType::Native))
            .expect("la compilazione nativa deve riuscire con una toolchain disponibile");

        assert_eq!(out.artifacts.len(), 1, "atteso un singolo modulo dinamico");
        assert_eq!(out.artifacts[0].file_name, target.module_file_name());
        assert!(!out.artifacts[0].bytes.is_empty(), "il modulo non deve essere vuoto");
    }

    /// Req 6.3 — diagnostiche di una compilazione fallita: sorgente malformato.
    #[test]
    fn native_compile_failure_reports_diagnostics() {
        let target = TargetPlatform::host();
        if discover_toolchain(target).is_none() {
            eprintln!("toolchain C++ assente: test saltato");
            return;
        }

        let proj = TestProject::new("native-fail");
        proj.write_source("mod.cpp", "this is not valid c++ !!!\n");

        let compiler = NativeCompiler::new(target);
        let err = compiler
            .compile(proj.path(), &manifest_of(ModType::Native))
            .expect_err("un sorgente malformato deve fallire la compilazione");
        assert!(!err.is_empty(), "attese diagnostiche della toolchain");
    }
}
