title      RED: early random drops, the average queue and ECN marks
title[it]  RED: scarti casuali anticipati, la coda media ed ECN
level  3
layer  3
topic  qdisc
about  A 1 Mbit/s bottleneck managed by Random Early Detection. A steady flow (h1, marked as
about  ECN-capable with tos=2) fills 80% of the link; twice, a second flow (h3, not ECN-capable)
about  adds a burst that overloads it. RED does not look at the instantaneous queue but at its
about  moving average: short bursts pass untouched, a queue that stays long is answered with
about  drops whose probability rises between the min and max thresholds, well before the buffer is
about  full. With ecn=1, packets of ECN-capable flows are marked (CE) instead of dropped.
about
about  Watch: on r1 the graphs "queue" and "avg queue" (how slowly the average follows), the
about  drop probability in the qdisc panel, counters red.early, red.forced, red.marks; in the
about  packet inspector the ECN field of the marked packets. Remember that a constant-rate UDP
about  source ignores both drops and marks: with TCP the marks alone would slow the sender.
about  Try: wq=0.002 (the classic, very slow average) or 0.2; min and max; maxp; ecn=0; gentle=0.
about[it]  Una strozzatura da 1 Mbit/s gestita con Random Early Detection. Un flusso costante (h1,
about[it]  marcato come ECN-capable con tos=2) riempie l'80% del collegamento; due volte un secondo
about[it]  flusso (h3, non ECN-capable) aggiunge una raffica che lo sovraccarica. RED non guarda la coda
about[it]  istantanea ma la sua media mobile: le raffiche brevi passano indenni, a una coda che resta
about[it]  lunga si risponde con scarti la cui probabilità cresce fra le soglie min e max, ben prima che
about[it]  il buffer sia pieno. Con ecn=1 i pacchetti dei flussi ECN-capable vengono marcati (CE) invece che scartati.
about[it]
about[it]  Osserva: su r1 i grafici "queue" e "avg queue" (quanto lentamente la media segue), la
about[it]  probabilità di scarto nel pannello qdisc, i contatori red.early, red.forced, red.marks;
about[it]  nell'ispettore dei pacchetti il campo ECN dei pacchetti marcati. Ricorda che una sorgente
about[it]  UDP a velocità costante ignora sia gli scarti sia le marcature: con TCP basterebbero le
about[it]  marcature a rallentare il mittente.
about[it]  Prova: wq=0.002 (il classico, media lentissima) o 0.2; min e max; maxp; ecn=0; gentle=0.
param      min 5 RED minimum threshold (packets of average queue)
param[it]  min Soglia minima di RED (pacchetti di coda media)
param      max 15 RED maximum threshold
param[it]  max Soglia massima di RED
param      maxp 0.1 Drop probability at the maximum threshold
param[it]  maxp Probabilità di scarto alla soglia massima
param      ecn 1 Mark ECN-capable packets instead of dropping them (1 = on)
param[it]  ecn Marca i pacchetti ECN-capable invece di scartarli (1 = attivo)
param+     wq 0.02 Weight of the moving average
param[it]  wq Peso della media mobile
param+     gentle 1 Gentle mode: probability rises to 1 between max and 2 x max
param[it]  gentle Modo gentle: la probabilità sale a 1 fra max e 2 x max
param+     queue 60 Physical buffer (packets)
param[it]  queue Buffer fisico (pacchetti)

node h1 host x=8 y=25 role=steady
node h3 host x=8 y=75 role=bursts
node r1 router x=38 y=50 role=RED
node r2 router x=65 y=50
node h2 host x=92 y=25
node h4 host x=92 y=75
link h1 r1 delay=1ms
link h3 r1 delay=1ms
link r1 r2 bw=1M delay=5ms qdisc=red queue=$queue min=$min max=$max maxp=$maxp wq=$wq gentle=$gentle ecn=$ecn
link r2 h2 delay=1ms
link r2 h4 delay=1ms
auto all

run r1 qdisc
run h2 sink
run h4 sink
at 1s run h1 cbr h2 rate=800k size=500 tos=2
at 4s run h3 cbr h4 rate=600k size=500 duration=3s
at 11s run h3 cbr h4 rate=600k size=500 duration=3s

note 1s The steady flow uses 80% of the link: the queue is almost always empty and the average stays under the minimum threshold. RED does nothing.
note[it] 1s Il flusso costante usa l'80% del collegamento: la coda è quasi sempre vuota e la media resta sotto la soglia minima. RED non fa nulla.
note 4s Burst: 1.4 Mbit/s offered to a 1 Mbit/s link. The queue grows at once, the average follows more slowly; when it crosses the minimum threshold RED starts to mark or drop a few packets at random.
note[it] 4s Raffica: 1,4 Mbit/s offerti a un collegamento da 1 Mbit/s. La coda cresce subito, la media segue più lentamente; quando supera la soglia minima RED inizia a marcare o scartare qualche pacchetto a caso.
note 6s The sources do not react, so the average passes the maximum threshold: now the probability climbs towards 1 and arrivals are dropped whether ECN-capable or not: marking is only for mild congestion.
note[it] 6s Le sorgenti non reagiscono, quindi la media supera la soglia massima: ora la probabilità sale verso 1 e gli arrivi vengono scartati, ECN-capable o no: la marcatura vale solo per la congestione lieve.
note 7s The burst ends: the queue drains in a moment, the average decays slowly and RED goes quiet again.
note[it] 7s La raffica finisce: la coda si svuota in un attimo, la media decade lentamente e RED torna a tacere.

duration 16s
# marks replace drops for the ECN-capable flow only when ecn=1
at 15900ms run r1 check marks "($ecn == 1 && r1:red.marks > 0) || ($ecn == 0 && r1:red.marks == 0)"
expect r1 check.marks == 1
expect r1 red.early > 0
expect r1 red.drops > 50
expect h2 sink.delay_ms < 120
expect h2 sink.received > 1500
