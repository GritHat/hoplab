title  Lamport clocks: putting events in order without a shared clock
title[it] Orologi di Lamport: ordinare gli eventi senza un orologio comune
level  2
layer  8
topic  clock
about  Three processes exchange messages over links with different delays. Each keeps a Lamport clock: one counter, incremented at every event and pushed forward by the timestamps it receives.
about  Watch the log: every event shows the clock after it. A receive event always gets a timestamp larger than the matching send, however long the message travelled; n3 sits idle and still jumps ahead when the first message reaches it.
about  Try: switch mode to vector and see the log say which events are concurrent; raise the delay of n3 and check that timestamps still respect causality.
about[it] Tre processi si scambiano messaggi su collegamenti con ritardi diversi. Ognuno tiene un orologio di Lamport: un contatore, incrementato a ogni evento e spinto in avanti dai timestamp che riceve.
about[it] Osserva il registro: ogni evento mostra l'orologio dopo l'evento. Una ricezione ha sempre un timestamp maggiore dell'invio corrispondente, per quanto a lungo abbia viaggiato il messaggio; n3 resta fermo e fa comunque un salto in avanti quando lo raggiunge il primo messaggio.
about[it] Prova: imposta mode a vector e guarda il registro indicare quali eventi sono concorrenti; aumenta il ritardo di n3 e verifica che i timestamp rispettino ancora la causalità.
param mode lamport Clock: lamport or vector
param[it] mode Orologio: lamport o vector
param+ slow 80ms Delay of the link of n3
param[it] slow Ritardo del collegamento di n3

node n1 host
node n2 host
node n3 host
node s1 switch
link n1 s1 delay=5ms
link n2 s1 delay=10ms
link n3 s1 delay=$slow
auto all

run n1 clock peers=n1,n2,n3 mode=$mode
run n2 clock peers=n1,n2,n3 mode=$mode
run n3 clock peers=n1,n2,n3 mode=$mode

note 1s n1 has three local events: its clock runs ahead of the others, which are still at zero.
note[it] 1s n1 ha tre eventi locali: il suo orologio corre avanti rispetto agli altri, ancora a zero.
at 1s   run n1 clock event
at 1.1s run n1 clock event
at 1.2s run n1 clock event
note 2s n1 sends to n2: the receive event must be later than the send, so n2 jumps past the timestamp it reads in the message.
note[it] 2s n1 invia a n2: la ricezione deve essere successiva all'invio, quindi n2 salta oltre il timestamp letto nel messaggio.
at 2s   run n1 clock send n2 text=a
at 3s   run n2 clock event
note 4s n2 sends to n3 over the slow link. The chain n1 → n2 → n3 is causal: the timestamps along it only grow.
note[it] 4s n2 invia a n3 sul collegamento lento. La catena n1 → n2 → n3 è causale: i timestamp lungo di essa possono solo crescere.
at 4s   run n2 clock send n3 text=b
at 5s   run n3 clock send n1 text=c
note 6s n1 and n3 act at the same instant without having heard of each other: concurrent events. Lamport clocks give them some numbers anyway; only vector clocks can tell they are concurrent.
note[it] 6s n1 e n3 agiscono nello stesso istante senza sapere l'uno dell'altro: eventi concorrenti. Gli orologi di Lamport assegnano comunque dei numeri; solo gli orologi vettoriali sanno dire che sono concorrenti.
at 6s   run n1 clock event
at 6s   run n3 clock event
duration 8s

expect n1 clock.received == 1
expect n2 clock.received == 1
expect n3 clock.received == 1
expect n2 clock.lamport >= 5
expect n3 clock.lamport >= 8
expect n1 clock.lamport >= 10
