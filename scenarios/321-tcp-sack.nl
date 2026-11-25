title      Many losses in one window: NewReno against SACK
title[it]  Molte perdite in una finestra: NewReno contro SACK
level  3
layer  4
topic  tcp
about  Slow start doubles the window every round trip until the queue of the bottleneck
about  overflows, and then a whole burst of segments is lost at once. What happens next depends
about  on what the receiver can tell the sender. With cumulative ACKs alone the sender learns of
about  one hole per round trip: NewReno retransmits it, waits for the partial ACK that reveals
about  the next one, and so on, and with many holes its patience (and its timer) runs out.
about  With selective acknowledgements every ACK lists the blocks that did arrive: the sender
about  marks the holes on its scoreboard and repairs them all in about one round trip, sending
about  whenever its estimate of what is still in the network (the pipe) is below the window.
about
about  The lab runs the transfer twice on the same path: first h1 with the `sack` you chose,
about  then, at 12 s, h3 with cumulative ACKs only, as a reference. Compare the two summary
about  lines: retransmissions, timeouts, time. With `sack` 0 both runs recover the NewReno way.
about  At detail 2 the log shows each recovery; open a duplicate ACK in the Packets view and
about  read the SACK option, and at detail 2 the tcp panel of h1 counts the segments in flight,
about  SACKed and lost. Try a longer `delay` (each round trip NewReno spends costs more), a
about  larger `queue` (a bigger burst), or `cc` cubic.
about[it]  Lo slow start raddoppia la finestra a ogni giro finché la coda del collo di bottiglia
about[it]  trabocca, e allora un'intera raffica di segmenti va persa in una volta. Che cosa
about[it]  succede dopo dipende da ciò che il ricevente sa dire al mittente. Con i soli ACK
about[it]  cumulativi il mittente scopre un buco per giro: NewReno lo ritrasmette, aspetta
about[it]  l'ACK parziale che rivela il successivo, e così via, e con molti buchi la sua pazienza
about[it]  (e il suo timer) si esaurisce. Con i riscontri selettivi ogni ACK elenca i blocchi
about[it]  arrivati: il mittente segna i buchi sul suo tabellone e li ripara tutti in circa un
about[it]  giro, trasmettendo ogni volta che la sua stima di ciò che è ancora in rete (il pipe)
about[it]  è sotto la finestra.
about[it]
about[it]  Il laboratorio esegue il trasferimento due volte sullo stesso percorso: prima h1 con
about[it]  il `sack` scelto, poi, a 12 s, h3 con i soli ACK cumulativi, come riferimento. Confronta
about[it]  le due righe di riepilogo: ritrasmissioni, timeout, tempo. Con `sack` 0 entrambe le
about[it]  esecuzioni recuperano alla maniera di NewReno.
about[it]  A dettaglio 2 il registro mostra ogni recupero; apri un ACK duplicato nella vista
about[it]  Pacchetti e leggi l'opzione SACK, e a dettaglio 2 il pannello tcp di h1 conta i
about[it]  segmenti in volo, riscontrati selettivamente e persi. Prova un `delay` più lungo
about[it]  (ogni giro speso da NewReno costa di più), una `queue` più grande (una raffica più
about[it]  ampia) o `cc` cubic.
param  sack 1 Selective acknowledgements (1) or cumulative ACKs only (0)
param[it] sack Riscontri selettivi (1) o solo ACK cumulativi (0)
param  cc newreno Congestion control (newreno, cubic, reno)
param[it] cc Controllo di congestione (newreno, cubic, reno)
param+ delay 40ms One-way delay of the bottleneck
param[it] delay Ritardo del collo di bottiglia (una direzione)
param+ queue 12 Bottleneck queue (packets)
param[it] queue Coda del collo di bottiglia (pacchetti)
param+ bytes 300k Bytes to transfer
param[it] bytes Byte da trasferire

node h1 host   x=8  y=30
node h3 host   x=8  y=70
node r1 router x=36 y=50
node r2 router x=64 y=50
node h2 host   x=92 y=30
node h4 host   x=92 y=70
link h1 r1 delay=1ms bw=100M
link r1 r2 delay=$delay bw=4M queue=$queue
link r2 h2 delay=1ms bw=100M
link h3 r1 delay=1ms bw=100M
link r2 h4 delay=1ms bw=100M
auto all

run h2 tcp sink
at 1s run h1 tcp send h2 bytes=$bytes cc=$cc sack=$sack
run h4 tcp sink
at 12s run h3 tcp send h4 bytes=$bytes cc=$cc sack=0

note 1s Slow start: the window doubles each round trip and the queue of r1 fills.
note[it] 1s Slow start: la finestra raddoppia a ogni giro e la coda di r1 si riempie.
note 1400ms The queue overflowed: a burst of segments of the same window is lost. Watch how the holes are repaired.
note[it] 1400ms La coda ha traboccato: una raffica di segmenti della stessa finestra è persa. Osserva come vengono riparati i buchi.

note 12s The reference run: the same transfer from h3 with cumulative ACKs only (NewReno recovery).
note[it] 12s L'esecuzione di riferimento: lo stesso trasferimento da h3 con i soli ACK cumulativi (recupero NewReno).
duration 30s
# With SACK every retransmission is the repair of a hole the receiver reported,
# and the transfer never does worse than the cumulative-ACK reference; without
# it there is no scoreboard (the two runs differ only in details of timing).
at 29s run h1 check repair  "($sack == 1 && h1:tcp.sack_retransmits >= 3 && h1:tcp.retransmits <= h3:tcp.retransmits && h1:tcp.timeouts <= h3:tcp.timeouts && h1:tcp.time_ms <= 1.1 * h3:tcp.time_ms) || ($sack == 0 && h1:tcp.sack_retransmits == 0)"
expect h1 tcp.send_done == 1
expect h3 tcp.send_done == 1
expect h1 tcp.fast_retransmits >= 1
expect h3 tcp.sack_retransmits == 0
expect h1 check.repair == 1
