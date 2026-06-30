// tests/module_loader_smoke_fixture.cpp — dylib fittizia, benigna e caricabile,
// usata SOLO dallo smoke test del seam Module_Loader (External Mod Loading,
// task 5.3). Viene costruita come libreria MODULE (bundle caricabile via
// dlopen) ed espone un unico simbolo con linkage C che lo smoke test risolve
// tramite `IModuleLoader::resolveEntryPoint` dopo una vera estrazione + dlopen
// su macOS. Nessuna logica reale: serve solo a fornire un Mod_Module caricabile
// e un entry point esportato noto.
//
// Logica originale Pulse (Requisito 27). Stack: C++20/23 (Requisito 26.1).

// Entry point esportato (linkage C → nome simbolo stabile, senza name mangling)
// che lo smoke test risolve dal modulo caricato. Il valore di ritorno è
// irrilevante: il test verifica solo che il simbolo sia risolvibile.
extern "C" int pulse_module_loader_smoke_entry(void) { return 4242; }
