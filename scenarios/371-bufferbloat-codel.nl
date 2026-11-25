title      Bufferbloat: a big buffer against CoDel, side by side
title[it]  Bufferbloat: un grande buffer contro CoDel, fianco a fianco
level  3
layer  3
topic  qdisc
about  Two identical paths, each with a 1 Mbit/s bottleneck that receives more traffic than it can
about  carry. Path A (top) has a plain drop-tail buffer of `buffer` packets; path B (bottom) manages
about  the same buffer with an AQM (CoDel by default). Both lose the same share of packets, because
about  the overload is the same and the UDP source does not slow down. But on A the buffer stays
about  full, so every packet, including the pings, waits behind hundreds of others: latency
about  explodes for no gain in throughput. CoDel drops from the head as soon as packets have been
about  waiting longer than the target for a whole interval, and keeps the queue short.
about
about  Watch: the ping rtt graphs of a1 and b1; mean delay in the sinks a2 and b2; the queue and
about  wait graphs and the qdisc panels of ra and rb. Try: buffer=20 (a small buffer hides the
about  problem and costs nothing here); aqm=pie or aqm=red; aqm=fifo (path B becomes path A); target
about  and interval; load=900k (no overload: both behave the same, CoDel never drops). One caution
about  about what you see: CoDel was designed for senders that slow down when a packet is dropped. This
about  UDP source is deaf, so CoDel has to raise its drop rate step by step until it matches the
about  overload: with interval=500ms or load=3M that takes most of the run and the queue stays long.
about[it]  Due percorsi identici, ciascuno con una strozzatura da 1 Mbit/s che riceve più traffico di
about[it]  quanto possa trasportare. Il percorso A (in alto) ha un semplice buffer drop-tail di `buffer`
about[it]  pacchetti; il percorso B (in basso) gestisce lo stesso buffer con un AQM (CoDel di default).
about[it]  Entrambi perdono la stessa quota di pacchetti, perché il sovraccarico è lo stesso e la sorgente
about[it]  UDP non rallenta. Ma su A il buffer resta pieno, quindi ogni pacchetto, ping compresi, aspetta
about[it]  dietro centinaia di altri: la latenza esplode senza alcun guadagno di throughput. CoDel scarta
about[it]  dalla testa appena i pacchetti aspettano più del target per un intero intervallo, e tiene la coda corta.
about[it]
about[it]  Osserva: i grafici ping rtt di a1 e b1; il ritardo medio nei sink a2 e b2; i grafici di coda
about[it]  e attesa e i pannelli qdisc di ra e rb. Prova: buffer=20 (un buffer piccolo nasconde il
about[it]  problema e qui non costa nulla); aqm=pie o aqm=red; aqm=fifo (il percorso B diventa il percorso
about[it]  A); target e interval; load=900k (nessun sovraccarico: i due si comportano allo stesso modo,
about[it]  CoDel non scarta mai). Un'avvertenza su ciò che vedi: CoDel è stato progettato per mittenti che
about[it]  rallentano quando un pacchetto viene scartato. Questa sorgente UDP è sorda, quindi CoDel deve
about[it]  alzare il proprio ritmo di scarto passo dopo passo finché eguaglia il sovraccarico: con
about[it]  interval=500ms o load=3M ci vuole gran parte dell'esecuzione e la coda resta lunga.
param      buffer 200 Buffer of both bottlenecks (packets)
param[it]  buffer Buffer di entrambe le strozzature (pacchetti)
param      load 1300k Offered load on each 1 Mbit/s bottleneck
param[it]  load Carico offerto a ciascuna strozzatura da 1 Mbit/s
param      aqm codel Queue management on path B (codel, pie, red, fifo)
param[it]  aqm Gestione della coda sul percorso B (codel, pie, red, fifo)
param+     target 10ms CoDel/PIE target delay
param[it]  target Ritardo obiettivo di CoDel/PIE
param+     interval 100ms CoDel interval
param[it]  interval Intervallo di CoDel

