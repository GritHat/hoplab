title      Congestion control on a bottleneck: the sawtooth
title[it]  Controllo di congestione su un collo di bottiglia: il dente di sega
level  3
layer  4
topic  tcp
about  A long transfer crosses a bottleneck link much slower than the access links. Nothing
about  tells the sender how fast the bottleneck is: it probes by growing its congestion window
about  until the router queue overflows, then backs off. Slow start overshoots first; after that
about  the window follows the AIMD sawtooth (Reno/NewReno), a cubic curve that plateaus near the
about  last maximum (CUBIC), or stays near the bandwidth-delay product without filling the queue
about  (the simplified BBR).
about
about  Watch the cwnd graph in the tcp panel of h1 and the queue of r1 towards r2. Compare the
about  final goodput with the bottleneck rate. Try each `cc`; shrink `queue` to a few packets
about  (more losses, lower utilisation for loss-based algorithms) or grow it to 200 (bufferbloat:
about  see the rtt series climb); add random `loss` 0.01 and see who suffers.
about[it]  Un trasferimento lungo attraversa un collo di bottiglia molto più lento dei
about[it]  collegamenti di accesso. Nessuno dice al mittente quanto è veloce il collo di
about[it]  bottiglia: lo scopre facendo crescere la finestra di congestione finché la coda del
about[it]  router trabocca, poi arretra. Prima lo slow start esagera; poi la finestra segue il
about[it]  dente di sega AIMD (Reno/NewReno), una curva cubica che si appiattisce vicino
about[it]  all'ultimo massimo (CUBIC), oppure resta vicino al prodotto banda-ritardo senza
about[it]  riempire la coda (il BBR semplificato).
about[it]
about[it]  Osserva il grafico di cwnd nel pannello tcp di h1 e la coda di r1 verso r2.
about[it]  Confronta il goodput finale con la velocità del collo di bottiglia. Prova ogni
about[it]  `cc`; riduci `queue` a pochi pacchetti (più perdite, minor utilizzo per gli
about[it]  algoritmi basati sulle perdite) o portala a 200 (bufferbloat: guarda salire la
about[it]  serie rtt); aggiungi `loss` 0.01 casuale e guarda chi ne soffre.
param  cc reno Congestion control (tahoe, reno, newreno, cubic, bbr)
param[it] cc Controllo di congestione (tahoe, reno, newreno, cubic, bbr)
param  bw 2M Bottleneck rate
param[it] bw Velocità del collo di bottiglia
param  queue 25 Bottleneck queue (packets)
param[it] queue Coda del collo di bottiglia (pacchetti)
param+ delay 20ms One-way delay of the bottleneck
param[it] delay Ritardo del collo di bottiglia (una direzione)
param+ loss 0 Random loss on the bottleneck
param[it] loss Perdita casuale sul collo di bottiglia
param+ ssthresh 0 Initial ssthresh in segments (0 = unlimited slow start)
param[it] ssthresh ssthresh iniziale in segmenti (0 = slow start illimitato)

node h1 host   x=8  y=50
node r1 router x=36 y=50
node r2 router x=64 y=50
node h2 host   x=92 y=50
link h1 r1 delay=1ms bw=100M
link r1 r2 delay=$delay bw=$bw queue=$queue loss=$loss
link r2 h2 delay=1ms bw=100M
auto all

run h2 tcp sink
at 1s run h1 tcp send h2 bytes=5M cc=$cc ssthresh=$ssthresh

note 1s Slow start: cwnd doubles each round trip. The bottleneck queue at r1 starts to build.
note[it] 1s Slow start: cwnd raddoppia a ogni giro. La coda del collo di bottiglia su r1 comincia a crescere.
note 2s The queue overflowed: many segments of one window are lost. Recovery, then congestion avoidance.
note[it] 2s La coda ha traboccato: molti segmenti di una finestra sono persi. Recupero, poi congestion avoidance.
note 8s Steady state: additive increase until the queue is full, multiplicative decrease at each loss.
note[it] 8s Regime: incremento additivo finché la coda è piena, decremento moltiplicativo a ogni perdita.

duration 90s
expect h1 tcp.send_done == 1
expect h2 tcp.sink_bytes == 5000000
expect h1 tcp.goodput_kbps >= 800
