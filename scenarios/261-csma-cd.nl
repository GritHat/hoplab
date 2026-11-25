title      Listen before talking: CSMA and Ethernet's CSMA/CD
title[it]  Ascoltare prima di parlare: CSMA e il CSMA/CD di Ethernet
level  2
layer  2
topic  mac
about  Three laboratories, same stations and same heavy load, three disciplines. `nonpers` senses the channel and, if it is busy, goes away for a random time (non-persistent CSMA). `pers` keeps listening and transmits the instant the channel is free (1-persistent CSMA): everyone who was waiting starts together and collides. `ethernet` is 1-persistent too, but listens while it transmits: a collision is noticed within a couple of propagation delays, the frame is aborted with a short jam, and the retry is delayed by a random number of slots from a window that doubles at each collision (binary exponential backoff).
about  In the mac panels compare the red areas: a collision costs a whole frame time in the first two labs, a sliver in `ethernet`. Carrier sensing can still fail, because a signal needs time to arrive: the parameter prop is that propagation delay, and a = prop/frame decides how good CSMA can be.
about  Try: prop=10ms (a = 0.1: long cable, more collisions, CSMA/CD loses its edge; frame=10ms does the same from the other side, since only the ratio a counts); prop=0.2ms; load=0.3 (all three are fine) and load=2 (overload: who degrades gracefully?).
about[it]  Tre laboratori, stesse stazioni e stesso carico pesante, tre discipline. `nonpers` ascolta il canale e, se è occupato, se ne va per un tempo casuale (CSMA non persistente). `pers` resta in ascolto e trasmette nell'istante in cui il canale è libero (CSMA 1-persistente): tutti quelli che aspettavano partono insieme e collidono. Anche `ethernet` è 1-persistente, ma ascolta mentre trasmette: una collisione si nota entro un paio di ritardi di propagazione, il frame viene interrotto con un breve jam e il nuovo tentativo è ritardato di un numero casuale di slot preso da una finestra che raddoppia a ogni collisione (backoff esponenziale binario).
about[it]  Nei pannelli mac confronta le zone rosse: una collisione costa un intero tempo di frame nei primi due laboratori, una scheggia in `ethernet`. Il carrier sensing può comunque fallire, perché un segnale impiega tempo ad arrivare: il parametro prop è quel ritardo di propagazione, e a = prop/frame decide quanto può essere efficace CSMA.
about[it]  Prova: prop=10ms (a = 0,1: cavo lungo, più collisioni, CSMA/CD perde il vantaggio; frame=10ms fa lo stesso dall'altro lato, perché conta solo il rapporto a); prop=0.2ms; load=0.3 (vanno bene tutti e tre) e load=2 (sovraccarico: chi degrada con grazia?).
param      load 0.9 Offered load (new frames per frame time, all stations)
param[it]  load Carico offerto (nuovi frame per tempo di frame, tutte le stazioni)
param      prop 1ms Propagation delay between stations
param[it]  prop Ritardo di propagazione fra le stazioni
param+     stations 10 Number of stations
param+[it] stations Numero di stazioni
param+     frame 100ms Duration of one frame
param+[it] frame Durata di un frame

node nonpers host x=20 y=50 role=CSMA
node pers host x=50 y=50 role=CSMA-1
node ethernet host x=80 y=50 role=CSMA/CD

run nonpers mac proto=csma stations=$stations load=$load frame=$frame prop=$prop
run pers mac proto=csma1 stations=$stations load=$load frame=$frame prop=$prop
run ethernet mac proto=csmacd stations=$stations load=$load frame=$frame prop=$prop

note 1s      Three ways to behave when the channel is busy. Open the mac panel of each node in turn.
note[it] 1s  Tre modi di comportarsi quando il canale è occupato. Apri il pannello mac di ciascun nodo, uno alla volta.
note 12s     `pers`: after every frame, all the stations that were waiting transmit together. Red follows green.
note[it] 12s `pers`: dopo ogni frame tutte le stazioni in attesa trasmettono insieme. Al verde segue il rosso.
note 25s     `ethernet`: collisions still happen, but they are thin: detected and aborted at once, then exponential backoff.
note[it] 25s `ethernet`: le collisioni ci sono ancora ma sono sottili: rilevate e interrotte subito, poi backoff esponenziale.
note 45s     Compare S in the three panels with the 18% and 37% of the ALOHA lab.
note[it] 45s Confronta S nei tre pannelli con il 18% e il 37% del laboratorio ALOHA.

duration 60s
# light load: every discipline carries what is offered
at 59900ms run ethernet check light "$load > 0.5 || (nonpers:mac.throughput_pct >= 75 * $load && pers:mac.throughput_pct >= 75 * $load && ethernet:mac.throughput_pct >= 75 * $load)"
# heavy load on a short cable (a = prop/frame up to 0.02): CSMA/CD is the best, 1-persistent the worst
at 59900ms run ethernet check heavy "$load < 0.8 || $prop / $frame > 0.02 || (ethernet:mac.throughput_pct >= 70 && nonpers:mac.throughput_pct >= 55 && pers:mac.throughput_pct <= 68 && pers:mac.collisions >= 20 && pers:mac.throughput_pct < ethernet:mac.throughput_pct)"
# long cable (a from 0.08): collisions are found late, and CSMA/CD loses its edge over polite waiting
at 59900ms run ethernet check long "$load < 0.8 || $prop / $frame < 0.08 || ethernet:mac.throughput_pct <= nonpers:mac.throughput_pct + 5"
expect ethernet check.light == 1
expect ethernet check.heavy == 1
expect ethernet check.long == 1
