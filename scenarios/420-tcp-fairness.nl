title      Two flows, one bottleneck: fairness of Reno, CUBIC and BBR
title[it]  Due flussi, un collo di bottiglia: equità fra Reno, CUBIC e BBR
level  4
layer  4
topic  tcp
about  Two senders share a bottleneck. With the same algorithm and the same round-trip time,
about  AIMD converges towards an equal split. Mix algorithms, or round-trip times, and the split
about  is no longer fair: CUBIC is more aggressive than Reno on fast long paths; a model-based
about  sender like BBR does not back off on loss and can starve or be starved by loss-based
about  flows depending on the buffer size; a flow with a shorter RTT grows its window faster.
about  How to make heterogeneous congestion controllers coexist is an open research question.
about
about  The second flow starts later, while the first already fills the link: watch how long it
about  takes to gain its share. Compare the goodput lines at the end and the cwnd graphs of
about  h1 and h2. Try: cc1=cubic cc2=reno; cc1=bbr cc2=cubic with queue=10 and queue=200;
about  delay2=40ms to give the second flow a longer RTT. The bbr here is a simplified model.
about[it]  Due mittenti condividono un collo di bottiglia. Con lo stesso algoritmo e lo stesso
about[it]  tempo di andata e ritorno, AIMD converge verso una divisione equa. Mescolando
about[it]  algoritmi o tempi di andata e ritorno la divisione non è più equa: CUBIC è più
about[it]  aggressivo di Reno sui percorsi veloci e lunghi; un mittente basato su un modello
about[it]  come BBR non arretra sulle perdite e può affamare o essere affamato dai flussi
about[it]  basati sulle perdite a seconda della dimensione del buffer; un flusso con RTT più
about[it]  breve fa crescere la finestra più in fretta. Come far convivere controllori di
about[it]  congestione eterogenei è una questione di ricerca aperta.
about[it]
about[it]  Il secondo flusso parte più tardi, quando il primo riempie già il collegamento:
about[it]  osserva quanto impiega a conquistare la sua parte. Confronta le righe di goodput
about[it]  finali e i grafici cwnd di h1 e h2. Prova: cc1=cubic cc2=reno; cc1=bbr cc2=cubic
about[it]  con queue=10 e queue=200; delay2=40ms per dare al secondo flusso un RTT più lungo.
about[it]  Il bbr qui è un modello semplificato.
param  cc1 reno Congestion control of flow 1 (h1)
param[it] cc1 Controllo di congestione del flusso 1 (h1)
param  cc2 cubic Congestion control of flow 2 (h2)
param[it] cc2 Controllo di congestione del flusso 2 (h2)
param  queue 40 Bottleneck queue (packets)
param[it] queue Coda del collo di bottiglia (pacchetti)
param+ bw 4M Bottleneck rate
param[it] bw Velocità del collo di bottiglia
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
link r1 r2 delay=15ms bw=$bw queue=$queue
link r2 d1 delay=2ms bw=100M
link r2 d2 delay=2ms bw=100M
auto all

run d1 tcp sink
run d2 tcp sink
at 1s run h1 tcp send d1 bytes=6M cc=$cc1
at 6s run h2 tcp send d2 bytes=4M cc=$cc2

note 1s Flow 1 is alone: it takes the whole bottleneck.
note[it] 1s Il flusso 1 è solo: si prende tutto il collo di bottiglia.
note 6s Flow 2 starts in slow start and collides with flow 1 in the queue of r1: both lose packets.
note[it] 6s Il flusso 2 parte in slow start e si scontra con il flusso 1 nella coda di r1: entrambi perdono pacchetti.
note 12s Are the two cwnd curves converging? Compare them in the tcp panels of h1 and h2.
note[it] 12s Le due curve cwnd stanno convergendo? Confrontale nei pannelli tcp di h1 e h2.

duration 120s
expect h1 tcp.send_done == 1
expect h2 tcp.send_done == 1
expect d1 tcp.sink_bytes == 6000000
expect d2 tcp.sink_bytes == 4000000
