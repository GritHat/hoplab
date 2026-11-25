title      Correcting instead of discarding: repetition and Hamming codes
title[it]  Correggere invece di scartare: ripetizione e codici di Hamming
level  1
layer  2
topic  coding
about  Three identical links, noisy enough that most frames arrive with at least one flipped bit. The first (c1–c2) only detects errors with a CRC and throws the frame away. The second (h1–h2) uses a Hamming(7,4) code: 3 extra bits for every 4 let the receiver find and flip back one wrong bit in each group of 7. The third (r1–r2) sends every frame three times and takes a majority vote.
about  Watch the log: "bit errors … detected, frame discarded" on the first link, "corrected by the link code" on the others. Compare sink.received on c2, h2 and r2. Correction is not free: the links are slow (1 Mbit/s) so you can see that a Hamming frame stays on the wire 1.75 times longer and a repeated one 3 times longer; compare sink.delay_ms.
about  Try: ber=3e-3 (Hamming starts failing: two errors in one block get "corrected" wrongly and only the UDP checksum saves the day); ber=1e-5 (the CRC link is fine and the redundancy of the others is wasted).
about[it]  Tre collegamenti identici, abbastanza rumorosi che la maggior parte dei frame arriva con almeno un bit invertito. Il primo (c1–c2) rileva soltanto gli errori con un CRC e butta il frame. Il secondo (h1–h2) usa un codice di Hamming(7,4): 3 bit in più ogni 4 permettono al receiver di trovare e reinvertire un bit sbagliato in ogni gruppo di 7. Il terzo (r1–r2) invia ogni frame tre volte e decide a maggioranza.
about[it]  Nel registro: "bit errors … detected, frame discarded" sul primo collegamento, "corrected by the link code" sugli altri. Confronta sink.received su c2, h2 e r2. Correggere non è gratis: i collegamenti sono lenti (1 Mbit/s) per mostrare che un frame Hamming occupa il filo 1,75 volte più a lungo e uno ripetuto 3 volte; confronta sink.delay_ms.
about[it]  Prova: ber=3e-3 (Hamming comincia a sbagliare: due errori nello stesso blocco vengono "corretti" male e solo il checksum UDP salva la situazione); ber=1e-5 (il collegamento con CRC funziona e la ridondanza degli altri è sprecata).
param      ber 3e-4 Bit error rate of the three links
param[it]  ber Tasso di errore sul bit dei tre collegamenti

node c1 host x=15 y=20
node c2 host x=85 y=20
node h1 host x=15 y=50
node h2 host x=85 y=50
node r1 host x=15 y=80
node r2 host x=85 y=80
link c1 c2 bw=1M delay=5ms ber=$ber codec=crc32
link h1 h2 bw=1M delay=5ms ber=$ber codec=hamming74
link r1 r2 bw=1M delay=5ms ber=$ber codec=rep3
auto all

run c2 sink
run h2 sink
run r2 sink
run c1 cbr c2 rate=40k size=500 duration=20s
run h1 cbr h2 rate=40k size=500 duration=20s
run r1 cbr r2 rate=40k size=500 duration=20s

note 1s      Top: CRC, detect and discard. Middle: Hamming(7,4). Bottom: three copies and a vote. Same noise everywhere.
note[it] 1s  In alto: CRC, rileva e scarta. Al centro: Hamming(7,4). In basso: tre copie e voto a maggioranza. Stesso rumore ovunque.
note 8s      Packets on the lower links travel visibly longer: redundancy is paid in airtime. Compare sink.delay_ms.
note[it] 8s  I pacchetti sui collegamenti in basso viaggiano visibilmente più a lungo: la ridondanza si paga in tempo di trasmissione. Confronta sink.delay_ms.
note 20s     c2 received a fraction of the datagrams; h2 and r2 nearly all of them, with no retransmission.
note[it] 20s c2 ha ricevuto una frazione dei datagrammi; h2 e r2 quasi tutti, senza alcuna ritrasmissione.

duration 22s
# the CRC link delivers only the frames the noise spared: about 200*exp(-ber*bits)
at 21900ms run c2 check discard "abs(c2:sink.received - 200 * exp(-$ber * 4400)) <= 45"
# correcting delivers at least as much as discarding, and nearly everything up to ber 1e-3
at 21900ms run c2 check correct "h2:sink.received >= c2:sink.received && r2:sink.received >= c2:sink.received && ($ber > 1e-3 || (h2:sink.received >= 185 && r2:sink.received >= 185))"
expect c2 check.discard == 1
expect c2 check.correct == 1
