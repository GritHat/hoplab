title      Throughput against load: the S(G) curves of every access protocol
title[it]  Throughput in funzione del carico: le curve S(G) di tutti i protocolli di accesso
level  3
layer  2
topic  mac
about  The classic figure of every networking textbook, measured instead of drawn. For each protocol the bench simulates nine offered loads and reports the attempt rate G (frames per frame time), the throughput S and, where a closed form exists (Abramson for ALOHA, Kleinrock and Tobagi for CSMA), the theoretical value at that G.
about  Read the tables in the log or the bars in the bench's mac panel (the yellow mark is the theory). Things to notice: ALOHA peaks at G = 0.5 and slotted ALOHA at G = 1, then both fall as collisions take over; non-persistent CSMA keeps climbing with G where 1-persistent turns down early; CSMA/CD holds near 1 under overload because collisions are cheap; CSMA/CA pays a fixed price in idle slots and acknowledgements. The models assume infinitely many stations and Poisson attempts: with ten stations and queues the agreement is in shape and within a few percent, not exact.
about  Try: prop=10ms (a = 0.1) and watch every CSMA curve drop while the ALOHA ones do not move; stations=3 or 24; `bench mac sweep proto=csmap p=0.01`.
about[it]  La figura classica di ogni libro di reti, misurata invece che disegnata. Per ogni protocollo il banco simula nove carichi offerti e riporta il tasso di tentativi G (frame per tempo di frame), il throughput S e, dove esiste una formula chiusa (Abramson per ALOHA, Kleinrock e Tobagi per CSMA), il valore teorico per quel G.
about[it]  Leggi le tabelle nel registro o le barre nel pannello mac del banco (il segno giallo è la teoria). Da notare: ALOHA ha il massimo a G = 0,5 e slotted ALOHA a G = 1, poi entrambi calano quando prevalgono le collisioni; il CSMA non persistente continua a salire con G mentre l'1-persistente piega presto; CSMA/CD resta vicino a 1 in sovraccarico perché le collisioni costano poco; CSMA/CA paga un prezzo fisso in slot vuoti e riscontri. I modelli assumono infinite stazioni e tentativi di Poisson: con dieci stazioni e code l'accordo è nella forma ed entro qualche punto percentuale, non esatto.
about[it]  Prova: prop=10ms (a = 0,1) e guarda scendere tutte le curve CSMA mentre quelle ALOHA non si muovono; stations=3 oppure 24; `bench mac sweep proto=csmap p=0.01`.
param      prop 1ms Propagation delay (frame time is 100 ms)
param[it]  prop Ritardo di propagazione (il tempo di frame è 100 ms)
param      stations 10 Number of stations
param[it]  stations Numero di stazioni

node bench host x=50 y=50

run bench mac sweep proto=aloha stations=$stations frame=100ms prop=$prop
at 2s  run bench mac sweep proto=slotted stations=$stations frame=100ms prop=$prop
at 4s  run bench mac sweep proto=csma stations=$stations frame=100ms prop=$prop
at 6s  run bench mac sweep proto=csma1 stations=$stations frame=100ms prop=$prop
at 8s  run bench mac sweep proto=csmap stations=$stations frame=100ms prop=$prop
at 10s run bench mac sweep proto=csmacd stations=$stations frame=100ms prop=$prop
at 12s run bench mac sweep proto=csmaca stations=$stations frame=100ms prop=$prop

note 0s      Pure ALOHA: S = G·e^(-2G). The best it can do is 18%, at G = 0.5.
note[it] 0s  ALOHA puro: S = G·e^(-2G). Il massimo è il 18%, a G = 0,5.
note 2s      Slotted ALOHA: S = G·e^(-G), twice as good, 37% at G = 1.
note[it] 2s  Slotted ALOHA: S = G·e^(-G), il doppio, 37% a G = 1.
note 4s      Non-persistent CSMA: listening first changes everything when the propagation delay is small.
note[it] 4s  CSMA non persistente: ascoltare prima cambia tutto quando il ritardo di propagazione è piccolo.
note 6s      1-persistent CSMA: greedy, good at light load, self-defeating at heavy load.
note[it] 6s  CSMA 1-persistente: avido, buono a carico leggero, controproducente a carico pesante.
note 10s     CSMA/CD: collisions cost almost nothing, so overload does not hurt.
note[it] 10s CSMA/CD: le collisioni non costano quasi nulla, quindi il sovraccarico non fa danni.
note 12s     CSMA/CA: no collision detection on radio; backoff slots and acknowledgements instead.
note[it] 12s CSMA/CA: in radio non si possono rilevare le collisioni; al loro posto slot di backoff e riscontri.

duration 14s
# the ALOHA peaks do not depend on the cable: 18% and 37% (a little more with few stations)
# (a handful of stations do a little better than the infinite-population 18.4%: N = 2 peaks at 25%)
at 13900ms run bench check aloha "bench:mac.sweeps == 7 && bench:mac.aloha.peak >= 12 && bench:mac.aloha.peak <= 23 + ($stations < 4) * 5 && bench:mac.slotted.peak >= 31 && bench:mac.slotted.peak <= 52"
# carrier sensing is as good as the cable is short: a = prop / 100 ms
at 13900ms run bench check short "$prop / 100ms > 0.02 || (bench:mac.csma.peak >= 62 && bench:mac.csmacd.peak >= 80 && bench:mac.csmaca.peak >= 60)"
# (with two or three stations there is seldom anybody to collide with, whatever the cable:
# the claims about long cables and about persistence are for a populated channel)
at 13900ms run bench check long "$prop / 100ms < 0.08 || $stations < 4 || (bench:mac.csmacd.peak <= 75 && bench:mac.csma.peak <= 75)"
# and listening always beats not listening
at 13900ms run bench check order "bench:mac.csma.peak > bench:mac.slotted.peak && ($stations < 4 || bench:mac.csmacd.peak > bench:mac.csma1.peak)"
expect bench check.aloha == 1
expect bench check.short == 1
expect bench check.long == 1
expect bench check.order == 1
