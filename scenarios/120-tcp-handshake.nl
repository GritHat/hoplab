title      TCP, segment by segment: handshake, data, teardown
title[it]  TCP, segmento per segmento: handshake, dati, chiusura
level  1
layer  4
topic  tcp
about  The whole life of a TCP connection on a quiet link. h1 opens a connection to h2 with the
about  three-way handshake (SYN, SYN+ACK, ACK), sends a few thousand bytes in a handful of
about  segments, acknowledged by cumulative ACKs (one for every two segments), and closes with FIN in both directions.
about  A second attempt, towards a port where nobody listens, is refused with a RST.
about
about  Watch the Packets and Sequence views at detail 2: sequence numbers count bytes, the ACK
about  number is the next byte expected, SYN and FIN each consume one sequence number.
about  Try: raise `bytes` and see more segments in flight at once (the initial window is `iw`
about  segments); at detail 3 the log also names every data segment and every ACK. With so few
about  segments a loss is rare: to see a retransmission try `loss=0.1 bytes=50000`.
about[it]  Tutta la vita di una connessione TCP su un collegamento tranquillo. h1 apre una
about[it]  connessione verso h2 con l'handshake a tre vie (SYN, SYN+ACK, ACK), invia qualche
about[it]  migliaio di byte in pochi segmenti, confermati da ACK cumulativi (uno ogni due segmenti), e chiude
about[it]  con un FIN in entrambe le direzioni. Un secondo tentativo, verso una porta dove
about[it]  nessuno ascolta, viene rifiutato con un RST.
about[it]
about[it]  Osserva le viste Pacchetti e Sequenza a dettaglio 2: i numeri di sequenza contano i
about[it]  byte, il numero di ACK è il prossimo byte atteso, SYN e FIN consumano ciascuno un
about[it]  numero di sequenza. Prova: aumenta `bytes` per vedere più segmenti in volo insieme
about[it]  (la finestra iniziale è di `iw` segmenti); a dettaglio 3 il registro nomina anche
about[it]  ogni segmento dati e ogni ACK. Con così pochi segmenti una perdita è rara: per
about[it]  vedere una ritrasmissione prova `loss=0.1 bytes=50000`.
param  bytes 4000 Bytes to transfer
param[it] bytes Byte da trasferire
param  delay 50ms One-way delay of the link
param[it] delay Ritardo del collegamento (una direzione)
param+ iw 2 Initial congestion window (segments)
param[it] iw Finestra di congestione iniziale (segmenti)
param+ loss 0 Packet loss on the link
param[it] loss Perdita di pacchetti sul collegamento

node h1 host x=20 y=50
node h2 host x=80 y=50
link h1 h2 delay=$delay bw=10M loss=$loss
auto all

run h2 tcp sink
at 1s run h1 tcp send h2 bytes=$bytes iw=$iw
at 6s run h1 tcp send h2 port=81 bytes=100

note 1s h1 sends SYN: "I want to talk, my first sequence number is X". h2 answers SYN+ACK, h1 completes with ACK: three segments, one and a half round trips.
note[it] 1s h1 invia SYN: «voglio parlare, il mio primo numero di sequenza è X». h2 risponde SYN+ACK, h1 completa con ACK: tre segmenti, un giro e mezzo.
note 1.3s Data flows: each segment carries up to MSS bytes; every ACK says which byte h2 expects next.
note[it] 1.3s I dati scorrono: ogni segmento porta fino a MSS byte; ogni ACK dice quale byte h2 si aspetta.
note 2s Teardown: FIN from h1, ACK and FIN from h2, final ACK. Each direction is closed separately.
note[it] 2s Chiusura: FIN da h1, ACK e FIN da h2, ACK finale. Ogni direzione si chiude separatamente.
note 6s Port 81 has no listener: h2 answers the SYN with RST and h1 reports "connection refused".
note[it] 6s Sulla porta 81 nessuno ascolta: h2 risponde al SYN con RST e h1 segnala «connessione rifiutata».

duration 40s
expect h1 tcp.send_done == 1
expect h1 tcp.send_failed == 1
expect h2 tcp.sink_done == 1
