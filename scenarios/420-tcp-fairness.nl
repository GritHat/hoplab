title      Two flows, one bottleneck: fairness of Reno, CUBIC and BBR
title[it]  Due flussi, un collo di bottiglia: equità fra Reno, CUBIC e BBR
level  4
layer  4
topic  tcp
about  Two senders share a bottleneck for the same forty seconds. With the same algorithm and
about  the same round-trip time, AIMD converges towards an equal split. Mix algorithms, or
about  round-trip times, and the split is no longer fair: a flow with a shorter RTT grows its
about  window faster; CUBIC backs off less than Reno at each loss (to 70% instead of 50%) and
about  takes the larger share, more so on fast long paths (raise `bw` and `delay`); BBR does not back off on loss, so against a
about  loss-based flow it takes most of a shallow buffer and loses ground in a deep one, where
about  the other fills the queue and inflates the RTT that BBR tries to keep small. How to make
about  heterogeneous congestion controllers coexist is an open research question.
about
about  Each sender reports its steady-state rate, measured over the last thirty seconds, when
about  both flows are past their start-up; the last line gives their shares as Jain's fairness
about  index (1 = equal, 0.5 = one flow has everything). The two flows start within a random
about  half second of each other, so different seeds give different runs: repeat with a few
about  seeds before believing a number. Compare the cwnd graphs of h1 and h2. Try: cc1=cubic
about  cc2=cubic; cc1=bbr cc2=cubic with queue=10 and queue=200; cc1=bbr cc2=bbr; delay2=40ms
about  to give the second flow a longer RTT.
about[it]  Due mittenti condividono un collo di bottiglia per gli stessi quaranta secondi. Con
about[it]  lo stesso algoritmo e lo stesso tempo di andata e ritorno, AIMD converge verso una
about[it]  divisione equa. Mescolando algoritmi o tempi di andata e ritorno la divisione non è
about[it]  più equa: un flusso con RTT più breve fa crescere la finestra più in fretta; CUBIC
about[it]  arretra meno di Reno a ogni perdita (al 70% invece che al 50%) e si prende la quota
about[it]  maggiore, ancor più sui percorsi veloci e lunghi (aumenta `bw` e `delay`); BBR non arretra sulle perdite, quindi contro un
about[it]  flusso basato sulle perdite si prende gran parte di un buffer piccolo e perde terreno
about[it]  in uno profondo, dove l'altro riempie la coda e gonfia l'RTT che BBR cerca di tenere
about[it]  basso. Come far convivere controllori di congestione eterogenei è una questione di
about[it]  ricerca aperta.
about[it]
about[it]  Ogni mittente riporta la sua velocità a regime, misurata sugli ultimi trenta secondi,
about[it]  quando entrambi i flussi hanno superato l'avvio; l'ultima riga dà le loro quote come
about[it]  indice di equità di Jain (1 = uguali, 0,5 = un flusso ha tutto). I due flussi partono
about[it]  entro mezzo secondo casuale l'uno dall'altro, quindi semi diversi danno esecuzioni
about[it]  diverse: ripeti con qualche seme prima di credere a un numero. Confronta i grafici
about[it]  cwnd di h1 e h2. Prova: cc1=cubic cc2=cubic; cc1=bbr cc2=cubic con queue=10 e
about[it]  queue=200; cc1=bbr cc2=bbr; delay2=40ms per dare al secondo flusso un RTT più lungo.
param  cc1 reno Congestion control of flow 1 (h1)
param[it] cc1 Controllo di congestione del flusso 1 (h1)
param  cc2 cubic Congestion control of flow 2 (h2)
param[it] cc2 Controllo di congestione del flusso 2 (h2)
param  queue 40 Bottleneck queue (packets)
param[it] queue Coda del collo di bottiglia (pacchetti)
param+ bw 4M Bottleneck rate
param[it] bw Velocità del collo di bottiglia
param+ delay 15ms One-way delay of the bottleneck
param[it] delay Ritardo del collo di bottiglia (una direzione)
param+ delay2 2ms Access delay of flow 2 (raise it for RTT unfairness)
param[it] delay2 Ritardo di accesso del flusso 2 (aumentalo per l'iniquità da RTT)

node h1 host   x=8  y=25
node h2 host   x=8  y=75
node r1 router x=36 y=50
node r2 router x=64 y=50
node d1 host   x=92 y=25
node d2 host   x=92 y=75
link h1 r1 delay=2ms bw=100M
link h2 r1 delay=$delay2 bw=100M
link r1 r2 delay=$delay bw=$bw queue=$queue
link r2 d1 delay=2ms bw=100M
link r2 d2 delay=2ms bw=100M
auto all

run d1 tcp sink
run d2 tcp sink
at 1s run h1 tcp send d1 duration=40s warmup=10s cc=$cc1 jitter=500ms sndbuf=1M
at 1s run h2 tcp send d2 duration=40s warmup=10s cc=$cc2 jitter=500ms sndbuf=1M

note 1s Both flows start (within half a second of each other) in slow start and collide in the queue of r1.
note[it] 1s Entrambi i flussi partono (entro mezzo secondo l'uno dall'altro) in slow start e si scontrano nella coda di r1.
note 11s From here on the steady-state rates are measured. Compare the cwnd curves in the tcp panels of h1 and h2.
note[it] 11s Da qui in poi si misurano le velocità a regime. Confronta le curve cwnd nei pannelli tcp di h1 e h2.
note 41s The senders stop: read the two steady rates and Jain's index in the log.
note[it] 41s I mittenti si fermano: leggi nel registro le due velocità a regime e l'indice di Jain.

duration 60s
expect h1 tcp.send_done == 1
expect h2 tcp.send_done == 1
expect h1 tcp.steady_kbps > 100
expect h2 tcp.steady_kbps > 100
expect h1 tcp.jain_pct >= 50
