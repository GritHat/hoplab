title  A replicated counter that cannot lose increments (CRDT)
title[it] Un contatore replicato che non può perdere incrementi (CRDT)
level  3
layer  8
topic  crdt
about  Three replicas of a "likes" counter accept increments and decrements locally, at the same time, without asking anybody. Every half second each replica sends its whole state to one random peer, over links that lose packets.
about  Watch the panel: the counter is not one number but one pair (increments, decrements) per replica; merging takes the maximum of each entry and the value is the sum. Whatever the order of the messages, however many are lost or duplicated, the three replicas end with the same value: 2 + 1 + 3 − 1 = 5.
about  Try: loss=0.6 (convergence only takes longer), sync=2s, or a link down during the increments.
about[it] Tre repliche di un contatore di "mi piace" accettano incrementi e decrementi in locale, nello stesso momento, senza chiedere a nessuno. Ogni mezzo secondo ogni replica invia il suo intero stato a una replica scelta a caso, su collegamenti che perdono pacchetti.
about[it] Osserva il pannello: il contatore non è un numero ma una coppia (incrementi, decrementi) per replica; la fusione prende il massimo di ogni voce e il valore è la somma. Qualunque sia l'ordine dei messaggi, per quanti se ne perdano o duplichino, le tre repliche finiscono con lo stesso valore: 2 + 1 + 3 − 1 = 5.
about[it] Prova: loss=0.6 (la convergenza richiede solo più tempo), sync=2s, oppure un collegamento interrotto durante gli incrementi.
param loss 0.2 Packet loss on every link
param[it] loss Perdita di pacchetti su ogni collegamento
param sync 500ms Gossip period
param[it] sync Periodo del gossip

node s1 switch
node n1 host
node n2 host
node n3 host
link n1 s1 delay=10ms dup=0.1
link n2 s1 delay=10ms dup=0.1
link n3 s1 delay=10ms dup=0.1
# the links become lossy after one second, when every replica has resolved
# the addresses of the others: the lab is about the data, not about ARP
run n1 ping n2 count=1
run n1 ping n3 count=1
run n2 ping n3 count=1
at 1s link n1 s1 loss=$loss
at 1s link n2 s1 loss=$loss
at 1s link n3 s1 loss=$loss
auto all

run n1 crdt peers=n1,n2,n3 type=pncounter sync=$sync
run n2 crdt peers=n1,n2,n3 type=pncounter sync=$sync
run n3 crdt peers=n1,n2,n3 type=pncounter sync=$sync

note 2s Three concurrent updates on three replicas: +2 on n1, +1 on n2, +3 on n3. For a moment the replicas disagree.
note[it] 2s Tre aggiornamenti concorrenti su tre repliche: +2 su n1, +1 su n2, +3 su n3. Per un momento le repliche sono in disaccordo.
at 2s   run n1 crdt inc by=2
at 2s   run n2 crdt inc
at 2s   run n3 crdt inc by=3
note 4s Gossip spreads the per-replica totals; each merge can only move a replica towards the common value 6.
note[it] 4s Il gossip diffonde i totali per replica; ogni fusione può solo avvicinare una replica al valore comune 6.
at 6s   run n2 crdt dec
note 6s A decrement on n2: it is an increment of n2's "decrements" entry, so it merges like everything else.
note[it] 6s Un decremento su n2: è un incremento della voce "decrementi" di n2, quindi si fonde come tutto il resto.
duration 180s

expect n1 crdt.value == 5
expect n2 crdt.value == 5
expect n3 crdt.value == 5
expect n1 crdt.converged == 1
expect n2 crdt.converged == 1
expect n3 crdt.converged == 1
