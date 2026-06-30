// loader/hooking/hook_chain_registry.cpp — implementazione (scheletro) della
// HookChainRegistry (Hook_Chaining, Requisiti 1, 4, 5, 7, 11).
//
// Questo file fissa lo scheletro della Registry (task 3.1): costruttore,
// introspezione (installCount/chainSize/hasInstall/chainOrder) e gli stub delle
// operazioni mutative (insertLink/removeOwner/teardown), le cui implementazioni
// complete arrivano nei task successivi (3.2-3.5 per la Fase B, 5.x per la Fase
// C). Tutti gli accessi alla mappa `chains_` sono serializzati dal `mutex_`
// strutturale (Req 9.3).
#include "hooking/hook_chain_registry.hpp"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <string>
#include <utility>

#include "hooking/hook_gate.hpp"
#include "lifecycle/hook_ownership.hpp"  // pulse::lifecycle::OwnedHook

namespace pulse::hooking {

namespace {

// Lunghezza del prologo originale letto e persistito nel RollbackStore PRIMA di
// ogni install (Req 5.4). Coerente con `kRollbackPrologueBytes` del Mod_Loader
// e con il prologo modellato dal FakeBackend host-testabile.
inline constexpr std::size_t kPrologueBytes = 16;

// Formatta un Target_Address come `0x...` per la diagnostica (Req 11.1-11.3).
std::string formatAddress(std::uintptr_t address) {
    std::ostringstream os;
    os << "0x" << std::hex << address;
    return os.str();
}

}  // namespace

HookChainRegistry::HookChainRegistry(IHookBackend& backend,
                                     RollbackStore& rollback,
                                     pulse::lifecycle::HookOwnershipLedger& ledger,
                                     HookEventSink log)
    : backend_(backend),
      rollback_(rollback),
      ledger_(ledger),
      log_(std::move(log)) {}

// ---------------------------------------------------------------------------
// Operazioni mutative — SCHELETRO (task 3.1).
//
// Le implementazioni complete sono nei task successivi:
//   * insertLink 0→1 (install via Hook_Gate + Head_Thunk)  — task 3.2;
//   * insertLink n→n+1 (relinking dei vicini)               — task 3.3;
//   * removeOwner / teardown (Fase C)                       — task 5.x.
// Gli stub non effettuano alcuna mutazione: nessuna install, nessun anello.
// ---------------------------------------------------------------------------

ChainOpResult HookChainRegistry::insertLink(std::uintptr_t target,
                                            const bindings::FunctionBinding& binding,
                                            const LinkSpec& link) {
    std::lock_guard<std::mutex> guard(mutex_);

    // Determina se questo è il PRIMO anello ammesso sul Target_Address: o non
    // esiste ancora un ChainSlot, o ne esiste uno con catena vuota (caso di un
    // target la cui catena è stata svuotata). In entrambi i casi la transizione
    // è 0→1 (task 3.2).
    const auto it = chains_.find(target);
    const bool firstLink = (it == chains_.end()) || it->second.chain.empty();

    if (firstLink) {
        return installFirst(target, binding, link);
    }

    // -----------------------------------------------------------------------
    // Transizione n→n+1 (anello successivo, Req 1.2, 1.7, 4.1, 4.2, 4.3).
    //
    // Il Target_Address possiede già una Underlying_Installation attiva: NON si
    // tocca il backend (nessuna seconda DobbyHook, nessun errore "indirizzo già
    // hookato", Req 1.7) e NON si muta il prologo né il Rollback_Store. Si
    // inserisce l'anello nella catena secondo il Chain_Order (priority DESC,
    // loadOrder ASC) e si ri-cablano i SOLI Trampoline_Slot dei vicini.
    // -----------------------------------------------------------------------
    return insertSubsequent(it->second, target, binding, link);
}

ChainOpResult HookChainRegistry::insertSubsequent(ChainSlot& slot,
                                                  std::uintptr_t target,
                                                  const bindings::FunctionBinding& binding,
                                                  const LinkSpec& link) {
    // Anche per gli anelli successivi il binding deve essere risolto/verificato
    // prima di ammettere l'anello alla catena (Req 10.3): un simbolo non risolto
    // non aggiunge alcun anello. Il backend NON viene interrogato (l'unica
    // install esiste già e copre il target).
    if (!binding.resolved) {
        HookError err = make_incompatibility_error(link.symbol);
        emit("hook-chaining: mod '" + link.owner + "' simbolo '" + link.symbol +
             "' @ " + formatAddress(target) + " rifiutato: " + err.message);
        return ChainOpResult{ChainOpOutcome::Rejected, std::move(err), {}};
    }

    // Costruisci il nodo dell'anello. La priorità è ricondotta al dominio
    // [0,1000] esattamente come fa `HookChain::add`, così l'indice di
    // inserimento calcolato qui (pre-add) coincide con la posizione effettiva
    // nella vista ordinata post-add.
    HookNode node;
    node.owner = link.owner;
    node.priority = clampHookPriority(link.priority);
    node.loadOrder = link.loadOrder;
    node.detour = link.detour;
    node.slot = link.slot;

    // Posizione di inserimento nel Chain_Order: `HookChain::add` usa
    // `upper_bound` con `precedes`, posizionando il nuovo nodo DOPO tutti gli
    // anelli equivalenti già presenti. Calcolando lo stesso upper_bound sulla
    // vista pre-add otteniamo l'indice esatto che il nodo assumerà post-add.
    const auto& before = slot.chain.orderedNodes();
    const auto insertIt = std::upper_bound(
        before.begin(), before.end(), node,
        [](const HookNode& a, const HookNode& b) { return HookChain::precedes(a, b); });
    const std::size_t insertIdx =
        static_cast<std::size_t>(std::distance(before.begin(), insertIt));

    // Inserisce l'anello mantenendo l'ordinamento. NESSUNA chiamata al backend.
    slot.chain.add(std::move(node));

    // Vista ordinata aggiornata: il nuovo anello è in posizione `insertIdx`.
    const auto& nodes = slot.chain.orderedNodes();
    const std::size_t n = nodes.size();
    const HookNode& inserted = nodes[insertIdx];

    // -----------------------------------------------------------------------
    // Relinking dei soli vicini (Req 4.2, 4.3). Lo slot dell'anello inserito
    // punta al successore (o al Real_Trampoline se l'inserito è la nuova coda);
    // lo slot del predecessore punta al detour dell'inserito; se l'inserito è la
    // nuova testa si aggiorna `currentHead` con una singola scrittura atomica
    // (nessuna seconda install). Si toccano SOLO i puntatori dei vicini.
    // -----------------------------------------------------------------------

    // *nuovo.slot = succ.detour  (o Real_Trampoline se nuovo è coda). Store
    // atomico a semantica release: una dispatch in volo non osserva mai un
    // puntatore lacerato (Req 9.1).
    if (inserted.slot != nullptr) {
        if (insertIdx + 1 < n) {
            store_slot(inserted.slot, nodes[insertIdx + 1].detour);
        } else {
            store_slot(inserted.slot, slot.realTrampoline.address());
        }
    }

    ChainOpOutcome outcome;
    if (insertIdx == 0) {
        // Nuovo Chain_Head: la successiva invocazione esegue per primo il nuovo
        // anello, senza una seconda Underlying_Installation (Req 4.3).
        set_head(slot.head, inserted.detour);
        outcome = ChainOpOutcome::InsertedHead;
    } else {
        // *pred.slot = nuovo.detour: il predecessore cede ora all'inserito
        // (Req 4.2). Store atomico release (Req 9.1).
        const HookNode& pred = nodes[insertIdx - 1];
        if (pred.slot != nullptr) {
            store_slot(pred.slot, inserted.detour);
        }
        outcome = (insertIdx + 1 == n) ? ChainOpOutcome::InsertedTail
                                       : ChainOpOutcome::InsertedMiddle;
    }

    // Attribuisci l'anello al Mod_Id proprietario (Req 6.1). registryIndex non è
    // noto a questo livello (lo SDK registry è cablato in Fase D): 0 di default.
    ledger_.attribute(pulse::lifecycle::OwnedHook{link.owner, link.symbol, target, 0});

    // Diagnostica osservabile dell'aggiunta: Mod_Id + Target_Address + posizione
    // nel Chain_Order (Req 11.1). Nessun evento di install (l'unica
    // Underlying_Installation è invariata, Req 1.2).
    emit("hook-chaining: aggiunto anello mod '" + link.owner + "' @ " +
         formatAddress(target) + " in posizione " + std::to_string(insertIdx) +
         (insertIdx == 0 ? " (Chain_Head)" : ""));

    // Costruisci la vista del Chain_Order risultante (Chain_Head → coda, Req 11.4).
    std::vector<ModId> order;
    order.reserve(n);
    for (const auto& node : nodes) {
        order.push_back(node.owner);
    }

    // Guardia di consolidamento: gli invarianti strutturali della catena devono
    // valere dopo ogni inserimento successivo (task 3.4).
    assert(slotInvariantsHold(slot));

    return ChainOpResult{outcome, HookError{}, std::move(order)};
}

ChainOpResult HookChainRegistry::installFirst(std::uintptr_t target,
                                              const bindings::FunctionBinding& binding,
                                              const LinkSpec& link) {
    // -----------------------------------------------------------------------
    // Ammissione (gate predicates) PRIMA di toccare il backend o di persistere
    // alcunché: il gate ammette un anello SOLO SE il backend è disponibile e il
    // binding è risolto (Req 10.2, 10.5). Verifichiamo gli stessi predicati qui
    // così, se l'anello è inammissibile, NON creiamo alcun ChainSlot, NON
    // leggiamo/persistiamo byte e NON effettuiamo alcuna install parziale
    // (Req 1.8): nessuna catena e diagnostica con Mod_Id + Target_Address.
    // -----------------------------------------------------------------------
    if (!backend_.available()) {
        // Backend non disponibile a runtime: zero install, diagnostica che
        // nomina il backend (Req 1.8, 10.5).
        HookError err = make_backend_unavailable_error(backend_.name());
        emit("hook-chaining: mod '" + link.owner + "' @ " + formatAddress(target) +
             " rifiutato: " + err.message);
        return ChainOpResult{ChainOpOutcome::Rejected, std::move(err), {}};
    }
    if (!binding.resolved) {
        // Simbolo non risolto: nessun anello, nessuna install (Req 1.8, 10.3).
        HookError err = make_incompatibility_error(link.symbol);
        emit("hook-chaining: mod '" + link.owner + "' simbolo '" + link.symbol +
             "' @ " + formatAddress(target) + " rifiutato: " + err.message);
        return ChainOpResult{ChainOpOutcome::Rejected, std::move(err), {}};
    }

    // Crea il ChainSlot del target (default-construct in place: HeadCell ha un
    // std::atomic non copiabile/spostabile, quindi l'unordered_map lo costruisce
    // sul posto e il puntatore alla HeadCell resta stabile a ogni rehash).
    ChainSlot& slot = chains_[target];
    slot.address = target;

    // L'unica DobbyHook NON punta al detour della testa, bensì all'Head_Thunk
    // stabile per-indirizzo (signature-agnostic). Cambiare testa diventerà una
    // singola scrittura su `currentHead`, senza re-install (Req 4.3, 5.2).
    slot.headThunk = make_head_thunk(slot.head);

    // Persisti gli `originalBytes` del prologo nel RollbackStore con
    // `owner = Mod_Id` PRIMA dell'install (Req 5.4): la rimozione dell'ultimo
    // anello (1→0) potrà così ripristinare i byte byte-esatto.
    if (auto original = backend_.readOriginal(target, kPrologueBytes);
        original.has_value()) {
        RollbackRecord record;
        record.owner = link.owner;
        record.symbol = link.symbol;
        record.address = target;
        record.originalBytes = original.value().bytes();
        // version/platformId restano ai default: la Registry non conosce il
        // Runtime_Context; il ripristino byte-esatto richiede solo address +
        // originalBytes. Il cablaggio in runtime_entry (Fase D) può arricchirli.
        rollback_.add(std::move(record));
    }

    // Ammetti ed effettua l'UNICA install via Hook_Gate: il gate ri-verifica
    // available()/resolved e delega a `backend_.install(target, Head_Thunk)`,
    // restituendo il Real_Trampoline (Req 1.6).
    HookGate gate(backend_, [this](std::string_view m) { emit(std::string{m}); });
    GateResult gated = gate.install(binding, slot.headThunk);
    if (!gated.installed()) {
        // Gate negato o install fallita: NESSUNA catena, NESSUNA install
        // parziale (Req 1.8). Dismetti il ChainSlot appena creato.
        chains_.erase(target);
        emit("hook-chaining: mod '" + link.owner + "' @ " + formatAddress(target) +
             " install rifiutata/fallita: " + gated.error.message);
        return ChainOpResult{ChainOpOutcome::Rejected, gated.error, {}};
    }

    // Install riuscita: registra l'unica Underlying_Installation e il trampolino
    // reale (coda → Real_Original).
    slot.realTrampoline = gated.trampoline;
    slot.installed = true;

    // Aggiungi l'anello alla catena (priority DESC, loadOrder ASC; la priorità è
    // ricondotta al dominio [0,1000] da HookChain::add).
    HookNode node;
    node.owner = link.owner;
    node.priority = link.priority;
    node.loadOrder = link.loadOrder;
    node.detour = link.detour;
    node.slot = link.slot;
    slot.chain.add(std::move(node));

    // Cabla la testa e la coda: con un solo anello L0 è sia testa sia coda, così
    // currentHead = L0.detour e *L0.slot = Real_Trampoline (equivalenza con
    // l'installazione diretta, Req 3.7). Lo slot è scritto con store atomico
    // release, come `currentHead` (Req 9.1).
    set_head(slot.head, link.detour);
    if (link.slot != nullptr) {
        store_slot(link.slot, slot.realTrampoline.address());
    }

    // Attribuisci l'anello al Mod_Id proprietario (Req 6.1). registryIndex non è
    // noto a questo livello (lo SDK registry è cablato in Fase D): 0 di default.
    ledger_.attribute(pulse::lifecycle::OwnedHook{link.owner, link.symbol, target, 0});

    // Diagnostica osservabile (Req 11.1 aggiunta, 11.3 install creata).
    emit("hook-chaining: creata Underlying_Installation @ " + formatAddress(target) +
         " (install)");
    emit("hook-chaining: aggiunto anello mod '" + link.owner + "' @ " +
         formatAddress(target) + " in posizione 0 (Chain_Head)");

    // Guardia di consolidamento: dopo la transizione 0→1 la catena ha un solo
    // anello (testa == coda), una sola Underlying_Installation e lo slot punta
    // al Real_Trampoline (task 3.4).
    assert(slotInvariantsHold(slot));

    return ChainOpResult{ChainOpOutcome::CreatedInstall, HookError{}, {link.owner}};
}

void HookChainRegistry::relinkAfterRemoval(ChainSlot& slot, const ModId& owner) {
    // Rimuove gli anelli del `owner` dalla catena. `HookChain::remove` preserva
    // l'ordine relativo degli anelli restanti (Req 5.6). NESSUNA chiamata al
    // backend, NESSUN ripristino dei byte: la Underlying_Installation resta
    // attiva (Req 5.5).
    slot.chain.remove(owner);

    // Ri-cabla la catena rimasta secondo l'invariante di cablaggio:
    //   currentHead = detour del (nuovo) Chain_Head           (Req 5.2)
    //   *Li.slot    = L(i+1).detour  per ogni i < n-1         (Req 5.1)
    //   *L(n-1).slot = Real_Trampoline                        (Req 3.4, 5.1)
    // Per la rimozione di un singolo anello questo tocca, in pratica, i soli
    // puntatori dei vicini: gli slot degli altri anelli puntano già al
    // successore corretto, quindi la loro riscrittura è idempotente. L'ordine e
    // l'esecuzione degli anelli restanti non sono alterati (Req 5.6).
    //
    // PRECONDIZIONE: la catena è non vuota (keep-install). Il chiamante
    // (removeOwner) instrada altrove la transizione 1→0 (ultimo anello, task 5.2).
    const auto& nodes = slot.chain.orderedNodes();
    const std::size_t n = nodes.size();

    // currentHead punta ora al detour del nuovo Chain_Head: la successiva
    // invocazione esegue per primo il nuovo head, senza re-install (Req 5.2).
    set_head(slot.head, nodes.front().detour);

    // *pred.slot = succ.detour (o Real_Trampoline se diventa coda): il
    // predecessore dell'anello rimosso cede ora al successore (Req 5.1). Store
    // atomico release: una dispatch in volo non osserva un puntatore lacerato
    // (Req 9.1).
    for (std::size_t i = 0; i < n; ++i) {
        if (nodes[i].slot == nullptr) {
            continue;  // anelli host di solo ordinamento: nessuno slot da cablare.
        }
        store_slot(nodes[i].slot, (i + 1 < n) ? nodes[i + 1].detour
                                              : slot.realTrampoline.address());
    }
}

std::vector<ChainOpResult> HookChainRegistry::removeOwner(const ModId& owner) {
    std::lock_guard<std::mutex> guard(mutex_);
    return removeOwnerLocked(owner);
}

std::vector<ChainOpResult> HookChainRegistry::removeOwnerLocked(const ModId& owner) {
    std::vector<ChainOpResult> results;

    // Conta gli anelli del `owner` effettivamente attribuiti come installati nel
    // ledger PRIMA della rimozione: serve solo a decidere se emettere la
    // diagnostica di rilascio (release è comunque idempotente).
    const std::size_t ownedBefore = ledger_.hooksOf(owner).size();

    // Rimozione selettiva per Mod_Id su tutte le catene (Req 6.2/6.4). Per
    // ciascuna catena che contiene il `owner` si determina la transizione:
    //   * se restano anelli  → rimozione non-ultima: relink dei vicini, install
    //     mantenuta, byte NON ripristinati (task 5.1, Req 5.1, 5.2, 5.5, 5.6);
    //   * se il `owner` detiene l'ultimo/gli ultimi anelli → transizione 1→0
    //     (backend.remove + restore byte-esatto + dismissione del ChainSlot),
    //     gestita da `removeLast` (task 5.2, Req 5.3, 5.4).
    //
    // L'iterazione usa un iteratore esplicito così la dismissione del ChainSlot
    // alla transizione 1→0 (`chains_.erase`) non invalida il ciclo.
    for (auto it = chains_.begin(); it != chains_.end();) {
        const std::uintptr_t address = it->first;
        ChainSlot& slot = it->second;

        if (!slot.chain.contains(owner)) {
            ++it;
            continue;
        }

        // Conta gli anelli del `owner` per stabilire quanti anelli restano dopo
        // la rimozione.
        std::size_t ownerLinks = 0;
        for (const auto& node : slot.chain.orderedNodes()) {
            if (node.owner == owner) {
                ++ownerLinks;
            }
        }
        const std::size_t remaining = slot.chain.size() - ownerLinks;

        if (remaining == 0) {
            // Transizione 1→0 (ultimo anello, task 5.2): rimuove l'unica
            // Underlying_Installation dal backend e ripristina i byte originali
            // byte-esatto via Rollback_Store, poi dismette il ChainSlot.
            ChainOpResult result = removeLast(slot, address, owner);
            const bool disposed = (result.outcome == ChainOpOutcome::RemovedLastInstall);
            results.push_back(std::move(result));
            if (disposed) {
                // Dismissione del ChainSlot del target (transizione 1→0
                // completata): nessuna install né anello residuo su `address`.
                it = chains_.erase(it);
            } else {
                // Rimozione fallita e isolata: il ChainSlot resta invariato.
                ++it;
            }
            continue;
        }

        // Rimozione non-ultima (task 5.1): ri-cabla i vicini mantenendo l'unica
        // Underlying_Installation, senza ripristinare i byte originali.
        relinkAfterRemoval(slot, owner);

        // Diagnostica osservabile della rimozione: Mod_Id + Target_Address
        // (Req 11.2). NESSUN evento di rimozione install: la
        // Underlying_Installation è mantenuta (Req 5.5).
        emit("hook-chaining: rimosso anello mod '" + owner + "' @ " +
             formatAddress(address) + " (install mantenuta)");

        // Vista del Chain_Order risultante (Chain_Head → coda, Req 11.4).
        std::vector<ModId> order;
        const auto& nodes = slot.chain.orderedNodes();
        order.reserve(nodes.size());
        for (const auto& node : nodes) {
            order.push_back(node.owner);
        }

        // Guardia di consolidamento: gli invarianti strutturali della catena
        // devono valere dopo la rimozione non-ultima (task 3.4 / 5.1).
        assert(slotInvariantsHold(slot));

        results.push_back(ChainOpResult{ChainOpOutcome::RemovedKeepInstall,
                                        HookError{}, std::move(order), address});
        ++it;
    }

    // -----------------------------------------------------------------------
    // Rilascio selettivo dell'attribuzione dal HookOwnershipLedger (task 5.3,
    // Req 6.4). Dopo aver rimosso dalle catene gli anelli del solo `owner`
    // (relinking dei restanti / transizione 1→0 sui target svuotati), si
    // rilascia dal ledger l'attribuzione dei SOLI hook di quel Mod_Id: tutti gli
    // altri Mod_Id (incl. quelli che condividono un Target_Address) restano
    // attribuiti senza ambiguità (Req 6.3, 6.4). `release` rimuove dalla lista
    // degli installati esclusivamente gli `OwnedHook` con `owner == owner`,
    // mantenendo invariata l'attribuzione altrui; ne consegue l'invariante
    // globale Req 6.5: l'insieme degli anelli abilitati resta pari all'unione
    // degli anelli delle mod nello stato Enabled. NON tocca la mappa
    // indice→owner delle finestre di epoca (consente il re-enable, task 5.4).
    ledger_.release(owner);
    if (ownedBefore > 0) {
        emit("hook-chaining: rilasciata attribuzione ledger per mod '" + owner +
             "' (" + std::to_string(ownedBefore) + " anelli)");
    }

    return results;
}

ChainOpResult HookChainRegistry::removeLast(ChainSlot& slot,
                                            std::uintptr_t target,
                                            const ModId& owner) {
    // -----------------------------------------------------------------------
    // Transizione 1→0 (Req 5.3, 5.4). Il `owner` detiene l'ultimo/gli ultimi
    // anelli del Target_Address: dopo la rimozione la catena è vuota, quindi
    // l'unica Underlying_Installation deve essere rimossa e i byte originali
    // ripristinati byte-esatto.
    //
    // Rimuove prima gli anelli del `owner` dalla catena (che diventa vuota,
    // Req 5.6 banalmente preservato): da qui in poi nessun anello resta
    // abilitato su `target`.
    // -----------------------------------------------------------------------
    slot.chain.remove(owner);

    // Localizza il RollbackRecord persistito alla transizione 0→1 (owner = Mod_Id
    // del primo anello, indicizzato per Target_Address). Il ripristino
    // byte-esatto attinge agli `originalBytes` di questo record (Req 5.4).
    const RollbackRecord* record = nullptr;
    for (const auto& r : rollback_.records()) {
        if (r.address == target) {
            record = &r;
            break;
        }
    }

    // Write-back dei byte originali instradato attraverso il backend: rimuovere
    // l'unica DobbyHook ripristina il prologo del Target_Address byte-esatto
    // (Req 5.3 + 5.4). Una sola rimozione dal backend per l'intera catena.
    const auto writeThroughBackend =
        [this](std::uintptr_t address,
               const std::vector<std::uint8_t>& /*originalBytes*/) {
            return backend_.remove(address).has_value();
        };

    bool restored = false;
    HookError failure{};
    if (record != nullptr) {
        // Ripristino byte-esatto tramite il Rollback_Store del singolo target
        // (Req 5.4): `restore` invoca il write-back con gli `originalBytes`
        // persistiti; il write-back rimuove l'unica DobbyHook (Req 5.3).
        const RestoreOutcome outcome = rollback_.restore(*record, writeThroughBackend);
        restored = outcome.ok();
        if (!restored) {
            failure = HookError{HookErrorCode::BackendFailure,
                                outcome.error.has_value()
                                    ? outcome.error->message
                                    : std::string{"rimozione dell'ultima "
                                                  "Underlying_Installation fallita"}};
        }
    } else {
        // Difensivo: nessun record persistito per il target (non dovrebbe
        // accadere, gli `originalBytes` sono persistiti a 0→1). Rimuove comunque
        // l'unica DobbyHook per non lasciare una install residua (Req 5.3, 7.4).
        const auto removedOutcome = backend_.remove(target);
        restored = removedOutcome.has_value();
        if (!restored) {
            failure = removedOutcome.error();
        }
    }

    if (!restored) {
        // Fallimento isolato della transizione 1→0 (Req 7.6): il ChainSlot NON
        // viene dismesso (il chiamante lo conserva); diagnostica con Mod_Id +
        // Target_Address. La catena resta vuota ma marcata installata così
        // l'invariante installed==(!chain.empty()) non è verificato qui (lo
        // slot resta in uno stato di errore osservabile dal chiamante).
        emit("hook-chaining: rimozione ultimo anello mod '" + owner + "' @ " +
             formatAddress(target) +
             " fallita: " + failure.message);
        return ChainOpResult{ChainOpOutcome::Rejected, std::move(failure), {}, target};
    }

    // Rimozione riuscita: nessuna Underlying_Installation residua, currentHead
    // azzerato (nessun Chain_Head). Lo slot è ora vuoto e non installato; la
    // dismissione effettiva dalla mappa `chains_` è eseguita da `removeOwner`.
    slot.installed = false;
    set_head(slot.head, nullptr);

    // Diagnostica osservabile: rimozione anello (Req 11.2) + rimozione della
    // Underlying_Installation (Req 11.3).
    emit("hook-chaining: rimosso anello mod '" + owner + "' @ " +
         formatAddress(target) + " (ultimo anello)");
    emit("hook-chaining: rimossa Underlying_Installation @ " +
         formatAddress(target) + " (remove)");

    return ChainOpResult{ChainOpOutcome::RemovedLastInstall, HookError{}, {}, target};
}

void HookChainRegistry::teardown(const std::vector<ModId>& reverseOrder) {
    std::lock_guard<std::mutex> guard(mutex_);

    // -----------------------------------------------------------------------
    // Teardown in ordine inverso con isolamento dei fallimenti (Req 7.1-7.6).
    //
    // Il chiamante passa l'inverso di `LoadPlan.order` (Req 7.1): si rimuovono
    // gli anelli delle mod nello stato Enabled in quell'ordine. Ogni mod è
    // smontata via `removeOwnerLocked` (variante non-locking: il `mutex_` è già
    // detenuto qui per l'intera sequenza, così non si tenta un secondo lock
    // dello stesso mutex non ricorsivo — niente deadlock). Per ciascuna catena
    // svuotata `removeOwnerLocked` instrada la transizione 1→0 (rimozione
    // dell'unica Underlying_Installation + ripristino byte-esatto via
    // Rollback_Store, Req 7.2, 7.3); le catene che conservano altri anelli
    // mantengono l'install (Req 5.5). Un install→remove dell'intera catena di N
    // anelli riporta così i byte identici allo stato pre-installazione (Req 7.5).
    //
    // Ogni fallimento di rimozione è ISOLATO: la causa è registrata con Mod_Id e
    // Target_Address e il teardown PROSEGUE con le mod restanti (Req 7.6),
    // senza propagare eccezioni.
    // -----------------------------------------------------------------------
    emit("hook-chaining: teardown avviato (" + std::to_string(reverseOrder.size()) +
         " mod, ordine inverso a LoadPlan.order)");

    for (const ModId& owner : reverseOrder) {
        const std::vector<ChainOpResult> results = removeOwnerLocked(owner);
        for (const ChainOpResult& result : results) {
            if (result.outcome == ChainOpOutcome::Rejected) {
                // Fallimento isolato della rimozione di un anello/installazione:
                // registra la causa con Mod_Id + Target_Address e prosegui
                // (Req 7.6).
                emit("hook-chaining: teardown — fallimento isolato per mod '" +
                     owner + "' @ " + formatAddress(result.target) + ": " +
                     result.error.message + " (proseguo con le mod restanti)");
            }
        }
    }

    // Al termine, in assenza di fallimenti, non resta alcuna
    // Underlying_Installation né alcun anello residuo (Req 7.4). Eventuali
    // ChainSlot residui derivano esclusivamente da fallimenti isolati già
    // registrati sopra (non vengono dismessi così lo stato d'errore resta
    // osservabile dal chiamante). Le catene completamente smontate sono già
    // state rimosse dalla mappa `chains_` da `removeOwnerLocked`.
    emit("hook-chaining: teardown completato (" +
         std::to_string(installCountLocked()) + " install residue)");
}

// ---------------------------------------------------------------------------
// Introspezione/diagnostica.
// ---------------------------------------------------------------------------

ChainOrderView HookChainRegistry::chainOrder(std::uintptr_t target) const {
    std::lock_guard<std::mutex> guard(mutex_);
    ChainOrderView order;
    if (auto it = chains_.find(target); it != chains_.end()) {
        const auto& nodes = it->second.chain.orderedNodes();
        order.reserve(nodes.size());
        for (const auto& node : nodes) {
            order.push_back(node.owner);
        }
    }
    return order;
}

std::size_t HookChainRegistry::installCount() const noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    return installCountLocked();
}