node a1 host x=8 y=25
node ra router x=40 y=25 role=FIFO
node a2 host x=92 y=25
node b1 host x=8 y=75
node rb router x=40 y=75 role=AQM
node b2 host x=92 y=75
link a1 ra bw=10M delay=2ms
link ra a2 bw=1M delay=5ms qdisc=fifo queue=$buffer
link b1 rb bw=10M delay=2ms
link rb b2 bw=1M delay=5ms qdisc=$aqm queue=$buffer target=$target interval=$interval
auto all

run ra qdisc
run rb qdisc
run a2 sink
run b2 sink
run a1 ping a2 count=0 interval=500ms
run b1 ping b2 count=0 interval=500ms
at 2s run a1 cbr a2 rate=$load size=500 duration=16s
at 2s run b1 cbr b2 rate=$load size=500 duration=16s

note 0s Both paths are idle: ping takes about 15 ms on each.
note[it] 0s Entrambi i percorsi sono scarichi: il ping impiega circa 15 ms su ciascuno.
note 2s The overload starts on both paths. Each queue grows at the same pace: excess traffic has to wait somewhere.
note[it] 2s Il sovraccarico parte su entrambi i percorsi. Le due code crescono allo stesso ritmo: il traffico in eccesso deve pur aspettare da qualche parte.
note 4s CoDel has seen packets wait longer than its target for a whole interval and starts dropping, more and more often, until the queue shrinks. The FIFO just keeps filling.
note[it] 4s CoDel ha visto pacchetti aspettare più del target per un intero intervallo e inizia a scartare, sempre più spesso, finché la coda si accorcia. La FIFO continua solo a riempirsi.
note 8s The FIFO is full: every packet on path A now waits almost a second. Path B loses the same traffic but delivers the rest quickly.
note[it] 8s La FIFO è piena: ogni pacchetto sul percorso A ora aspetta quasi un secondo. Il percorso B perde lo stesso traffico ma consegna il resto rapidamente.
note 18s The sources stop; the full buffer of path A still needs most of a second to drain.
note[it] 18s Le sorgenti si fermano; il buffer pieno del percorso A ha ancora bisogno di quasi un secondo per svuotarsi.

duration 21s
# on the wire each 500-byte datagram is 546 bytes: the 1 Mbit/s bottleneck is overloaded above about 915k
at 20900ms run ra check overload "$load * 1.092 > 1M"
at 20900ms run ra check aqm "is_$aqm == is_codel || is_$aqm == is_pie || is_$aqm == is_red"
at 20900ms run ra check drops "rb:codel.drops + rb:pie.drops + rb:red.drops"
# path A: a full drop-tail buffer makes everybody wait buffer x 4.4 ms
at 20900ms run ra check bloat "(ra:check.overload == 1 && a2:sink.delay_ms >= 0.6 * $buffer * 4.37) || (ra:check.overload == 0 && a2:sink.delay_ms < 30)"
# path B without an AQM is path A; with one it waits less and delivers about as much
at 20900ms run ra check same "ra:check.aqm == 1 || abs(a2:sink.delay_ms - b2:sink.delay_ms) < 20"
at 20900ms run ra check managed "ra:check.aqm == 0 || (ra:check.overload == 0 && ra:check.drops == 0 && b2:sink.delay_ms < 30) || (ra:check.overload == 1 && ra:check.drops > 50 && b2:sink.delay_ms <= a2:sink.delay_ms + 5 && b2:sink.received >= 0.8 * a2:sink.received)"
# CoDel and PIE near their usual settings, against a moderate overload: far below the bloated path
at 20900ms run ra check short "ra:check.overload == 0 || $buffer < 100 || (is_$aqm != is_codel && is_$aqm != is_pie) || $interval > 200ms || $load > 2M || b2:sink.delay_ms < 0.5 * a2:sink.delay_ms"
expect ra check.bloat == 1
expect ra check.same == 1
expect ra check.managed == 1
expect ra check.short == 1
