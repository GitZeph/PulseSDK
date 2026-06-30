# Pulse — framework di test e target CTest (Requisiti 26.2, 26.3)
#
# Integra GoogleTest (unit test / asserzioni esemplificative) e RapidCheck
# (property-based testing, ≥100 iterazioni) tramite FetchContent, così la
# toolchain di test è riproducibile su Windows/macOS/Linux senza dipendere
# da pacchetti preinstallati. Definisce inoltre uno smoke test minimale che
# compila e linka contro loader + SDK per validare la toolchain.

include(FetchContent)

# ---------------------------------------------------------------------------
# GoogleTest — unit test e fixture.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
)

# Su Windows usa la CRT condivisa per evitare mismatch di runtime.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# Non installare/abilitare i target accessori di GoogleTest.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)

# ---------------------------------------------------------------------------
# RapidCheck — property-based testing (Requisito: stack di test PBT).
# ---------------------------------------------------------------------------
FetchContent_Declare(
    rapidcheck
    GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
    GIT_TAG        b2d9ed2dddefc4b84318d664b4f221eb792d89c7
    GIT_SHALLOW    FALSE
)

# Non costruire test/esempi interni di RapidCheck nel nostro albero.
set(RC_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(RC_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest rapidcheck)

include(GoogleTest)

# ---------------------------------------------------------------------------
# Smoke test della toolchain (Requisiti 26.2, 26.3).
# Compila e linka contro loader + SDK + GoogleTest + RapidCheck.
# ---------------------------------------------------------------------------
add_executable(pulse_smoke_test smoke_test.cpp)

target_link_libraries(pulse_smoke_test
    PRIVATE
        pulse::sdk
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

# L'header di integrazione RapidCheck+GoogleTest vive in extras/gtest.
target_include_directories(pulse_smoke_test
    PRIVATE ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_features(pulse_smoke_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

# Registra ogni TEST() come caso CTest individuale.
gtest_discover_tests(pulse_smoke_test)

# ---------------------------------------------------------------------------
# Bindings System — unit test (task 3.1, Req 20.1, 20.2).
# Gli header dei bindings vivono in loader/bindings/ (include PRIVATE del
# loader): il test aggiunge esplicitamente loader/ alla include path.
# ---------------------------------------------------------------------------
add_executable(pulse_bindings_test bindings_test.cpp)

target_link_libraries(pulse_bindings_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_bindings_test
    PRIVATE ${CMAKE_SOURCE_DIR}/loader
)

target_compile_features(pulse_bindings_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_bindings_test)

# ---------------------------------------------------------------------------
# Binding verification (resolved sse verificato) + exact-pair — unit test
# (task 3.9, Req 4.2, 4.3, 4.4, 4.5).
# Verifica binding_verifier + EmbeddedBindingsProvider::addVerifiedSet (affianca
# il .pbind reale), il lookup exact-pair senza sostituzione e l'invariante zero
# hook su indirizzi non risolti via HookGate + FakeBackend. Header del loader in
# loader/ (include PRIVATE): il test aggiunge loader/ alla include path.
# ---------------------------------------------------------------------------
add_executable(pulse_binding_verification_test binding_verification_test.cpp)

target_link_libraries(pulse_binding_verification_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_binding_verification_test
    PRIVATE ${CMAKE_SOURCE_DIR}/loader
)

target_compile_features(pulse_binding_verification_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_binding_verification_test)

# ---------------------------------------------------------------------------
# GD_API_Surface — runtime sottile (Surface_Runtime_Report, SurfaceResolver,
# gate di provenienza) — unit test (task 13.1-13.4, Req 6, 7, 8, 9).
# Riusa `binding_verifier` e i provider esistenti (con provider finti per la
# policy d'ordine embedded-first). Gli header del loader vivono in loader/
# (include PRIVATE): il test aggiunge esplicitamente loader/ alla include path.
# ---------------------------------------------------------------------------
add_executable(pulse_surface_runtime_test surface_runtime_test.cpp)

target_link_libraries(pulse_surface_runtime_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_surface_runtime_test
    PRIVATE ${CMAKE_SOURCE_DIR}/loader
)

target_compile_features(pulse_surface_runtime_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_surface_runtime_test)

message(STATUS "Pulse: target di test configurati (GoogleTest + RapidCheck, smoke test).")

# ---------------------------------------------------------------------------
# Unit test del FakeBackend in-memory (task 4.2, Req 2.2).
# Header-only: include `loader/hooking/fake_backend.hpp` e l'interfaccia
# pubblica `pulse_loader/hooking/hook_backend.hpp` (via pulse::loader).
# ---------------------------------------------------------------------------
add_executable(pulse_fake_backend_test fake_backend_test.cpp)

target_link_libraries(pulse_fake_backend_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

# Consente l'include "hooking/fake_backend.hpp" relativo alla radice loader/.
target_include_directories(pulse_fake_backend_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_fake_backend_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_fake_backend_test)

message(STATUS "Pulse: target FakeBackend test configurato (task 4.2).")

# ---------------------------------------------------------------------------
# Unit test della macro PULSE_HOOK minimale (task 4.3, Req 5.1, 5.3, 2.2).
# Header-only: include il header pubblico dello SDK <pulse/hooks.hpp>.
# ---------------------------------------------------------------------------
add_executable(pulse_hooks_test hooks_test.cpp)

target_link_libraries(pulse_hooks_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
)

target_compile_features(pulse_hooks_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hooks_test)

message(STATUS "Pulse: target PULSE_HOOK test configurato (task 4.3).")

# ---------------------------------------------------------------------------
# Unit test del LoaderCore — fallimento del rilevamento GD_Version (task 2.3,
# Req 1.7). Inietta un IVersionDetector fittizio (detect() -> nullopt) e un
# DiagnosticSink capturing, verifica InitOutcome::VersionDetectionFailed e il
# log diagnostico. Gli header del core vivono in loader/ (include "core/...").
# ---------------------------------------------------------------------------
add_executable(pulse_loader_core_test loader_core_test.cpp)

target_link_libraries(pulse_loader_core_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

# Consente gli include "core/..." relativi alla radice loader/.
target_include_directories(pulse_loader_core_test
    PRIVATE ${CMAKE_SOURCE_DIR}/loader
)

target_compile_features(pulse_loader_core_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_loader_core_test)

message(STATUS "Pulse: target LoaderCore test configurato (task 2.3).")

# ---------------------------------------------------------------------------
# Unit test del round-trip di `readOriginal` (task 4.6, Req 2.2).
# Verifica che readOriginal restituisca esattamente i byte del prologo
# sovrascritto, anche dopo install (presupposto del rollback, Req 18.1).
# Header-only per FakeBackend; linka pulse::loader per make_minhook_backend().
# ---------------------------------------------------------------------------
add_executable(pulse_read_original_test read_original_test.cpp)

target_link_libraries(pulse_read_original_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

# Consente gli include "hooking/..." relativi alla radice loader/.
target_include_directories(pulse_read_original_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_read_original_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_read_original_test)

message(STATUS "Pulse: target readOriginal round-trip test configurato (task 4.6).")

# ---------------------------------------------------------------------------
# Property test P1 — trasparenza dell'hook (task 4.5, Req 2.2, 5.3).
# Tag: "Feature: pulse-sdk, Property 1". Model-based: confronta la chiamata
# attraverso il detour PULSE_HOOK + callOriginal contro la chiamata diretta
# all'originale, su input randomizzati (RapidCheck, ≥100 iterazioni di default).
# Usa il FakeBackend in-memory (loader/hooking/) per modellare l'install e
# l'header pubblico dello SDK <pulse/hooks.hpp> per la macro PULSE_HOOK.
# ---------------------------------------------------------------------------
add_executable(pulse_property1_test property1_hook_transparency_test.cpp)

target_link_libraries(pulse_property1_test
    PRIVATE
        pulse::sdk
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

# Header di integrazione RapidCheck+GoogleTest (extras/gtest) e radice loader/
# per l'include "hooking/fake_backend.hpp".
target_include_directories(pulse_property1_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property1_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property1_test)

message(STATUS "Pulse: target Property 1 test configurato (task 4.5).")

# ---------------------------------------------------------------------------
# Cablaggio centralizzato dell'MVP — unit test (task 4.4, Req 1.2, 2.2, 5.3).
# Verifica il flusso bootstrap → core → bindings → backend → PULSE_HOOK e
# l'esecuzione del detour seguito dall'originale con logging. Header del loader
# in loader/ (include PRIVATE): il test aggiunge loader/ alla include path.
# ---------------------------------------------------------------------------
add_executable(pulse_mvp_loader_test mvp_loader_test.cpp)

target_link_libraries(pulse_mvp_loader_test
    PRIVATE
        pulse::loader
        pulse::sdk
        GTest::gtest_main
)

target_include_directories(pulse_mvp_loader_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_mvp_loader_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_mvp_loader_test)

message(STATUS "Pulse: target MVP loader test configurato (task 4.4).")

# ---------------------------------------------------------------------------
# Unit test della catena di hook ordinata (task 6.1, Req 3.1, 3.2, 3.3).
# Verifica HookNode/HookChain: inserimento ordinato per priority DESC con
# tie-break loadOrder ASC, rimozione selettiva per owner e determinismo.
# Header del loader in loader/hooking/ (include PRIVATE): il test aggiunge
# loader/ alla include path. La logica è in hook_chain.cpp (compilata in
# pulse::loader via glob hooking/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_hook_chain_test hook_chain_test.cpp)

target_link_libraries(pulse_hook_chain_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_hook_chain_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_hook_chain_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hook_chain_test)

message(STATUS "Pulse: target HookChain test configurato (task 6.1).")

# ---------------------------------------------------------------------------
# Unit test della transizione 0→1 di HookChainRegistry (task 3.2, Req 1.3, 1.6,
# 1.8, 5.4). Verifica il PRIMO anello: install unica via Hook_Gate + Head_Thunk,
# persistenza byte-esatta nel RollbackStore PRIMA dell'install, cablaggio di
# currentHead/slot, e il rifiuto senza install parziale quando il gate nega o
# l'install fallisce. Logica in hook_chain_registry.cpp (compilata in
# pulse::loader via glob hooking/*.cpp); FakeBackend header-only sotto loader/.
# ---------------------------------------------------------------------------
add_executable(pulse_hook_chain_registry_first_install_test
    hook_chain_registry_first_install_test.cpp)

target_link_libraries(pulse_hook_chain_registry_first_install_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_hook_chain_registry_first_install_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_hook_chain_registry_first_install_test
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hook_chain_registry_first_install_test)

message(STATUS "Pulse: target HookChainRegistry 0->1 test configurato (task 3.2).")

# ---------------------------------------------------------------------------
# Unit test del vincolo di ammissione dell'Hook_Gate e dell'invarianza dei byte
# (task 7.1, Req 10.2, 10.3, 10.4, 10.5). Consolida: ammissione del PRIMO anello
# SOLO via Hook_Gate (binding risolto + available()), simbolo non risolto → 0
# anelli + 0 install + diagnostica con Mod_Id e simbolo, backend non disponibile
# → 0 install + diagnostica che nomina il backend, e — cuore del task — quando
# NESSUN anello raggiunge Enabled i byte modellati (eseguibile/asset, via
# FakeBackend) restano byte-per-byte invariati. Logica in
# hook_chain_registry.cpp; FakeBackend header-only sotto loader/.
# ---------------------------------------------------------------------------
add_executable(pulse_hook_chain_registry_byte_invariance_test
    hook_chain_registry_byte_invariance_test.cpp)

target_link_libraries(pulse_hook_chain_registry_byte_invariance_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_hook_chain_registry_byte_invariance_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_hook_chain_registry_byte_invariance_test
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hook_chain_registry_byte_invariance_test)

message(STATUS "Pulse: target HookChainRegistry ammissione/byte-invarianza test configurato (task 7.1).")

# ---------------------------------------------------------------------------
# Unit test della transizione n→n+1 di HookChainRegistry (task 3.3, Req 1.2,
# 1.7, 4.1, 4.2, 4.3). Verifica l'inserimento di un anello successivo su una
# catena con install attiva: relinking dei SOLI slot dei vicini (pred→inserito→
# succ), aggiornamento di currentHead se il nuovo anello è la testa, esito
# InsertedHead/InsertedMiddle/InsertedTail, NESSUNA seconda DobbyHook e prologo/
# Rollback_Store invariati. Logica in hook_chain_registry.cpp; FakeBackend
# header-only sotto loader/.
# ---------------------------------------------------------------------------
add_executable(pulse_hook_chain_registry_insert_link_test
    hook_chain_registry_insert_link_test.cpp)

target_link_libraries(pulse_hook_chain_registry_insert_link_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_hook_chain_registry_insert_link_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_hook_chain_registry_insert_link_test
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hook_chain_registry_insert_link_test)

# ---------------------------------------------------------------------------
# Raggruppamento del registro SDK per Target_Address risolto (task 7.2,
# Requisito 10.1). Verifica `group_registry_by_target`: itera l'intero registro
# (non solo la prima registrazione), raggruppa per target risolto, ordina nel
# Chain_Order (priority DESC con clamp, loadOrder ASC), scarta le registrazioni
# non risolte. Logica pura host-testabile (registry_grouping.cpp sotto loader/).
# ---------------------------------------------------------------------------
add_executable(pulse_hook_chain_registry_grouping_test
    hook_chain_registry_grouping_test.cpp)

target_link_libraries(pulse_hook_chain_registry_grouping_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_hook_chain_registry_grouping_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_hook_chain_registry_grouping_test
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hook_chain_registry_grouping_test)

message(STATUS "Pulse: target HookChainRegistry n->n+1 test configurato (task 3.3).")

# ---------------------------------------------------------------------------
# Diagnostica osservabile e vista del Chain_Order di HookChainRegistry
# (task 7.3, Requisiti 11.1, 11.2, 11.3, 11.4). Consolida, per catene a più
# anelli: aggiunta → evento con Mod_Id + Target_Address + posizione (Req 11.1);
# rimozione → evento con Mod_Id + Target_Address (Req 11.2); creazione/rimozione
# dell'unica Underlying_Installation → evento con Target_Address + operazione
# (Req 11.3); chainOrder(target) come ChainOrderView dal Chain_Head alla coda
# (Req 11.4). Logica in hook_chain_registry.cpp; FakeBackend header-only.
# ---------------------------------------------------------------------------
add_executable(pulse_hook_chain_registry_diagnostics_test
    hook_chain_registry_diagnostics_test.cpp)

target_link_libraries(pulse_hook_chain_registry_diagnostics_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_hook_chain_registry_diagnostics_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_hook_chain_registry_diagnostics_test
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hook_chain_registry_diagnostics_test)

message(STATUS "Pulse: target HookChainRegistry diagnostica/Chain_Order view test configurato (task 7.3).")


# ---------------------------------------------------------------------------
# Unit test degli invarianti di catena di HookChainRegistry (task 3.4, Req 1.4,
# 4.5, 4.6, 5.5). Verifica, attraverso sequenze di inserimento e su più
# Target_Address distinti: installed == (chain non vuota) con una sola
# Underlying_Installation per target (mai una seconda DobbyHook); un ChainSlot
# distinto per target distinto con catene disgiunte (nessun Hook_Link condiviso);
# e l'invarianza di prologo e Rollback_Store all'aggiunta di anelli a una catena
# con install attiva. Logica in hook_chain_registry.cpp; FakeBackend header-only.
# ---------------------------------------------------------------------------
add_executable(pulse_hook_chain_registry_invariants_test
    hook_chain_registry_invariants_test.cpp)

target_link_libraries(pulse_hook_chain_registry_invariants_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_hook_chain_registry_invariants_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_hook_chain_registry_invariants_test
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hook_chain_registry_invariants_test)

message(STATUS "Pulse: target HookChainRegistry invarianti test configurato (task 3.4).")

# ---------------------------------------------------------------------------
# Unit test dell'arnia di dispatch simulata di HookChainRegistry (task 3.5, Req
# 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.8, 3.9, 4.4). Verifica il veicolo di test host
# (ChainDispatchHarness) che emula il percorso reale costruito dalla Registry
# ri-cablando i Trampoline_Slot: attraversamento nel Chain_Order (testa per
# prima), inoltro dei parametri al successivo e propagazione a monte del valore
# di ritorno, raggiungimento del Real_Original via trampolino, equivalenza del
# caso a singolo anello, e short-circuit (un anello senza callOriginal ferma la
# catena, restituisce il valore dell'anello interruttore + evento Mod_Id+target).
# Header-only chain_dispatch_harness.hpp + hook_chain_registry.cpp; FakeBackend.
# ---------------------------------------------------------------------------
add_executable(pulse_hook_chain_registry_dispatch_test
    hook_chain_registry_dispatch_test.cpp)

target_link_libraries(pulse_hook_chain_registry_dispatch_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_hook_chain_registry_dispatch_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_hook_chain_registry_dispatch_test
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hook_chain_registry_dispatch_test)

message(STATUS "Pulse: target HookChainRegistry dispatch simulata test configurato (task 3.5).")

# ---------------------------------------------------------------------------
# HookChainRegistry rimozione non-ultima (relinking senza disinstallazione) —
# unit test (task 5.1, Req 5.1, 5.2, 5.6).
# Verifica removeOwner per la rimozione di un anello da una catena con altri
# anelli abilitati: relinking dei vicini (pred.slot -> succ.detour o
# Real_Trampoline se diventa coda), aggiornamento di currentHead se rimossa la
# testa, Underlying_Installation mantenuta e byte NON ripristinati, ordine ed
# esecuzione dei restanti invariati, esito RemovedKeepInstall e diagnostica
# osservabile. Logica in hook_chain_registry.cpp; FakeBackend header-only.
# ---------------------------------------------------------------------------
add_executable(pulse_hook_chain_registry_remove_link_test
    hook_chain_registry_remove_link_test.cpp)

target_link_libraries(pulse_hook_chain_registry_remove_link_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_hook_chain_registry_remove_link_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_hook_chain_registry_remove_link_test
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hook_chain_registry_remove_link_test)

message(STATUS "Pulse: target HookChainRegistry rimozione non-ultima test configurato (task 5.1).")

# ---------------------------------------------------------------------------
# HookChainRegistry rimozione dell'ultimo anello (transizione 1→0) — unit test
# (task 5.2, Req 5.3, 5.4).
# Verifica removeOwner per la rimozione dell'unico anello rimasto su un target:
# rimozione dell'unica Underlying_Installation dal backend (installCount cala),
# ripristino byte-esatto degli originalBytes via Rollback_Store, dismissione del
# ChainSlot (chainSize 0, hasInstall false, currentHead null), esito
# RemovedLastInstall, diagnostica osservabile, isolamento del fallimento di
# remove. Logica in hook_chain_registry.cpp; FakeBackend header-only.
# ---------------------------------------------------------------------------
add_executable(pulse_hook_chain_registry_remove_last_test
    hook_chain_registry_remove_last_test.cpp)

target_link_libraries(pulse_hook_chain_registry_remove_last_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_hook_chain_registry_remove_last_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_hook_chain_registry_remove_last_test
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hook_chain_registry_remove_last_test)

message(STATUS "Pulse: target HookChainRegistry rimozione ultima test configurato (task 5.2).")

# ---------------------------------------------------------------------------
# HookChainRegistry cablaggio del HookOwnershipLedger — unit test
# (task 5.3, Req 6.1, 6.2, 6.3, 6.4, 6.5).
# Verifica l'attribuzione per Mod_Id all'inserimento (hooksOf) e la rimozione
# selettiva per Mod_Id: removeOwner rilascia dal ledger esclusivamente gli
# anelli del Mod_Id rimosso (preservando gli altri), corretto anche con
# Target_Address condivisi tra più Mod_Id, mantenendo l'invariante globale
# (ledger allInstalled == unione degli anelli delle mod presenti). Logica in
# hook_chain_registry.cpp / hook_ownership.cpp; FakeBackend header-only.
# ---------------------------------------------------------------------------
add_executable(pulse_hook_chain_registry_ledger_test
    hook_chain_registry_ledger_test.cpp)

target_link_libraries(pulse_hook_chain_registry_ledger_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_hook_chain_registry_ledger_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_hook_chain_registry_ledger_test
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hook_chain_registry_ledger_test)

message(STATUS "Pulse: target HookChainRegistry ledger test configurato (task 5.3).")

# ---------------------------------------------------------------------------
# Target: HookChainRegistry round-trip di re-enable test
# (task 5.4, Req 5.7).
# Verifica che una mod disabilitata e nuovamente abilitata reinserisca l'anello
# nella posizione del Chain_Order determinata da Hook_Priority e load order,
# producendo lo stesso Chain_Order del solo enable, sia su una catena rimasta
# viva (keep-install) sia su un Target_Address smontato a 0 e ricostruito.
# Logica in hook_chain_registry.cpp; FakeBackend header-only.
# ---------------------------------------------------------------------------
add_executable(pulse_hook_chain_registry_reenable_test
    hook_chain_registry_reenable_test.cpp)

target_link_libraries(pulse_hook_chain_registry_reenable_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_hook_chain_registry_reenable_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_hook_chain_registry_reenable_test
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hook_chain_registry_reenable_test)

message(STATUS "Pulse: target HookChainRegistry re-enable test configurato (task 5.4).")

# ---------------------------------------------------------------------------
# HookChainRegistry — teardown ordinato (task 5.5, Req 7.1-7.6).
# Verifica lo smontaggio in ordine inverso a LoadPlan.order con isolamento dei
# fallimenti: rimozione nell'ordine fornito, azzeramento (nessuna install né
# anello residuo), round-trip byte-esatto dell'intera catena di N anelli, e
# isolamento di un fallimento di rimozione (causa con Mod_Id + Target_Address,
# prosecuzione con le mod restanti). Logica in hook_chain_registry.cpp;
# FakeBackend header-only.
# ---------------------------------------------------------------------------
add_executable(pulse_hook_chain_registry_teardown_test
    hook_chain_registry_teardown_test.cpp)

target_link_libraries(pulse_hook_chain_registry_teardown_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_hook_chain_registry_teardown_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_hook_chain_registry_teardown_test
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hook_chain_registry_teardown_test)

message(STATUS "Pulse: target HookChainRegistry teardown test configurato (task 5.5).")

# ---------------------------------------------------------------------------
# HookChainRegistry — coerenza della dispatch rispetto alle mutazioni
# (concorrenza modellata) (task 5.6, Req 9.1, 9.2, 9.3).
# Verifica il modello di concorrenza: mutazione strutturale serializzata dal
# mutex della Registry; `currentHead`/Trampoline_Slot scritti con store atomico
# release e letti con load acquire (nessun puntatore lacerato, Req 9.1); una
# mutazione iniettata DURANTE una dispatch in corso non altera la dispatch in
# volo (vista coerente, snapshot all'ingresso) e ha effetto dalla invocazione
# successiva (Req 9.2); nessuno stato parzialmente modificato osservabile dopo
# le mutazioni serializzate (Req 9.3). Logica in hook_chain_registry.cpp +
# head_thunk.hpp/chain_dispatch_harness.hpp; FakeBackend header-only.
# ---------------------------------------------------------------------------
add_executable(pulse_hook_chain_registry_concurrency_test
    hook_chain_registry_concurrency_test.cpp)

target_link_libraries(pulse_hook_chain_registry_concurrency_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_hook_chain_registry_concurrency_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_hook_chain_registry_concurrency_test
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hook_chain_registry_concurrency_test)

message(STATUS "Pulse: target HookChainRegistry concorrenza modellata test configurato (task 5.6).")

# ---------------------------------------------------------------------------
# Bootstrap Android — unit test (task 23.2, Req 1.1, 1.2).
# Verifica AndroidBootstrap: platform() coerente con l'ABI di compilazione
# (arm64-v8a / armeabi-v7a), entry point del runtime iniettabile e fallback
# diagnostico UnsupportedHost su host non-Android (Req 1.3). Header del loader
# in loader/bootstrap/ (include PRIVATE): il test aggiunge loader/ alla include
# path. La logica è in android_bootstrap.cpp (compilata in pulse::loader via
# glob bootstrap/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_android_bootstrap_test android_bootstrap_test.cpp)

target_link_libraries(pulse_android_bootstrap_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_android_bootstrap_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_android_bootstrap_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_android_bootstrap_test)

message(STATUS "Pulse: target Android bootstrap test configurato (task 23.2).")

# ---------------------------------------------------------------------------
# Gating degli hook su binding.resolved — unit test (task 9.1, Req 20.3, 20.4).
# Verifica che l'Hooking Engine NON installi alcun hook su binding assenti o
# non risolti (0 hook su indirizzi non risolti) e segnali un errore di
# incompatibilità, installando invece i binding risolti. Header-only: include
# `hooking/hook_gate.hpp`, `hooking/fake_backend.hpp` e `bindings/bindings.hpp`
# (radice loader/ nella include path).
# ---------------------------------------------------------------------------
add_executable(pulse_hook_gate_test hook_gate_test.cpp)

target_link_libraries(pulse_hook_gate_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_hook_gate_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_hook_gate_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hook_gate_test)

message(STATUS "Pulse: target hook gate test configurato (task 9.1).")

# ---------------------------------------------------------------------------
# Property test P3 — gate sul backend non disponibile (task 3.4, Req 3.8, 10.3).
# Tag: "Feature: pulse-gd-integration, Property 3". Su input randomizzati
# (RapidCheck, ≥100 iterazioni di default) verifica che, con un backend la cui
# `available()==false`, qualunque insieme di richieste di install (binding
# risolti / non risolti / assenti) produca SEMPRE 0 hook installati (backend
# mai invocato) e, per ogni richiesta, una diagnostica che NOMINA il backend
# via `name()`. Header-only: include `hooking/hook_gate.hpp`,
# `hooking/hook_backend.hpp` e `bindings/bindings.hpp` (radice loader/ nella
# include path); integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_gd_property3_test hook_gate_availability_property_test.cpp)

target_link_libraries(pulse_gd_property3_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_gd_property3_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_gd_property3_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_gd_property3_test)

message(STATUS "Pulse: target Property 3 (pulse-gd-integration) test configurato (task 3.4).")

# ---------------------------------------------------------------------------
# Property test P6 — round-trip del Binding_Set_File `.pbind` (task 3.7,
# Req 4.6). Tag: "Feature: pulse-gd-integration, Property 6". Su input
# randomizzati (RapidCheck, ≥100 iterazioni di default) genera un `BindingSet`
# arbitrario valido e verifica che `parse_pbind(serialize_pbind(set))` riesca
# sempre e produca un set uguale all'originale a livello di campo (GD_Version,
# piattaforma, e per ogni funzione symbol/offset/signature/verified) senza voci
# aggiunte/rimosse/alterate, con round-trip idempotente. Header dei bindings in
# loader/ (include relativo alla radice loader/) e integrazione
# RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_gd_property6_test pbind_roundtrip_property_test.cpp)

target_link_libraries(pulse_gd_property6_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_gd_property6_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_gd_property6_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_gd_property6_test)

message(STATUS "Pulse: target Property 6 (pulse-gd-integration) test configurato (task 3.7).")

# ---------------------------------------------------------------------------
# Property test P7 — gestione sicura di `.pbind` malformati (task 3.8,
# Req 4.7). Tag: "Feature: pulse-gd-integration, Property 7". Su input
# randomizzati (RapidCheck, ≥100 iterazioni di default) genera un `.pbind`
# VALIDO e vi applica una mutazione garantita malformante (header obbligatorio
# mancante, valore non valido, chiave sconosciuta, riga non "key = value",
# campo di funzione mancante/non valido, ...) e verifica che `parse_pbind`:
# (a) non lanci/termini il processo ritornando un risultato; (b) ritorni
# `error` con causa NON vuota; (c) lasci comunque caricabile il set embedded.
# Una seconda proprietà fuzza contenuti arbitrari per ribadire la robustezza
# (nessuna terminazione, causa non vuota sugli errori). Header dei bindings in
# loader/ (include relativo alla radice loader/) e integrazione
# RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_gd_property7_test pbind_malformed_property_test.cpp)

target_link_libraries(pulse_gd_property7_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_gd_property7_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_gd_property7_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_gd_property7_test)

message(STATUS "Pulse: target Property 7 (pulse-gd-integration) test configurato (task 3.8).")

# ---------------------------------------------------------------------------
# Property test P2 — determinismo dell'ordinamento della catena di hook
# (task 6.2, Req 3.2, 3.3). Tag: "Feature: pulse-sdk, Property 2".
# Genera insiemi randomizzati di HookNode e verifica che orderedNodes() sia
# (a) ordinato per priority DESC poi loadOrder ASC e (b) deterministico:
# inserire gli stessi nodi in ordini diversi (mescolati) produce la stessa
# sequenza finale (RapidCheck, ≥100 iterazioni di default).
# Header del loader in loader/hooking/ (include PRIVATE): aggiunge loader/
# alla include path; la logica è in hook_chain.cpp (in pulse::loader).
# ---------------------------------------------------------------------------
add_executable(pulse_property2_test property2_hook_chain_determinism_test.cpp)

target_link_libraries(pulse_property2_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

# Header di integrazione RapidCheck+GoogleTest (extras/gtest) e radice loader/
# per l'include "hooking/hook_chain.hpp".
target_include_directories(pulse_property2_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property2_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property2_test)

message(STATUS "Pulse: target Property 2 test configurato (task 6.2).")

# ---------------------------------------------------------------------------
# Property test P36 — lookup esatto e nessun hook su indirizzi non risolti
# (task 9.2, Req 20.1, 20.2, 20.3, 20.4).
# Tag: "Feature: pulse-sdk, Property 36". Su input randomizzati (RapidCheck,
# ≥100 iterazioni di default) verifica: (1) BindingSet::resolve e
# EmbeddedBindingsProvider::load risolvono SOLO su corrispondenza esatta di
# simbolo e di coppia (GD_Version, piattaforma) — nessun fuzzy-match; (2)
# instradando binding risolti/non risolti/assenti attraverso HookGate con un
# FakeBackend, l'invariante numeroHookSuIndirizziNonRisolti == 0 vale sempre.
# Header dei bindings/hooking in loader/ (include relativo alla radice loader/)
# e header di integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property36_test property36_bindings_lookup_test.cpp)

target_link_libraries(pulse_property36_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property36_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property36_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property36_test)

message(STATUS "Pulse: target Property 36 test configurato (task 9.2).")

# ---------------------------------------------------------------------------
# Property test P36 (cross-platform) — lookup esatto su più coppie
# (GD_Version, piattaforma) via OnlineBindingsProvider (task 25.2, Req 20.1,
# 20.2, 20.3, 20.4). Tag: "Feature: pulse-sdk, Property 36". Estende P36 (già
# coperto sul set embedded singolo dal task 9.2) a molte coppie generate
# casualmente (RapidCheck, ≥100 iterazioni di default), usando trasporto e
# verifica di firma fittizi iniettati (host, senza rete) e un fallback nullo:
# (1) per ogni coppia PROVISIONATA, load carica esattamente quel set e resolve
# restituisce il binding corretto (no fuzzy-match); (2) per una coppia NON
# provisionata load non risolve nulla e, instradando l'esito attraverso
# HookGate + FakeBackend, l'invariante numeroHookSuIndirizziNonRisolti == 0
# vale sempre. Header di bindings/hooking in loader/ (include relativo alla
# radice loader/) e integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property36_cross_platform_test property36_cross_platform_lookup_test.cpp)

target_link_libraries(pulse_property36_cross_platform_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property36_cross_platform_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property36_cross_platform_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property36_cross_platform_test)

message(STATUS "Pulse: target Property 36 cross-platform test configurato (task 25.2).")

# ---------------------------------------------------------------------------
# Unit test dell'esecuzione della catena di hook (task 6.3, Req 3.1, 3.4, 3.5).
# Verifica HookChain::dispatch + HookContext::callNext(): concatenamento
# completo (2..64) con originale in coda, stop senza next con log di
# interruzione, isolamento delle eccezioni con prosecuzione della catena.
# Header del loader in loader/hooking/ (include PRIVATE): aggiunge loader/ alla
# include path; la logica è in hook_chain.cpp (compilata in pulse::loader).
# ---------------------------------------------------------------------------
add_executable(pulse_hook_chain_dispatch_test hook_chain_dispatch_test.cpp)

target_link_libraries(pulse_hook_chain_dispatch_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_hook_chain_dispatch_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_hook_chain_dispatch_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hook_chain_dispatch_test)

message(STATUS "Pulse: target HookChain dispatch test configurato (task 6.3).")

# ---------------------------------------------------------------------------
# Property test P5 — isolamento delle eccezioni nella catena di hook
# (task 6.6, Req 3.5). Tag: "Feature: pulse-sdk, Property 5".
# Su catene randomizzate di hook (0..64) in cui un sottoinsieme casuale di
# gestori lancia eccezioni (RapidCheck, ≥100 iterazioni di default) verifica
# che HookChain::dispatch: (a) isoli l'eccezione senza propagarla al chiamante;
# (b) prosegua la catena invocando il gestore successivo, eseguendo ogni
# gestore non lanciante nell'ordine della catena e raggiungendo la funzione
# originale; (c) registri un evento di errore con mod + funzione per ogni
# gestore lanciante. Header del loader in loader/hooking/ (include relativo
# alla radice loader/); la logica è in hook_chain.cpp (compilata in
# pulse::loader). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property5_test property5_hook_exception_isolation_test.cpp)

target_link_libraries(pulse_property5_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

# Header di integrazione RapidCheck+GoogleTest (extras/gtest) e radice loader/
# per l'include "hooking/hook_chain.hpp".
target_include_directories(pulse_property5_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property5_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property5_test)

message(STATUS "Pulse: target Property 5 test configurato (task 6.6).")

# ---------------------------------------------------------------------------
# Unit test del coordinatore HookEngine — retry/rollback atomico e rimozione
# selettiva (task 7.1, Req 2.4, 2.5). Verifica: max 3 tentativi di install poi
# rollback atomico (0 hook residui, errore con funzione+causa), successo a un
# tentativo successivo, atomicità del batch (installAll) e rimozione selettiva
# che ripristina solo gli hook di una mod lasciando intatti gli altri.
# Header-only: include `hooking/hook_engine.hpp` e `hooking/fake_backend.hpp`
# (radice loader/ nella include path); la logica di catena è in hook_chain.cpp
# (compilata in pulse::loader).
# ---------------------------------------------------------------------------
add_executable(pulse_hook_engine_test hook_engine_test.cpp)

target_link_libraries(pulse_hook_engine_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_hook_engine_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_hook_engine_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hook_engine_test)

message(STATUS "Pulse: target HookEngine test configurato (task 7.1).")

# ---------------------------------------------------------------------------
# Property test P6 — rimozione selettiva degli hook (task 7.2, Req 2.4).
# Tag: "Feature: pulse-sdk, Property 6". Su insiemi randomizzati di hook
# posseduti da più mod su più funzioni (RapidCheck, ≥100 iterazioni di
# default), verifica che remove(owner) rimuova SOLO gli hook di quella mod
# (hooksForOwner(owner)==0), lasci intatti i conteggi delle altre mod e
# ripristini il detour sul backend SOLO per le funzioni la cui catena resta
# vuota. Usa l'HookEngine sostenuto dal FakeBackend in-memory (header-only):
# include "hooking/hook_engine.hpp" e "hooking/fake_backend.hpp" (radice
# loader/ nella include path); la logica di catena è in hook_chain.cpp
# (compilata in pulse::loader). Header di integrazione RapidCheck+GoogleTest
# in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property6_test property6_selective_hook_removal_test.cpp)

target_link_libraries(pulse_property6_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property6_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property6_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property6_test)

message(STATUS "Pulse: target Property 6 test configurato (task 7.2).")

# ---------------------------------------------------------------------------
# Property test P7 — atomicità del fallimento di installazione
# (task 7.3, Req 2.5). Tag: "Feature: pulse-sdk, Property 7".
# Su scenari randomizzati (RapidCheck, ≥100 iterazioni di default) verifica che
# quando un install (singolo) o un batch (installAll) fallisce dopo i 3
# tentativi, l'Hooking Engine non lasci modifiche parziali: funzioni bersaglio
# invariate, totalHooks tornato allo stato pre-chiamata (rollback atomico del
# batch), esito di errore con funzione + causa e numero di tentativi sul backend
# per un bersaglio persistentemente fallito pari a kMaxInstallAttempts (3).
# Header-only: include `hooking/hook_engine.hpp` e `hooking/fake_backend.hpp`
# (radice loader/ nella include path); la logica di catena è in hook_chain.cpp
# (compilata in pulse::loader). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property7_test property7_install_failure_atomicity_test.cpp)

target_link_libraries(pulse_property7_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property7_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property7_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property7_test)

message(STATUS "Pulse: target Property 7 test configurato (task 7.3).")

# ---------------------------------------------------------------------------
# Unit test di RollbackStore / RollbackRecord (task 8.1, Req 18.1, 18.4, 18.5).
# Verifica: round-trip in-memory serialize/deserialize (byte arbitrari,
# version, platformId); round-trip persist->reload su file in directory
# temporanea (i record sopravvivono al "riavvio"); write-through di add();
# ripristino dei byte originali via callback e via FakeBackend; interruzione
# del ripristino su fallimento con segnalazione della funzione (Req 18.5);
# file assente => store vuoto e rilevamento dati corrotti.
# Header del loader in loader/hooking/ (include relativo alla radice loader/);
# la logica di persistenza è in rollback_store.cpp (compilata in pulse::loader
# via glob hooking/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_rollback_store_test rollback_store_test.cpp)

target_link_libraries(pulse_rollback_store_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_rollback_store_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_rollback_store_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_rollback_store_test)

message(STATUS "Pulse: target RollbackStore test configurato (task 8.1).")

# ---------------------------------------------------------------------------
# Property test P8 — round-trip del rollback degli hook (task 8.3, Req 18.1,
# 18.4). Tag: "Feature: pulse-sdk, Property 8". Su insiemi randomizzati di
# RollbackRecord (RapidCheck, ≥100 iterazioni di default) verifica: (1) il
# round-trip di persistenza serialize/deserialize e persist→load da file
# temporaneo restituisce record identici (Req 18.1); (2) il round-trip di
# ripristino — dopo seed+install sul FakeBackend (live != original) — riporta i
# byte live esattamente agli originali per ogni bersaglio via restoreAll
# (Req 18.4). Header del loader in loader/hooking/ (include relativo alla
# radice loader/); la logica di persistenza è in rollback_store.cpp (compilata
# in pulse::loader). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property8_test property8_rollback_round_trip_test.cpp)

target_link_libraries(pulse_property8_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property8_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property8_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property8_test)

message(STATUS "Pulse: target Property 8 test configurato (task 8.3).")

# ---------------------------------------------------------------------------
# Unit test della sentinella crash + auto-disable (task 8.2, Req 18.2, 18.3,
# 18.5). Con tempo e persistenza iniettati (clock controllato + directory
# temporanea + RestoreWriteFn) verifica: crash entro 60 s -> auto-disable al
# riavvio con ripristino dei byte (solo della mod imputata) e messaggio
# all'User; fallimento del ripristino -> abort, mod che resta disabilitata e
# segnalazione della funzione (Req 18.5); crash dopo 60 s / chiusura pulita ->
# nessun auto-disable; persistenza della lista disabilitate al riavvio.
# Header del loader in loader/hooking/ (include relativo alla radice loader/);
# la logica e' in crash_sentinel.cpp e rollback_store.cpp (compilate in
# pulse::loader via glob hooking/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_crash_sentinel_test crash_sentinel_test.cpp)

target_link_libraries(pulse_crash_sentinel_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_crash_sentinel_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_crash_sentinel_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_crash_sentinel_test)

message(STATUS "Pulse: target CrashSentinel test configurato (task 8.2).")

# ---------------------------------------------------------------------------
# Unit test del DependencyResolver (task 11.1, Req 4.1, 4.2, 4.3).
# Verifica: ordine topologico (dipendenze prima) e determinismo del tie-break;
# esclusione di mod con dipendenza mancante o di versione incompatibile con
# segnalazione e proseguimento delle restanti mod; rilevamento dei cicli con
# esclusione e identificazione di tutti i membri del ciclo, distinguendo i
# membri del ciclo dalle mod che dipendono dal ciclo. Header del loader in
# loader/lifecycle/ (include relativo alla radice loader/); la logica è in
# dependency_resolver.cpp (compilata in pulse::loader via glob lifecycle/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_dependency_resolver_test dependency_resolver_test.cpp)

target_link_libraries(pulse_dependency_resolver_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_dependency_resolver_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_dependency_resolver_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_dependency_resolver_test)

message(STATUS "Pulse: target DependencyResolver test configurato (task 11.1).")

# ---------------------------------------------------------------------------
# Property test P9 — ordine topologico di caricamento (task 11.2, Req 4.1).
# Tag: "Feature: pulse-sdk, Property 9". Su DAG casuali (RapidCheck, ≥100
# iterazioni di default) verifica che DependencyResolver::resolve(...).order:
# (a) collochi ogni mod dopo tutte le sue dipendenze (ordine topologico
# valido); (b) contenga esattamente le mod risolvibili (in un DAG con vincoli
# sempre soddisfatti nessuna mod è esclusa, quindi `order` è una permutazione
# dell'input ed `excluded` è vuoto); (c) sia deterministico rispetto all'ordine
# di input (permutare i manifest in ingresso non cambia l'`order` in uscita).
# Il DAG è generato con archi sempre verso indici inferiori (aciclicità per
# costruzione). Header del loader in loader/lifecycle/ (include relativo alla
# radice loader/); la logica è in dependency_resolver.cpp (compilata in
# pulse::loader). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property9_test property9_topological_load_order_test.cpp)

target_link_libraries(pulse_property9_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property9_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property9_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property9_test)

message(STATUS "Pulse: target Property 9 test configurato (task 11.2).")

# ---------------------------------------------------------------------------
# Property test P11 — rilevamento e isolamento dei cicli (task 11.4, Req 4.3).
# Tag: "Feature: pulse-sdk, Property 11". Su grafi randomizzati che contengono
# intenzionalmente uno o più cicli di dipendenze (gruppi disgiunti ad anello),
# verifica (RapidCheck, ≥100 iterazioni di default) che resolve() escluda TUTTI
# i membri di ogni ciclo con kind==DependencyCycle riportandone i membri,
# nessun membro compaia in order, e le mod acicliche/indipendenti carichino in
# ordine topologico valido. Header del loader in loader/lifecycle/ (include
# relativo alla radice loader/); la logica è in dependency_resolver.cpp
# (compilata in pulse::loader). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property11_test property11_dependency_cycle_isolation_test.cpp)

target_link_libraries(pulse_property11_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property11_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property11_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property11_test)

message(STATUS "Pulse: target Property 11 test configurato (task 11.4).")

# ---------------------------------------------------------------------------
# Property test P10 — esclusione di dipendenze mancanti/incompatibili
# (task 11.3, Req 4.2). Tag: "Feature: pulse-sdk, Property 10".
# Su grafi randomizzati in cui alcune mod dichiarano una dipendenza non
# installata o di versione incompatibile (RapidCheck, ≥100 iterazioni di
# default), verifica che DependencyResolver::resolve() escluda esattamente
# quelle mod con il motivo corretto (MissingDependency / IncompatibleDependency)
# identificando la dipendenza problematica, mentre tutte le mod sane e
# indipendenti restano nell'ordine di caricamento e nessuna mod esclusa vi
# compare. Header del loader in loader/lifecycle/ (include relativo alla radice
# loader/); la logica è in dependency_resolver.cpp (compilata in pulse::loader).
# Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property10_test
    property10_missing_incompatible_dependency_exclusion_test.cpp)

target_link_libraries(pulse_property10_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property10_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property10_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property10_test)

message(STATUS "Pulse: target Property 10 test configurato (task 11.3).")

# ---------------------------------------------------------------------------
# Property 2 (external-mod-loading) — esclusione per incompatibilità di
# piattaforma/versione (estremi inclusi). Su input randomizzati con RapidCheck
# (≥100 iterazioni) verifica che `check_compatibility` ammetta la mod SSE la
# sezione [compat] è completa, la piattaforma bersaglio coincide con la
# proiezione del Runtime_Context e la versione di GD promossa cade
# nell'intervallo [min, max] a estremi inclusi; altrimenti la esclude con
# esattamente una causa tra MissingCompat / PlatformMismatch / VersionOutOfRange.
# Header del loader in loader/lifecycle/ e loader/core/ (include relativo alla
# radice loader/); la logica è in compatibility.cpp (compilata in pulse::loader).
# Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property2_test
    eml_property2_compatibility_exclusion_test.cpp)

target_link_libraries(pulse_eml_property2_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property2_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property2_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property2_test)

message(STATUS "Pulse: target external-mod-loading Property 2 test configurato (task 1.5).")

# ---------------------------------------------------------------------------
# Property 6 (external-mod-loading) — integrità sensibile alla mutazione
# (proprietà METAMORFICA). Su input randomizzati con RapidCheck (≥100 iterazioni)
# verifica che un Mod_Package valido sia accettato dal `ModManifestValidator`,
# mentre lo STESSO package con UN solo byte mutato in una QUALSIASI voce coperta
# da `MANIFEST.sha256` (`pulse.toml`, `code/module.pulsebin`, `resources/...`)
# sia rifiutato: con causa `IntegrityMismatch` quando il manifest resta
# analizzabile (Req 2.5/2.6), oppure `ManifestUnparsable` se la mutazione del
# manifest ne rompe l'analisi (ordine garantito manifest → integrità).
# Header del loader in loader/lifecycle/ e loader/package/ (include relativo alla
# radice loader/); la logica è in mod_manifest_validator.cpp (in pulse::loader).
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property6_test
    eml_property6_integrity_mutation_test.cpp)

target_link_libraries(pulse_eml_property6_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property6_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property6_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property6_test)

message(STATUS "Pulse: target external-mod-loading Property 6 test configurato (task 3.7).")

# ---------------------------------------------------------------------------
# Property test P5 (external-mod-loading, task 3.6) — Rifiuto della validazione
# senza caricare codice. Per ogni Mod_Package con file richiesto mancante,
# manifest non analizzabile, digest non conforme o senza entry point, il
# Mod_Manifest_Validator rifiuta la mod senza esporre contenuto (manifest ==
# nullopt) con diagnostica e causa dell'insieme chiuso; le altre proseguono.
# Validates: Requisiti 2.2, 2.4, 2.6, 2.8. Logica in mod_manifest_validator.cpp
# (compilata in pulse::loader). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property5_test
    eml_property5_validation_rejection_test.cpp)

target_link_libraries(pulse_eml_property5_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property5_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property5_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property5_test)

message(STATUS "Pulse: target external-mod-loading Property 5 test configurato (task 3.6).")

# ---------------------------------------------------------------------------
# Property test P20 (external-mod-loading, task 5.7) — Invariante globale di
# proprietà degli hook. Tag: "Feature: external-mod-loading, Property 20".
# Su sequenze randomizzate di enable/disable/teardown (RapidCheck, ≥100
# iterazioni) verifica che IN OGNI ISTANTE l'insieme degli hook installati nel
# HookOwnershipLedger sia esattamente l'unione degli hook delle sole mod
# Enabled, senza alcun hook installato privo di una mod proprietaria Enabled.
# Validates: Requisiti 7.3, 9.6. Logica in hook_ownership.cpp (compilata in
# pulse::loader via glob lifecycle/*.cpp); header pubblico <pulse/hooks.hpp>.
# Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property20_test
    eml_property20_hook_ownership_global_invariant_test.cpp)

target_link_libraries(pulse_eml_property20_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property20_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property20_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property20_test)

message(STATUS "Pulse: target external-mod-loading Property 20 test configurato (task 5.7).")

# ---------------------------------------------------------------------------
# Unit test della state machine del ModManager (task 12.1, Req 4.4, 4.5, 4.6,
# 4.8). Verifica: transizioni ammesse applicate e non ammesse rifiutate con
# stato invariato + segnalazione che identifica mod e transizione; entry point
# invocato all'enable (e solo all'enable); terminator invocati in ordine
# inverso al caricamento, solo per le mod abilitate. Header del loader in
# loader/lifecycle/ (include relativo alla radice loader/); la logica è in
# mod_manager.cpp (compilata in pulse::loader via glob lifecycle/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_mod_manager_test mod_manager_test.cpp)

target_link_libraries(pulse_mod_manager_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_mod_manager_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_mod_manager_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_mod_manager_test)

message(STATUS "Pulse: target ModManager state machine test configurato (task 12.1).")

# ---------------------------------------------------------------------------
# Unit test del Manifest e del parser/serializer pulse.toml (task 13.1,
# Req 16.1, 16.5). Verifica presenza dei campi e round-trip
# serialize->parse->equal (più parse∘serialize∘parse == parse), round-trip dei
# vincoli di versione e dei default tipizzati dei settings, tolleranza del
# parser a commenti/spaziatura e riconciliazione con il DependencyResolver.
# Header-only: include "lifecycle/manifest.hpp" (radice loader/ nella include
# path); la logica di risoluzione è in dependency_resolver.cpp (in pulse::loader
# via glob lifecycle/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_manifest_test manifest_test.cpp)

target_link_libraries(pulse_manifest_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_manifest_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_manifest_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_manifest_test)

message(STATUS "Pulse: target Manifest test configurato (task 13.1).")

# ---------------------------------------------------------------------------
# Property test P13 — ordine inverso di terminazione (task 12.4, Req 4.8).
# Tag: "Feature: pulse-sdk, Property 13". Su insiemi randomizzati di mod con
# ordine di caricamento randomizzato e un sottoinsieme casuale abilitato
# (RapidCheck, ≥100 iterazioni di default), verifica che ModManager::shutdown()
# invochi i terminator delle SOLE mod abilitate nell'esatto INVERSO dell'ordine
# di caricamento (loadOrder filtrato alle abilitate, poi rovesciato), che i
# terminator delle mod non abilitate non siano mai invocati e che l'ordine
# restituito coincida con la sequenza tracciata. Usa entry point che hanno
# sempre successo per ottenere uno stato Enabled deterministico e l'API pubblica
# stabile registerMod/enable/shutdown. Header del loader in loader/lifecycle/
# (include relativo alla radice loader/); la logica è in mod_manager.cpp
# (compilata in pulse::loader). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property13_test property13_reverse_order_termination_test.cpp)

target_link_libraries(pulse_property13_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property13_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property13_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property13_test)

message(STATUS "Pulse: target Property 13 test configurato (task 12.4).")

# ---------------------------------------------------------------------------
# Property test P12 — validità delle transizioni di stato della mod
# (task 12.3, Req 4.4, 4.5). Tag: "Feature: pulse-sdk, Property 12".
# Su sequenze randomizzate di richieste di transizione contro una mod
# registrata (RapidCheck, ≥100 iterazioni di default) verifica: ogni
# transizione APPLICATA è ammessa da isAllowed(prevState, target) e porta a
# newState == target; ogni transizione RIFIUTATA mantiene lo stato invariato e
# produce una segnalazione che identifica la mod e la transizione (from →
# requested); lo stato è sempre uno dei quattro stati validi e cambia solo
# tramite una transizione ammessa. La mod è registrata con un entry point che
# ha sempre successo, così l'esito dipende solo da isAllowed (robusto rispetto
# al task 12.2). Header del loader in loader/lifecycle/ (include relativo alla
# radice loader/); la logica è in mod_manager.cpp (compilata in pulse::loader
# via glob lifecycle/*.cpp). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property12_test
    property12_mod_state_transition_validity_test.cpp)

target_link_libraries(pulse_property12_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property12_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property12_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property12_test)

message(STATUS "Pulse: target Property 12 test configurato (task 12.3).")

# ---------------------------------------------------------------------------
# Unit test della validazione del Manifest contro lo schema (task 13.2,
# Req 16.2, 16.3, 16.4). Verifica: Manifest valido accettato; ogni regola di
# schema violata segnalata sul campo corretto; violazioni multiple simultanee
# tutte elencate (non solo la prima, Req 16.4); Manifest assente
# (std::optional vuoto e ParseResult fallito) rifiutato (Req 16.3). Header-only:
# include `lifecycle/manifest_validation.hpp` e `lifecycle/manifest.hpp`
# (radice loader/ nella include path). Nessuna dipendenza dalla compilazione di
# un .cpp: la logica di validazione è interamente inline negli header.
# ---------------------------------------------------------------------------
add_executable(pulse_manifest_validation_test manifest_validation_test.cpp)

target_link_libraries(pulse_manifest_validation_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_manifest_validation_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_manifest_validation_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_manifest_validation_test)

message(STATUS "Pulse: target Manifest validation test configurato (task 13.2).")

# ---------------------------------------------------------------------------
# Unit test del Pulse Package `.pulse` (task 13.3, Req 16.2). Verifica che il
# manifest (`pulse.toml`) sia letto/validato PRIMA di esporre qualsiasi entry
# di codice: pulse.toml assente/invalido -> open fallisce e nessun codice è
# accessibile (nessun PulsePackage costruito); presenza/integrità di
# `MANIFEST.sha256` (match/mismatch); enumerazione di `code/` e `resources/` e
# accesso a `SIGNATURE.sig`. Header-only: include "package/pulse_package.hpp" e
# "lifecycle/manifest.hpp" (radice loader/ nella include path); la logica di
# parsing del manifest è in dependency_resolver.cpp (in pulse::loader via glob
# lifecycle/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_package_test package_test.cpp)

target_link_libraries(pulse_package_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_package_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_package_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_package_test)

message(STATUS "Pulse: target Pulse Package test configurato (task 13.3).")

# ---------------------------------------------------------------------------
# Unit test del gate di installazione su firma e integrità (task 37.1, Req
# 23.3, 23.4, 23.5, 28.6, 28.7). Verifica che l'InstallGate verifichi la firma
# digitale PRIMA di installare qualsiasi file (≤10 s via orologio iniettabile)
# e l'integrità (`MANIFEST.sha256`) PRIMA di eseguire codice: firma assente o
# non valida OPPURE integrità fallita -> nessun file installato, nessun codice
# eseguito + messaggio. Header-only: include "package/install_gate.hpp",
# "package/pulse_package.hpp" e "lifecycle/manifest.hpp" (radice loader/ nella
# include path); la logica di parsing del manifest è in dependency_resolver.cpp
# (in pulse::loader via glob lifecycle/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_install_gate_test install_gate_test.cpp)

target_link_libraries(pulse_install_gate_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_install_gate_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_install_gate_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_install_gate_test)

message(STATUS "Pulse: target Install Gate test configurato (task 37.1).")

# ---------------------------------------------------------------------------
# Firma del Marketplace in pubblicazione — unit test (task 37.2, Req 23.1,
# 23.2). Controparte del gate di installazione: PRODUCE e REGISTRA
# `SIGNATURE.sig` in pubblicazione, modellando il budget di 5 s con un orologio
# iniettabile e un sink di pubblicazione ispezionabile. Verifica: firma
# riuscita entro budget -> SIGNATURE.sig registrato e package pubblicato; la
# firma prodotta è accettata da un verificatore corrispondente (consistenza con
# install_gate.hpp); fallimento del signer -> pubblicazione rifiutata, nessun
# package disponibile + messaggio all'autore; firma oltre budget -> rifiuto.
# Header-only: include "package/marketplace_signer.hpp", "package/install_gate.hpp",
# "package/pulse_package.hpp" e "lifecycle/manifest.hpp" (radice loader/ nella
# include path); la logica di parsing del manifest è in dependency_resolver.cpp
# (in pulse::loader via glob lifecycle/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_marketplace_signer_test marketplace_signer_test.cpp)

target_link_libraries(pulse_marketplace_signer_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_marketplace_signer_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_marketplace_signer_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_marketplace_signer_test)

message(STATUS "Pulse: target Marketplace Signer test configurato (task 37.2).")

# ---------------------------------------------------------------------------
# Property test P37 — gate di installazione su firma e integrità (task 37.3,
# Req 23.3, 23.4, 23.5, 28.6, 28.7). Tag: "Feature: pulse-sdk, Property 37".
# Su package randomizzati (RapidCheck, ≥100 iterazioni di default) che variano
# in modo indipendente firma presente/assente, firma valida/non valida (via
# verificatore fittizio) e integrità corretta/corrotta (entry manomessa dopo il
# calcolo di MANIFEST.sha256), verifica l'invariante: l'installazione riesce
# (filesInstalled>0 AND codeExecuted) SE E SOLO SE firma presente AND valida AND
# integrità corretta; in ogni caso di fallimento zero file installati e zero
# codice eseguito (RecordingSink a zero effetti) e l'errore riflette il primo
# gate fallito (firma prima dell'integrità). Header-only: include
# "package/install_gate.hpp", "package/pulse_package.hpp" e
# "lifecycle/manifest.hpp" (radice loader/ nella include path); la logica di
# parsing del manifest è in dependency_resolver.cpp (in pulse::loader via glob
# lifecycle/*.cpp). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property37_test property37_install_gate_test.cpp)

target_link_libraries(pulse_property37_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property37_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property37_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property37_test)

message(STATUS "Pulse: target Property 37 test configurato (task 37.3).")

# ---------------------------------------------------------------------------
# Property test P38 — isolamento dei fallimenti di inizializzazione delle mod
# (task 12.5, Req 4.7, 28.4, 28.5). Tag: "Feature: pulse-sdk, Property 38".
# Su insiemi randomizzati di mod il cui entry point è assegnato casualmente a
# riuscire, restituire errore o lanciare un'eccezione (RapidCheck, ≥100
# iterazioni di default), verifica che ModManager::enableAll(loadOrder) isoli
# ciascun fallimento alla sola mod imputata: enabled == mod riuscite (stato
# Enabled), failed == mod fallite (stato Disabled) con flag `threw` corretto e
# causa, initFailures().size() == numero di mod fallite, tutte le mod sono
# visitate e nessuna eccezione propaga fuori da enableAll (Req 28.4). Header
# del loader in loader/lifecycle/ (include relativo alla radice loader/); la
# logica è in mod_manager.cpp (compilata in pulse::loader via glob
# lifecycle/*.cpp). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property38_test property38_init_failure_isolation_test.cpp)

target_link_libraries(pulse_property38_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property38_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property38_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property38_test)

message(STATUS "Pulse: target Property 38 test configurato (task 12.5).")

# ---------------------------------------------------------------------------
# Property test P35 — soundness della validazione del Manifest (task 13.5,
# Req 16.2, 16.4). Tag: "Feature: pulse-sdk, Property 35".
# Su input randomizzati (RapidCheck, ≥100 iterazioni di default) verifica:
#   (1) un generatore di Manifest conformi a TUTTE le regole di schema produce
#       `validate(m).valid == true` con zero violazioni (soundness);
#   (2) partendo da un Manifest valido, iniettando una o più violazioni di
#       regola (id vuoto / id > 256 / schema_version < 1 / zero entry point /
#       entry point con kind o symbol vuoto / id di dipendenza vuoto / permesso
#       vuoto o non riconosciuto / setting con nome vuoto, tipo errato o default
#       incoerente), `validate` ritorna `valid == false` e l'insieme dei campi
#       riportati copre OGNI violazione iniettata (Req 16.4 — elenco completo).
# Header-only: include `lifecycle/manifest_validation.hpp` e
# `lifecycle/manifest.hpp` (radice loader/ nella include path); la logica di
# validazione è interamente inline negli header (consumati, non modificati).
# Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property35_test property35_manifest_validation_soundness_test.cpp)

target_link_libraries(pulse_property35_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property35_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property35_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property35_test)

message(STATUS "Pulse: target Property 35 test configurato (task 13.5).")

# ---------------------------------------------------------------------------
# Property test P34 — round-trip del Manifest (task 13.4, Req 16.5).
# Tag: "Feature: pulse-sdk, Property 34". Su Manifest GENERATI casualmente
# (validi per costruzione: id non vuoto, ≥1 entry point, SemVer, dipendenze con
# vincolo min + max opzionale, permessi, settings con default della variante
# coerente al tipo) verifica (RapidCheck, ≥100 iterazioni di default):
#   (a) parse(serialize(m)).ok && parse(serialize(m)) == m;
#   (b) serialize(parse(serialize(m))) == serialize(m) (punto fisso della forma
#       canonica) e parse∘serialize∘parse == parse.
# L'alfabeto delle stringhe include i caratteri che richiedono escaping (apice,
# backslash, \n, \r, \t) e i simboli che potrebbero confondere il parser TOML.
# I default float sono limitati ai double finiti (round-trip esatto garantito
# da to_chars/from_chars, confrontati esattamente da Manifest::operator==).
# Header-only: include `lifecycle/manifest.hpp` (radice loader/ nella include
# path), consumato senza modifiche. Integrazione RapidCheck+GoogleTest in
# extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property34_test property34_manifest_round_trip_test.cpp)

target_link_libraries(pulse_property34_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property34_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property34_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property34_test)

message(STATUS "Pulse: target Property 34 test configurato (task 13.4).")

# ---------------------------------------------------------------------------
# Unit test di pulse::storage::ModStorage (task 20.1, Req 10.1, 10.4, 10.5).
# Verifica: round-trip set/get; chiave assente -> std::nullopt senza errore
# bloccante (Req 10.4); rifiuto su capacità superata che preserva invariati i
# dati già presenti e segnala il superamento (Req 10.5); isolamento per
# identità della Mod e capacità di default ≥ 10 MB (Req 10.1). Header-only:
# include il header pubblico dello SDK <pulse/storage.hpp>.
# ---------------------------------------------------------------------------
add_executable(pulse_storage_test storage_test.cpp)

target_link_libraries(pulse_storage_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
)

target_compile_features(pulse_storage_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_storage_test)

message(STATUS "Pulse: target ModStorage test configurato (task 20.1).")

# ---------------------------------------------------------------------------
# Unit test delle impostazioni tipizzate (task 19.1, Req 9.1, 9.4, 9.6).
# Verifica: SettingDecl con nome univoco 1..64, tipo supportato e default
# conforme (Req 9.1); rifiuto di nomi fuori dai limiti e default non conformi
# (Req 9.1); rifiuto dei nomi duplicati con segnalazione del nome in conflitto
# (Req 9.6); fallback al default dichiarato con segnalazione su valore
# persistito non valido (Req 9.4). Header-only: include l'header pubblico dello
# SDK <pulse/settings.hpp>.
# ---------------------------------------------------------------------------
add_executable(pulse_settings_test settings_test.cpp)

target_link_libraries(pulse_settings_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
)

target_compile_features(pulse_settings_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_settings_test)

message(STATUS "Pulse: target settings tipizzati test configurato (task 19.1).")

# ---------------------------------------------------------------------------
# Unit test di pulse::PulseField<T, Key, Class> (task 16.1, Req 6.1, 6.2, 6.3).
# Verifica la porzione storage/get/set/default/registro del field injection:
# default tipizzato quando il campo non è mai stato scritto (Req 6.3); round-trip
# set→get per-istanza e isolamento per-istanza (Req 6.2); più chiavi/campi
# indipendenti sulla stessa classe e capacità del registro fino a 256 campi per
# classe (Req 6.1). Header-only: include il header pubblico dello SDK
# <pulse/fields.hpp>.
# ---------------------------------------------------------------------------
add_executable(pulse_field_test pulse_field_test.cpp)

target_link_libraries(pulse_field_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
)

target_compile_features(pulse_field_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_field_test)

message(STATUS "Pulse: target PulseField test configurato (task 16.1).")

# ---------------------------------------------------------------------------
# Unit test dell'EventBus dello SDK (task 18.1, Req 7.1, 7.2, 7.3, 7.4, 7.5,
# 7.6). Verifica: registrazione solo per tipi dichiarati con rifiuto dei tipi
# non dichiarati; invocazione in ordine di registrazione; isolamento delle
# eccezioni con prosecuzione; stop su Propagation::Consumed; deregistrazione di
# tutti i gestori di una mod su disable. Header-only: include l'header pubblico
# dello SDK <pulse/events.hpp> (via pulse::sdk).
# ---------------------------------------------------------------------------
add_executable(pulse_event_bus_test event_bus_test.cpp)

target_link_libraries(pulse_event_bus_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
)

target_compile_features(pulse_event_bus_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_event_bus_test)

message(STATUS "Pulse: target EventBus test configurato (task 18.1).")

# ---------------------------------------------------------------------------
# Unit test della verifica della firma a compile-time + risoluzione a load-time
# (task 17.1, Req 5.2, 5.4). Verifica: il concept `SignatureMatches` contro una
# specializzazione di `BindingTraits` (vero su firma identica, falso su
# ritorno/parametri diversi, permissivo senza binding noto), asserito via
# static_assert e requires; l'espansione di PULSE_HOOK su firma compatibile
# compila ed è registrata; PULSE_HOOK_PRIORITY registra la priorità di catena;
# `resolve_all` annulla SOLO l'hook irrisolto lasciando installati gli altri.
# Header-only: include il header pubblico dello SDK <pulse/hooks.hpp>.
# ---------------------------------------------------------------------------
add_executable(pulse_hooks_signature_resolution_test
    hooks_signature_resolution_test.cpp)

target_link_libraries(pulse_hooks_signature_resolution_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
)

target_compile_features(pulse_hooks_signature_resolution_test
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hooks_signature_resolution_test)

message(STATUS "Pulse: target firma+risoluzione hook test configurato (task 17.1).")

# ---------------------------------------------------------------------------
# Property test P17 — invocazione dei gestori di evento in ordine di
# registrazione (task 18.3, Req 7.3). Tag: "Feature: pulse-sdk, Property 17".
# Su un numero randomizzato N di gestori (0..64) registrati su un tipo di
# evento dichiarato (RapidCheck, ≥100 iterazioni di default), verifica che
# EventBus::emit invochi i gestori NELL'ORDINE DI REGISTRAZIONE: ogni gestore
# appende il proprio indice di registrazione a una traccia condivisa e la
# traccia risultante è esattamente 0, 1, ..., N-1 (Req 7.3). L'EventBus è
# esposto pubblicamente da <pulse/events.hpp> (link a pulse::sdk).
# Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property17_test property17_event_registration_order_test.cpp)

target_link_libraries(pulse_property17_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property17_test
    PRIVATE ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_features(pulse_property17_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property17_test)

message(STATUS "Pulse: target Property 17 test configurato (task 18.3).")

# ---------------------------------------------------------------------------
# Property test P16 — registrazione eventi solo per tipi dichiarati
# (task 18.2, Req 7.2). Tag: "Feature: pulse-sdk, Property 16".
# Su una sequenza randomizzata di operazioni declare/register su un pool fisso
# di tipi di evento distinti EventTag<N> (RapidCheck, ≥100 iterazioni di
# default), verifica che EventBus::on<E> abbia successo SE E SOLO SE E è stato
# dichiarato: la registrazione su un tipo NON dichiarato è rifiutata
# (registered==false, eventTypeDeclared==false) senza aggiungere gestori, e
# handlerCount<E>() riflette esclusivamente le registrazioni riuscite (Req 7.2).
# L'EventBus è esposto pubblicamente da <pulse/events.hpp> (link a pulse::sdk).
# Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property16_test
    property16_event_registration_declared_types_test.cpp)

target_link_libraries(pulse_property16_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property16_test
    PRIVATE ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_features(pulse_property16_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property16_test)

message(STATUS "Pulse: target Property 16 test configurato (task 18.2).")

# ---------------------------------------------------------------------------
# Unit test del cleanup al distruttore di PulseField (task 16.2, Req 6.4).
# Verifica `pulse::fields::releaseInstance<Class>(instance)`: alla distruzione
# di un'istanza si rilascia, in UNA sola operazione, tutto lo stato iniettato
# di quell'istanza attraverso più campi/chiavi della stessa classe, senza
# alterare lo stato delle altre istanze (Req 6.4 + isolamento per-istanza,
# Req 6.2); no-op sicuro su istanze senza stato; cleanup limitato al registro
# della classe; un cleaner per campo distinto (registrazione idempotente per
# tipo). Header-only: include il header pubblico dello SDK <pulse/fields.hpp>.
# ---------------------------------------------------------------------------
add_executable(pulse_field_release_test pulse_field_release_test.cpp)

target_link_libraries(pulse_field_release_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
)

target_compile_features(pulse_field_release_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_field_release_test)

message(STATUS "Pulse: target PulseField release/cleanup test configurato (task 16.2).")

# ---------------------------------------------------------------------------
# Unit test del sistema di logging e degli helper popup (task 21.1, Req 13.1,
# 13.2, 13.3, 13.4, 13.5, 13.6). Verifica: Level insieme chiuso {Debug, Info,
# Warning, Error}; log valido registrato con livello + identità mod e
# recuperabile per la sessione (Req 13.1, 13.5); rifiuto di un livello fuori
# insieme (codice grezzo) senza registrare (Req 13.2); destinazione non
# disponibile -> errore SinkUnavailable preservando l'esecuzione della mod
# (Req 13.6); popup con titolo non vuoto ≤100 e corpo ≤1000, rifiuto senza
# mostrare su titolo vuoto o contenuti oltre i limiti (Req 13.3, 13.4).
# Header-only: include il header pubblico dello SDK <pulse/log.hpp>.
# ---------------------------------------------------------------------------
add_executable(pulse_log_test log_test.cpp)

target_link_libraries(pulse_log_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
)

target_compile_features(pulse_log_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_log_test)

message(STATUS "Pulse: target logging+popup test configurato (task 21.1).")

# ---------------------------------------------------------------------------
# Property test P14 — isolamento per-istanza dei campi iniettati (task 16.3,
# Req 6.1, 6.2, 6.3). Tag: "Feature: pulse-sdk, Property 14".
# Su sequenze randomizzate di operazioni `set` distribuite su più istanze
# distinte di una classe del gioco e su più chiavi PulseField (RapidCheck,
# ≥100 iterazioni di default), verifica — confrontando contro un modello di
# riferimento `(chiave, istanza) -> valore atteso` — che get(instance)
# restituisca esattamente l'ultimo valore assegnato a QUELLA istanza per quella
# chiave (Req 6.2), che una set su un'istanza non alteri mai le altre (Req 6.2)
# e che le istanze mai scritte restituiscano il default tipizzato T{} (Req 6.3).
# Header-only: include il header pubblico dello SDK <pulse/fields.hpp> e
# l'header di integrazione RapidCheck+GoogleTest (extras/gtest).
# ---------------------------------------------------------------------------
add_executable(pulse_property14_test property14_field_instance_isolation_test.cpp)

target_link_libraries(pulse_property14_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property14_test
    PRIVATE ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_features(pulse_property14_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property14_test)

message(STATUS "Pulse: target Property 14 test configurato (task 16.3).")

# ---------------------------------------------------------------------------
# Property test P18 — isolamento delle eccezioni negli eventi (task 18.4,
# Req 7.4). Tag: "Feature: pulse-sdk, Property 18".
# Su N randomizzato (0..64) gestori registrati su un tipo dichiarato, con un
# sottoinsieme randomizzato che lancia eccezioni (RapidCheck, ≥100 iterazioni
# di default), verifica che EventBus::emit isoli ogni eccezione: TUTTI i
# gestori vengono invocati (un gestore che lancia non ferma i successivi),
# emit ritorna normalmente (nessuna eccezione propagata) e il numero di errori
# registrati in errors() è pari al numero di gestori che lanciano (Req 7.4).
# L'EventBus è esposto pubblicamente da <pulse/events.hpp> (link a pulse::sdk).
# Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property18_test
    property18_event_exception_isolation_test.cpp)

target_link_libraries(pulse_property18_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property18_test
    PRIVATE ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_features(pulse_property18_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property18_test)

message(STATUS "Pulse: target Property 18 test configurato (task 18.4).")

# ---------------------------------------------------------------------------
# Property test P19 — il consumo dell'evento interrompe la propagazione
# (task 18.5, Req 7.6). Tag: "Feature: pulse-sdk, Property 19".
# Su N randomizzato (0..64) gestori e una posizione randomizzata del
# consumatore (o nessuno — incluse prima e ultima posizione), verifica
# (RapidCheck, ≥100 iterazioni di default) che emit invochi i gestori in
# ordine di registrazione FINO AL PRIMO che restituisce Propagation::Consumed
# (incluso) e poi si fermi: il numero di gestori invocati è (k+1) se esiste un
# consumatore in posizione k, altrimenti N; i gestori dopo il primo consumatore
# non vengono mai invocati (verifica via traccia condivisa). L'EventBus è
# esposto pubblicamente da <pulse/events.hpp> (link a pulse::sdk).
# Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property19_test
    property19_event_consumed_stops_propagation_test.cpp)

target_link_libraries(pulse_property19_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property19_test
    PRIVATE ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_features(pulse_property19_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property19_test)

message(STATUS "Pulse: target Property 19 test configurato (task 18.5).")

# ---------------------------------------------------------------------------
# Property test P15 — rilascio dello stato iniettato alla distruzione
# (task 16.4, Req 6.4). Tag: "Feature: pulse-sdk, Property 15".
# Su popolazioni randomizzate di istanze con valori per più campi/chiavi della
# stessa classe (RapidCheck, ≥100 iterazioni di default) verifica che
# `pulse::fields::releaseInstance<Class>(x)` rilasci entro un'unica operazione
# TUTTI i campi dell'istanza distrutta (has()==false, lettura al default
# tipizzato) lasciando intatte le istanze sopravvissute, che conservano
# esattamente i propri valori su ogni chiave. Header-only: include il header
# pubblico dello SDK <pulse/fields.hpp>. Integrazione RapidCheck+GoogleTest in
# extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property15_test property15_field_state_release_test.cpp)

target_link_libraries(pulse_property15_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property15_test
    PRIVATE ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_features(pulse_property15_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property15_test)

message(STATUS "Pulse: target Property 15 test configurato (task 16.4).")

# ---------------------------------------------------------------------------
# Compile-fail test del field injection (task 16.5, Req 6.5, 6.6).
#
# Verifica le proprietà di NON-compilazione del field injection type-safe:
#   * Req 6.5 — accesso a un campo iniettato con un tipo errato → errore di
#     compilazione (snippet `fields_compilefail_wrong_type.cpp`);
#   * Req 6.6 — chiave già usata per la stessa classe con un tipo diverso →
#     errore di compilazione (snippet `fields_compilefail_key_type_conflict.cpp`).
#
# Pattern: ogni snippet negativo è una OBJECT library `EXCLUDE_FROM_ALL` (non
# entra nel build normale, che altrimenti fallirebbe). Un test CTest costruisce
# quel target con `cmake --build` e la proprietà `WILL_FAIL TRUE` fa PASSARE il
# test quando la compilazione FALLISCE. La controparte POSITIVA
# (`fields_compilepass_valid.cpp`) DEVE invece compilare: è un eseguibile del
# build normale, con un test CTest aggiuntivo che ne verifica la compilazione
# con esito di SUCCESSO. Header-only: include il header pubblico dello SDK
# <pulse/fields.hpp> via pulse::sdk.
# ---------------------------------------------------------------------------

# --- Snippet negativo 1: accesso con tipo errato (Req 6.5). DEVE NON compilare.
add_library(pulse_field_compilefail_wrong_type OBJECT EXCLUDE_FROM_ALL
    fields_compilefail_wrong_type.cpp)

target_link_libraries(pulse_field_compilefail_wrong_type PRIVATE pulse::sdk)

target_compile_features(pulse_field_compilefail_wrong_type
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

add_test(NAME pulse_field_compilefail_wrong_type
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
            --target pulse_field_compilefail_wrong_type
            --config $<CONFIG>)
set_tests_properties(pulse_field_compilefail_wrong_type
    PROPERTIES WILL_FAIL TRUE)

# --- Snippet negativo 2: conflitto chiave/tipo sulla stessa classe (Req 6.6).
#     DEVE NON compilare.
add_library(pulse_field_compilefail_key_type_conflict OBJECT EXCLUDE_FROM_ALL
    fields_compilefail_key_type_conflict.cpp)

target_link_libraries(pulse_field_compilefail_key_type_conflict
    PRIVATE pulse::sdk)

target_compile_features(pulse_field_compilefail_key_type_conflict
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

add_test(NAME pulse_field_compilefail_key_type_conflict
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
            --target pulse_field_compilefail_key_type_conflict
            --config $<CONFIG>)
set_tests_properties(pulse_field_compilefail_key_type_conflict
    PROPERTIES WILL_FAIL TRUE)

# --- Controparte positiva: uso valido. DEVE compilare (Req 6.5, 6.6).
add_executable(pulse_field_compilepass_valid fields_compilepass_valid.cpp)

target_link_libraries(pulse_field_compilepass_valid PRIVATE pulse::sdk)

target_compile_features(pulse_field_compilepass_valid
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

# Test esplicito: la compilazione dell'uso valido deve avere SUCCESSO.
add_test(NAME pulse_field_compilepass_valid
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
            --target pulse_field_compilepass_valid
            --config $<CONFIG>)

message(STATUS "Pulse: target compile-fail field injection configurati (task 16.5).")

# ---------------------------------------------------------------------------
# Compile-fail test della firma dell'hook (task 17.2, Req 5.2).
#
# Verifica la proprietà di NON-compilazione del controllo di firma di
# PULSE_HOOK: dichiarare un hook la cui firma è INCOMPATIBILE con quella
# esposta da `BindingTraits<Symbol>::Fn` (tipo di ritorno o parametri diversi)
# deve far FALLIRE la compilazione con diagnostica (lo static_assert di
# `SignatureMatches` nella macro).
#
# Pattern (mirror di task 16.5): lo snippet negativo è una OBJECT library
# `EXCLUDE_FROM_ALL` (non entra nel build normale, che altrimenti fallirebbe).
# Un test CTest lo costruisce con `cmake --build` e la proprietà `WILL_FAIL
# TRUE` fa PASSARE il test quando la compilazione FALLISCE. La controparte
# POSITIVA (`hooks_compilepass_signature.cpp`) DEVE invece compilare: è un
# eseguibile del build normale, con un test CTest che ne verifica la
# compilazione con esito di SUCCESSO. Header-only: include il header pubblico
# dello SDK <pulse/hooks.hpp> via pulse::sdk.
# ---------------------------------------------------------------------------

# --- Snippet negativo: firma incompatibile col binding (Req 5.2). DEVE NON compilare.
add_library(pulse_hook_compilefail_signature OBJECT EXCLUDE_FROM_ALL
    hooks_compilefail_signature.cpp)

target_link_libraries(pulse_hook_compilefail_signature PRIVATE pulse::sdk)

target_compile_features(pulse_hook_compilefail_signature
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

add_test(NAME pulse_hook_compilefail_signature
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
            --target pulse_hook_compilefail_signature
            --config $<CONFIG>)
set_tests_properties(pulse_hook_compilefail_signature
    PROPERTIES WILL_FAIL TRUE)

# --- Controparte positiva: firma compatibile col binding. DEVE compilare (Req 5.2).
add_executable(pulse_hook_compilepass_signature hooks_compilepass_signature.cpp)

target_link_libraries(pulse_hook_compilepass_signature PRIVATE pulse::sdk)

target_compile_features(pulse_hook_compilepass_signature
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

# Test esplicito: la compilazione della firma compatibile deve avere SUCCESSO.
add_test(NAME pulse_hook_compilepass_signature
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
            --target pulse_hook_compilepass_signature
            --config $<CONFIG>)

message(STATUS "Pulse: target compile-fail firma hook configurati (task 17.2).")

# ---------------------------------------------------------------------------
# Property test P23 — fallback al default su impostazione persistita non valida
# (task 19.3, Req 9.4). Tag: "Feature: pulse-sdk, Property 23".
# Su dichiarazioni casuali (tipo casuale + default conforme) e valori persistiti
# generati per essere SOMETIMES conformi e SOMETIMES non conformi (tipo errato,
# oppure per Enum un'etichetta non ammessa), verifica (RapidCheck, ≥100
# iterazioni di default) che SettingsRegistry::loadPersisted: restituisca il
# valore persistito (senza fallback/segnalazione) quando conforme, e ricada sul
# default dichiarato (usedDefault, report con il nome) quando non conforme,
# lasciando la dichiarazione invariata. Header-only dello SDK pubblico
# <pulse/settings.hpp> (via pulse::sdk). Integrazione RapidCheck+GoogleTest in
# extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property23_test property23_settings_persisted_fallback_test.cpp)

target_link_libraries(pulse_property23_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property23_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_features(pulse_property23_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property23_test)

message(STATUS "Pulse: target Property 23 test configurato (task 19.3).")

# ---------------------------------------------------------------------------
# Property test P25 — chiave inesistente restituisce valore assente
# (task 20.3, Req 10.4). Tag: "Feature: pulse-sdk, Property 25".
# Su insiemi randomizzati di scritture (key,value) e di chiavi interrogate
# (RapidCheck, ≥100 iterazioni di default) verifica l'invariante
# get(queryKey).has_value() == (queryKey è stata scritta), che contains()
# concordi con get(), e che l'interrogazione di chiavi assenti restituisca
# sempre std::nullopt senza lanciare né bloccare. Header-only: include il
# header pubblico dello SDK <pulse/storage.hpp>; integrazione
# RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property25_test property25_storage_absent_key_test.cpp)

target_link_libraries(pulse_property25_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property25_test
    PRIVATE ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_features(pulse_property25_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property25_test)

message(STATUS "Pulse: target Property 25 test configurato (task 20.3).")

# ---------------------------------------------------------------------------
# Property test P24 — round-trip read-after-write dello storage (task 20.2,
# Req 10.2). Tag: "Feature: pulse-sdk, Property 24". Su sequenze randomizzate
# di `set` (chiavi da un piccolo pool, valori binary-safe inclusi NUL
# incorporati; RapidCheck, ≥100 iterazioni di default) verifica che
# `ModStorage::get(key)` restituisca SEMPRE l'ultimo valore scritto per quella
# chiave, confrontando con un modello std::map. Capacità ampia affinché ogni
# scrittura riesca. Header-only dello SDK <pulse/storage.hpp> (via pulse::sdk);
# integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property24_test property24_storage_read_after_write_test.cpp)

target_link_libraries(pulse_property24_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property24_test
    PRIVATE ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_features(pulse_property24_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property24_test)

message(STATUS "Pulse: target Property 24 test configurato (task 20.2).")

# ---------------------------------------------------------------------------
# Property test P22 — unicità dei nomi delle impostazioni (task 19.2, Req 9.6).
# Tag: "Feature: pulse-sdk, Property 22". Su sequenze randomizzate di declare()
# con nomi estratti da un piccolo pool (collisioni frequenti) e tipi/default
# sempre conformi (RapidCheck, ≥100 iterazioni di default), verifica che ogni
# declare() sia accettato se e solo se il nome non era già stato dichiarato,
# che il duplicato sia rifiutato con segnalazione del nome in conflitto, e che
# il registro conservi esattamente la prima dichiarazione per quel nome.
# Header pubblico dello SDK <pulse/settings.hpp> (via pulse::sdk); integrazione
# RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property22_test property22_setting_name_uniqueness_test.cpp)

target_link_libraries(pulse_property22_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property22_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_features(pulse_property22_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property22_test)

message(STATUS "Pulse: target Property 22 test configurato (task 19.2).")

# ---------------------------------------------------------------------------
# Property test P32 — validazione del livello di log (task 21.2, Req 13.2).
# Tag: "Feature: pulse-sdk, Property 32". Su un codice di livello intero
# randomizzato che copre valori validi (0..3) e invalidi (negativi, >3) e su
# messaggi binary-safe arbitrari (RapidCheck, ≥100 iterazioni di default),
# verifica che Logger::log(code, msg) abbia successo SSE code ∈ [0,3]: un
# codice fuori insieme è rifiutato con LogErrorCode::InvalidLevel e nulla è
# registrato nel SessionLogStore (dimensione invariata); un codice valido
# registra esattamente un record il cui Level coincide con levelFromCode(code).
# Header-only: include il header pubblico dello SDK <pulse/log.hpp> (via
# pulse::sdk). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property32_test property32_log_level_validation_test.cpp)

target_link_libraries(pulse_property32_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property32_test
    PRIVATE ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_features(pulse_property32_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property32_test)

message(STATUS "Pulse: target Property 32 test configurato (task 21.2).")

# ---------------------------------------------------------------------------
# Property test P26 — rifiuto su capacità superata preservando i dati
# (task 20.4, Req 10.5). Tag: "Feature: pulse-sdk, Property 26".
# Con una capacità PICCOLA iniettata (32..128 byte) e una sequenza randomizzata
# di `set` (chiavi da un piccolo pool, valori binary-safe corti inclusi NUL
# incorporati; RapidCheck, ≥100 iterazioni di default) verifica, rispetto a un
# modello di riferimento con contabilità dei byte (used - previousEntryCost +
# newEntryCost), che ogni `set` la cui occupazione proiettata supererebbe la
# capacità sia rifiutato con StorageErrorCode::CapacityExceeded lasciando
# INVARIATI dati e usedBytes(), mentre i `set` che rientrano abbiano successo e
# aggiornino lo stato in accordo col modello. Header pubblico dello SDK
# <pulse/storage.hpp> (via pulse::sdk); integrazione RapidCheck+GoogleTest in
# extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property26_test property26_storage_capacity_rejection_test.cpp)

target_link_libraries(pulse_property26_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property26_test
    PRIVATE ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_features(pulse_property26_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property26_test)

message(STATUS "Pulse: target Property 26 test configurato (task 20.4).")

# ---------------------------------------------------------------------------
# Property test P33 — validazione del contenuto dei popup (task 21.3, Req 13.4).
# Tag: "Feature: pulse-sdk, Property 33". Su coppie (titolo, corpo) con
# lunghezze campionate attorno ai confini (titolo: 0,1,99,100,101; corpo:
# 0,999,1000,1001) più lunghezze arbitrarie (RapidCheck, >=100 iterazioni di
# default), verifica che validatePopup/showPopup accettino SSE titolo non vuoto
# E titolo.size()<=100 E corpo.size()<=1000, altrimenti rifiutino con il codice
# di errore corretto secondo la precedenza di validatePopup (titolo vuoto ->
# EmptyTitle; altrimenti titolo troppo lungo -> TitleTooLong; altrimenti corpo
# troppo lungo -> BodyTooLong). Un IPopupPresenter registratore garantisce che
# un popup rifiutato non venga MAI mostrato e uno accettato esattamente una
# volta. L'header pubblico dello SDK <pulse/log.hpp> espone showPopup/
# validatePopup (link a pulse::sdk). Integrazione RapidCheck+GoogleTest in
# extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property33_test property33_popup_content_validation_test.cpp)

target_link_libraries(pulse_property33_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property33_test
    PRIVATE ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_features(pulse_property33_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property33_test)

message(STATUS "Pulse: target Property 33 test configurato (task 21.3).")

# ---------------------------------------------------------------------------
# Unit test del bootstrap macOS (task 23.1, Req 1.1, 1.2, 1.3, 1.4).
# Verifica MacOSBootstrap: platform()==MacOS, invocazione dell'entry point del
# runtime iniettabile (early-load prima della scena iniziale) ed idempotenza
# su host Apple; fallback diagnostico UnsupportedHost su host non-Apple. Gli
# header del bootstrap vivono in loader/bootstrap/ (include "bootstrap/..."):
# il test aggiunge loader/ alla include path; la logica è in
# macos_bootstrap.cpp (compilata in pulse::loader via glob bootstrap/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_macos_bootstrap_test macos_bootstrap_test.cpp)

target_link_libraries(pulse_macos_bootstrap_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_macos_bootstrap_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_macos_bootstrap_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_macos_bootstrap_test)

message(STATUS "Pulse: target MacOSBootstrap test configurato (task 23.1).")

# ---------------------------------------------------------------------------
# Unit test del bootstrap iOS (task 23.3, Req 1.1, 1.2, 1.3). Verifica
# IOSBootstrap: platform()==IOSArm64; sull'host di sviluppo non-iOS inject()
# ritorna UnsupportedHost senza eseguire l'entry point (Req 1.3); sul target
# iOS (gating TARGET_OS_IPHONE) invoca l'entry point del runtime prima della
# scena iniziale e preserva l'idempotenza. Header del loader in
# loader/bootstrap/ (include PRIVATE): il test aggiunge loader/ alla include
# path; la logica è in ios_bootstrap.cpp (in pulse::loader via glob bootstrap/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_ios_bootstrap_test ios_bootstrap_test.cpp)

target_link_libraries(pulse_ios_bootstrap_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_ios_bootstrap_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_ios_bootstrap_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_ios_bootstrap_test)

message(STATUS "Pulse: target iOS bootstrap test configurato (task 23.3).")

# ---------------------------------------------------------------------------
# Unit test dell'entry point centralizzato del runtime (task 23.4, Req 1.3,
# 1.4, 1.6). Verifica CentralizedLoader::start(): iniezione fallita → log +
# avvio senza mod (Req 1.3); percorso felice → mod caricate tramite un unico
# entry point (Req 1.4); inizializzazione oltre il budget di 10 s → watchdog
# che interrompe il caricamento, logga il timeout e avvia GD senza mod (Req
# 1.6). Bootstrap, clock e step di init sono iniettati per forzare il timeout
# in modo deterministico senza dormire. Header del loader in loader/core/ e
# loader/bootstrap/ (include relativo alla radice loader/); la logica è in
# centralized_loader.cpp (compilata in pulse::loader via glob core/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_centralized_loader_test centralized_loader_test.cpp)

target_link_libraries(pulse_centralized_loader_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_centralized_loader_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_centralized_loader_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_centralized_loader_test)

message(STATUS "Pulse: target CentralizedLoader test configurato (task 23.4).")

# ---------------------------------------------------------------------------
# OnlineBindingsProvider — unit test (task 25.1, Req 20.1, 20.2).
# Verifica il provider online auto-aggiornante con fetcher/verifier fittizi
# iniettati (host, senza rete): download verificato => set online; firma non
# valida o fetch fallito => fallback embedded; cache hit evita il ri-fetch;
# coppia non esatta => nullopt (nessun fuzzy-match); resolve a corrispondenza
# esatta del simbolo; round-trip del formato .pbind. Header dei bindings in
# loader/bindings/ (include relativo alla radice loader/); la logica è in
# online_bindings_provider.cpp ed embedded_bindings_provider.cpp (compilate in
# pulse::loader via glob bindings/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_online_bindings_provider_test online_bindings_provider_test.cpp)

target_link_libraries(pulse_online_bindings_provider_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_online_bindings_provider_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_online_bindings_provider_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_online_bindings_provider_test)

message(STATUS "Pulse: target OnlineBindingsProvider test configurato (task 25.1).")

# ---------------------------------------------------------------------------
# Property test P3 — concatenamento completo della catena (2..64)
# (task 6.4, Req 3.1). Tag: "Feature: pulse-sdk, Property 3".
# Su catene randomizzate di N hook con 2 ≤ N ≤ 64 (RapidCheck, ≥100 iterazioni
# di default) verifica che HookChain::dispatch + HookContext::callNext() visiti
# tutti gli N gestori nell'ordine deterministico (priority DESC, loadOrder ASC)
# e termini invocando la funzione originale esattamente una volta in coda,
# quando ogni gestore chiama callNext(). L'originale è modellato dal trampolino
# del FakeBackend in-memory (loader/hooking/). Header del loader in
# loader/hooking/ (include relativo alla radice loader/); la logica di catena è
# in hook_chain.cpp (compilata in pulse::loader). Integrazione
# RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property3_test property3_chain_full_traversal_test.cpp)

target_link_libraries(pulse_property3_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property3_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property3_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property3_test)

message(STATUS "Pulse: target Property 3 test configurato (task 6.4).")

# ---------------------------------------------------------------------------
# Property test P4 — interruzione della catena senza next
# (task 6.5, Req 3.4). Tag: "Feature: pulse-sdk, Property 4".
# Su catene randomizzate di N hook con 1 ≤ N ≤ 64 (RapidCheck, ≥100 iterazioni
# di default) verifica che, se il gestore in una posizione casuale K ritorna
# SENZA invocare callNext(), allora i gestori dopo K e la funzione originale
# NON vengono mai eseguiti, mentre i gestori 0..K vengono eseguiti una sola
# volta nell'ordine deterministico (priority DESC, loadOrder ASC) e viene
# registrato un evento di interruzione con mod + funzione. Header del loader in
# loader/hooking/ (include relativo alla radice loader/); la logica di catena è
# in hook_chain.cpp (compilata in pulse::loader). Integrazione
# RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property4_test property4_chain_stop_without_next_test.cpp)

target_link_libraries(pulse_property4_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property4_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property4_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property4_test)

message(STATUS "Pulse: target Property 4 test configurato (task 6.5).")

# ---------------------------------------------------------------------------
# Checker della checklist di parità con Geode (task 33.1, Req 25.3).
# Analizza `docs/geode-parity.md` e FALLISCE se la mappatura di parità non è
# completa: capability attesa assente, oppure riga priva del criterio
# osservabile o del requisito Pulse mappato (forma `Req N`). Il percorso del
# documento è iniettato a compile-time (PULSE_GEODE_PARITY_DOC) così il test è
# eseguibile da qualunque working directory. Non linka librerie del progetto:
# è un controllo di documentazione basato su GoogleTest.
# ---------------------------------------------------------------------------
add_executable(pulse_geode_parity_checker_test geode_parity_checker_test.cpp)

target_link_libraries(pulse_geode_parity_checker_test
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(pulse_geode_parity_checker_test
    PRIVATE PULSE_GEODE_PARITY_DOC="${PROJECT_SOURCE_DIR}/docs/geode-parity.md"
)

target_compile_features(pulse_geode_parity_checker_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_geode_parity_checker_test)

message(STATUS "Pulse: target Geode parity checker configurato (task 33.1).")

# ---------------------------------------------------------------------------
# Checker di completezza della documentazione Pulse-vs-Geode (task 39.1,
# Req 24.1/24.2/24.3/24.4). Analizza `docs/pulse-vs-geode.md` e FALLISCE se:
# meno di 10 miglioramenti; una riga di miglioramento priva di ID / limite
# Geode / approccio Pulse / criterio osservabile (indicando gli elementi
# mancanti); meno di 10 funzionalità nella tabella di confronto; o assenza
# della versione di Geode di riferimento. Il percorso del documento è iniettato
# a compile-time (PULSE_VS_GEODE_DOC) così il test è eseguibile da qualunque
# working directory. Non linka librerie del progetto: è un controllo di
# documentazione basato su GoogleTest.
# ---------------------------------------------------------------------------
add_executable(pulse_pulse_vs_geode_checker_test pulse_vs_geode_checker_test.cpp)

target_link_libraries(pulse_pulse_vs_geode_checker_test
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(pulse_pulse_vs_geode_checker_test
    PRIVATE PULSE_VS_GEODE_DOC="${PROJECT_SOURCE_DIR}/docs/pulse-vs-geode.md"
)

target_compile_features(pulse_pulse_vs_geode_checker_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_pulse_vs_geode_checker_test)

message(STATUS "Pulse: target Pulse-vs-Geode completeness checker configurato (task 39.1).")

# ---------------------------------------------------------------------------
# Property test P39 — completezza della documentazione dei miglioramenti
# (task 39.2, Req 24.1, 24.2). Tag: "Feature: pulse-sdk, Property 39".
# GENERA modelli in-memory della documentazione (righe con sottoinsiemi casuali
# delle 4 colonne presenti/vuote, conteggi casuali) e applica lo STESSO predicato
# di completezza del checker, verificando l'invariante: il modello è giudicato
# completo sse (righe miglioramenti ≥ 10) AND (ogni riga ha le 4 colonne non
# vuote con ID valido IMP-NN) AND (righe di confronto ≥ 10 con tutti gli stati);
# verifica inoltre che gli elementi segnalati come mancanti siano esattamente
# quelli assenti (Req 24.2). Un caso analizza il documento REALE
# docs/pulse-vs-geode.md (via la macro PULSE_VS_GEODE_DOC, come il checker) e ne
# asserisce la completezza con lo stesso predicato. Integrazione
# RapidCheck+GoogleTest in extras/gtest; non linka librerie del progetto.
# ---------------------------------------------------------------------------
add_executable(pulse_property39_test property39_improvements_doc_completeness_test.cpp)

target_link_libraries(pulse_property39_test
    PRIVATE
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property39_test
    PRIVATE ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_definitions(pulse_property39_test
    PRIVATE PULSE_VS_GEODE_DOC="${PROJECT_SOURCE_DIR}/docs/pulse-vs-geode.md"
)

target_compile_features(pulse_property39_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property39_test)

message(STATUS "Pulse: target Property 39 test configurato (task 39.2).")

# ---------------------------------------------------------------------------
# Unit test del NodeRegistry — registro dei node-ID dell'interfaccia
# (task 28.1, Req 8.1, 8.2, 8.3, 8.6, 8.7). Header-only: include l'header
# pubblico dello SDK <pulse/ui_nodes.hpp> (implementazione in sdk/ui/node_id.hpp).
# Verifica: id stabile e ≤ 256 caratteri (Req 8.1); byId restituisce il nodo o
# nullopt se assente (Req 8.2, 8.3); id > 256 caratteri rifiutato (Req 8.1);
# id distinti di mod diverse preservati (Req 8.6); collisione che mantiene
# entrambi i nodi distinti per (id, ModId) e la segnala senza overwrite (Req 8.7).
# ---------------------------------------------------------------------------
add_executable(pulse_node_registry_test node_registry_test.cpp)

target_link_libraries(pulse_node_registry_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
)

target_compile_features(pulse_node_registry_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_node_registry_test)

message(STATUS "Pulse: target NodeRegistry test configurato (task 28.1).")

# ---------------------------------------------------------------------------
# Unit test del client HTTP (task 30.1, Req 12.1–12.6).
# Header-only: include il header pubblico dello SDK <pulse/http.hpp>. Usa un
# FakeTransport in-memory (nessuna rete reale) e un predicato di permesso
# iniettato per verificare: ogni metodo HTTP, rifiuto del corpo richiesta
# >10 MB e della risposta >50 MB, validazione dello stato 100..599, categorie
# di errore {network, timeout}, limite di 16 richieste concorrenti per Mod
# (17ª rifiutata) e gating del permesso "network" (negazione prima del
# trasporto). Linka Threads per std::mutex del client.
# ---------------------------------------------------------------------------
add_executable(pulse_http_client_test http_client_test.cpp)

find_package(Threads REQUIRED)

target_link_libraries(pulse_http_client_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
        Threads::Threads
)

target_compile_features(pulse_http_client_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_http_client_test)

message(STATUS "Pulse: target HttpClient test configurato (task 30.1).")

# ---------------------------------------------------------------------------
# Property test P29 — limite di concorrenza delle richieste HTTP (task 30.2,
# Req 12.5). Tag: "Feature: pulse-sdk, Property 29". Header-only: include il
# header pubblico dello SDK <pulse/http.hpp>. Su conteggi randomizzati di
# richieste per-mod (alcuni ≤16, altri >16) su 1..3 Mod (RapidCheck, ≥100
# iterazioni di default), usando un FakeTransport che mantiene le richieste in
# volo finché non sono rilasciate, verifica l'invariante: le prime fino-a-16
# richieste per Mod sono accettate e ogni richiesta oltre la 16ª è rifiutata con
# ConcurrencyLimitExceeded; activeRequests per Mod non supera mai 16; il limite è
# applicato in modo indipendente per ogni Mod. Linka Threads per std::mutex del
# client. Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property29_test property29_http_concurrency_limit_test.cpp)

find_package(Threads REQUIRED)

target_link_libraries(pulse_property29_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
        rapidcheck
        Threads::Threads
)

target_include_directories(pulse_property29_test
    PRIVATE ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_features(pulse_property29_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property29_test)

message(STATUS "Pulse: target Property 29 test configurato (task 30.2).")

# ---------------------------------------------------------------------------
# Unit test dell'hot-reload dev (task 32.1, Req 15.1, 15.2, 15.3, 15.4, 15.5).
# Verifica l'orchestrazione lato loader del servizio HotReload: rifiuto fuori
# dalla modalità sviluppo (Req 15.5); rimozione degli hook della versione
# precedente PRIMA dell'installazione di quelli nuovi, con ordine osservato sul
# FakeBackend (Req 15.2); reload riuscito entro il budget di 5 s con conferma
# (Req 15.1, 15.3); ripristino dello stato precedente su installazione fallita
# o budget superato, mantenendo GD attivo (Req 15.4, 15.1). Usa un clock
# iniettato per pilotare il budget. Header-only per FakeBackend/HookEngine;
# include "lifecycle/hot_reload.hpp" (radice loader/ nella include path); la
# logica è in hot_reload.cpp (compilata in pulse::loader via glob lifecycle/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_hot_reload_test hot_reload_test.cpp)

target_link_libraries(pulse_hot_reload_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_hot_reload_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_hot_reload_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hot_reload_test)

message(STATUS "Pulse: target HotReload test configurato (task 32.1).")

# ---------------------------------------------------------------------------
# Unit test del sistema asincrono / task (task 29.1, Req 11.1–11.5).
# Header-only: include il header pubblico dello SDK <pulse/async.hpp>
# (implementazione in sdk/async/task.hpp). Usa il TaskScheduler host-testabile
# per verificare: avvio non bloccante che esegue il lavoro su un thread
# separato (Req 11.1); continuazione eseguita sul thread principale solo al
# pump/frame successivo, anche in caso di errore con la causa (Req 11.2, 11.3);
# rifiuto del 65° task simultaneo per mod (Req 11.5); cancellazione alla
# disabilitazione che impedisce l'esecuzione delle continuazioni pendenti
# (Req 11.4). Linka Threads per std::thread del worker.
# ---------------------------------------------------------------------------
add_executable(pulse_task_async_test task_async_test.cpp)

find_package(Threads REQUIRED)

target_link_libraries(pulse_task_async_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
        Threads::Threads
)

target_compile_features(pulse_task_async_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_task_async_test)

message(STATUS "Pulse: target Task async test configurato (task 29.1).")

# ---------------------------------------------------------------------------
# Property test P27 — limite di concorrenza dei task asincroni (task 29.2,
# Req 11.5). Tag: "Feature: pulse-sdk, Property 27". Su un numero modesto di
# mod e conteggi di spawn per-mod randomizzati (alcuni sotto, altri sopra 64),
# con un lavoro bloccante (rilasciato da un flag atomico) che mantiene i task
# in volo, verifica l'invariante: per ogni mod i primi fino-a-64 spawn riescono
# e ogni spawn oltre 64 è rifiutato con ConcurrencyLimitExceeded; activeCount
# non supera mai 64; il limite è applicato in isolamento per-mod (una mod al
# limite non riduce la capacità di un'altra). Header-only: include il header
# pubblico dello SDK <pulse/async.hpp>. Integrazione RapidCheck+GoogleTest in
# extras/gtest; linka Threads per std::thread del worker.
# ---------------------------------------------------------------------------
add_executable(pulse_property27_test property27_task_concurrency_limit_test.cpp)

find_package(Threads REQUIRED)

target_link_libraries(pulse_property27_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
        rapidcheck
        Threads::Threads
)

target_include_directories(pulse_property27_test
    PRIVATE ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_features(pulse_property27_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property27_test)

message(STATUS "Pulse: target Property 27 test configurato (task 29.2).")

# ---------------------------------------------------------------------------
# Property test P28 — cancellazione dei task alla disabilitazione (task 29.3,
# Req 11.4). Tag: "Feature: pulse-sdk, Property 28". Su insiemi randomizzati di
# task distribuiti su 1..3 mod, con un sottoinsieme casuale di mod disabilitate
# (cancelMod) in un momento casuale — prima del completamento (task in volo)
# oppure dopo il completamento ma prima del pump — verifica l'invariante: al
# pumpMainThread() NESSUNA continuazione di una mod disabilitata viene eseguita,
# mentre OGNI continuazione di una mod non disabilitata viene eseguita
# esattamente una volta. Il lavoro è bloccante (rilasciato da un flag atomico)
# per controllare la tempistica della cancellazione; i contatori per-task
# tracciano le esecuzioni. Header-only: include <pulse/async.hpp>. Integrazione
# RapidCheck+GoogleTest in extras/gtest; linka Threads per std::thread del worker.
# ---------------------------------------------------------------------------
add_executable(pulse_property28_test property28_task_cancellation_test.cpp)

target_link_libraries(pulse_property28_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
        rapidcheck
        Threads::Threads
)

target_include_directories(pulse_property28_test
    PRIVATE ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_features(pulse_property28_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property28_test)

message(STATUS "Pulse: target Property 28 test configurato (task 29.3).")

# ---------------------------------------------------------------------------
# Property test P20 — assenza di sovrascrittura dei node-ID (task 28.3,
# Req 8.6, 8.7). Tag: "Feature: pulse-sdk, Property 20". Su insiemi
# randomizzati di assegnazioni (modId, nodeAddress, id) attraverso più mod
# (RapidCheck, ≥100 iterazioni di default) verifica che NodeRegistry: (a)
# preservi ogni node-ID distinto; (b) non sovrascriva mai il primo registrante
# (byId(id) lo restituisce sempre); (c) mantenga ogni coppia (id, modId)
# risolvibile via byId(id, modId); (d) segnali la collisione (hasCollision)
# tenendo distinti i nodi che condividono un id. Header pubblico dello SDK
# <pulse/ui_nodes.hpp> (via pulse::sdk) e integrazione RapidCheck+GoogleTest in
# extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property20_test property20_node_id_no_overwrite_test.cpp)

target_link_libraries(pulse_property20_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property20_test
    PRIVATE ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_features(pulse_property20_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property20_test)

message(STATUS "Pulse: target Property 20 test configurato (task 28.3).")

# ---------------------------------------------------------------------------
# Unit test del sistema di layout (task 28.2, Req 8.4, 8.5). Header-only:
# include l'header pubblico aggregato dello SDK <pulse/ui.hpp> (implementazione
# in sdk/ui/layout.hpp). Verifica: apply() dispone i figli in riga mantenendo
# le dimensioni entro il budget di 16 ms misurato con un clock iniettabile
# (Req 8.4); su regole non valide o in conflitto applica un fallback che
# mantiene tutti i figli nei limiti del contenitore (Req 8.5) e segnala il
# contenitore interessato tramite sink ed esito (Req 8.5).
# ---------------------------------------------------------------------------
add_executable(pulse_layout_test layout_test.cpp)

target_link_libraries(pulse_layout_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
)

target_compile_features(pulse_layout_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_layout_test)

message(STATUS "Pulse: target Layout test configurato (task 28.2).")

# ---------------------------------------------------------------------------
# Unit test del controller del Mod Manager in-game (task 31.1, Req 22.1, 22.2,
# 22.3, 22.4, 22.5, 22.6, 22.7, 22.8). Riusa la state machine `ModManager`
# (loader/lifecycle/mod_manager.hpp) senza reimplementarla e verifica il
# controller `ModManagerUI`: elenco vuoto con messaggio / elenco con stato di
# abilitazione entro il budget (clock iniettabile); abilita/disabilita con
# fallback su transizione rifiutata (rejection sink del manager) + messaggio;
# ricerca Marketplace (fake) entro budget e su irraggiungibilità; install con
# rollback "tutto o niente" su fallimento; segnale di aggiornamenti disponibili.
# Header del loader in loader/lifecycle/ (include relativo alla radice loader/);
# la logica è in mod_manager_ui.cpp (compilata in pulse::loader via glob
# lifecycle/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_mod_manager_ui_test mod_manager_ui_test.cpp)

target_link_libraries(pulse_mod_manager_ui_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_mod_manager_ui_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_mod_manager_ui_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_mod_manager_ui_test)

message(STATUS "Pulse: target Mod Manager UI test configurato (task 31.1).")

# ---------------------------------------------------------------------------
# Property test P21 — contenimento del layout di fallback (task 28.4, Req 8.5).
# Tag: "Feature: pulse-sdk, Property 21". Su limiti di contenitore e insiemi di
# figli randomizzati (inclusi figli sovradimensionati / fuori dai bordi) e regole
# di spacing/padding eventualmente NEGATIVE o in conflitto (RapidCheck, ≥100
# iterazioni di default), verifica l'invariante del fallback (Req 8.5): quando
# le regole sono in conflitto/non valide, apply() applica il fallback che
# mantiene OGNI figlio entro i limiti del contenitore (clamp di posizione e
# dimensione) e SEGNALA il contenitore (sink + esito); nei casi validi non
# ricade sul fallback e i figli restano contenuti. Header pubblico dello SDK
# <pulse/ui.hpp> (promuove i tipi di layout nel namespace pulse) e integrazione
# RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property21_test property21_fallback_layout_containment_test.cpp)

target_link_libraries(pulse_property21_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property21_test
    PRIVATE ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_features(pulse_property21_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property21_test)

message(STATUS "Pulse: target Property 21 test configurato (task 28.4).")

# ---------------------------------------------------------------------------
# Unit test dell'UI auto-generata delle impostazioni (task 31.2, Req 9.2, 9.3,
# 9.5, 10.3, 10.6). Verifica il controller host-testabile
# pulse::SettingsUiController esposto da <pulse/settings_ui.hpp>: un controllo
# di input per ciascuna impostazione dichiarata con il ControlKind corretto
# (incl. opzioni enum); edit valido persistito e riletto entro il budget di 1 s
# (clock iniettato); edit non conforme rifiutato con valore precedente
# conservato e messaggio emesso (nulla persistito); eliminazione dei dati
# persistiti su rimozione mod solo dopo conferma dell'User, conservati
# altrimenti. Header-only dello SDK (pulse::sdk).
# ---------------------------------------------------------------------------
add_executable(pulse_settings_ui_test settings_ui_test.cpp)

target_link_libraries(pulse_settings_ui_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
)

target_compile_features(pulse_settings_ui_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_settings_ui_test)

message(STATUS "Pulse: target Settings UI test configurato (task 31.2).")

# ---------------------------------------------------------------------------
# Unit test della generazione e gestione dei CrashReport (task 38.1, Req 21.1,
# 21.2, 21.3, 21.4, 21.5, 21.6). Header-only: include
# `telemetry/crash_report.hpp` con clock, capturer, transmitter, consenso e
# store TUTTI iniettati (host, senza crash reale/rete/disco). Verifica:
# generazione entro 10 s con traccia dello stack; attribuito vs non attribuito;
# opt-in (nessuna trasmissione senza consenso, ritenzione locale >= 30 giorni);
# con consenso trasmissione riuscita entro 30 s; ritentativi fino a 3 e
# ritenzione su fallimento persistente. La radice loader/ è nella include path
# per l'include "telemetry/crash_report.hpp".
# ---------------------------------------------------------------------------
add_executable(pulse_crash_report_test crash_report_test.cpp)

target_link_libraries(pulse_crash_report_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_crash_report_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_crash_report_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_crash_report_test)

message(STATUS "Pulse: target CrashReport test configurato (task 38.1).")

# ---------------------------------------------------------------------------
# Unit test del Sandbox a permessi (task 35.1, Req 17.1, 17.2, 17.3, 17.4,
# 17.5, 17.6). Verifica: allow solo se dichiarato+approvato; deny prima di ogni
# effetto + violazione registrata sul sink quando non dichiarato/non approvato;
# installazione rifiutata su permesso non riconosciuto e su sezione permessi
# omessa; flusso di approvazione/rifiuto onorato (rifiuto => Mod non abilitata,
# stato invariato; consenso granulare). Header-only: include
# "sandbox/sandbox.hpp" (radice loader/ nella include path).
# ---------------------------------------------------------------------------
add_executable(pulse_sandbox_test sandbox_test.cpp)

target_link_libraries(pulse_sandbox_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_sandbox_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_sandbox_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_sandbox_test)

message(STATUS "Pulse: target Sandbox test configurato (task 35.1).")

# ---------------------------------------------------------------------------
# Property test P31 — rifiuto di dichiarazioni di permesso non valide
# (task 35.3, Req 17.2). Tag: "Feature: pulse-sdk, Property 31".
# Su liste di dichiarazione miste (token validi + token sconosciuti da un pool
# disgiunto) e talvolta la sezione omessa (nullopt) (RapidCheck, ≥100
# iterazioni di default) verifica l'invariante: sezione omessa ->
# PermissionsSectionOmitted; token non riconosciuto -> UnrecognizedPermission
# col token offensivo nel messaggio; in entrambi i casi la Mod non è installata
# e nessun consenso è registrato; altrimenti l'installazione riesce e l'insieme
# approvato coincide con dichiarati∩concessi. Header-only: include
# "sandbox/sandbox.hpp" (radice loader/ nella include path); integrazione
# RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property31_test property31_sandbox_permission_rejection_test.cpp)

target_link_libraries(pulse_property31_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property31_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property31_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property31_test)

message(STATUS "Pulse: target Property 31 test configurato (task 35.3).")

# ---------------------------------------------------------------------------
# Unit test dello ScriptingRuntime (task 36.1, Req 19.1, 19.2, 19.3, 19.4,
# 19.5). Verifica, con un orologio iniettato e un FakeScriptEngine in-memory
# (nessuna dipendenza da sol2/QuickJS), il caricamento entro il budget di ≤5 s,
# l'isolamento dell'errore di sintassi al load alla sola Mod, il gating delle
# capability di hooking/eventi/UI sui permessi iniettati e l'isolamento degli
# errori di esecuzione, senza terminare il gioco. Header-only: include
# `scripting/scripting_runtime.hpp` e `scripting/fake_script_engine.hpp`
# (radice loader/ nella include path).
# ---------------------------------------------------------------------------
add_executable(pulse_scripting_runtime_test scripting_runtime_test.cpp)

target_link_libraries(pulse_scripting_runtime_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_scripting_runtime_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_scripting_runtime_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_scripting_runtime_test)

message(STATUS "Pulse: target ScriptingRuntime test configurato (task 36.1).")

# ---------------------------------------------------------------------------
# Property test P30 — gating delle capability sui permessi, ramo SCRIPTING via
# ScriptingRuntime (task 36.2, Req 12.6, 17.3, 17.4, 19.3, 19.4).
# Tag: "Feature: pulse-sdk, Property 30". Su combinazioni randomizzate di
# capability concesse (Hooking/Events/UI) e query randomizzate (RapidCheck,
# ≥100 iterazioni di default) verifica l'invariante: useCapability(mod, cap)
# riesce SE E SOLO SE il predicato di permesso iniettato concede quella
# capability; altrimenti è negata PRIMA di qualsiasi effetto (PermissionDenied
# con Mod + capability), dimostrando un gating IDENTICO al ramo Sandbox nativo
# (coperto dal task 35.2). Header-only: include `scripting/scripting_runtime.hpp`
# e `scripting/fake_script_engine.hpp` (radice loader/ nella include path);
# integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property30_scripting_test property30_scripting_capability_gating_test.cpp)

target_link_libraries(pulse_property30_scripting_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property30_scripting_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property30_scripting_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property30_scripting_test)

message(STATUS "Pulse: target Property 30 scripting test configurato (task 36.2).")

# ---------------------------------------------------------------------------
# Property test P30 — gating delle capability sui permessi, ramo Sandbox
# NATIVO (task 35.2, Req 12.6, 17.3, 17.4, 19.3, 19.4).
# Tag: "Feature: pulse-sdk, Property 30". Su combinazioni randomizzate di
# (permesso dichiarato, permesso approvato) e su query randomizzate (RapidCheck,
# ≥100 iterazioni di default) verifica l'invariante: check(mod, perm) riesce SE
# E SOLO SE perm è dichiarato nel Manifest ED approvato dall'User; altrimenti è
# negato PRIMA di qualsiasi effetto e registra esattamente UNA violazione
# coerente (id Mod + permesso). Copre anche il caso della Mod non installata
# (sempre negata). Header-only: include "sandbox/sandbox.hpp" (radice loader/
# nella include path); integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property30_sandbox_test property30_sandbox_capability_gating_test.cpp)

target_link_libraries(pulse_property30_sandbox_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property30_sandbox_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property30_sandbox_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property30_sandbox_test)

message(STATUS "Pulse: target Property 30 (Sandbox nativo) test configurato (task 35.2).")

# ---------------------------------------------------------------------------
# Unit test del benchmark di regressione "a vuoto" (task 40.1, Req 28.1,
# 28.2, 28.3). Esercita l'harness header-only `tools/regression_benchmark.hpp`
# con fonti di misura iniettate (deterministico sull'host, senza tempo reale né
# gioco vero): frame rate entro/oltre il 2% (28.1), crash/freeze => fallimento
# del criterio di stabilità (28.2), init <= / > 2000 ms (28.3) ed esito
# aggregato (pass sse tutti i criteri passano). L'header vive in tools/ (fuori
# dai glob delle librerie): si aggiunge ${PROJECT_SOURCE_DIR} alla include path
# per risolvere `#include "tools/regression_benchmark.hpp"`.
# ---------------------------------------------------------------------------
add_executable(pulse_regression_benchmark_test regression_benchmark_test.cpp)

target_link_libraries(pulse_regression_benchmark_test
    PRIVATE
        GTest::gtest_main
)

target_include_directories(pulse_regression_benchmark_test
    PRIVATE ${PROJECT_SOURCE_DIR}
)

target_compile_features(pulse_regression_benchmark_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_regression_benchmark_test)

message(STATUS "Pulse: target benchmark di regressione test configurato (task 40.1).")

# ---------------------------------------------------------------------------
# Unit test del gate di similarità del codice (task 39.3, Req 27.1, 27.2, 27.3,
# 27.4, 27.5). Header-only e di sola lettura: include "tools/similarity_gate.hpp"
# (radice del progetto nella include path) e verifica la soglia >= 30%, il
# blocco identico di >= 8 righe consecutive, l'attribuzione di licenza per i
# frammenti derivati e l'immutabilità degli input. Nessuna dipendenza dal
# loader/SDK: il corpus Geode è iniettato dai test.
# ---------------------------------------------------------------------------
add_executable(pulse_similarity_gate_test similarity_gate_test.cpp)

target_link_libraries(pulse_similarity_gate_test
    PRIVATE
        GTest::gtest_main
)

# Consente l'include "tools/similarity_gate.hpp" relativo alla radice progetto.
target_include_directories(pulse_similarity_gate_test
    PRIVATE ${PROJECT_SOURCE_DIR}
)

target_compile_features(pulse_similarity_gate_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_similarity_gate_test)

message(STATUS "Pulse: target similarity gate test configurato (task 39.3).")

# ---------------------------------------------------------------------------
# Property test P40 — soglia del gate di similarità del codice (task 39.4,
# Req 27.3, 27.4). Tag: "Feature: pulse-sdk, Property 40".
# Su percentuali e percorsi file arbitrari (RapidCheck, >= 100 iterazioni di
# default), inclusi valori a cavallo del confine 29.99 / 30.0 / 30.01, verifica
# l'invariante: un file è bloccato dalla similarità SE E SOLO SE pct >= 30.0 e,
# quando bloccato, il messaggio identifica file + percentuale (Req 27.3/27.4).
# Copre inoltre la regola delle >= 8 righe identiche consecutive (run 7/8/9) e
# l'immutabilità degli input. Header-only e di sola lettura: include
# "tools/similarity_gate.hpp" (radice del progetto nella include path); nessuna
# dipendenza dal loader/SDK. Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property40_test property40_similarity_gate_threshold_test.cpp)

target_link_libraries(pulse_property40_test
    PRIVATE
        GTest::gtest_main
        rapidcheck
)

# Header di integrazione RapidCheck+GoogleTest (extras/gtest) e radice progetto
# per l'include "tools/similarity_gate.hpp".
target_include_directories(pulse_property40_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}
)

target_compile_features(pulse_property40_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property40_test)

message(STATUS "Pulse: target Property 40 test configurato (task 39.4).")

# ---------------------------------------------------------------------------
# Property test P1 (pulse-gd-integration) — idempotenza dell'entry point
# centralizzato (task 3.2, Req 2.6). Tag: "Feature: pulse-gd-integration,
# Property 1". Su N >= 1 invocazioni simulate del guard di early-load, in ordine
# arbitrario e anche concorrenti (RapidCheck, >= 100 iterazioni di default),
# verifica che l'entry centralizzato sia eseguito esattamente una volta. La
# guardia reale (`run_pulse_early_load_once` in macos_bootstrap.cpp) è compilata
# solo sotto PULSE_LOADER_ARTIFACT e in namespace anonimo, quindi non linkabile
# dall'host: il test replica lo stesso primitivo `std::atomic_flag` run-once.
# Nessuna dipendenza dal loader/SDK. Integrazione RapidCheck+GoogleTest in
# extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_idempotency_property_test idempotency_property_test.cpp)

target_link_libraries(pulse_idempotency_property_test
    PRIVATE
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_idempotency_property_test
    PRIVATE ${rapidcheck_SOURCE_DIR}/extras/gtest/include
)

target_compile_features(pulse_idempotency_property_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

# Il test concorrente usa std::thread.
find_package(Threads REQUIRED)
target_link_libraries(pulse_idempotency_property_test PRIVATE Threads::Threads)

gtest_discover_tests(pulse_idempotency_property_test)

message(STATUS "Pulse: target Property 1 (pulse-gd-integration) test configurato (task 3.2).")

# ---------------------------------------------------------------------------
# Unit test della policy fail-open centralizzata (task 3.12, Req 2.7, 2.8, 5.3,
# 5.4, 9.6, 10.1, 10.2, 10.4). Verifica che `CentralizedLoader` +
# `make_fail_open_init_step(...)` (riuso di `HookGate`) degradino verso "GD
# parte con zero mod" per ogni nuovo punto di fallimento (piattaforma senza
# Platform_Bootstrap reale, versione/piattaforma non rilevata, coppia senza
# `.pbind`, backend non disponibile, simbolo non risolto): 0 hook, diagnostica
# della causa, esito "senza mod" ed eseguibile/asset (modellati da un buffer)
# invariati; il caso felice installa esattamente un hook. Header del loader in
# loader/ (include PRIVATE): il test aggiunge loader/ alla include path.
# ---------------------------------------------------------------------------
add_executable(pulse_fail_open_policy_test fail_open_policy_test.cpp)

target_link_libraries(pulse_fail_open_policy_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_fail_open_policy_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_fail_open_policy_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_fail_open_policy_test)

message(STATUS "Pulse: target fail-open policy test configurato (task 3.12).")

# ---------------------------------------------------------------------------
# Property test P4 — binding risolto se e solo se verificato (task 3.10,
# Req 4.3). Tag: "Feature: pulse-gd-integration, Property 4". Su input
# randomizzati (RapidCheck, ≥100 iterazioni di default) genera un `BindingSet`
# con un MIX di voci che variano lungo tutte le dimensioni rilevanti — categoria
# dell'offset (zero / sentinel / valido), claim di verifica (.pbind verified
# true/false), conformità del prologo (true/false) — e verifica, attraverso
# `verify_binding_set` (la stessa logica di `addVerifiedSet`), che per OGNI voce
# `resolved == (claimedVerified ∧ offset≠0 ∧ offset≠sentinel ∧ prologoConforme)`,
# cioè `resolved` è vero SE E SOLO SE la voce è genuinamente verificata. Header
# dei bindings in loader/ (include relativo alla radice loader/) e integrazione
# RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_gd_property4_test binding_resolved_iff_verified_property_test.cpp)

target_link_libraries(pulse_gd_property4_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_gd_property4_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_gd_property4_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_gd_property4_test)

message(STATUS "Pulse: target Property 4 (pulse-gd-integration) test configurato (task 3.10).")

# ---------------------------------------------------------------------------
# Property test P5 — lookup esatto della coppia e zero hook su indirizzi non
# risolti (task 3.11, Req 4.4, 4.5, 5.2).
# Tag: "Feature: pulse-gd-integration, Property 5" (estende pulse-sdk Property
# 36 al PROVIDER REALE — EmbeddedBindingsProvider con addVerifiedSet — e alla
# detection esatta). Su input randomizzati (RapidCheck, ≥100 iterazioni di
# default) approvvigiona un EmbeddedBindingsProvider reale su coppie
# (GD_Version, piattaforma) distinte con un MIX di bindings verificati/non
# verificati e verifica: (a) load/resolve restituiscono un binding SSE la coppia
# esatta è indicizzata e il simbolo è presente — nessun fuzzy-match né
# sostituzione da coppia diversa (Req 4.4, 5.2); (b) instradando i binding
# attraverso HookGate + FakeBackend, l'invariante
# numeroHookSuIndirizziNonRisolti == 0 vale sempre (Req 4.5). Header dei
# bindings/hooking in loader/ (include relativo alla radice loader/) e
# integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_gd_property5_test exact_pair_lookup_property_test.cpp)

target_link_libraries(pulse_gd_property5_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_gd_property5_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_gd_property5_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_gd_property5_test)

message(STATUS "Pulse: target Property 5 (pulse-gd-integration) test configurato (task 3.11).")

# ---------------------------------------------------------------------------
# Property test P2 (pulse-gd-integration) — diagnostica della causa di
# fallimento del bootstrap (task 3.13, Req 2.7). Tag: "Feature:
# pulse-gd-integration, Property 2". Su input randomizzati (RapidCheck, >= 100
# iterazioni di default) verifica che per OGNI causa di fallimento iniettata
# l'esito di bootstrap sia `injected == false` con una `DiagnosticError` il cui
# codice coincide con la causa e il cui messaggio è NON vuoto. Esercita: (a) la
# fabbrica reale `BootstrapResult::failure` (header platform-agnostic), (b) un
# modello che inietta ciascuna causa dell'enum `BootstrapErrorCode`, e (c) il
# `MacOSBootstrap` reale (fallimento dell'entry point e seconda inject()).
# Header del bootstrap in loader/bootstrap/ (include relativo alla radice
# loader/); linka pulse::loader per MacOSBootstrap::inject(); integrazione
# RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_gd_property2_test bootstrap_diagnostic_property_test.cpp)

target_link_libraries(pulse_gd_property2_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_gd_property2_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_gd_property2_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_gd_property2_test)

message(STATUS "Pulse: target Property 2 (pulse-gd-integration) test configurato (task 3.13).")

# ---------------------------------------------------------------------------
# Cablaggio della demo mod sull'HookEngine reale — unit test (task 3.15,
# Req 9.1, 9.2, 9.3, 9.6). Verifica `pulse::loader::mvp::DemoMod`: risolve
# `MenuLayer::init` dai bindings reali (EmbeddedBindingsProvider) e installa
# ESATTAMENTE un hook su quella funzione attraverso `pulse::hooking::HookEngine`
# + FakeBackend (Req 9.1); il detour identifica la demo mod e l'hook nel log
# (Req 9.2) e invoca l'originale via trampolino finto ritornandone il valore
# (Req 9.3); su simbolo non risolto installa 0 hook, logga la causa e lascia
# proseguire GD (Req 9.6). Header del loader in loader/ (include relativo alla
# radice loader/) e header pubblico dello SDK <pulse/hooks.hpp>.
# ---------------------------------------------------------------------------
add_executable(pulse_gd_demo_mod_test demo_mod_test.cpp)

target_link_libraries(pulse_gd_demo_mod_test
    PRIVATE
        pulse::loader
        pulse::sdk
        GTest::gtest_main
)

target_include_directories(pulse_gd_demo_mod_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_gd_demo_mod_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_gd_demo_mod_test)

message(STATUS "Pulse: target demo mod (pulse-gd-integration) test configurato (task 3.15).")

# ---------------------------------------------------------------------------
# Property test P13 — fail-open: zero hook e byte invariati su ambiente non
# supportato (task 3.14, Req 2.8, 5.3, 5.4, 9.6, 10.1, 10.2, 10.4).
# Tag: "Feature: pulse-gd-integration, Property 13". Su input randomizzati
# (RapidCheck, ≥100 iterazioni di default) genera, per ogni iterazione, una
# delle CINQUE cause di ambiente non supportato (bootstrap UnsupportedHost,
# rilevamento di GD_Version/piattaforma fallito, coppia rilevata senza
# Binding_Set_File `.pbind`, Hooking_Backend non disponibile a runtime, funzione
# bersaglio non risolta) e verifica l'invariante fail-open via
# `pulse::loader::CentralizedLoader` + `make_fail_open_init_step` (che riusa
# `HookGate`): esito "senza mod" con la `StartReason` attesa, 0 hook installati
# (backend mai invocato), almeno una diagnostica registrata (che nomina il
# backend nel caso non disponibile), e un buffer di byte che MODELLA
# l'eseguibile/asset di Geometry Dash (albero di file finto) lasciato
# byte-per-byte invariato (Req 10.4). Header del loader in loader/ (include
# relativo alla radice loader/) e integrazione RapidCheck+GoogleTest in
# extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_gd_property13_test fail_open_property_test.cpp)

target_link_libraries(pulse_gd_property13_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_gd_property13_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_gd_property13_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_gd_property13_test)

message(STATUS "Pulse: target Property 13 (pulse-gd-integration) test configurato (task 3.14).")

# ---------------------------------------------------------------------------
# Unit test dell'ordine di log della demo mod (task 3.16, Req 9.5).
# Verifica l'example 9.5: quando il detour dimostrativo (cablato dalla DemoMod
# sull'HookEngine reale) scatta, la facility diagnostica registra l'esecuzione
# del DETOUR PRIMA dell'esecuzione dell'ORIGINALE. Host-testabile: usa il
# DemoLogSink per osservare i log del detour e un trampolino finto iniettabile
# come stand-in dell'originale, intrecciando entrambi in una sequenza condivisa
# per osservarne l'ordine relativo. Header del loader in loader/ (include
# relativo alla radice loader/) e header pubblico dello SDK <pulse/hooks.hpp>.
# ---------------------------------------------------------------------------
add_executable(pulse_gd_demo_log_order_test demo_log_order_test.cpp)

target_link_libraries(pulse_gd_demo_log_order_test
    PRIVATE
        pulse::loader
        pulse::sdk
        GTest::gtest_main
)

target_include_directories(pulse_gd_demo_log_order_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_gd_demo_log_order_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_gd_demo_log_order_test)

message(STATUS "Pulse: target demo log order (pulse-gd-integration) test configurato (task 3.16).")

# ---------------------------------------------------------------------------
# Unit test del HookOwnershipLedger (task 5.5, Req 5.8, 9.6).
# Verifica le finestre di epoca attorno al registro globale di pulse::hooks
# (openEpoch/closeEpoch), il seeding della demo al Mod_Id riservato
# kBuiltinDemoModId, l'attribuzione/interrogazione/rimozione degli hook
# installati per owner e l'unione globale (= mod attive). Header del loader in
# loader/lifecycle/ (include relativo alla radice loader/) e header pubblico
# dello SDK <pulse/hooks.hpp>; la logica e' in hook_ownership.cpp (compilata in
# pulse::loader via glob lifecycle/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_hook_ownership_test hook_ownership_test.cpp)

target_link_libraries(pulse_hook_ownership_test
    PRIVATE
        pulse::loader
        pulse::sdk
        GTest::gtest_main
)

target_include_directories(pulse_hook_ownership_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_hook_ownership_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_hook_ownership_test)

message(STATUS "Pulse: target HookOwnershipLedger test configurato (task 5.5).")

# ---------------------------------------------------------------------------
# Smoke test (example) del seam Module_Loader (task 5.3, Req 11.1, 11.2, 11.4).
# Verifica il contratto strutturale del seam IModuleLoader: factory non nulla +
# semantica di available() per piattaforma, stub non-Apple available()==false,
# contratto load/resolveEntryPoint/unload via FakeModuleLoader (host-testabile)
# e, su Apple, l'estrazione reale su file temporaneo con permessi ristretti
# (0600) + cleanup allo unload, usando una dylib fixture benigna e caricabile.
# Il comportamento dlopen runtime end-to-end è coperto in Fase E. Header del
# loader in loader/lifecycle/ (include relativo alla radice loader/); la logica
# del seam è in module_loader.cpp (compilata in pulse::loader).
# ---------------------------------------------------------------------------

# Fixture: dylib benigna caricabile (libreria MODULE = bundle dlopen-abile) che
# espone un entry point esportato con linkage C. Il suo path è passato allo
# smoke test via compile definition; lo smoke ne legge i byte e li carica.
add_library(pulse_module_loader_smoke_fixture MODULE module_loader_smoke_fixture.cpp)

target_compile_features(pulse_module_loader_smoke_fixture PRIVATE cxx_std_${PULSE_CXX_STANDARD})

add_executable(pulse_module_loader_smoke_test module_loader_smoke_test.cpp)

target_link_libraries(pulse_module_loader_smoke_test
    PRIVATE
        pulse::loader
        pulse::sdk
        GTest::gtest_main
)

target_include_directories(pulse_module_loader_smoke_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_module_loader_smoke_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

# Path della dylib fixture (per la verifica reale di estrazione/perms/cleanup su
# Apple) e dipendenza di build così la fixture esiste prima dell'esecuzione.
target_compile_definitions(pulse_module_loader_smoke_test
    PRIVATE PULSE_MODULE_LOADER_SMOKE_FIXTURE="$<TARGET_FILE:pulse_module_loader_smoke_fixture>"
)
add_dependencies(pulse_module_loader_smoke_test pulse_module_loader_smoke_fixture)

gtest_discover_tests(pulse_module_loader_smoke_test)

message(STATUS "Pulse: target Module_Loader smoke test configurato (task 5.3).")

# ---------------------------------------------------------------------------
# Unit test della discovery della Mods_Directory (task 3.1, Req 1.1-1.5).
# Verifica la scoperta dei candidati Mod_Package su un filesystem virtuale via
# i seam iniettabili DirectoryLister/PackageOpener: solo voci di primo livello
# (no ricorsione), candidato sse `.pulse` apribile, directory assente/vuota ->
# set vuoto + esito registrato, directory non leggibile -> set vuoto +
# diagnostica con dir/causa, voce non `.pulse`/non apribile -> ignorata con
# diagnostica, ordinamento deterministico per entryName. Header del loader in
# loader/lifecycle/ (include relativo alla radice loader/); la logica e' in
# mod_loader.cpp (compilata in pulse::loader via glob lifecycle/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_mod_loader_discovery_test mod_loader_discovery_test.cpp)

target_link_libraries(pulse_mod_loader_discovery_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_mod_loader_discovery_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_mod_loader_discovery_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_mod_loader_discovery_test)

message(STATUS "Pulse: target discovery Mods_Directory test configurato (task 3.1).")

# ---------------------------------------------------------------------------
# Unit test della dedup deterministica per Mod_Id (task 3.2, Req 1.6, 1.7).
# Verifica `dedup_by_mod_id` (funzione componibile applicata all'output di
# `discover_mods`): ogni Mod_Id al più una volta (Req 1.6); in caso di
# collisione vince il Mod_Package il cui `entryName` è il minimo lessicografico
# e gli altri sono esclusi con diagnostica che nomina il Mod_Id duplicato e le
# voci coinvolte (Req 1.7); vincitore indipendente dall'ordine d'ingresso;
# output ordinato per `entryName`; pipeline discover -> dedup end-to-end.
# Header del loader in loader/lifecycle/ (include relativo alla radice loader/);
# la logica è in mod_loader.cpp (compilata in pulse::loader via glob
# lifecycle/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_mod_loader_dedup_test mod_loader_dedup_test.cpp)

target_link_libraries(pulse_mod_loader_dedup_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_mod_loader_dedup_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_mod_loader_dedup_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_mod_loader_dedup_test)

message(STATUS "Pulse: target dedup Mod_Id test configurato (task 3.2).")

# ---------------------------------------------------------------------------
# Property test P4 — discovery solo primo livello e isolamento voci invalide
# (task 3.3, Req 1.1, 1.5). Tag: "Feature: external-mod-loading, Property 4".
# Su alberi di Mods_Directory GENERATI casualmente (mix di voci `.pulse` valide,
# `.pulse` non apribili, voci non `.pulse` e sottocartelle con voci nidificate),
# verifica che l'insieme dei candidati sia esattamente le voci di primo livello
# riconoscibili, che ogni voce non riconoscibile produca una diagnostica e che
# le voci nidificate non vengano mai scoperte (nessuna discesa ricorsiva).
# Header del loader in loader/lifecycle/ (include relativo alla radice loader/);
# la logica è in mod_loader.cpp (compilata in pulse::loader via glob
# lifecycle/*.cpp). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property4_test
    eml_property4_discovery_first_level_isolation_test.cpp)

target_link_libraries(pulse_eml_property4_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property4_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property4_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property4_test)

message(STATUS "Pulse: target external-mod-loading Property 4 test configurato (task 3.3).")

# ---------------------------------------------------------------------------
# Property test P23 — entry point non risolvibile isolato senza invocazione
# (task 5.4, Req 5.3). Tag: "Feature: external-mod-loading, Property 23".
# Su batch di mod GENERATE casualmente (Mod_Id distinti; mix di entry point
# risolvibili e NON esportati dal Mod_Module) verifica, sul seam IModuleLoader
# tramite FakeModuleLoader (host, nessun dlopen), che un entry point non
# risolvibile non venga MAI invocato (contatore di chiamate a zero), produca una
# diagnostica con Mod_Id + nome dell'entry point e segnali la mod non
# caricabile, mentre le mod con entry point risolvibile proseguono e vengono
# invocate esattamente una volta. Header del loader in loader/lifecycle/
# (include relativo alla radice loader/); FakeModuleLoader è header-only.
# Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property23_test
    eml_property23_entrypoint_unresolvable_isolation_test.cpp)

target_link_libraries(pulse_eml_property23_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property23_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property23_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property23_test)

message(STATUS "Pulse: target external-mod-loading Property 23 test configurato (task 5.4).")

# ---------------------------------------------------------------------------
# Property test P3 — dedup deterministico e unicità del Mod_Id scoperto
# (task 3.4, Req 1.6, 1.7). Tag: "Feature: external-mod-loading, Property 3".
# Su insiemi di candidati GENERATI casualmente (entryName distinti, modId da un
# pool ristretto → collisioni frequenti) verifica che ogni Mod_Id compaia al più
# una volta (Req 1.6), che in collisione vinca il minimo lessicografico di
# entryName con diagnostica delle voci escluse (Req 1.7) e che l'esito sia
# deterministico rispetto all'ordine d'ingresso. Header del loader in
# loader/lifecycle/ (include relativo alla radice loader/); la logica è in
# mod_loader.cpp (compilata in pulse::loader). Integrazione RapidCheck+GoogleTest
# in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property3_test
    eml_property3_dedup_mod_id_test.cpp)

target_link_libraries(pulse_eml_property3_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property3_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property3_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property3_test)

message(STATUS "Pulse: target external-mod-loading Property 3 test configurato (task 3.4).")

# ---------------------------------------------------------------------------
# Property test P9 — attribuzione degli hook al Mod_Id proprietario
# (task 5.6, Req 5.8). Tag: "Feature: external-mod-loading, Property 9".
# Su sequenze di caricamenti di mod GENERATE casualmente (Mod_Id distinti,
# numero di hook per mod casuale) verifica che ogni hook effettivamente
# installato sia attribuito esattamente al Mod_Id la cui finestra di epoca lo ha
# registrato e che hooksOf(modId) restituisca esattamente i suoi hook installati
# (nessuna fuga tra owner). Header del loader in loader/lifecycle/ (include
# relativo alla radice loader/) e header pubblico dello SDK <pulse/hooks.hpp>;
# la logica è in hook_ownership.cpp (compilata in pulse::loader via glob
# lifecycle/*.cpp). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property9_test
    eml_property9_hook_ownership_attribution_test.cpp)

target_link_libraries(pulse_eml_property9_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property9_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property9_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property9_test)

message(STATUS "Pulse: target external-mod-loading Property 9 test configurato (task 5.6).")

# ---------------------------------------------------------------------------
# Property test P7 — ordine di caricamento topologico e transitivo
# (task 7.2, Req 4.2). Tag: "Feature: external-mod-loading, Property 7".
# Su grafi di dipendenze ACICLICI generati casualmente verifica che in
# `ResolvedLoadPlan.order` ogni mod compaia DOPO tutte le mod da cui dipende
# direttamente o transitivamente, e che si carichino SOLO le mod presenti in
# `order` (le mod escluse per dipendenza mancante/propagata sono assenti).
# L'oracolo della relazione di dipendenza transitiva e dell'insieme delle mod
# escluse è ricalcolato in modo indipendente da `resolve_load_plan`. Header del
# loader in loader/lifecycle/ (include relativo alla radice loader/); la logica
# è in mod_loader.cpp (compilata in pulse::loader). Integrazione
# RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property7_test
    eml_property7_topological_transitive_load_order_test.cpp)

target_link_libraries(pulse_eml_property7_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property7_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property7_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property7_test)

message(STATUS "Pulse: target external-mod-loading Property 7 test configurato (task 7.2).")

# ---------------------------------------------------------------------------
# Property test P8 — esclusione e propagazione delle dipendenze
# (task 7.3, Req 4.3, 4.4, 4.5, 4.6). Tag: "Feature: external-mod-loading,
# Property 8". Su grafi di dipendenze generati casualmente — con dipendenze
# mancanti, vincoli di versione incompatibili, cicli (anche self-loop) ed
# esclusioni transitive — verifica che `resolve_load_plan` escluda ogni mod con
# la categoria di causa corretta (DependencyUnsatisfied per mancante/
# incompatibile/transitiva, DependencyCycle per i membri del ciclo) e faccia
# proseguire in `order` tutte le mod restanti caricabili, ognuna dopo le sue
# dipendenze. L'oracolo della partizione {caricata|esclusa+causa} è ricalcolato
# in modo INDIPENDENTE da `resolve_load_plan` (e dal DependencyResolver). Header
# del loader in loader/lifecycle/ (include relativo alla radice loader/); la
# logica è in mod_loader.cpp (compilata in pulse::loader). Integrazione
# RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property8_test
    eml_property8_dependency_exclusion_propagation_test.cpp)

target_link_libraries(pulse_eml_property8_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property8_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property8_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property8_test)

message(STATUS "Pulse: target external-mod-loading Property 8 test configurato (task 7.3).")

# ---------------------------------------------------------------------------
# Property test P11 — zero hook su indirizzi non risolti, con diagnostica
# attribuita (task 7.6, Req 5.7, 9.1, 9.4). Tag: "Feature: external-mod-loading,
# Property 11". Con `FakeModuleLoader` + `FakeBackend` (che modella i byte delle
# funzioni bersaglio) e un `SymbolResolver` che marca un mix casuale di
# `PULSE_HOOK` come risolti/non risolti, verifica che il cablaggio del
# ModManager installi ZERO hook sugli indirizzi non risolti, attribuisca al
# Mod_Id i soli hook risolti e produca per ogni `PULSE_HOOK` non risolto una
# diagnostica che identifica Mod_Id + simbolo. L'oracolo della partizione
# {risolti installati|non risolti diagnosticati} è ricalcolato in modo
# INDIPENDENTE dal cablaggio. Header del loader in loader/lifecycle/ (include
# relativo alla radice loader/); la logica è in mod_loader.cpp (compilata in
# pulse::loader). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property11_test
    eml_property11_unresolved_zero_hooks_diagnostic_test.cpp)

target_link_libraries(pulse_eml_property11_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property11_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property11_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property11_test)

message(STATUS "Pulse: target external-mod-loading Property 11 test configurato (task 7.6).")

# ---------------------------------------------------------------------------
# Property test P14 — fallimento di init/re-init → Disabled con rollback
# byte-esatto (task 7.10, Requisiti 6.2, 7.6).
#
# Property 14: per OGNI mod il cui entry point fallisce (errore o eccezione) sia
# all'enable sia al re-enable, il Mod_Manager la porta a Disabled, gli eventuali
# hook installati nel tentativo sono rimossi byte-esatto via Rollback_Store e le
# altre mod valide proseguono Enabled invariate. Modello host-testable su
# FakeModuleLoader + FakeBackend (byte delle funzioni bersaglio) + RollbackStore;
# l'oracolo dei byte originali e degli indirizzi per mod è ricalcolato in modo
# INDIPENDENTE dal cablaggio. Header del loader in loader/lifecycle/ (include
# relativo alla radice loader/); la logica è in mod_loader.cpp (compilata in
# pulse::loader). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property14_test
    eml_property14_init_reinit_failure_rollback_test.cpp)

target_link_libraries(pulse_eml_property14_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property14_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property14_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property14_test)

message(STATUS "Pulse: target external-mod-loading Property 14 test configurato (task 7.10).")

# ---------------------------------------------------------------------------
# External Mod Loading — Property 16 (task 7.12): round-trip
# enable→disable→enable. La sequenza enable → disable → enable riporta l'insieme
# degli hook installati di proprietà del mod ESATTAMENTE all'insieme dei suoi
# hook con binding risolto e lascia la mod Enabled; il disable rimuove tutti gli
# hook (zero mentre Disabled) byte-esatto via RollbackStore e il re-enable riusa
# la finestra di epoca memorizzata SENZA un nuovo dlopen. Modello host-testable
# su FakeModuleLoader + FakeBackend (byte delle funzioni bersaglio) +
# RollbackStore; l'oracolo dell'insieme degli indirizzi risolti è ricalcolato in
# modo INDIPENDENTE dal cablaggio. Header del loader in loader/lifecycle/
# (include relativo alla radice loader/); la logica è in mod_loader.cpp
# (compilata in pulse::loader). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property16_test
    eml_property16_enable_disable_enable_roundtrip_test.cpp)

target_link_libraries(pulse_eml_property16_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property16_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property16_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property16_test)

message(STATUS "Pulse: target external-mod-loading Property 16 test configurato (task 7.12).")

# ---------------------------------------------------------------------------
# external-mod-loading Property 15 (task 7.16): round-trip install→remove
# byte-esatto per mod. PER OGNI mod caricata, install seguito da remove dei suoi
# hook riporta i byte di tutte le funzioni bersaglio identici allo stato
# pre-installazione. Modello host-testable su FakeModuleLoader + FakeBackend
# (modella seedOriginal/install/remove/liveBytes dei byte delle funzioni
# bersaglio) + RollbackStore; lo snapshot pre-install è l'oracolo INDIPENDENTE
# dal cablaggio. Header del loader in loader/lifecycle/ (include relativo alla
# radice loader/); la logica è in mod_loader.cpp (compilata in pulse::loader).
# Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property15_test
    eml_property15_install_remove_byte_exact_roundtrip_test.cpp)

target_link_libraries(pulse_eml_property15_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property15_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property15_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property15_test)

message(STATUS "Pulse: target external-mod-loading Property 15 test configurato (task 7.16).")

# ---------------------------------------------------------------------------
# External Mod Loading — Property 17 (task 7.13): invarianza su transizioni non
# ammesse. Per OGNI mod e OGNI transizione di stato NON ammessa dalla state
# machine del ModManager, la transizione è rifiutata lasciando INVARIATI lo
# stato corrente e gli hook installati di proprietà della mod, e produce una
# diagnostica (Req 7.4). Modello host-testable su FakeModuleLoader + FakeBackend
# (byte delle funzioni bersaglio) + RollbackStore + cablaggio reale
# ModManagerWiring: la mod è guidata nei quattro stati raggiungibili (incluso
# Enabled con i suoi hook realmente installati) e, per ogni bersaglio non
# ammesso, si verifica l'invarianza bit-per-bit di stato e hook + la
# segnalazione. Header del loader in loader/lifecycle/ (include relativo alla
# radice loader/); la logica è in mod_loader.cpp (compilata in pulse::loader).
# Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property17_test
    eml_property17_disallowed_transition_invariance_test.cpp)

target_link_libraries(pulse_eml_property17_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property17_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property17_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property17_test)

message(STATUS "Pulse: target external-mod-loading Property 17 test configurato (task 7.13).")

# ---------------------------------------------------------------------------
# External Mod Loading — Property 18 (task 7.15): rollback transazionale su
# install parziale fallita. Per OGNI mod e OGNI punto di fallimento iniettato
# DOPO che almeno un suo hook è già stato installato, gli hook già installati di
# quella mod sono rimossi byte-esatto via RollbackStore, il fallimento è
# confinato al solo Mod_Id (la mod termina Disabled con zero hook) e le mod
# restanti proseguono Enabled con i loro hook (Req 9.5). Modello host-testable su
# FakeModuleLoader + FakeBackend (byte delle funzioni bersaglio, fallimento di
# install iniettato via failInstallAt sul k-esimo hook con k≥1) + RollbackStore;
# l'oracolo dei byte originali e degli indirizzi per mod è ricalcolato in modo
# INDIPENDENTE dal cablaggio. Header del loader in loader/lifecycle/ (include
# relativo alla radice loader/); la logica è in mod_loader.cpp (compilata in
# pulse::loader). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property18_test
    eml_property18_partial_install_rollback_test.cpp)

target_link_libraries(pulse_eml_property18_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property18_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property18_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property18_test)

message(STATUS "Pulse: target external-mod-loading Property 18 test configurato (task 7.15).")

# ---------------------------------------------------------------------------
# External Mod Loading — Property 21 (task 7.20): partizione completa degli
# esiti osservabili. Tag: "Feature: external-mod-loading, Property 21".
# Su insiemi randomizzati di Mod_Id individuati, a ciascuno assegnato un esito
# casuale (caricata / esclusa(causa) / isolata(causa)) e registrato sul
# DiagnosticLedger (RapidCheck, ≥100 iterazioni di default) verifica:
#   (1) COMPLETEZZA — l'unione di {loaded, excluded, isolated} coincide con le
#       mod individuate (partitionComplete()/missingFrom() vuoto);
#   (2) DISGIUNZIONE — i tre esiti sono a due a due disgiunti, ogni mod in
#       ESATTAMENTE uno; ogni esclusa/isolata porta esattamente una causa;
#   (3) ogni Mod_Id caricato compare UNA SOLA volta in loaded() (Req 10.1);
#   (4) REJECTION del doppio record — ritentare un esito diverso su un Mod_Id
#       già registrato è rifiutato e la mod resta nel suo PRIMO esito.
# Il DiagnosticLedger è un registro PURO (mod_loader.cpp, compilato in
# pulse::loader); header in loader/lifecycle/ (include relativo alla radice
# loader/). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property21_test
    eml_property21_outcome_partition_test.cpp)

target_link_libraries(pulse_eml_property21_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property21_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property21_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property21_test)

message(STATUS "Pulse: target external-mod-loading Property 21 test configurato (task 7.20).")

# ---------------------------------------------------------------------------
# Property test P1 — round-trip del Mod_Manifest con compatibilità GD
# (task 1.3, Req 3.5). Tag: "Feature: external-mod-loading, Property 1".
# Su Mod_Manifest GENERATI casualmente (validi per costruzione: id non vuoto,
# ≥1 entry point, SemVer, dipendenze, permessi, settings con default coerente
# al tipo) ESTESI con la sezione `[compat]` randomizzata (platform dall'insieme
# finito assente/presente; intervallo `[min, max]` di versione di GD con
# min<=max assente/presente) verifica (RapidCheck, ≥100 iterazioni di default):
#   (a) parse(serialize(m)).ok && parse(serialize(m)) == m campo-per-campo,
#       inclusi compat.platform e compat.gdVersion;
#   (b) serialize(parse(serialize(m))) == serialize(m) (punto fisso della forma
#       canonica, compresa la tabella `[compat]`).
# Header-only: include `lifecycle/manifest.hpp` (radice loader/ nella include
# path), consumato senza modifiche. Integrazione RapidCheck+GoogleTest in
# extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_property1_mod_manifest_round_trip_compat_test
    property1_mod_manifest_round_trip_compat_test.cpp)

target_link_libraries(pulse_property1_mod_manifest_round_trip_compat_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_property1_mod_manifest_round_trip_compat_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_property1_mod_manifest_round_trip_compat_test
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_property1_mod_manifest_round_trip_compat_test)

message(STATUS "Pulse: target Property 1 (round-trip Mod_Manifest + compat) configurato (task 1.3).")

# ---------------------------------------------------------------------------
# Unit test del cablaggio del DependencyResolver nel Mod_Loader
# (task 7.1, Req 4.1-4.7). Verifica `resolve_load_plan`: proiezione dei
# Mod_Manifest compatibili in ResolverManifest via toResolverManifest(),
# invocazione del DependencyResolver e traduzione dell'esito in
# ResolvedLoadPlan{order, excluded}: carica SOLO `order` nell'ordine topologico
# (Req 4.1, 4.2); ogni mod esclusa (dipendenza mancante/incompatibile, ciclo,
# esclusione transitiva) diventa una DiagnosticEntry con la CauseCategory
# corretta dell'insieme chiuso e il messaggio con la causa del resolver
# (Req 4.3/4.4/4.5/4.6); `order` vuoto -> zero mod + esito registrato (Req 4.7).
# Header del loader in loader/lifecycle/ (include relativo alla radice loader/);
# la logica e' in mod_loader.cpp (compilata in pulse::loader via glob
# lifecycle/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_mod_loader_resolve_test mod_loader_resolve_test.cpp)

target_link_libraries(pulse_mod_loader_resolve_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_mod_loader_resolve_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_mod_loader_resolve_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_mod_loader_resolve_test)

message(STATUS "Pulse: target Mod_Loader resolve (DependencyResolver) test configurato (task 7.1).")

# ---------------------------------------------------------------------------
# Mod_Loader wiring (task 7.4): cablaggio del ModManager con EntryPointFn/
# TerminatorFn e install attribuito via finestre di epoca. Verifica che l'entry
# point sia invocato esattamente una volta all'enable (Req 5.4), che resolve_all
# + HookGate installino i soli hook risolti della finestra persistendo i byte
# originali con owner=Mod_Id prima dell'install (Req 5.5/5.6/8.3/9.5),
# attribuendoli al Mod_Id (Req 5.8) e cablando il trampolino; nessun install su
# indirizzi non risolti con diagnostica attribuita (Req 5.7/9.1/9.4); il
# terminator rimuove i soli hook del Mod_Id (Req 7.3). Seam host-testabili
# (FakeModuleLoader, FakeBackend, resolver/invoker iniettati). Header del loader
# in loader/lifecycle/ e header pubblico dello SDK <pulse/hooks.hpp>; la logica
# e' in mod_loader.cpp (compilata in pulse::loader via glob lifecycle/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_mod_loader_wiring_test mod_loader_wiring_test.cpp)

target_link_libraries(pulse_mod_loader_wiring_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_mod_loader_wiring_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_mod_loader_wiring_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_mod_loader_wiring_test)

message(STATUS "Pulse: target Mod_Loader wiring (ModManager) test configurato (task 7.4).")

# ---------------------------------------------------------------------------
# Coesistenza demo + mod esterna sullo stesso Target_Address via HookChainRegistry
# cablata nel ModManagerWiring (Hook_Chaining, task 7.5 — fix di integrazione,
# Req 8.1, 8.2, 8.4). Verifica che, con un HookChainRegistry condiviso cablato
# nel cablaggio, una mod esterna che hooka la STESSA funzione della demo diventi
# un SECONDO anello della medesima catena sull'UNICA Underlying_Installation
# (una sola install nel backend, nessuna seconda DobbyHook), e che il teardown
# relinki alla rimozione non-ultima e disinstalli byte-esatto sull'ultima.
# Seam host (FakeModuleLoader, FakeBackend, RollbackStore, ledger). La logica è
# in mod_loader.cpp + hook_chain_registry.cpp (compilate in pulse::loader).
# ---------------------------------------------------------------------------
add_executable(pulse_mod_loader_chain_coexistence_test
    mod_loader_chain_coexistence_test.cpp)

target_link_libraries(pulse_mod_loader_chain_coexistence_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_mod_loader_chain_coexistence_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_mod_loader_chain_coexistence_test
    PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_mod_loader_chain_coexistence_test)

message(STATUS "Pulse: target Mod_Loader chain coexistence test configurato (task 7.5 fix).")

# ---------------------------------------------------------------------------
# Mod_Loader fail-open (task 7.7): isolamento fail-open per-mod e barriera
# no-throw. Verifica `ModManagerWiring::runNoThrow` e la barriera no-throw di
# `onEnable`: modulo non caricabile → fallimento isolato, 0 hook del mod, le
# altre proseguono (Req 6.1/6.6); entry point in errore → Disabled, 0 hook del
# mod, rollback byte-esatto (Req 6.2); entry point che lancia / eccezione in uno
# stadio per-mod → nessuna eccezione propagata, mod isolata, le altre proseguono
# (Req 6.3/6.4); più fallimenti → tutte le mod valide restanti abilitate
# indipendentemente dal numero di fallimenti, hook delle valide invariati
# (Req 6.5/6.6). Seam host-testabili (FakeModuleLoader, FakeBackend,
# resolver/invoker iniettati). La logica e' in mod_loader.cpp (compilata in
# pulse::loader via glob lifecycle/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_mod_loader_fail_open_test mod_loader_fail_open_test.cpp)

target_link_libraries(pulse_mod_loader_fail_open_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_mod_loader_fail_open_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_mod_loader_fail_open_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_mod_loader_fail_open_test)

message(STATUS "Pulse: target Mod_Loader fail-open (no-throw barrier) test configurato (task 7.7).")

# ---------------------------------------------------------------------------
# Mod_Loader fail-open su Module_Loader/backend non disponibile (task 7.23,
# Requisiti 9.1, 9.2, 9.3, 11.5). Module_Loader available()==false → zero mod,
# zero install, diagnostica con la piattaforma del Runtime_Context (byte
# invariati); backend available()==false → HookGate non installa alcun hook +
# diagnostica che nomina il backend. Seam host-testabili (FakeModuleLoader con
# setAvailable, UnavailableBackend locale). La logica e' in mod_loader.cpp
# (compilata in pulse::loader via glob lifecycle/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_mod_loader_module_unavailable_test mod_loader_module_unavailable_test.cpp)

target_link_libraries(pulse_mod_loader_module_unavailable_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_mod_loader_module_unavailable_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_mod_loader_module_unavailable_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_mod_loader_module_unavailable_test)

message(STATUS "Pulse: target Mod_Loader Module_Loader/backend unavailable fail-open test configurato (task 7.23).")

# ---------------------------------------------------------------------------
# Mod_Loader lifecycle (task 7.11): enable/disable/re-enable e transizioni di
# stato. Verifica `ModManagerWiring::enable`/`disable` sopra la state machine
# del `ModManager`: enable → Enabled con entry point invocato (Req 7.1); disable
# → rimozione byte-esatto di TUTTI gli hook del mod via RollbackStore/backend,
# ZERO hook del mod mentre Disabled, Mod_Module conservato e non scaricato
# (Req 7.2/7.3/7.7); re-enable → re-invoca l'entry point, ri-esegue resolve_all
# e reinstalla i soli hook risolti riusando gli indici della finestra
# memorizzata SENZA nuovo dlopen (Req 7.5); round-trip enable→disable→enable
# ripristina esattamente l'insieme degli hook risolti del mod; transizione non
# ammessa → rifiutata mantenendo stato e hook + diagnostica (Req 7.4). Seam
# host-testabili (FakeModuleLoader, FakeBackend, resolver/invoker iniettati). La
# logica e' in mod_loader.cpp (compilata in pulse::loader via glob lifecycle/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_mod_loader_lifecycle_test mod_loader_lifecycle_test.cpp)

target_link_libraries(pulse_mod_loader_lifecycle_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_mod_loader_lifecycle_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_mod_loader_lifecycle_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_mod_loader_lifecycle_test)

message(STATUS "Pulse: target Mod_Loader lifecycle (enable/disable/re-enable) test configurato (task 7.11).")

# ---------------------------------------------------------------------------
# Mod_Loader teardown test (task 7.17, Requisiti 8.1, 8.2, 8.3, 8.4, 8.5, 8.6,
# 8.7, 8.8). Verifica `ModManagerWiring::teardown(manager, order)` sopra
# `ModManager::shutdown`: terminator di ogni mod Enabled invocato esattamente
# una volta in ordine INVERSO rispetto a `order` (Req 8.1); rimozione di tutti
# gli hook di ogni Mod_Id byte-esatto via RollbackStore/backend (Req 8.2/8.3/8.4);
# fallimento del terminator isolato con hook comunque rimossi e proseguimento
# (Req 8.5); a hook rimossi unload del Mod_Module (Req 8.7) e mod a Removed
# (Req 8.8); al termine zero hook attribuiti installati (Req 8.6). Seam host-
# testabili (FakeModuleLoader, FakeBackend, resolver/invoker iniettati). La
# logica e' in mod_loader.cpp (compilata in pulse::loader via glob lifecycle/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_mod_loader_teardown_test mod_loader_teardown_test.cpp)

target_link_libraries(pulse_mod_loader_teardown_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_mod_loader_teardown_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_mod_loader_teardown_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_mod_loader_teardown_test)

message(STATUS "Pulse: target Mod_Loader teardown (ordine inverso/unload/Removed) test configurato (task 7.17).")

# ---------------------------------------------------------------------------
# Test: rollback transazionale su install parziale fallita (task 7.14, Req 9.5).
# Se l'install degli hook di una mod fallisce dopo che almeno un suo hook e' gia'
# installato, gli hook gia' installati di quella mod sono rimossi byte-esatto via
# RollbackStore/backend, il fallimento e' confinato al solo Mod_Id (mod Disabled,
# zero hook) e le mod restanti proseguono. Seam host-testabili (FakeModuleLoader,
# FakeBackend con failInstallAt, resolver/invoker iniettati). La logica e' in
# mod_loader.cpp (compilata in pulse::loader via glob lifecycle/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_mod_loader_partial_install_rollback_test
    mod_loader_partial_install_rollback_test.cpp)

target_link_libraries(pulse_mod_loader_partial_install_rollback_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_mod_loader_partial_install_rollback_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_mod_loader_partial_install_rollback_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_mod_loader_partial_install_rollback_test)

message(STATUS "Pulse: target Mod_Loader rollback transazionale install parziale test configurato (task 7.14).")

# ---------------------------------------------------------------------------
# Property test P10 — entry point invocato esattamente una volta all'enable
# (task 7.5, Req 5.4, 7.1). Tag: "Feature: external-mod-loading, Property 10".
# Su batch di mod GENERATI casualmente (Enabled / EntryFail / Unresolvable)
# abilitati via il cablaggio reale `ModManagerWiring` + `ModManager::enableAll`
# verifica (RapidCheck, >=100 iterazioni di default) che ogni mod abilitata con
# successo raggiunga lo stato Enabled con `entryPointInvocations == 1` (Req 5.4,
# 7.1), che ri-richiedere `enable` su una mod gia' Enabled sia rifiutato dalla
# state machine senza re-invocare l'entry point (esattamente una volta TRAMITE
# la transizione verso Enabled), e che l'insieme delle mod Enabled e il numero
# di invocazioni per mod coincidano con un oracolo indipendente. Seam
# host-testabili (FakeModuleLoader, FakeBackend, resolver/invoker iniettati).
# Header del loader in loader/lifecycle/ (include relativo alla radice loader/)
# e header pubblico dello SDK <pulse/hooks.hpp>; la logica e' in mod_loader.cpp/
# mod_manager.cpp/hook_ownership.cpp (compilate in pulse::loader via glob
# lifecycle/*.cpp). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property10_test
    eml_property10_entrypoint_invoked_once_test.cpp)

target_link_libraries(pulse_eml_property10_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property10_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property10_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property10_test)

message(STATUS "Pulse: target external-mod-loading Property 10 test configurato (task 7.5).")

# ---------------------------------------------------------------------------
# Property test P12 — isolamento fail-open senza propagazione di eccezioni
# (task 7.8, Req 6.1, 6.3, 6.4, 6.6). Tag: "Feature: external-mod-loading,
# Property 12". Su batch di mod GENERATI casualmente che mescolano mod valide e
# mod che falliscono a stadi iniettati diversi (caricamento del modulo, entry
# point in errore/eccezione, eccezione nel resolver/resolve_all) in numero
# arbitrario, abilitati via la barriera no-throw `ModManagerWiring::runNoThrow`,
# verifica (RapidCheck, >=100 iterazioni di default) che: nessuna eccezione sia
# propagata (runNoThrow e' noexcept), tutte e sole le mod valide raggiungano
# Enabled con i loro hook installati indipendentemente dal numero di fallimenti
# (Req 6.1, 6.6), ogni mod fallita sia isolata (Disabled) con 0 hook e produca
# una diagnostica che ne nomina il Mod_Id (Req 6.4), e gli hook installati nel
# backend siano esattamente l'unione degli hook delle mod valide (Req 6.6).
# Seam host-testabili (FakeModuleLoader, FakeBackend, resolver/invoker
# iniettati). Header del loader in loader/lifecycle/ (include relativo alla
# radice loader/) e header pubblico dello SDK <pulse/hooks.hpp>; la logica e' in
# mod_loader.cpp/mod_manager.cpp/hook_ownership.cpp (compilate in pulse::loader
# via glob lifecycle/*.cpp). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property12_test
    eml_property12_fail_open_isolation_test.cpp)

target_link_libraries(pulse_eml_property12_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property12_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property12_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property12_test)

message(STATUS "Pulse: target external-mod-loading Property 12 test configurato (task 7.8).")

# ---------------------------------------------------------------------------
# Property test P13 — non-interferenza tra mod isolate (task 7.9, Req 6.5).
# Tag: "Feature: external-mod-loading, Property 13". Proprieta' METAMORFICA: si
# esegue lo stesso batch in due scenari (MIXED = mod valide + mod fallite;
# VALID-ONLY = solo le mod valide, stesso ordine) via il cablaggio reale
# `ModManagerWiring::runNoThrow` + `ModManager`, e si asserisce (RapidCheck,
# >=100 iterazioni di default) che l'insieme degli hook installati attribuiti
# alle mod valide (catturato dal `HookOwnershipLedger`) sia IDENTICO nei due
# scenari, mod per mod e globalmente: il fallimento isolato (modulo non
# caricabile, entry point in errore, entry point che lancia, entry point che
# registra-poi-lancia) non aggiunge ne' rimuove hook altrui (Req 6.5). Seam
# host-testabili (FakeModuleLoader, FakeBackend, resolver/invoker iniettati).
# Header del loader in loader/lifecycle/ (include relativo alla radice loader/)
# e header pubblico dello SDK <pulse/hooks.hpp>; la logica e' in mod_loader.cpp/
# mod_manager.cpp/hook_ownership.cpp (compilate in pulse::loader via glob
# lifecycle/*.cpp). Integrazione RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property13_test
    eml_property13_isolated_non_interference_test.cpp)

target_link_libraries(pulse_eml_property13_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property13_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property13_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property13_test)

message(STATUS "Pulse: target external-mod-loading Property 13 test configurato (task 7.9).")

# ---------------------------------------------------------------------------
# Unit test del DiagnosticLedger (task 7.19, Req 10.1, 10.2, 10.3, 10.4).
# Verifica la partizione completa a tre vie {Loaded, Excluded, Isolated} (ogni
# mod individuata in esattamente uno, Mod_Id caricato una sola volta), l'insieme
# CHIUSO di cause per Excluded/Isolated e gli eventi hook install/remove
# (Mod_Id, simbolo, operazione), anche cablati nei percorsi install/remove di
# ModManagerWiring (FakeBackend/FakeModuleLoader). Header del loader in loader/
# (include PRIVATE): aggiunge loader/ alla include path; la logica e' in
# mod_loader.cpp (compilata in pulse::loader via glob lifecycle/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_diagnostic_ledger_test diagnostic_ledger_test.cpp)

target_link_libraries(pulse_diagnostic_ledger_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_diagnostic_ledger_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_diagnostic_ledger_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_diagnostic_ledger_test)

message(STATUS "Pulse: target DiagnosticLedger test configurato (task 7.19).")

# ---------------------------------------------------------------------------
# Property 22 (task 7.21) — Diagnostica con causa appartenente all'insieme
# CHIUSO (Feature: external-mod-loading, Property 22; Validates: Requisiti
# 10.2, 10.3). Verifica che ogni mod ESCLUSA/ISOLATA registri Mod_Id, esito
# (Excluded vs Isolated) ed ESATTAMENTE una CauseCategory dell'insieme chiuso,
# e che ogni evento hook identifichi Mod_Id, simbolo e tipo operazione
# (Install | Remove). Header del loader in loader/lifecycle/ (include PRIVATE
# della radice loader/); la logica e' in mod_loader.cpp (compilata in
# pulse::loader via glob lifecycle/*.cpp). Integrazione RapidCheck+GoogleTest
# in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property22_test
    eml_property22_diagnostic_closed_set_cause_test.cpp)

target_link_libraries(pulse_eml_property22_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property22_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property22_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property22_test)

message(STATUS "Pulse: target external-mod-loading Property 22 test configurato (task 7.21).")

# ---------------------------------------------------------------------------
# Property 24 — Fail-open su Module_Loader non disponibile, byte invariati
# (Feature: external-mod-loading, Property 24; Validates: Requisiti 9.3, 11.5).
# Verifica, tramite FakeModuleLoader (host, nessun dlopen) e FakeBackend (byte
# modellati delle funzioni bersaglio), che con `available()==false` il
# Mod_Loader carichi ZERO mod, registri una diagnostica che nomina la
# piattaforma del Runtime_Context e il loader e lasci i byte byte-per-byte
# INVARIATI (zero install), e che la stessa invarianza valga quando il loader e'
# disponibile ma nessuna mod termina Enabled. Header del loader in
# loader/lifecycle/ (include PRIVATE della radice loader/); la logica fail-open
# e' in mod_loader.cpp (compilata in pulse::loader). Integrazione
# RapidCheck+GoogleTest in extras/gtest.
# ---------------------------------------------------------------------------
add_executable(pulse_eml_property24_test
    eml_property24_module_loader_unavailable_byte_invariance_test.cpp)

target_link_libraries(pulse_eml_property24_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
        rapidcheck
)

target_include_directories(pulse_eml_property24_test
    PRIVATE
        ${rapidcheck_SOURCE_DIR}/extras/gtest/include
        ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_eml_property24_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_eml_property24_test)

message(STATUS "Pulse: target external-mod-loading Property 24 test configurato (task 7.24).")

# ---------------------------------------------------------------------------
# Unit test dell'orchestratore Mod_Loader (task 7.22, Requisiti 1.1, 6.3, 9.6).
# Verifica la pipeline host-testabile end-to-end del `ModLoader` (discovery →
# dedup → validazione → compatibilità → risoluzione dipendenze → cablaggio ed
# enable via ModManagerWiring), l'adattatore `asInitStep` verso il
# CentralizedLoader (RuntimeInitFn che riporta installedHooks e non lancia mai),
# il seeding della demo interna sul Mod_Id riservato del ledger, il teardown
# byte-esatto e il fail-open su Module_Loader non disponibile. Seam host
# (PackageOpener su PackageArchive in memoria, FakeModuleLoader, FakeBackend,
# resolver/invoker iniettati). Header del loader in loader/lifecycle/ (include
# PRIVATE della radice loader/); la logica è in mod_loader.cpp (compilata in
# pulse::loader via glob lifecycle/*.cpp).
# ---------------------------------------------------------------------------
add_executable(pulse_mod_loader_orchestrator_test
    mod_loader_orchestrator_test.cpp)

target_link_libraries(pulse_mod_loader_orchestrator_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_mod_loader_orchestrator_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_mod_loader_orchestrator_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_mod_loader_orchestrator_test)

message(STATUS "Pulse: target Mod_Loader orchestrator test configurato (task 7.22).")

# ---------------------------------------------------------------------------
# Lettore `.pulse` (ZIP STORED) su disco + riconciliazione del binario nativo
# (Fase E — abilitazione del caricamento delle mod esterne).
# Verifica `read_pulse_archive`/`open_pulse_file` (parser ZIP central-directory,
# solo STORED, fail-open su Deflate/buffer malformati) costruendo ZIP a mano
# come buffer di byte (nessuna dipendenza da cargo/toolchain), e che il
# `ModManifestValidator` accetti una mod nativa con sola `code/*.dylib` e
# rifiuti una mod nativa priva di qualunque binario nativo. Header del loader in
# loader/ (include PRIVATE): il test aggiunge loader/ alla include path; la
# logica è in package/pulse_zip_reader.cpp (compilata in pulse::loader).
# ---------------------------------------------------------------------------
add_executable(pulse_zip_reader_test pulse_zip_reader_test.cpp)

target_link_libraries(pulse_zip_reader_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_zip_reader_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_zip_reader_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_zip_reader_test)

message(STATUS "Pulse: target PulseZipReader test configurato (Fase E).")

# ---------------------------------------------------------------------------
# Mod registrar — unit test (external-mod-loading).
# Verifica `pulse::loader::register_external_hook`: il Loader possiede una copia
# stabile della stringa del simbolo (HookRegistration.symbol è una view non
# proprietaria) anche se la registrazione parte da temporanei. Questo eseguibile
# NON definisce `pulse_loader_register_hook`, quindi register_hook usa il path
# locale. Header del core in loader/ (include "core/..."): aggiunge loader/ alla
# include path; register_external_hook è in core/mod_registrar.cpp (in
# pulse::loader).
# ---------------------------------------------------------------------------
add_executable(pulse_mod_registrar_test mod_registrar_test.cpp)

target_link_libraries(pulse_mod_registrar_test
    PRIVATE
        pulse::loader
        GTest::gtest_main
)

target_include_directories(pulse_mod_registrar_test
    PRIVATE ${PROJECT_SOURCE_DIR}/loader
)

target_compile_features(pulse_mod_registrar_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

gtest_discover_tests(pulse_mod_registrar_test)

message(STATUS "Pulse: target mod registrar test configurato (external-mod-loading).")

# ---------------------------------------------------------------------------
# Delega adattiva dello SDK — unit test (external-mod-loading).
# Verifica che `pulse::hooks::register_hook` deleghi a `pulse_loader_register_hook`
# (risolto via dlsym(RTLD_DEFAULT,...)) quando il processo lo espone, senza far
# crescere il registro locale. L'eseguibile DEFINISCE esso stesso l'entry point
# simulato; per questo è DEDICATO e linka solo l'header-only pulse::sdk (definire
# l'entry point farebbe delegare ogni register_hook del binario, quindi va tenuto
# isolato dal path locale del registrar test).
# ---------------------------------------------------------------------------
add_executable(pulse_sdk_hook_delegation_test sdk_hook_delegation_test.cpp)

target_link_libraries(pulse_sdk_hook_delegation_test
    PRIVATE
        pulse::sdk
        GTest::gtest_main
)

target_compile_features(pulse_sdk_hook_delegation_test PRIVATE cxx_std_${PULSE_CXX_STANDARD})

# Su macOS/Linux assicura che il simbolo definito nell'eseguibile sia nella
# tabella dinamica così dlsym(RTLD_DEFAULT,...) lo trovi.
if(APPLE)
    target_link_options(pulse_sdk_hook_delegation_test PRIVATE
        "LINKER:-export_dynamic")
elseif(UNIX)
    target_link_options(pulse_sdk_hook_delegation_test PRIVATE
        "LINKER:--export-dynamic")
endif()

gtest_discover_tests(pulse_sdk_hook_delegation_test)

message(STATUS "Pulse: target SDK hook delegation test configurato (external-mod-loading).")