std::size_t HookChainRegistry::installCountLocked() const noexcept {
    std::size_t count = 0;
    for (const auto& [address, slot] : chains_) {
        if (slot.installed) {
            ++count;
        }
    }
    return count;
}

std::size_t HookChainRegistry::chainSize(std::uintptr_t target) const {
    std::lock_guard<std::mutex> guard(mutex_);
    if (auto it = chains_.find(target); it != chains_.end()) {
        return it->second.chain.size();
    }
    return 0;
}

bool HookChainRegistry::hasInstall(std::uintptr_t target) const {
    std::lock_guard<std::mutex> guard(mutex_);
    if (auto it = chains_.find(target); it != chains_.end()) {
        return it->second.installed;
    }
    return false;
}

void* HookChainRegistry::currentHead(std::uintptr_t target) const {
    std::lock_guard<std::mutex> guard(mutex_);
    if (auto it = chains_.find(target); it != chains_.end()) {
        return it->second.head.currentHead.load(std::memory_order_acquire);
    }
    return nullptr;
}

void* HookChainRegistry::realTrampoline(std::uintptr_t target) const {
    std::lock_guard<std::mutex> guard(mutex_);
    if (auto it = chains_.find(target); it != chains_.end() && it->second.installed) {
        return it->second.realTrampoline.address();
    }
    return nullptr;
}

