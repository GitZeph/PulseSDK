// sdk/async/task.hpp — sistema asincrono / task dello SDK Pulse
// (Layer 5, Requisito 11). Implementazione header-only sotto namespace
// `pulse::async`, esposta ai Developer dall'header pubblico <pulse/async.hpp>.
//
// Modello osservabile (Requisito 11):
//   * spawn(modId, work)  — avvia `work` su un thread separato dal thread
//                           principale del gioco SENZA bloccare l'avvio
//                           (l'operazione di avvio ritorna in pochi µs, ben
//                           sotto 1 ms — Req 11.1). Al massimo 64 task
//                           simultanei per ogni Mod: oltre il limite l'avvio è
//                           rifiutato con un esito di errore (Req 11.5).
//   * then(onMain)        — registra una continuazione che verrà eseguita sul
//                           thread principale al "frame successivo", cioè alla
//                           prossima chiamata di pumpMainThread(). La
//                           continuazione riceve un Result<T> che, in caso di
//                           fallimento del lavoro, descrive la causa (Req
//                           11.2/11.3), senza arrestare il thread principale.
//   * pumpMainThread()    — drena ed esegue, sul thread chiamante (il thread
//                           principale del gioco), le continuazioni pronte.
//                           Modella in modo deterministico il "tick di frame"
//                           così da rendere il sistema testabile sull'host.
//   * cancelMod(modId)    — annulla i task della Mod e impedisce l'esecuzione
//                           delle loro continuazioni sul thread principale,
//                           usato alla disabilitazione della Mod (Req 11.4).
//
// Questo modulo è INDIPENDENTE: non dipende da HttpClient né da altri servizi
// dello SDK. Stack: C++20/23 (Requisito 26.1). Solo libreria standard.
#ifndef PULSE_ASYNC_TASK_HPP
#define PULSE_ASYNC_TASK_HPP

#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulse::async {

// ---------------------------------------------------------------------------
// Tipi di base.
// ---------------------------------------------------------------------------

// Identità della Mod proprietaria del task (coerente con gli altri servizi
// dello SDK, es. pulse::storage::ModId).
using ModId = std::string;

// Limite massimo di task asincroni simultanei per singola Mod (Req 11.5).
inline constexpr std::size_t kMaxConcurrentTasksPerMod = 64;

// Categoria dell'errore di un task.
enum class TaskErrorCode {
    ConcurrencyLimitExceeded,  // superato il limite di 64 task/mod (Req 11.5).
    WorkFailed,                // il lavoro è terminato con eccezione (Req 11.3).
    Cancelled,                 // il task è stato annullato (Req 11.4).
};

// Esito di errore con categoria e descrizione leggibile della causa.
struct TaskError {
    TaskErrorCode code{TaskErrorCode::WorkFailed};
    std::string message;
};

// ---------------------------------------------------------------------------
// Result<T> — esito ok(valore) oppure fail(errore), senza eccezioni sul
// percorso felice. Usato sia per l'esito di spawn (Result<Task<T>>) sia per il
// risultato consegnato alla continuazione (Result<T>).
// ---------------------------------------------------------------------------
template <class T>
class Result {
public:
    [[nodiscard]] static Result ok(T value) {
        Result r;
        r.ok_ = true;
        r.value_ = std::move(value);
        return r;
    }
    [[nodiscard]] static Result fail(TaskError error) {
        Result r;
        r.ok_ = false;
        r.error_ = std::move(error);
        return r;
    }

    [[nodiscard]] bool isOk() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    // Precondizione: isOk(). Valore prodotto dal lavoro.
    [[nodiscard]] const T& value() const& { return *value_; }
    [[nodiscard]] T& value() & { return *value_; }
    [[nodiscard]] T&& value() && { return std::move(*value_); }

    // Precondizione: !isOk(). Descrive la causa del fallimento.
    [[nodiscard]] const TaskError& error() const noexcept { return error_; }

private:
    bool ok_{false};
    std::optional<T> value_{};
    TaskError error_{};
};

// ---------------------------------------------------------------------------
// Stato interno condiviso di un task.
// ---------------------------------------------------------------------------

// Base non templata: consente al TaskScheduler di tracciare e annullare task di
// tipi diversi tramite un registro omogeneo di weak_ptr.
struct TaskStateBase {
    ModId modId;
    std::atomic<bool> cancelled{false};
    bool done{false};      // il lavoro è terminato (guardato dal mutex dello scheduler).
    bool enqueued{false};  // la continuazione è già stata accodata per il pump.
    virtual ~TaskStateBase() = default;
};

template <class T>
struct TaskState : TaskStateBase {
    std::optional<Result<T>> result;                 // riempito dal worker.
    std::function<void(Result<T>)> continuation;     // impostata da then().

