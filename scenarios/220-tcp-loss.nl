title      TCP on a lossy path: timeout and fast retransmit
title[it]  TCP su un percorso con perdite: timeout e fast retransmit
level  2
layer  4
topic  tcp
about  h1 sends 200 kB to h2 across a link that loses packets at random. TCP notices a loss in
about  two ways: three duplicate ACKs (the receiver keeps asking for the missing byte while later
about  segments arrive) trigger a fast retransmit; if the ACKs do not come at all, the
about  retransmission timer (RTO) expires. Either way the congestion window shrinks.
about
about  Watch the tcp panel of h1: cwnd, ssthresh, srtt and RTO, and the counters of fast
about  retransmits and timeouts. In the log at detail 2 each loss event is explained.
about  Try: loss=0.2 (timeouts dominate, RTO backs off exponentially); cc=tahoe (window back to
about  1 after every loss) versus reno and newreno; a bigger `delay` (longer RTO).
about[it]  h1 invia 200 kB a h2 attraverso un collegamento che perde pacchetti a caso. TCP si
about[it]  accorge di una perdita in due modi: tre ACK duplicati (il ricevitore continua a
about[it]  chiedere il byte mancante mentre arrivano i segmenti successivi) innescano un fast
about[it]  retransmit; se gli ACK non arrivano affatto, scade il timer di ritrasmissione (RTO).
about[it]  In entrambi i casi la finestra di congestione si riduce.
about[it]
about[it]  Osserva il pannello tcp di h1: cwnd, ssthresh, srtt e RTO, e i contatori di fast
about[it]  retransmit e timeout. Nel registro a dettaglio 2 ogni perdita è spiegata. Prova:
about[it]  loss=0.2 (dominano i timeout, l'RTO raddoppia a ogni scadenza); cc=tahoe (finestra
about[it]  a 1 dopo ogni perdita) contro reno e newreno; un `delay` maggiore (RTO più lungo).
param  loss 0.05 Packet loss on the middle link
param[it] loss Perdita di pacchetti sul collegamento centrale
param  cc reno Congestion control (tahoe, reno, newreno, cubic, bbr)
param[it] cc Controllo di congestione (tahoe, reno, newreno, cubic, bbr)
param+ delay 20ms One-way delay of the middle link
param[it] delay Ritardo del collegamento centrale (una direzione)

node h1 host   x=10 y=50
node r1 router x=37 y=50
node r2 router x=63 y=50
node h2 host   x=90 y=50
link h1 r1 delay=1ms bw=100M
link r1 r2 delay=$delay bw=5M loss=$loss
link r2 h2 delay=1ms bw=100M
auto all

run h2 tcp sink
at 1s run h1 tcp send h2 bytes=200k cc=$cc

note 1s Slow start: cwnd doubles every round trip until the first loss.
note[it] 1s Slow start: cwnd raddoppia a ogni giro finché non arriva la prima perdita.
note 2s Look for "3 duplicate ACKs: fast retransmit" (quick repair) and "retransmission timeout" (slow repair, cwnd back to 1).
note[it] 2s Cerca «3 duplicate ACKs: fast retransmit» (riparazione rapida) e «retransmission timeout» (riparazione lenta, cwnd torna a 1).

duration 60s
expect h1 tcp.send_done == 1
expect h2 tcp.sink_bytes == 200000
expect h1 tcp.retransmits >= 1