bool HookChainRegistry::slotInvariantsHold(const ChainSlot& slot) const noexcept {
    const auto& nodes = slot.chain.orderedNodes();

    // installed == (chain non vuota): la Underlying_Installation esiste sse la
    // catena ha almeno un anello (Req 1.3, 1.4, 5.5). Di conseguenza il numero
    // di DobbyHook sull'indirizzo è (installed ? 1 : 0).
    if (slot.installed != !nodes.empty()) {
        return false;
    }
    if (nodes.empty()) {
        return true;  // catena vuota: nessun cablaggio da verificare.
    }

    // installed ⇒ currentHead == detour del Chain_Head (Req 3.1, 4.3, 5.2):
    // l'Head_Thunk salta sempre alla testa corrente.
    if (slot.head.currentHead.load(std::memory_order_acquire) != nodes.front().detour) {
        return false;
    }

    // Relinking dei vicini: per ogni anello non-coda lo slot punta al detour del
    // successore (Req 4.2, 5.1). Lo slot è letto con load atomico acquire,
    // simmetrico allo store release del relinking (Req 9.1). Gli slot nulli
    // (anelli host di solo ordinamento) sono ignorati: non partecipano al
    // cablaggio reale.
    const std::size_t n = nodes.size();
    for (std::size_t i = 0; i + 1 < n; ++i) {
        if (nodes[i].slot != nullptr &&
            load_slot(nodes[i].slot) != nodes[i + 1].detour) {
            return false;
        }
    }

    // L'ultimo anello (coda) inoltra al Real_Trampoline (Req 3.4).
    if (nodes.back().slot != nullptr &&
        load_slot(nodes.back().slot) != slot.realTrampoline.address()) {
        return false;
    }

    return true;
}

}  // namespace pulse::hooking
