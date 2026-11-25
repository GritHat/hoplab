title      Anatomy of BBR: a model of the path instead of a reaction to loss
title[it]  Anatomia di BBR: un modello del percorso invece di una reazione alle perdite
level  4
layer  4
topic  tcp
about  BBR keeps two numbers about the path: BtlBw, the largest delivery rate seen in the last
about  ten round trips, and RTprop, the smallest round-trip time seen in the last ten seconds.
about  It paces its packets at a gain times BtlBw and caps the data in flight at a multiple of
about  BtlBw x RTprop. This lab shows its four states one after the other. STARTUP doubles the
about  sending rate every round trip until the delivery rate stops growing for three rounds;
about  DRAIN sends slower than the bottleneck to empty the queue that this built; PROBE_BW
about  cruises at the bottleneck rate with an eight-phase cycle (one phase at 1.25 to look for
about  more bandwidth, one at 0.75 to drain what the probe left, six at 1); and after ten
about  seconds without a new minimum RTT, PROBE_RTT shrinks the window to four segments for
about  200 ms so that the queue empties and the true RTT shows again.
about
about  Twice the path changes under it: at 14 s the bottleneck halves, at 22 s it comes back.
about  Watch in the Graphs view of h1 how long each estimate takes to follow: the bandwidth
about  filter forgets the old maximum only after ten round trips, during which BBR overdrives
about  the link and the queue of r1 grows; the way up is found by the 1.25 probing phases, 25%
about  per cycle. The tcp panel of h1 shows the state, BtlBw, RTprop and the pacing gain; the
about  log at detail 2 names every transition. Try `queue` 5 (shallow buffer: STARTUP loses
about  packets, yet BBR does not slow down for it) and 300 (deep buffer: no loss at all);
about  `cc` cubic on the same path for contrast (look at the rtt series and at the queue).
about[it]  BBR tiene due numeri sul percorso: BtlBw, la massima velocità di consegna vista
about[it]  negli ultimi dieci giri, e RTprop, il minimo tempo di andata e ritorno visto negli
about[it]  ultimi dieci secondi. Trasmette i pacchetti al ritmo di un guadagno per BtlBw e
about[it]  limita i dati in volo a un multiplo di BtlBw x RTprop. Questo laboratorio mostra i
about[it]  suoi quattro stati uno dopo l'altro. STARTUP raddoppia la velocità di invio a ogni
about[it]  giro finché la velocità di consegna smette di crescere per tre giri; DRAIN trasmette
about[it]  più piano del collo di bottiglia per svuotare la coda così creata; PROBE_BW viaggia
about[it]  alla velocità del collo di bottiglia con un ciclo di otto fasi (una a 1,25 per
about[it]  cercare più banda, una a 0,75 per smaltire ciò che la sonda ha lasciato, sei a 1);
about[it]  e dopo dieci secondi senza un nuovo RTT minimo, PROBE_RTT riduce la finestra a
about[it]  quattro segmenti per 200 ms, così la coda si svuota e l'RTT vero torna visibile.
about[it]
about[it]  Due volte il percorso gli cambia sotto: a 14 s il collo di bottiglia si dimezza, a
about[it]  22 s torna com'era. Osserva nella vista Grafici di h1 quanto impiega ogni stima a
about[it]  seguirlo: il filtro della banda dimentica il vecchio massimo solo dopo dieci giri,
about[it]  durante i quali BBR sovraccarica il collegamento e la coda di r1 cresce; la salita
about[it]  viene trovata dalle fasi di sonda a 1,25, il 25% per ciclo. Il pannello tcp di h1
about[it]  mostra lo stato, BtlBw, RTprop e il guadagno di pacing; il registro a dettaglio 2
about[it]  nomina ogni transizione. Prova `queue` 5 (buffer piccolo: STARTUP perde pacchetti,
about[it]  eppure BBR non rallenta per questo) e 300 (buffer profondo: nessuna perdita);
about[it]  `cc` cubic sullo stesso percorso per contrasto (guarda la serie rtt e la coda).
param  queue 60 Bottleneck queue (packets)
param[it] queue Coda del collo di bottiglia (pacchetti)
param  cc bbr Congestion control (bbr; cubic or reno for contrast)
param[it] cc Controllo di congestione (bbr; cubic o reno per contrasto)
param+ bw 4M Bottleneck rate
param[it] bw Velocità del collo di bottiglia
param+ bw2 2M Bottleneck rate between 14 s and 22 s
param[it] bw2 Velocità del collo di bottiglia fra 14 s e 22 s
param+ delay 20ms One-way delay of the bottleneck
param[it] delay Ritardo del collo di bottiglia (una direzione)

node h1 host   x=8  y=50
node r1 router x=36 y=50
node r2 router x=64 y=50
node h2 host   x=92 y=50
link h1 r1 delay=1ms bw=100M
link r1 r2 delay=$delay bw=$bw queue=$queue
link r2 h2 delay=1ms bw=100M
auto all

run h2 tcp sink
at 1s run h1 tcp send h2 duration=30s cc=$cc sndbuf=1M
at 14s link r1 r2 bw=$bw2
at 22s link r1 r2 bw=$bw

note 1s STARTUP: the pacing rate doubles every round trip (gain 2.89) while the delivery rate keeps growing.
note[it] 1s STARTUP: la velocità di invio raddoppia a ogni giro (guadagno 2,89) finché la velocità di consegna continua a crescere.
note 2s The rate stopped growing: DRAIN empties the queue, then PROBE_BW cruises at the bottleneck rate.
note[it] 2s La velocità ha smesso di crescere: DRAIN svuota la coda, poi PROBE_BW viaggia alla velocità del collo di bottiglia.
note 11s Ten seconds without a new minimum RTT: PROBE_RTT, four segments in flight for 200 ms.
note[it] 11s Dieci secondi senza un nuovo RTT minimo: PROBE_RTT, quattro segmenti in volo per 200 ms.
note 14s The bottleneck halves. BtlBw is a maximum over ten round trips: for that long BBR sends too fast.
note[it] 14s Il collo di bottiglia si dimezza. BtlBw è un massimo su dieci giri: per tutto quel tempo BBR trasmette troppo in fretta.
note 22s The bottleneck is back. Each 1.25 probing phase finds 25% more.
note[it] 22s Il collo di bottiglia è tornato. Ogni fase di sonda a 1,25 trova il 25% in più.

duration 40s
expect h1 tcp.send_done == 1
expect h1 tcp.bbr_drain >= 1 if $cc == bbr
expect h1 tcp.bbr_probe_bw >= 2 if $cc == bbr
expect h1 tcp.bbr_probe_rtt >= 1 if $cc == bbr
# a loss-based controller has none of these states: it has no model of the
# path, fills the queue until it overflows and backs off on the losses
expect h1 tcp.bbr_drain == 0 if $cc != bbr
expect h1 tcp.bbr_probe_bw == 0 if $cc != bbr
expect h1 tcp.bbr_probe_rtt == 0 if $cc != bbr
expect h1 tcp.fast_retransmits >= 1 if $cc != bbr
expect h1 tcp.steady_kbps >= 2000
