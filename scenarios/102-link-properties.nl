title      What a link does to packets: delay, bandwidth, loss, queue
title[it]  Che cosa fa un collegamento ai pacchetti: ritardo, banda, perdita, coda
level 1
layer 1
topic link
about      A link is not a perfect pipe. Its propagation delay postpones every bit, its bandwidth limits how many bits per second get through (so a long frame takes longer to transmit than a short one), random loss removes frames, and when packets arrive faster than they can leave they wait in a queue, and are dropped when it is full. Here a 2 Mbit/s stream is pushed into a link slower than that: watch the queue number grow on the map next to h1, the delay measured by the sink climb, and then the drops begin.
about
about      Things to try: change bw, delay, loss and queue and predict the ping time (2 x delay + transmission times) and the loss seen by the sink (about 1 - bw/2M when the queue is full). Open the Links tab of h1 and the Graphs view (g) on h2.
about[it]  Un collegamento non è un tubo perfetto. Il ritardo di propagazione posticipa ogni bit, la larghezza di banda limita quanti bit al secondo passano (quindi un frame lungo impiega più tempo di uno corto a essere trasmesso), la perdita casuale elimina frame, e quando i pacchetti arrivano più in fretta di quanto possano partire aspettano in una coda, e vengono scartati quando è piena. Qui un flusso da 2 Mbit/s viene spinto in un collegamento più lento: osserva il numero della coda crescere sulla mappa accanto a h1, il ritardo misurato dal sink salire, e poi iniziare gli scarti.
about[it]
about[it]  Da provare: cambia bw, delay, loss e queue e prevedi il tempo di ping (2 x ritardo + tempi di trasmissione) e la perdita vista dal sink (circa 1 - bw/2M a coda piena). Apri la scheda Collegamenti di h1 e la vista Grafici (g) su h2.
param      bw 1M Bandwidth of the link
param[it]  bw Larghezza di banda del collegamento
param      delay 20ms Propagation delay
param[it]  delay Ritardo di propagazione
param      loss 0 Random loss (0..1)
param[it]  loss Perdita casuale (0..1)
param+     queue 20 Queue length (packets)
param[it]  queue Lunghezza della coda (pacchetti)

node h1 host x=15 y=50
node h2 host x=85 y=50
link h1 h2 bw=$bw delay=$delay loss=$loss queue=$queue
auto all

run h2 sink
run h1 ping h2 count=3 interval=500ms
note 0     An idle link: the ping time is twice the propagation delay plus the time to transmit the frames.
note[it] 0 Collegamento scarico: il tempo di ping è il doppio del ritardo di propagazione più il tempo per trasmettere i frame.
at 3s run h1 cbr h2 rate=2M size=1000 duration=4s
note 3s    2 Mbit/s offered to a slower link: the queue fills, the delay grows, then packets are dropped.
note[it] 3s 2 Mbit/s offerti a un collegamento più lento: la coda si riempie, il ritardo cresce, poi i pacchetti vengono scartati.
at 4s run h1 ping h2 count=3 interval=500ms
note 4s    The same ping now waits in the queue behind the stream: compare its time with the first one.
note[it] 4s Lo stesso ping ora aspetta in coda dietro il flusso: confronta il suo tempo con il primo.
duration 9s
expect h1 ping.reply >= 4
expect h2 sink.received > 100
expect h2 sink.lost > 50
