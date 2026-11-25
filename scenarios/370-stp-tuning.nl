title      Engineering a spanning tree: priorities, port costs, timers and a root failure
title[it]  Progettare uno spanning tree: priorità, costi delle porte, timer e guasto della radice
level  3
layer  2
topic  stp
about  Four switches in a ring. Left alone, the root would be whichever switch happens to have the
about  lowest MAC address; here the administrator chooses it with the bridge priority (param root).
about  Port costs follow the bandwidth (10^9 / bit rate: 100 for 10 Mbit/s, 10 for 100 Mbit/s), so
about  s1 reaches the root through the fast link s4 - s3 even though both paths have two hops, and
about  the redundant port is blocked on the slow side. At 20 s the root crashes: a dead bridge is
about  noticed only when its information reaches max-age, then the survivors elect a new root and
about  the blocked port walks through listening and learning.
about
about  Watch: role, state and cost of each port in the switch panels; the outage after the crash
about  lasts about max-age + 2 x forward delay; two steady streams between h1 and h2 measure it (50
about  datagrams per second: each one missing at h2 is 20 ms of outage).
about  Try: rapid=1 (Rapid Spanning Tree: the dead root is given up after 3 missing hellos instead
about  of max-age, and the spare port opens with a proposal/agreement handshake instead of two
about  forward delays: the outage falls from about 11 s to about 2 s); root=s4 (same story from the other
about  side); root=s1 or root=s2 (the hosts hang off those two, so a root there takes one host down
about  with it: the survivors still elect a new root, but h1 and h2 are cut off for good); shorter
about  timers (hello=200ms maxage=1s fwd=500ms) and how far you can go before a lost BPDU is mistaken
about  for a failure (add loss=0.3 to a link from the console); `s1 stp cost p1 500` to push s1 to
about  the other path; fast=10M (no fast link: all costs are equal, s1 keeps the direct link to s2,
about  the streams never cross the root and its death interrupts nothing). The timeline fits
about  max-age + 2 x forward delay up to 24 s and forward delays up to 7 s.
about[it]  Quattro switch ad anello. Lasciati a sé stessi, la radice sarebbe lo switch che per caso ha
about[it]  l'indirizzo MAC più basso; qui la sceglie l'amministratore con la priorità del bridge (parametro
about[it]  root). I costi delle porte seguono la banda (10^9 / bit rate: 100 per 10 Mbit/s, 10 per 100 Mbit/s),
about[it]  quindi s1 raggiunge la radice attraverso il collegamento veloce s4 - s3 anche se entrambi i
about[it]  percorsi hanno due hop, e la porta ridondante viene bloccata sul lato lento. A 20 s la radice va
about[it]  in crash: un bridge morto viene notato solo quando la sua informazione raggiunge max-age, poi i
about[it]  superstiti eleggono una nuova radice e la porta bloccata passa per listening e learning.
about[it]
about[it]  Osserva: ruolo, stato e costo di ogni porta nei pannelli degli switch; l'interruzione dopo il
about[it]  crash dura circa max-age + 2 x forward delay; due flussi costanti fra h1 e h2 la misurano (50
about[it]  datagrammi al secondo: ognuno che manca a h2 vale 20 ms di interruzione).
about[it]  Prova: rapid=1 (Rapid Spanning Tree: la radice morta viene abbandonata dopo 3 hello mancanti
about[it]  invece di max-age, e la porta di riserva si apre con uno scambio proposal/agreement invece di
about[it]  due forward delay: l'interruzione scende da circa 11 s a circa 2 s); root=s4 (la stessa storia
about[it]  dall'altro lato); root=s1 o root=s2 (gli host sono attaccati a quei due, quindi una radice lì
about[it]  porta giù un host con sé: i superstiti eleggono comunque una nuova radice, ma h1 e h2 restano
about[it]  separati per sempre); timer più corti (hello=200ms maxage=1s fwd=500ms) e fin dove puoi
about[it]  spingerti prima che un BPDU perso venga scambiato per un guasto (aggiungi loss=0.3 a un
about[it]  collegamento dalla console); `s1 stp cost p1 500` per spingere s1 sull'altro percorso;
about[it]  fast=10M (nessun collegamento veloce: tutti i costi sono uguali, s1 tiene il collegamento
about[it]  diretto con s2, i flussi non attraversano mai la radice e la sua morte non interrompe nulla).
about[it]  La cronologia regge max-age + 2 x forward delay fino a 24 s e forward delay fino a 7 s.
param      root s3 Switch that gets the best priority (4096), becomes the root, and crashes at 20 s (s1..s4)
param[it]  root Switch che riceve la priorità migliore (4096), diventa la radice, e va in crash a 20 s (s1..s4)
param      hello 1s Hello time
param[it]  hello Hello time
param      maxage 6s Max age
param[it]  maxage Max age
param      fwd 3s Forward delay
param[it]  fwd Forward delay
param      rapid 0 Rapid Spanning Tree, 802.1w (1 = on)
param[it]  rapid Rapid Spanning Tree, 802.1w (1 = attivo)
param+     fast 100M Bandwidth of the fast link s3 - s4
param[it]  fast Banda del collegamento veloce s3 - s4

