// fields_compilefail_wrong_type.cpp — snippet di NON-compilazione (task 16.5,
// Requisito 6.5).
//
// Questo file DEVE FALLIRE la compilazione. È compilato da un target CMake
// `EXCLUDE_FROM_ALL` e un test CTest con proprietà `WILL_FAIL TRUE` ne asserisce
// il fallimento (vedi tests/CMakeLists.txt, target
// `pulse_field_compilefail_wrong_type`).
//
// Req 6.5: accedere a un campo iniettato con un tipo DIVERSO da quello con cui
// è stato dichiarato deve produrre un errore in fase di COMPILAZIONE. La
// type-safety è garantita per costruzione: `PulseField<std::string, ...>`
// memorizza/restituisce `std::string`, quindi leggerne il valore in un `int`
// (tipi non convertibili) non compila.
#include <pulse/fields.hpp>

#include <string>

namespace {

struct Player {
    int dummy{0};
};

void wrong_type_access() {
    // Campo dichiarato con tipo std::string.
    pulse::PulseField<std::string, "compilefail/name", Player> nameField;

    Player player;
    nameField.set(&player, std::string{"neo"});

    // ERRORE DI COMPILAZIONE ATTESO (Req 6.5): get() restituisce std::string,
    // non convertibile in int. L'accesso con tipo errato non compila.
    int wrong = nameField.get(&player);
    (void)wrong;
}

}  // namespace
