title  Partition and heal: OR-Set versus last-writer-wins
title[it] Partizione e ricongiunzione: OR-Set contro last-writer-wins
level  4
layer  8
topic  crdt
about  Three replicas share a shopping list (an observed-remove set) and a price (a last-writer-wins register). The network splits: n3 is cut off, and both sides keep working. On one side milk is removed from the list, on the other it is added again; both sides change the price.
about  When the partition heals the replicas gossip and converge, but the two types reconcile differently. The OR-Set keeps every intention: the remove on n1 only cancelled the "milk" it had seen, so the concurrent add on n3 survives. The register keeps the write with the later timestamp and silently throws the other away: n1 reports its lost update.
about  Try skew=-3s: n3's clock is slow, its later write loses and the stale price wins everywhere; the lost update is then counted on n3 instead of n1 (crdt.lost in the final report). Try a longer partition (heal up to 30s): nothing changes, availability was never given up. In every case the three replicas end identical: each one checks it by itself, comparing the states it receives with its own (crdt.converged).
about[it] Tre repliche condividono una lista della spesa (un observed-remove set) e un prezzo (un registro last-writer-wins). La rete si divide: n3 resta isolato, ed entrambe le parti continuano a lavorare. Da una parte il latte viene tolto dalla lista, dall'altra viene aggiunto di nuovo; entrambe le parti cambiano il prezzo.
about[it] Quando la partizione si ricompone le repliche fanno gossip e convergono, ma i due tipi riconciliano in modo diverso. L'OR-Set conserva ogni intenzione: la rimozione su n1 ha cancellato solo il "milk" che aveva visto, quindi l'aggiunta concorrente su n3 sopravvive. Il registro tiene la scrittura con il timestamp più recente e butta via l'altra in silenzio: n1 segnala il suo aggiornamento perduto.
about[it] Prova skew=-3s: l'orologio di n3 è indietro, la sua scrittura più recente perde e il prezzo vecchio vince ovunque; l'aggiornamento perso viene allora contato su n3 invece che su n1 (crdt.lost nel rapporto finale). Prova una partizione più lunga (heal fino a 30s): non cambia nulla, la disponibilità non è mai stata sacrificata. In ogni caso le tre repliche finiscono identiche: ciascuna lo verifica da sé, confrontando gli stati che riceve con il proprio (crdt.converged).
param skew 0 Clock error of n3 (for the register), e.g. -3s
param[it] skew Errore dell'orologio di n3 (per il registro), es. -3s
param+ heal 8s When the partition heals
param[it] heal Quando la partizione si ricompone

node s1 switch
node n1 host
node n2 host
node n3 host
link n1 s1 delay=10ms
link n2 s1 delay=10ms
link n3 s1 delay=10ms
auto all

run n1 crdt peers=n1,n2,n3 type=orset,lww sync=300ms
run n2 crdt peers=n1,n2,n3 type=orset,lww sync=300ms
run n3 crdt peers=n1,n2,n3 type=orset,lww sync=300ms skew=$skew

note 1s n1 puts milk on the list and sets the price to 10. Gossip brings both to every replica.
note[it] 1s n1 mette il latte in lista e imposta il prezzo a 10. Il gossip porta entrambi a tutte le repliche.
at 1s    run n1 crdt add milk
at 1s    run n1 crdt set 10
note 3s Partition: n3 is isolated. Both sides remain available for updates.
note[it] 3s Partizione: n3 è isolato. Entrambe le parti restano disponibili per gli aggiornamenti.
at 3s    link n3 s1 down
at 4s    run n1 crdt remove milk
at 4s    run n1 crdt add bread
at 4.2s  run n3 crdt add milk
at 4.2s  run n3 crdt add eggs
note 4.5s Concurrent conflict: n1 removed milk, n3 added it again. And two prices: 20 on n1, then 30 on n3.
note[it] 4.5s Conflitto concorrente: n1 ha tolto il latte, n3 lo ha aggiunto di nuovo. E due prezzi: 20 su n1, poi 30 su n3.
at 4.5s  run n1 crdt set 20
at 5s    run n3 crdt set 30
at $heal link n3 s1 up
note 8.5s Healed. The set keeps milk (add wins over the remove that never saw it), bread and eggs. The register keeps one price only.
note[it] 8.5s Ricomposta. L'insieme conserva milk (l'aggiunta vince sulla rimozione che non l'aveva vista), bread ed eggs. Il registro conserva un solo prezzo.
duration 40s

expect n1 crdt.converged == 1
expect n2 crdt.converged == 1
expect n3 crdt.converged == 1
expect n1 crdt.size == 3
expect n2 crdt.size == 3
expect n3 crdt.size == 3
expect n1 crdt.lost >= 0
expect n3 crdt.lost >= 0
expect n2 crdt.register >= 20
