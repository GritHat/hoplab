title      Detecting bit errors: nothing, a parity bit, a CRC
title[it]  Rilevare gli errori di bit: niente, un bit di parità, un CRC
level  1
layer  2
topic  coding
about  Three identical noisy links, three ways of protecting a frame. On the first (a1–a2) the link adds no check at all: damaged frames reach IP and UDP, whose own checksums are the last line of defence. On the second (b1–b2) one parity bit per frame catches every odd number of flipped bits and misses every even one. On the third (c1–c2) a 32-bit CRC, the one Ethernet uses, catches practically everything.
about  Watch the log for "bit errors": detected and discarded, or NOT detected and delivered damaged. Then look at a2 and b2: the counter udp.bad_checksum says how many damaged datagrams got past the link and were stopped only by UDP. On c2 it stays at zero.
about  Try: raise ber to 1e-3 (almost nothing survives: detection alone does not deliver data), lower it to 1e-6; run `a1 coding show code=parity pos=3,4` to see two flips fool the parity bit.
about[it]  Tre collegamenti rumorosi identici, tre modi di proteggere un frame. Sul primo (a1–a2) il collegamento non aggiunge alcun controllo: i frame danneggiati arrivano a IP e UDP, i cui checksum sono l'ultima difesa. Sul secondo (b1–b2) un bit di parità per frame scopre ogni numero dispari di bit invertiti e nessun numero pari. Sul terzo (c1–c2) un CRC a 32 bit, quello di Ethernet, scopre praticamente tutto.
about[it]  Nel registro cerca "bit errors": rilevati e scartati, oppure NON rilevati e consegnati danneggiati. Poi guarda a2 e b2: il contatore udp.bad_checksum dice quanti datagrammi danneggiati hanno superato il collegamento e sono stati fermati solo da UDP. Su c2 resta a zero.
about[it]  Prova: alza ber a 1e-3 (non sopravvive quasi nulla: la sola rilevazione non consegna dati), abbassalo a 1e-6; esegui `a1 coding show code=parity pos=3,4` per vedere due inversioni ingannare il bit di parità.
param      ber 1e-4 Bit error rate of the three links
param[it]  ber Tasso di errore sul bit dei tre collegamenti
param+     size 500 Bytes per datagram
param+[it] size Byte per datagramma

node a1 host x=15 y=20
node a2 host x=85 y=20
node b1 host x=15 y=50
node b2 host x=85 y=50
node c1 host x=15 y=80
node c2 host x=85 y=80
link a1 a2 delay=5ms ber=$ber codec=none
link b1 b2 delay=5ms ber=$ber codec=parity
link c1 c2 delay=5ms ber=$ber codec=crc32
auto all

run a2 sink
run b2 sink
run c2 sink
run a1 cbr a2 rate=40k size=$size duration=20s
run b1 cbr b2 rate=40k size=$size duration=20s
run c1 cbr c2 rate=40k size=$size duration=20s

note 1s      Same traffic, same noise on the three links. Top: no check. Middle: one parity bit. Bottom: CRC-32.
note[it] 1s  Stesso traffico e stesso rumore sui tre collegamenti. In alto: nessun controllo. Al centro: un bit di parità. In basso: CRC-32.
note 8s      Select a2, then b2, then c2 and compare udp.bad_checksum: damaged frames that the link let through.
note[it] 8s  Seleziona a2, poi b2, poi c2 e confronta udp.bad_checksum: frame danneggiati che il collegamento ha lasciato passare.
note 20s     All three lose data: detection only tells you a frame is bad. Getting it through needs retransmission or correction (next lab).
note[it] 20s Tutti e tre perdono dati: rilevare dice solo che il frame è rovinato. Per consegnarlo serve ritrasmettere o correggere (prossimo laboratorio).

duration 22s
expect a2 udp.bad_checksum >= 5
expect b2 udp.bad_checksum >= 1
expect c2 udp.bad_checksum == 0
expect c2 ip.bad_checksum == 0
expect c2 sink.received >= 90
