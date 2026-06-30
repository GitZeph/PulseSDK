// fields_compilepass_valid.cpp — controparte POSITIVA dei compile-fail test
// (task 16.5, Requisiti 6.5, 6.6).
//
// Questo file DEVE COMPILARE con successo: dimostra che l'uso valido del field
// injection NON è inibito dai controlli introdotti per Req 6.5/6.6. È compilato
// come parte del build normale dei test e da un target dedicato verificato da un
// test CTest che si attende SUCCESSO (vedi tests/CMakeLists.txt, target
// `pulse_field_compilepass_valid`).
#include <pulse/fields.hpp>

#include <string>

namespace {

struct Player {
    int dummy{0};
};
struct Enemy {
    int dummy{0};
};

}  // namespace

// Tipo canonico registrato per (Player, "valid/score") = int.
PULSE_FIELD_KEY(Player, "valid/score", int);

namespace {

// (Req 6.6) Dichiarazione del campo con il tipo canonico corretto: compila.
pulse::PulseField<int, "valid/score", Player> scoreField;

// Stessa chiave su una CLASSE DIVERSA con tipo diverso: nessun conflitto, il
// registro dei tipi è per-classe (compila).
pulse::PulseField<float, "valid/score", Enemy> enemyScoreField;

// Chiave non registrata: nessun vincolo di tipo (opt-in), compila con qualsiasi
// tipo coerente con se stesso.
pulse::PulseField<std::string, "valid/name", Player> nameField;

// (Req 6.5) Accesso con il tipo corretto: compila.
void valid_access() {
    Player player;

    scoreField.set(&player, 42);
    int score = scoreField.get(&player);
    (void)score;

    nameField.set(&player, std::string{"neo"});
    std::string name = nameField.get(&player);
    (void)name;

    Enemy enemy;
    enemyScoreField.set(&enemy, 1.5F);
    float es = enemyScoreField.get(&enemy);
    (void)es;
}

}  // namespace

// Punto di ingresso per il target eseguibile positivo (linkabile e avviabile).
int main() {
    valid_access();
    return 0;
}
