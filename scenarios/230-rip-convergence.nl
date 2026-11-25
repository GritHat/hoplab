title      RIP: routers discover the network, then route around a failure
title[it]  RIP: i router scoprono la rete, poi aggirano un guasto
level  2
layer  3
topic  rip
about  Four routers in a ring run RIP; a host hangs off r1 and another off r3. Nobody is told the
about  topology: every router only repeats to its neighbors "I reach these networks in N hops", and
about  after a couple of rounds everyone has a full table. Watch the Routes of r1 fill up and the
about  metric (hop count) of each entry. At 20 s the link r1-r2 fails: r1 marks what it reached
about  through r2 as unreachable (metric 16), triggered updates spread the news, and the traffic to h3
about  moves to the other side of the ring through r4.
about
about  Try: triggered=0 to see how slow bad news travel with periodic updates only; a longer update
about  period; `link r1 r2 up` in the console to watch the shorter path come back.
about[it]  Quattro router ad anello usano RIP; un host è collegato a r1 e uno a r3. Nessuno conosce la
about[it]  topologia: ogni router ripete ai vicini soltanto "raggiungo queste reti in N salti", e dopo
about[it]  un paio di giri tutti hanno la tabella completa. Osserva le rotte di r1 che si riempiono e la
about[it]  metrica (numero di salti) di ogni voce. A 20 s il collegamento r1-r2 si guasta: r1 segna come
about[it]  irraggiungibile (metrica 16) ciò che raggiungeva tramite r2, gli aggiornamenti immediati
about[it]  diffondono la notizia e il traffico verso h3 passa dall'altro lato dell'anello, via r4.
about[it]
about[it]  Prova: triggered=0 per vedere quanto viaggiano lente le cattive notizie con i soli
about[it]  aggiornamenti periodici; un periodo più lungo; `link r1 r2 up` nella console per veder
about[it]  tornare il percorso più corto.
param  update 5s Period of the RIP updates
param[it] update Periodo degli aggiornamenti RIP
param+ triggered 1 Triggered updates (1 = on)
param+[it] triggered Aggiornamenti immediati (1 = attivi)
param+ poison 0 Poisoned reverse (1 = on)
param+[it] poison Poisoned reverse (1 = attivo)

node h1 host   x=5  y=50
node r1 router x=25 y=50
node r2 router x=50 y=15
node r3 router x=75 y=50
node r4 router x=50 y=85
node h3 host   x=95 y=50
link h1 r1 delay=2ms
link r1 r2 delay=5ms
link r2 r3 delay=5ms
link r3 r4 delay=5ms
link r4 r1 delay=5ms
link r3 h3 delay=2ms
auto addr

run r1 rip update=$update triggered=$triggered poison=$poison
run r2 rip update=$update triggered=$triggered poison=$poison
run r3 rip update=$update triggered=$triggered poison=$poison
run r4 rip update=$update triggered=$triggered poison=$poison

note 0s  Each router starts knowing only its own networks and advertises them to its neighbors.
note[it] 0s  Ogni router parte conoscendo solo le proprie reti e le annuncia ai vicini.
note 12s The tables are complete: h1 reaches h3 in 3 router hops (r1, r2 or r4, r3).
note[it] 12s Le tabelle sono complete: h1 raggiunge h3 attraversando 3 router (r1, r2 oppure r4, r3).
note 20s Link r1-r2 fails. r1 and r2 poison the routes that used it and tell their neighbors at once.
note[it] 20s Il collegamento r1-r2 si guasta. r1 e r2 dichiarano irraggiungibili le rotte che lo usavano e avvisano subito i vicini.
note 40s RIP has reconverged: the path now goes around the ring through r4.
note[it] 40s RIP ha riconverguto: ora il percorso fa il giro dell'anello passando da r4.

at 12s run h1 ping h3 count=3
at 16s run h1 traceroute h3
at 20s link r1 r2 down
at 40s run h3 ping h1 count=3
at 45s run h1 traceroute h3
duration 52s
expect h1 ping.reply == 3
expect h3 ping.reply == 3
expect h1 traceroute.hops == 4
expect r1 rip.routes >= 5
