title      Priority queueing: voice and bulk traffic on a slow link
title[it]  Code a priorità: voce e traffico massivo su un collegamento lento
level  2
layer  3
topic  qdisc
about  A voice call (64 kbit/s, small packets marked DSCP EF with tos=184) and a bulk transfer that
about  alone exceeds the capacity share the slow link r1 - r2. With a strict-priority queue the
about  router always sends waiting voice packets first: the call sees no loss and almost no delay,
about  and all the pain of the overload falls on the bulk flow.
about
about  Watch: the panel of r1 (module qdisc) with the three bands; the sinks on v2 and b2 (loss,
about  delay, jitter). Try: qdisc=droptail, where both flows share one FIFO: the call now waits
about  behind bulk packets and loses its share of packets; a faster or slower link (bw); a bigger
about  queue (it only adds delay for the bulk flow).
about[it]  Una chiamata vocale (64 kbit/s, pacchetti piccoli marcati DSCP EF con tos=184) e un
about[it]  trasferimento massivo che da solo supera la capacità condividono il collegamento lento r1 - r2.
about[it]  Con una coda a priorità stretta il router invia sempre prima i pacchetti voce in attesa: la
about[it]  chiamata non vede perdite e quasi nessun ritardo, e tutto il peso del sovraccarico cade sul
about[it]  flusso massivo.
about[it]
about[it]  Osserva: il pannello di r1 (modulo qdisc) con le tre bande; i sink su v2 e b2 (perdita,
about[it]  ritardo, jitter). Prova: qdisc=droptail, dove i due flussi condividono un'unica FIFO: la
about[it]  chiamata ora aspetta dietro i pacchetti massivi e perde la sua quota di pacchetti; un
about[it]  collegamento più veloce o più lento (bw); una coda più grande (aggiunge solo ritardo al flusso massivo).
param      qdisc prio Queue discipline of the slow link (prio, droptail, fq)
param[it]  qdisc Disciplina di coda del collegamento lento (prio, droptail, fq)
param      bw 500k Capacity of the slow link
param[it]  bw Capacità del collegamento lento
param      bulk 700k Rate of the bulk flow
param[it]  bulk Velocità del flusso massivo
param+     queue 30 Queue capacity (packets)
param[it]  queue Capacità della coda (pacchetti)

node v1 host x=8 y=25 role=voice
node b1 host x=8 y=75 role=bulk
node r1 router x=35 y=50
node r2 router x=65 y=50
node v2 host x=92 y=25 role=voice
node b2 host x=92 y=75 role=bulk
link v1 r1 delay=1ms
link b1 r1 delay=1ms
link r1 r2 bw=$bw delay=5ms qdisc=$qdisc queue=$queue
link r2 v2 delay=1ms
link r2 b2 delay=1ms
auto all

run r1 qdisc
run v2 sink
run b2 sink
at 1s run v1 cbr v2 rate=64k size=160 tos=184
at 3s run b1 cbr b2 rate=$bulk size=1000

note 1s The call starts alone: 50 small packets per second, delay is just propagation plus transmission.
note[it] 1s La chiamata parte da sola: 50 pacchetti piccoli al secondo, il ritardo è solo propagazione più trasmissione.
note 3s The bulk flow starts and offers more than the link can carry. The queue of r1 fills up and packets must be dropped.
note[it] 3s Parte il flusso massivo e offre più di quanto il collegamento possa trasportare. La coda di r1 si riempie e bisogna scartare pacchetti.
note 6s With prio, voice packets (band 0) overtake everything: at most they wait for the one bulk packet already on the wire. Compare the delay of the two sinks.
note[it] 6s Con prio, i pacchetti voce (banda 0) sorpassano tutto: al massimo attendono l'unico pacchetto massivo già in trasmissione. Confronta il ritardo dei due sink.

duration 15s
expect v2 sink.lost == 0
expect v2 sink.received > 680
expect v2 sink.delay_ms < 40
expect b2 sink.lost > 50
expect r1 prio.drops > 50
