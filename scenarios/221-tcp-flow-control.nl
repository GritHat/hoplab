title      TCP flow control: a fast sender and a slow reader
title[it]  Controllo di flusso TCP: mittente veloce, lettore lento
level  2
layer  4
topic  tcp
about  The network here is fast and loses nothing; the bottleneck is the application on h2,
about  which reads from its socket slowly. Its small receive buffer fills up, the window it
about  advertises in every ACK shrinks to zero and the sender must stop, however large its
about  congestion window is. While the window is closed the sender probes it periodically
about  (zero-window probe); when the reader frees space the receiver sends a window update.
about
about  Watch the Window field in the Packets view and the log at detail 2. The transfer rate
about  ends up equal to the reading rate, not to the link rate.
about  Try: a larger `rwnd` (the buffer absorbs more before the sender is stopped); a faster
about  `readrate`; compare the goodput reported at the end with `readrate`.
about[it]  Qui la rete è veloce e non perde nulla; il collo di bottiglia è l'applicazione su
about[it]  h2, che legge lentamente dal suo socket. Il suo piccolo buffer di ricezione si
about[it]  riempie, la finestra annunciata in ogni ACK scende a zero e il mittente deve
about[it]  fermarsi, per quanto grande sia la sua finestra di congestione. Mentre la finestra
about[it]  è chiusa il mittente la sonda periodicamente (zero-window probe); quando il lettore
about[it]  libera spazio il ricevitore invia un aggiornamento di finestra.
about[it]
about[it]  Osserva il campo Window nella vista Pacchetti e il registro a dettaglio 2. La
about[it]  velocità del trasferimento finisce per coincidere con quella di lettura, non con
about[it]  quella del collegamento. Prova: un `rwnd` più grande (il buffer assorbe di più
about[it]  prima di fermare il mittente); un `readrate` più alto; confronta il goodput finale
about[it]  con `readrate`.
param  rwnd 8000 Receive buffer of h2 (bytes)
param[it] rwnd Buffer di ricezione di h2 (byte)
param  readrate 16k Rate at which the application on h2 reads (bit/s)
param[it] readrate Velocità di lettura dell'applicazione su h2 (bit/s)

node h1 host x=20 y=50
node h2 host x=80 y=50
link h1 h2 delay=10ms bw=10M
auto all

run h2 tcp sink rwnd=$rwnd readrate=$readrate
at 1s run h1 tcp send h2 bytes=40000

note 1s The first segments fill h2's buffer in a few milliseconds: the advertised window drops.
note[it] 1s I primi segmenti riempiono il buffer di h2 in pochi millisecondi: la finestra annunciata crolla.
note 2s Window 0: the sender is blocked by flow control, not by the network. It sends 1-byte probes until the window reopens.
note[it] 2s Finestra 0: il mittente è bloccato dal controllo di flusso, non dalla rete. Invia sonde di 1 byte finché la finestra non si riapre.

duration 45s
expect h1 tcp.send_done == 1
expect h2 tcp.sink_bytes == 40000
expect h1 tcp.zero_windows >= 1
expect h1 tcp.timeouts == 0