node s1 switch x=25 y=30
node s2 switch x=75 y=30
node s3 switch x=75 y=75
node s4 switch x=25 y=75
node h1 host x=8 y=12
node h2 host x=92 y=12
link s1 s2 bw=10M delay=2ms
link s2 s3 bw=10M delay=2ms
link s3 s4 bw=$fast delay=2ms
link s4 s1 bw=10M delay=2ms
link h1 s1 delay=1ms
link h2 s2 delay=1ms
ip h1 10.0.0.1/24
ip h2 10.0.0.2/24

run s1 stp hello=$hello maxage=$maxage fwd=$fwd rapid=$rapid portfast=1
run s2 stp hello=$hello maxage=$maxage fwd=$fwd rapid=$rapid portfast=1
run s3 stp hello=$hello maxage=$maxage fwd=$fwd rapid=$rapid portfast=1
run s4 stp hello=$hello maxage=$maxage fwd=$fwd rapid=$rapid portfast=1
run $root stp priority 4096
run h1 sink
run h2 sink
at 16s run h1 ping h2 count=3 interval=500ms
at 18s run h1 cbr h2 rate=40k size=100 duration=27s
at 18s run h2 cbr h1 rate=40k size=100 duration=27s
at 20s node $root down
at 46s run h1 ping h2 count=3 interval=500ms

note 0s Priority 4096 beats the default 32768: the chosen switch wins the election whatever its MAC address.
note[it] 0s La priorità 4096 batte il valore predefinito 32768: lo switch scelto vince l'elezione qualunque sia il suo indirizzo MAC.
note 2s s1 has two paths to the root: through s2 (100 + 100) or through s4 (100 + 10). It takes the cheaper one; the port towards s2 becomes alternate (blocked).
note[it] 2s s1 ha due percorsi verso la radice: attraverso s2 (100 + 100) o attraverso s4 (100 + 10). Sceglie il meno costoso; la porta verso s2 diventa alternate (bloccata).
note 16s h1 pings h2: they are neighbours, but the frames go the long way round s1 - s4 - s3 - s2, because the direct link is the blocked one. A spanning tree is loop-free, not shortest-path.
note[it] 16s h1 fa ping verso h2: sono vicini, ma i frame fanno il giro lungo s1 - s4 - s3 - s2, perché il collegamento diretto è quello bloccato. Uno spanning tree è privo di cicli, non a cammino minimo.
note 20s The root crashes. Its neighbours' links stay up, so nothing is noticed yet: the old information just gets older.
note[it] 20s La radice va in crash. I collegamenti dei vicini restano attivi, quindi per ora nessuno se ne accorge: la vecchia informazione semplicemente invecchia.
note 26s Max-age reached (plain STP; with rapid=1 three missing hellos were enough, at 23 s): the information about the dead root is discarded, the survivors hold a new election and the lowest identifier left wins.
note[it] 26s Raggiunto max-age (STP classico; con rapid=1 sono bastati tre hello mancanti, a 23 s): l'informazione sulla radice morta viene scartata, i superstiti tengono una nuova elezione e vince l'identificatore più basso rimasto.
note 33s The formerly blocked port is forwarding (after two forward delays; with rapid=1 it happened right after the election): h1 and h2 now talk over the direct link.
note[it] 33s La porta prima bloccata ora inoltra (dopo due forward delay; con rapid=1 è successo subito dopo l'elezione): h1 e h2 parlano sul collegamento diretto.

duration 50s
# s3 and s4 carry no host: when one of them dies as root, h1 and h2 can still be connected
at 49900ms run h1 check spare "is_$root == is_s3 || is_$root == is_s4"
# the lowest identifier left takes over (s1, or s2 when s1 is the one that died)
at 49900ms run h1 check election "(is_$root != is_s1 && s1:stp.is_root == 1 && s1:stp.root_changes >= 2) || (is_$root == is_s1 && s2:stp.is_root == 1)"
at 49900ms run h1 check outage "h1:cbr.sent - h2:sink.received"
# a dead bridge is found by silence: max age in plain STP, three hellos in RSTP; then the spare
# port opens: after two forward delays, or after a handshake
# (with fast=10M or less the streams never crossed the root: nothing to recover)
at 49900ms run h1 check recovery "h1:check.spare == 0 || ($rapid == 0 && ($fast <= 10M || h1:check.outage >= ($maxage - 2 * $hello + 2 * $fwd) / 20 - 30) && h1:check.outage <= ($maxage + $hello + 2 * $fwd) / 20 + 30) || ($rapid == 1 && h1:check.outage <= (3 * $hello + 1s) / 20)"
at 49900ms run h1 check reach "(h1:check.spare == 1 && ($maxage + 2 * $fwd > 24s || $fwd > 7s || (h1:ping.reply == 6 && s1:stp.blocked_ports == 0 && s2:stp.blocked_ports == 0))) || (h1:check.spare == 0 && h1:ping.reply == 3)"
expect h1 check.election == 1
expect h1 check.recovery == 1
expect h1 check.reach == 1
