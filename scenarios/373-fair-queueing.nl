title      Fair queueing: a small flow protected from a flood
title[it]  Fair queueing: un flusso piccolo protetto da un'inondazione
level  3
layer  3
topic  qdisc
about  A greedy sender ("hog", 3 Mbit/s) and a modest one ("mouse", 100 kbit/s) cross the same
about  1 Mbit/s link. In a single FIFO the hog fills the buffer and the mouse loses packets in the
about  same proportion and inherits the hog's delay. Fair queueing gives each flow its own queue and
about  serves them in turn with deficit round robin: each flow may take an equal share (here
about  500 kbit/s); the mouse uses less than its share, so it loses nothing and never waits, and the
about  hog gets all the rest but can only hurt itself.
about
about  Watch: the per-flow backlog in the qdisc panel of r1; loss and delay in the sinks d1 (hog)
about  and d2 (mouse). Try: qdisc=droptail to see the mouse suffer; mouse=600k (above its fair
about  share: now both are cut to 500 kbit/s); quantum=300 (finer interleaving) or quantum=9000 (the
about  mouse now waits for six hog packets at each turn); hog=600k (no overload: nothing to protect from).
about[it]  Un mittente ingordo ("hog", 3 Mbit/s) e uno modesto ("mouse", 100 kbit/s) attraversano lo
about[it]  stesso collegamento da 1 Mbit/s. In un'unica FIFO lo hog riempie il buffer e il mouse perde
about[it]  pacchetti nella stessa proporzione ed eredita il ritardo dello hog. Il fair queueing dà a ogni
about[it]  flusso la propria coda e le serve a turno con il deficit round robin: ogni flusso può prendere
about[it]  una quota uguale (qui 500 kbit/s); il mouse usa meno della sua quota, quindi non perde nulla e
about[it]  non aspetta mai, e lo hog prende tutto il resto ma può danneggiare solo sé stesso.
about[it]
about[it]  Osserva: l'arretrato per flusso nel pannello qdisc di r1; perdita e ritardo nei sink d1
about[it]  (hog) e d2 (mouse). Prova: qdisc=droptail per vedere il mouse soffrire; mouse=600k (oltre la
about[it]  quota equa: ora entrambi sono tagliati a 500 kbit/s); quantum=300 (alternanza più fine) o
about[it]  quantum=9000 (il mouse ora aspetta sei pacchetti dell'hog a ogni giro); hog=600k (nessun
about[it]  sovraccarico: nulla da cui proteggersi).
param      qdisc fq Queue discipline of the bottleneck (fq, droptail, fifo)
param[it]  qdisc Disciplina di coda della strozzatura (fq, droptail, fifo)
param      hog 3M Rate of the greedy flow
param[it]  hog Velocità del flusso ingordo
param      mouse 100k Rate of the small flow
param[it]  mouse Velocità del flusso piccolo
param+     queue 40 Queue capacity (packets, all flows together)
param[it]  queue Capacità della coda (pacchetti, tutti i flussi insieme)
param+     quantum 1514 DRR quantum (bytes a flow may send per round)
param[it]  quantum Quanto DRR (byte che un flusso può inviare per giro)

node hog host x=8 y=25
node mouse host x=8 y=75
node r1 router x=38 y=50
node r2 router x=65 y=50
node d1 host x=92 y=25
node d2 host x=92 y=75
link hog r1 delay=1ms
link mouse r1 delay=1ms
link r1 r2 bw=1M delay=5ms qdisc=$qdisc queue=$queue quantum=$quantum
link r2 d1 delay=1ms
link r2 d2 delay=1ms
auto all

run r1 qdisc
run d1 sink
run d2 sink
at 1s run mouse cbr d2 rate=$mouse size=200
at 3s run hog cbr d1 rate=$hog size=1000

note 1s The mouse is alone: no loss, delay is propagation plus transmission.
note[it] 1s Il mouse è solo: nessuna perdita, il ritardo è propagazione più trasmissione.
note 3s The hog starts sending three times the capacity of the link. Its queue fills at once.
note[it] 3s Lo hog inizia a inviare il triplo della capacità del collegamento. La sua coda si riempie subito.
note 6s Round robin: after each packet of the hog the scheduler looks at the mouse's queue. When the buffer is full, the flow with the longest queue (the hog) pays.
note[it] 6s Round robin: dopo ogni pacchetto dello hog lo scheduler guarda la coda del mouse. Quando il buffer è pieno, paga il flusso con la coda più lunga (lo hog).

duration 14s
# on the wire: mouse x 1.23 (200-byte datagrams), hog x 1.046; the fair share is 500 kbit/s each
at 13900ms run r1 check overload "$mouse * 1.23 + $hog * 1.046 > 1M"
at 13900ms run r1 check mouse_lost "mouse:cbr.sent - d2:sink.received"
at 13900ms run r1 check modest "$mouse * 1.23 <= 480k"
# fair queueing, mouse within its share: it loses nothing and waits at most one quantum of the hog
at 13900ms run r1 check shielded "is_$qdisc != is_fq || r1:check.modest == 0 || (r1:check.mouse_lost <= 5 && d2:sink.delay_ms < 25 + $quantum * 0.008)"
# fair queueing, mouse above its share while the hog wants more than its own: both are cut to 500 kbit/s
at 13900ms run r1 check capped "is_$qdisc != is_fq || r1:check.modest == 1 || $hog * 1.046 < 520k || (abs(d2:sink.received - 3300) < 700 && abs(d1:sink.received - 657) < 130)"
# one queue for everybody (droptail, fifo, or prio with unmarked packets): the mouse shares the hog's losses
at 13900ms run r1 check exposed "is_$qdisc == is_fq || (r1:check.overload == 1 && r1:check.mouse_lost > 0.2 * mouse:cbr.sent) || (r1:check.overload == 0 && r1:check.mouse_lost <= 5)"
expect r1 check.shielded == 1
expect r1 check.capped == 1
expect r1 check.exposed == 1
