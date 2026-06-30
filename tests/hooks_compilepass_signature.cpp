// hooks_compilepass_signature.cpp — controparte POSITIVA (task 17.2,
// Requisito 5.2).
//
// Questo file DEVE COMPILARE: è un eseguibile del build normale e un test
// CTest aggiuntivo ne verifica la compilazione con esito di SUCCESSO (vedi
// tests/CMakeLists.txt, target `pulse_hook_compilepass_signature`).
//
// Req 5.2: quando la firma dichiarata COINCIDE con quella esposta dal binding
// del simbolo, lo static_assert di `SignatureMatches` dentro PULSE_HOOK è
// soddisfatto e la dichiarazione dell'hook compila senza errori. (I nomi dei
// parametri non fanno parte del tipo funzione e sono ignorati nel confronto.)
#include <pulse/hooks.hpp>

namespace {
struct SomeType {
    int value{0};
};
}  // namespace

namespace pulse::hooks {
// Stesso binding del caso negativo: "CompilePass_target" -> bool(SomeType*).
template <>
struct BindingTraits<FixedString("CompilePass_target")> {
    using Fn = bool(SomeType*);
};
}  // namespace pulse::hooks

// Firma dichiarata == firma del binding (bool(SomeType*)) → COMPILA (Req 5.2).
PULSE_HOOK(CompilePass_target, bool, (SomeType* self)) {
    if (self == nullptr) {
        return false;
    }
    self->value += 1;
    return true;
}

int main() {
    // La sola presenza dell'hook registrato basta a validare la firma a
    // compile-time; il main mantiene l'eseguibile linkabile.
    return static_cast<int>(::pulse::hooks::count() == 0);
}