    // Eseguita sul thread principale durante pumpMainThread().
    void invokeContinuation() {
        if (continuation && result) {
            continuation(std::move(*result));
        }
    }
};

// Forward declaration: Task è restituito da TaskScheduler::spawn.
template <class T>
class Task;

// ---------------------------------------------------------------------------
// TaskScheduler — runtime host-testabile dei task asincroni.
//
// Tiene il conteggio dei task attivi per ModId (limite 64, Req 11.5), esegue il
// lavoro su thread separati (avvio non bloccante, Req 11.1), raccoglie le
// continuazioni pronte e le esegue in modo deterministico su pumpMainThread()
// (continuazione sul thread principale al frame successivo, Req 11.2/11.3) e
// offre cancelMod() per annullare i task di una Mod disabilitata (Req 11.4).
// ---------------------------------------------------------------------------
class TaskScheduler {
public:
    TaskScheduler() = default;
    ~TaskScheduler();  // unisce (join) i thread worker ancora in esecuzione.

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    // Avvia `work` per `modId` su un thread separato. Ritorna immediatamente
    // (Req 11.1). Se i task simultanei della Mod sono già 64, RIFIUTA l'avvio
    // con un errore ConcurrencyLimitExceeded (Req 11.5).
    template <class T>
    [[nodiscard]] Result<Task<T>> spawn(ModId modId, std::function<T()> work);

    // Esegue, sul thread chiamante, tutte le continuazioni pronte (tick di
    // frame). Le continuazioni di task annullati non vengono eseguite (Req
    // 11.4).
    void pumpMainThread();

    // Annulla i task della Mod e impedisce l'esecuzione delle loro
    // continuazioni sul thread principale (Req 11.4).
    void cancelMod(const ModId& modId);

    // Numero di task attualmente attivi (in volo) per `modId`. Utile ai test.
    [[nodiscard]] std::size_t activeCount(const ModId& modId) const;

    // Scheduler globale di default usato da Task<T>::spawn.
    static TaskScheduler& global();

private:
    template <class T>
    friend class Task;

    // Imposta la continuazione per `state`; se il lavoro è già terminato e non
    // annullato, accoda subito la continuazione per il prossimo pump.
    template <class T>
    void setContinuation(std::shared_ptr<TaskState<T>> state,
                         std::function<void(Result<T>)> fn);

    // Invocata dal worker al termine del lavoro: memorizza il risultato,
    // decrementa il conteggio attivo e, se possibile, accoda la continuazione.
    template <class T>
    void completeTask(std::shared_ptr<TaskState<T>> state, Result<T> result);

    // Accoda una continuazione pronta (chiamata con mtx_ già acquisito).
    void enqueueReadyLocked(std::function<void()> invoke) {
        readyQueue_.push_back(std::move(invoke));
    }

    mutable std::mutex mtx_;
    std::unordered_map<ModId, std::size_t> activeCounts_;
    std::unordered_map<ModId, std::vector<std::weak_ptr<TaskStateBase>>> registry_;
    std::vector<std::function<void()>> readyQueue_;
    std::vector<std::thread> threads_;
};

// ---------------------------------------------------------------------------
// Task<T> — handle a un'operazione asincrona avviata.
// ---------------------------------------------------------------------------
template <class T>
class Task {
public:
    // Avvia un task sullo scheduler globale di default (Req 11.1, 11.5).
    [[nodiscard]] static Result<Task<T>> spawn(ModId modId,
                                               std::function<T()> work) {
        return TaskScheduler::global().spawn<T>(std::move(modId), std::move(work));
    }

    // Registra la continuazione eseguita sul thread principale al frame
    // successivo, anche in caso di errore (Req 11.2/11.3). Ritorna *this per
    // consentire il concatenamento.
    Task& then(std::function<void(Result<T>)> onMain) {
        if (scheduler_ != nullptr && state_) {
            scheduler_->setContinuation(state_, std::move(onMain));
        }
        return *this;
    }

    // Identità della Mod proprietaria del task.
    [[nodiscard]] const ModId& modId() const { return state_->modId; }

private:
    friend class TaskScheduler;

    Task(TaskScheduler* scheduler, std::shared_ptr<TaskState<T>> state)
        : scheduler_(scheduler), state_(std::move(state)) {}

    TaskScheduler* scheduler_{nullptr};
    std::shared_ptr<TaskState<T>> state_{};
};

// ---------------------------------------------------------------------------
// Definizioni dei metodi templati e inline di TaskScheduler.
// ---------------------------------------------------------------------------

