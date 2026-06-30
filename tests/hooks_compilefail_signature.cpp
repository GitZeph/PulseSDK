// hooks_compilefail_signature.cpp — snippet di NON-compilazione (task 17.2,
// Requisito 5.2).
//
// Questo file DEVE FALLIRE la compilazione. È compilato da un target CMake
// `EXCLUDE_FROM_ALL` e un test CTest con proprietà `WILL_FAIL TRUE` ne asserisce
// il fallimento (vedi tests/CMakeLists.txt, target
// `pulse_hook_compilefail_signature`).
//
// Req 5.2: dichiarare un hook la cui firma (tipo di ritorno, numero o tipi dei
// parametri) è INCOMPATIBILE con quella esposta dal binding del simbolo deve
// impedire il completamento della compilazione con un messaggio diagnostico.
//
// La macro PULSE_HOOK emette
//   static_assert(SignatureMatches<FixedString(#Symbol), PulseFn>, ...)
// dove `PulseFn = ReturnType Params`. Se BindingTraits<Symbol>::Fn esiste e
// NON coincide con PulseFn, il concept `SignatureMatches` è falso e lo
// static_assert FALLISCE la compilazione.
#include <pulse/hooks.hpp>

namespace {
struct SomeType {
    int value{0};
};
}  // namespace

namespace pulse::hooks {
// Il layer dei bindings espone la firma canonica del bersaglio per il simbolo
// "CompileFail_target": bool(SomeType*).
template <>
struct BindingTraits<FixedString("CompileFail_target")> {
    using Fn = bool(SomeType*);
};
}  // namespace pulse::hooks

// ERRORE DI COMPILAZIONE ATTESO (Req 5.2): la firma dichiarata è
// `int(SomeType*)` (tipo di ritorno diverso) mentre il binding richiede
// `bool(SomeType*)`. Lo static_assert di SignatureMatches dentro PULSE_HOOK
// rende la compilazione impossibile con diagnostica.
PULSE_HOOK(CompileFail_target, int, (SomeType* self)) {
    if (self != nullptr) {
        self->value += 1;
    }
    return self != nullptr ? self->value : 0;
}
