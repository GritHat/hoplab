title      RIP: count to infinity, and how split horizon prevents it
title[it]  RIP: il conteggio all'infinito, e come lo evita lo split horizon
level  2
layer  3
topic  rip
about  Three routers in a line: h1 - r1 - r2 - r3 - h3. At 12 s the link r2-r3 fails and the network
about  of h3 is gone. By default this lab runs RIP WITHOUT its protections: r2 drops the route, but r1
about  still advertises "I reach it in 2 hops" (a route that went through r2 itself!). r2 believes it,
about  r1 then hears r2's worse metric and raises its own, and the two count up, one step per update,
about  until 16 = infinity. Meanwhile packets for h3 bounce between r1 and r2 until their TTL expires:
about  the ping from h1 reports "time to live exceeded".
about
about  (Whether the loop forms is a race: it does when r1's periodic update reaches r2 before r2's
about  bad news reach r1. Here the update timers are phased so that it always happens.)
about
about  Now rerun with split=1 (never advertise a route back where you learned it): r1 stays silent
about  about that network and the loop never forms. Add triggered=1 and the bad news reach r1 in
about  milliseconds instead of waiting for the next periodic update.
about[it]  Tre router in fila: h1 - r1 - r2 - r3 - h3. A 12 s il collegamento r2-r3 si guasta e la rete
about[it]  di h3 sparisce. Di default questo laboratorio usa RIP SENZA le sue protezioni: r2 elimina la
about[it]  rotta, ma r1 continua ad annunciare "la raggiungo in 2 salti" (una rotta che passava proprio
about[it]  da r2!). r2 ci crede, r1 sente poi la metrica peggiore di r2 e alza la propria, e i due
about[it]  contano, un passo per aggiornamento, fino a 16 = infinito. Intanto i pacchetti per h3
about[it]  rimbalzano fra r1 e r2 finché il TTL scade: il ping da h1 riporta "time to live exceeded".
about[it]
about[it]  (Che il ciclo si formi è una gara: succede quando l'aggiornamento periodico di r1 arriva a
about[it]  r2 prima che le cattive notizie di r2 arrivino a r1. Qui i timer sono sfasati in modo che
about[it]  accada sempre.)
about[it]
about[it]  Riprova con split=1 (mai annunciare una rotta verso chi te l'ha insegnata): r1 tace su
about[it]  quella rete e il ciclo non si forma. Aggiungi triggered=1 e le cattive notizie arrivano a
about[it]  r1 in millisecondi invece di aspettare il prossimo aggiornamento periodico.
param  split 0 Split horizon (0 = off: count to infinity)
param[it] split Split horizon (0 = spento: conteggio all'infinito)
param  triggered 0 Triggered updates (1 = on)
param[it] triggered Aggiornamenti immediati (1 = attivi)
param+ poison 0 Poisoned reverse (needs split=1)
param+[it] poison Poisoned reverse (richiede split=1)

node h1 host   x=5  y=50
node r1 router x=28 y=50
node r2 router x=50 y=50
node r3 router x=72 y=50
node h3 host   x=95 y=50
link h1 r1 delay=2ms
link r1 r2 delay=5ms
link r2 r3 delay=5ms
link r3 h3 delay=2ms
auto addr

run r1 rip update=2s split=$split triggered=$triggered poison=$poison phase=500ms jitter=0
run r2 rip update=2s split=$split triggered=$triggered poison=$poison phase=1500ms jitter=0
run r3 rip update=2s split=$split triggered=$triggered poison=$poison

note 0s  The three routers exchange their tables; r1 learns the network of h3 at 2 hops via r2.
note[it] 0s  I tre router si scambiano le tabelle; r1 impara la rete di h3 a 2 salti tramite r2.
note 12s Link r2-r3 fails: r2 loses its route to the network of h3. Watch what r1 tells r2 next.
note[it] 12s Il collegamento r2-r3 si guasta: r2 perde la rotta verso la rete di h3. Osserva cosa dice ora r1 a r2.
note 16s Without split horizon r1 and r2 point at each other: the metric climbs, packets loop until TTL 0.
note[it] 16s Senza split horizon r1 e r2 si indicano a vicenda: la metrica sale, i pacchetti girano finché il TTL arriva a 0.
note 26s Metric 16 reached: the route is finally declared unreachable everywhere.
note[it] 26s Raggiunta la metrica 16: la rotta è finalmente dichiarata irraggiungibile ovunque.

at 8s  run h1 ping h3 count=2
at 12s link r2 r3 down
at 17s run h1 ping h3 count=2 interval=2s
duration 60s
expect h1 ping.reply == 2
expect r2 rip.counting >= 3
expect r1 rip.counting >= 3
expect h1 ping.error >= 1
expect r1 rip.routes == 2