template <class T>
Result<Task<T>> TaskScheduler::spawn(ModId modId, std::function<T()> work) {
    auto state = std::make_shared<TaskState<T>>();
    state->modId = modId;

    {
        std::lock_guard<std::mutex> lock(mtx_);

        std::size_t& count = activeCounts_[modId];
        if (count >= kMaxConcurrentTasksPerMod) {
            // Limite superato: rifiuta l'avvio del task aggiuntivo (Req 11.5).
            return Result<Task<T>>::fail(TaskError{
                TaskErrorCode::ConcurrencyLimitExceeded,
                "avvio rifiutato: superato il limite di 64 task asincroni "
                "simultanei per la mod"});
        }
        ++count;
        registry_[modId].push_back(state);

        // Avvio non bloccante: la creazione del thread ritorna in pochi µs, ben
        // sotto 1 ms (Req 11.1). Il worker esegue `work` fuori dal thread
        // principale e poi notifica il completamento.
        threads_.emplace_back([this, state, work = std::move(work)]() mutable {
            Result<T> result = Result<T>::fail(
                TaskError{TaskErrorCode::WorkFailed, "lavoro non eseguito"});
            try {
                result = Result<T>::ok(work());
            } catch (const std::exception& e) {
                // L'errore viene consegnato alla continuazione (Req 11.3),
                // senza propagarsi al thread principale del gioco.
                result = Result<T>::fail(
                    TaskError{TaskErrorCode::WorkFailed, e.what()});
            } catch (...) {
                result = Result<T>::fail(TaskError{
                    TaskErrorCode::WorkFailed,
                    "il lavoro asincrono è terminato con un'eccezione "
                    "sconosciuta"});
            }
            this->completeTask(state, std::move(result));
        });
    }

    return Result<Task<T>>::ok(Task<T>(this, std::move(state)));
}

template <class T>
void TaskScheduler::setContinuation(std::shared_ptr<TaskState<T>> state,
                                    std::function<void(Result<T>)> fn) {
    std::lock_guard<std::mutex> lock(mtx_);
    state->continuation = std::move(fn);

    // Se il lavoro è già terminato (then() chiamata dopo il completamento) e il
    // task non è annullato, accoda subito la continuazione per il prossimo pump.
    if (state->done && !state->enqueued && !state->cancelled.load()) {
        state->enqueued = true;
        enqueueReadyLocked([state]() {
            if (!state->cancelled.load()) {
                state->invokeContinuation();
            }
        });
    }
}

template <class T>
void TaskScheduler::completeTask(std::shared_ptr<TaskState<T>> state,
                                 Result<T> result) {
    std::lock_guard<std::mutex> lock(mtx_);
    state->result = std::move(result);
    state->done = true;

    // Il task non è più "in volo": libera uno slot del limite di concorrenza.
    auto it = activeCounts_.find(state->modId);
    if (it != activeCounts_.end() && it->second > 0) {
        --it->second;
    }

    // Accoda la continuazione solo se è già stata registrata e il task non è
    // stato annullato (Req 11.4). Altrimenti sarà then() ad accodarla.
    if (state->continuation && !state->enqueued && !state->cancelled.load()) {
        state->enqueued = true;
        enqueueReadyLocked([state]() {
            if (!state->cancelled.load()) {
                state->invokeContinuation();
            }
        });
    }
}

inline void TaskScheduler::pumpMainThread() {
    // Estrae le continuazioni pronte sotto lock, poi le esegue FUORI dal lock
    // sul thread chiamante (il thread principale), così una continuazione può
    // a sua volta avviare nuovi task senza rischio di deadlock.
    std::vector<std::function<void()>> ready;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        ready.swap(readyQueue_);
    }
    for (auto& invoke : ready) {
        invoke();
    }
}

inline void TaskScheduler::cancelMod(const ModId& modId) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = registry_.find(modId);
    if (it == registry_.end()) {
        return;
    }
    // Marca annullati tutti i task (in volo o completati ma non ancora pompati)
    // della Mod: le continuazioni già accodate vengono saltate al pump e quelle
    // non ancora accodate non lo saranno (Req 11.4).
    for (auto& weak : it->second) {
        if (auto state = weak.lock()) {
            state->cancelled.store(true);
        }
    }
}

inline std::size_t TaskScheduler::activeCount(const ModId& modId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = activeCounts_.find(modId);
    return it != activeCounts_.end() ? it->second : 0u;
}

inline TaskScheduler::~TaskScheduler() {
    // Attende il termine di tutti i worker prima di distruggere lo stato: i
    // worker accedono a mtx_ e ai membri, quindi devono concludere qui.
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

inline TaskScheduler& TaskScheduler::global() {
    static TaskScheduler instance;
    return instance;
}

}  // namespace pulse::async

#endif  // PULSE_ASYNC_TASK_HPP
