// fields_compilefail_key_type_conflict.cpp — snippet di NON-compilazione
// (task 16.5, Requisito 6.6).
//
// Questo file DEVE FALLIRE la compilazione. È compilato da un target CMake
// `EXCLUDE_FROM_ALL` e un test CTest con proprietà `WILL_FAIL TRUE` ne asserisce
// il fallimento (vedi tests/CMakeLists.txt, target
// `pulse_field_compilefail_key_type_conflict`).
//
// Req 6.6: dichiarare un campo iniettato con una chiave GIÀ UTILIZZATA per la
// STESSA classe del gioco con un TIPO DIVERSO deve produrre un errore in fase di
// COMPILAZIONE (conflitto di dichiarazione). Il registro di tipi a compile-time
// (`PULSE_FIELD_KEY`) fissa il tipo canonico della chiave; il successivo
// `PulseField` con tipo diverso fallisce lo `static_assert` interno.
#include <pulse/fields.hpp>

namespace {
struct Player {
    int dummy{0};
};
}  // namespace

// Registra il tipo canonico della chiave "conflict/score" per Player = int.
PULSE_FIELD_KEY(Player, "conflict/score", int);

namespace {

// ERRORE DI COMPILAZIONE ATTESO (Req 6.6): la stessa chiave per la stessa
// classe è ri-dichiarata con un tipo diverso (float ≠ int). Lo static_assert
// in PulseField segnala il conflitto di dichiarazione.
pulse::PulseField<float, "conflict/score", Player> conflictingField;

}  // namespace
