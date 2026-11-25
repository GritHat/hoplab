title  Vector clocks and causal broadcast: the answer that overtakes the question
title[it] Orologi vettoriali e broadcast causale: la risposta che supera la domanda
level  3
layer  8
topic  clock
about  Three processes in a triangle; the direct link between n1 and n3 is slow. n1 broadcasts a question, n2 reads it and broadcasts the answer. The answer reaches n3 through a fast link, before the question does.
about  The scene is played twice. First with plain broadcast (order=none): n3 delivers the answer before the question and the module counts a causality violation. Then with the order chosen by the parameter (causal by default): the dependency vector carried by the answer tells n3 that something is missing, the answer waits in the hold-back queue and is delivered right after the question.
about  Try: order2=fifo (no help here: the two messages have different senders) and order2=total (every process delivers in the same order, at the cost of waiting for the acknowledgements over the slow link).
about[it] Tre processi a triangolo; il collegamento diretto fra n1 e n3 è lento. n1 diffonde una domanda, n2 la legge e diffonde la risposta. La risposta raggiunge n3 su un collegamento veloce, prima della domanda.
about[it] La scena viene recitata due volte. Prima con il broadcast semplice (order=none): n3 consegna la risposta prima della domanda e il modulo conta una violazione di causalità. Poi con l'ordine scelto dal parametro (causal di default): il vettore delle dipendenze portato dalla risposta dice a n3 che manca qualcosa, la risposta aspetta nella coda di attesa ed è consegnata subito dopo la domanda.
about[it] Prova: order2=fifo (qui non aiuta: i due messaggi hanno mittenti diversi) e order2=total (tutti i processi consegnano nello stesso ordine, al prezzo di attendere i riscontri sul collegamento lento).
param order2 causal Broadcast order in the second round: none, fifo, causal, total
param[it] order2 Ordine del broadcast nel secondo giro: none, fifo, causal, total
param+ slow 150ms Delay of the slow link n1 — n3 (the answer overtakes the question above about 110 ms)
param[it] slow Ritardo del collegamento lento n1 — n3 (la risposta supera la domanda oltre circa 110 ms)

node n1 host x=15 y=75
node n2 host x=50 y=20
node n3 host x=85 y=75
link n1 n2 delay=10ms
link n2 n3 delay=10ms
link n1 n3 delay=$slow
auto addr

run n1 clock peers=n1,n2,n3 mode=vector order=none
run n2 clock peers=n1,n2,n3 mode=vector order=none
run n3 clock peers=n1,n2,n3 mode=vector order=none

# resolve the addresses first, so that ARP does not add its own delay to the first round
at 100ms run n1 ping n3:eth1 count=1
at 100ms run n2 ping n3 count=1
at 100ms run n1 ping n2 count=1
note 1s Round 1, no ordering. n1 broadcasts the question; it reaches n2 quickly, n3 slowly.
note[it] 1s Primo giro, nessun ordinamento. n1 diffonde la domanda; arriva presto a n2, lentamente a n3.
at 1s    run n1 clock bcast text="what time is it?"
at 1.1s  run n2 clock bcast text="half past nine"
note 1.2s n3 has just delivered the answer to a question it has not seen: a causality violation.
note[it] 1.2s n3 ha appena consegnato la risposta a una domanda che non ha visto: una violazione di causalità.

at 3s    run n1 clock order=$order2
at 3s    run n2 clock order=$order2
at 3s    run n3 clock order=$order2
note 4s Round 2, same messages with ordered broadcast. Watch n3: the answer is held back until the question arrives.
note[it] 4s Secondo giro, stessi messaggi con il broadcast ordinato. Osserva n3: la risposta è trattenuta finché non arriva la domanda.
at 4s    run n1 clock bcast text="and now?"
at 4.1s  run n2 clock bcast text="ten o'clock"
duration 7s

at 6900ms run n3 check delivered "n1:clock.delivered == 4 && n2:clock.delivered == 4 && n3:clock.delivered == 4 && n2:clock.violations == 0"
# the answer leaves 100 ms after the question and travels over fast links: it overtakes the question
# on its way to n3 only if the slow link takes longer than that
at 6900ms run n3 check overtaken "$slow >= 130ms"
# round 1 (no ordering) breaks causality once; round 2 does it again unless the order is causal or total
at 6900ms run n3 check causality "($slow > 90ms && $slow < 130ms) || (n3:check.overtaken == 0 && n3:clock.violations == 0 && n3:clock.heldback == 0) || (n3:check.overtaken == 1 && (((is_$order2 == is_none || is_$order2 == is_fifo) && n3:clock.violations == 2) || ((is_$order2 == is_causal || is_$order2 == is_total) && n3:clock.violations == 1)) && (is_$order2 != is_causal || n3:clock.heldback == 1))"
expect n3 check.delivered == 1
expect n3 check.causality == 1
